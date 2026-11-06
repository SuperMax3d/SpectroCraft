"""The MCP server end to end: an in-memory MCP client <-> server.py <-> fake NDJSON app."""

from __future__ import annotations

import contextlib
import json
import stat
import sys
from pathlib import Path

import jsonschema
import pytest

mcp = pytest.importorskip("mcp")

import anyio  # noqa: E402
from mcp import ClientSession, types  # noqa: E402
from mcp.server.lowlevel import NotificationOptions  # noqa: E402
from mcp.shared.memory import create_client_server_memory_streams  # noqa: E402

from spectrocraft_mcp.server import Bridge, create_server  # noqa: E402

pytestmark = pytest.mark.anyio

FIXED = {
    "sc_status", "sc_launch", "sc_state", "sc_history", "sc_undo", "sc_redo", "sc_jump", "sc_run_step", "sc_run",
    "sc_open", "sc_save", "sc_close", "sc_models", "sc_install_model", "sc_devices", "sc_noise_suppress",
    "sc_transcribe", "sc_measure_levels", "sc_third_octave", "sc_spectrum", "sc_export_spectrogram", "sc_jobs",
    "sc_cancel", "sc_headless",
}


@contextlib.asynccontextmanager
async def session(notifications: list | None = None):
    server = create_server(Bridge())
    low = server._mcp_server

    async def on_message(msg):
        if notifications is not None:
            notifications.append(msg)

    async with create_client_server_memory_streams() as (client_streams, server_streams):
        async with anyio.create_task_group() as tg:
            tg.start_soon(
                lambda: low.run(server_streams[0], server_streams[1],
                                low.create_initialization_options(NotificationOptions(tools_changed=True)))
            )
            async with ClientSession(client_streams[0], client_streams[1], message_handler=on_message) as cs:
                await cs.initialize()
                yield cs
            tg.cancel_scope.cancel()


def _text(res: types.CallToolResult) -> str:
    return "\n".join(c.text for c in res.content if isinstance(c, types.TextContent))


def _json(res: types.CallToolResult) -> dict:
    assert not res.isError, _text(res)
    return json.loads(_text(res))


async def test_fixed_tools_without_app(control_dir):
    async with session() as cs:
        tools = (await cs.list_tools()).tools
        names = {t.name for t in tools}
        assert FIXED <= names
        assert names == FIXED  # no app: no dynamic tools yet
        for t in tools:
            jsonschema.Draft202012Validator.check_schema(t.inputSchema)
        save = next(t for t in tools if t.name == "sc_save")
        assert "confirm" in save.description and "confirm" in save.inputSchema["properties"]


async def test_no_instance_is_a_clear_tool_error(control_dir):
    async with session() as cs:
        res = await cs.call_tool("sc_state", {})
        assert res.isError
        text = _text(res)
        assert "no_instance" in text and "sc_launch" in text
        status = _json(await cs.call_tool("sc_status", {}))
        assert status["instances"] == [] and status["fileForwardPipeAnswered"] is False
        assert "sc_launch" in status["message"]


async def test_control_disabled_detected_by_pipe_probe(control_dir, monkeypatch):
    from spectrocraft_mcp import server as srv_mod

    monkeypatch.setattr(srv_mod.pipe_probe, "probe_file_forward", lambda *a, **k: True)
    async with session() as cs:
        res = await cs.call_tool("sc_state", {})
        assert res.isError and "control_disabled" in _text(res) and "Allow AI control" in _text(res)


async def test_dynamic_tools_from_describe(fake):
    async with session() as cs:
        tools = (await cs.list_tools()).tools
        names = {t.name for t in tools}
        assert FIXED <= names
        assert {"sc_app_state", "sc_effect_changeGain", "sc_step_run", "sc_doc_save", "sc_selection_set"} <= names
        assert "sc_session_hello" not in names
        for t in tools:
            jsonschema.Draft202012Validator.check_schema(t.inputSchema)
        gain = next(t for t in tools if t.name == "sc_effect_changeGain")
        assert gain.inputSchema["properties"]["db"] == {"type": "number", "minimum": -96, "maximum": 96, "description": "Gain in dB"}
        assert gain.inputSchema["required"] == ["db"]


async def test_dynamic_tool_call_routes_to_app(fake):
    async with session() as cs:
        await cs.list_tools()
        out = _json(await cs.call_tool("sc_step_run", {"step": {"type": "fadeOut"}, "select": "all", "doc": 7}))
        assert out["result"]["applied"] == "fadeOut" and out["undoDepth"] == 1
        req = [r for r in fake.requests if r["cmd"] == "step.run"][-1]
        assert req["doc"] == 7 and "doc" not in req["params"]


async def test_tools_list_changed_after_first_contact(fake):
    notes: list = []
    async with session(notes) as cs:
        # first tool call connects; describe is loaded and the tool list changes
        bridge_state = _json(await cs.call_tool("sc_state", {}))
        assert bridge_state["result"]["activeDoc"] == 7
        await anyio.sleep(0.1)
        kinds = [getattr(getattr(n, "root", n), "method", None) for n in notes]
        assert "notifications/tools/list_changed" in kinds


async def test_run_step_and_undo(fake):
    async with session() as cs:
        out = _json(await cs.call_tool("sc_run_step", {"type": "changeGain", "params": {"db": -3}, "start": 1.25, "end": 3.8, "channels": [0]}))
        assert out["result"]["applied"] == "changeGain"
        req = [r for r in fake.requests if r["cmd"] == "step.run"][-1]
        assert req["params"] == {"step": {"type": "changeGain", "params": {"db": -3}}, "select": {"start": 1.25, "end": 3.8}, "channels": [0]}
        hist = _json(await cs.call_tool("sc_history", {}))
        assert hist["result"]["states"][1]["opId"] == "step.changeGain"
        undo = _json(await cs.call_tool("sc_undo", {}))
        assert undo["undoDepth"] == 0 and undo["redoDepth"] == 1


async def test_app_errors_become_tool_errors_with_hint(fake):
    async with session() as cs:
        res = await cs.call_tool("sc_run_step", {"type": "changeGain"})
        assert res.isError
        t = _text(res)
        assert "invalid_params" in t and "step.params.db" in t
        res = await cs.call_tool("sc_save", {"path": "/tmp/out.wav"})
        assert res.isError and "confirm_required" in _text(res) and "pass confirm:true" in _text(res)
        ok = _json(await cs.call_tool("sc_save", {"path": "/tmp/out.wav", "confirm": True}))
        assert ok["result"]["unsaved"] is False
        res = await cs.call_tool("sc_run_step", {"type": "fadeOut", "start": 1.0})
        assert res.isError and "both start and end" in _text(res)


@pytest.mark.skipif(
    "progress_callback" not in __import__("inspect").signature(ClientSession.call_tool).parameters,
    reason="this mcp release's ClientSession cannot receive progress (server side is unaffected)",
)
async def test_job_progress_relayed(fake):
    seen: list[tuple[float, float | None, str | None]] = []

    async def on_progress(progress, total, message=None):
        seen.append((progress, total, message))

    async with session() as cs:
        res = await cs.call_tool("sc_install_model", {"model_id": "whisper-base"}, progress_callback=on_progress)
        out = _json(res)
        assert out["result"]["installed"] is True and out["job"].startswith("j-")
        assert seen, "no progress notification relayed"
        assert all(t == 1.0 for _, t, _ in seen)
        assert [p for p, _, _ in seen] == sorted(p for p, _, _ in seen)


async def test_job_failure_is_tool_error(fake):
    async with session() as cs:
        res = await cs.call_tool("sc_run", {"cmd": "ai.noiseSuppress", "params": {"fail": True}})
        assert res.isError and "failed" in _text(res) and "j-" in _text(res)


async def test_run_no_wait_and_jobs(fake):
    async with session() as cs:
        out = _json(await cs.call_tool("sc_run", {"cmd": "ai.models.install", "params": {"modelId": "x"}, "wait": False}))
        job = out["result"]["job"]
        listed = _json(await cs.call_tool("sc_jobs", {}))
        assert job in {j["job"] for j in listed["result"]["jobs"]}
        one = _json(await cs.call_tool("sc_jobs", {"job": job}))
        assert one["result"]["job"] == job
        cancel = _json(await cs.call_tool("sc_cancel", {"job": job}))
        assert cancel["result"]["state"] == "cancelling"


async def test_status_lists_instance_and_docs(fake):
    async with session() as cs:
        st = _json(await cs.call_tool("sc_status", {}))
        (inst,) = st["instances"]
        assert inst["pid"] == fake.pid and inst["selected"] is True
        assert inst["app"]["protocol"] == 1
        assert inst["docs"][0]["name"] == "recording_a.wav"
        assert "token" not in json.dumps(st)


async def test_events_from_other_sources_are_reported(fake):
    async with session() as cs:
        _json(await cs.call_tool("sc_state", {}))
        fake.emit("history.changed", {"doc": 7, "undoDepth": 5, "source": "menu"})
        await anyio.sleep(0.2)
        out = _json(await cs.call_tool("sc_state", {}))
        assert any(e["data"].get("source") == "menu" for e in out.get("eventsSinceLastCall", []))


async def test_resources(fake):
    async with session() as cs:
        uris = {str(r.uri) for r in (await cs.list_resources()).resources}
        assert {"spectrocraft://commands", "spectrocraft://journal/tail"} <= uris
        cmds = await cs.read_resource("spectrocraft://commands")
        data = json.loads(cmds.contents[0].text)
        assert any(c["id"] == "effect.changeGain" for c in data["commands"])
        await cs.call_tool("sc_run_step", {"type": "normalize", "select_all": True})
        tail = await cs.read_resource("spectrocraft://journal/tail")
        lines = [json.loads(x) for x in tail.contents[0].text.splitlines() if x.strip()]
        assert any(e["what"] == "history.changed" for e in lines)


@pytest.mark.skipif(sys.platform.startswith("win"), reason="shell wrapper")
async def test_headless_tool(control_dir, tmp_path, monkeypatch):
    fake_exe = Path(__file__).resolve().parent / "fake_exe.py"
    w = tmp_path / "SpectroCraft"
    w.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{fake_exe}" "$@"\n')
    w.chmod(w.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("SPECTROCRAFT_EXE", str(w))
    async with session() as cs:
        out = _json(await cs.call_tool("sc_headless", {"requests": [{"cmd": "app.state"}]}))
        assert out["exitCode"] == 0 and out["responses"][0]["ok"] is True


@pytest.mark.skipif(sys.platform.startswith("win"), reason="shell wrapper")
async def test_launch_tool_starts_and_connects(control_dir, tmp_path, monkeypatch):
    fake_exe = Path(__file__).resolve().parent / "fake_exe.py"
    w = tmp_path / "SpectroCraft"
    w.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{fake_exe}" "$@"\n')
    w.chmod(w.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("SPECTROCRAFT_EXE", str(w))
    monkeypatch.setenv("FAKE_EXE_LIFETIME", "10")
    async with session() as cs:
        out = _json(await cs.call_tool("sc_launch", {}))
        try:
            assert out["launched"] is True and out["app"]["protocol"] == 1
            again = _json(await cs.call_tool("sc_launch", {}))
            assert again["launched"] is False
            st = _json(await cs.call_tool("sc_state", {}))
            assert st["result"]["activeDoc"] == 7
        finally:
            import os
            os.kill(out["pid"], 15)
