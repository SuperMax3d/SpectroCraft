"""Against the real SpectroCraft executable in --json mode. Skipped unless SPECTROCRAFT_EXE is set.

    SPECTROCRAFT_EXE=/path/to/SpectroCraft pytest tools/mcp/tests/test_real_exe.py
"""

from __future__ import annotations

import os

import jsonschema
import pytest

from spectrocraft_mcp import dynamic, launcher

EXE = os.environ.get("SPECTROCRAFT_EXE")
pytestmark = pytest.mark.skipif(not EXE, reason="SPECTROCRAFT_EXE not set")


def _run(reqs, **kw):
    return launcher.run_headless(reqs, exe=EXE, timeout_s=120, **kw)


def test_describe_generates_valid_tools():
    res = _run([{"cmd": "app.describe", "params": {"withSchemas": True}}])
    assert res["exitCode"] == 0, res
    (reply,) = res["responses"]
    assert reply["ok"] is True and reply["id"] == "h1"
    describe = reply["result"]
    assert describe.get("protocol", 1) == 1
    tools = dynamic.build_tools(describe)
    assert tools, "app.describe published no command"
    for t in tools:
        jsonschema.Draft202012Validator.check_schema(t.input_schema)
    ids = {c["id"] for c in describe["commands"]}
    assert {"app.state", "step.run"} <= ids


def test_state_and_unknown_command_exit_codes():
    ok = _run([{"cmd": "app.state", "params": {}}])
    assert ok["exitCode"] == 0 and ok["responses"][0]["ok"] is True
    bad = _run([{"cmd": "no.such.command", "params": {}}])
    assert bad["exitCode"] == 1
    assert bad["responses"][0]["error"]["code"] == "unknown_cmd"


def test_step_validate():
    res = _run(
        [
            {"cmd": "step.validate", "params": {"step": {"type": "changeGain", "params": {"db": 6.0}}}},
            {"cmd": "step.validate", "params": {"step": {"type": "envelop", "params": {}}}},
        ],
        continue_on_error=True,
    )
    good, bad = res["responses"]
    assert good["ok"] is True
    assert bad["ok"] is False and bad["error"]["code"] == "invalid_params"
