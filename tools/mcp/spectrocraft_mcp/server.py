"""SpectroCraft MCP server (stdio) for Claude Code (RAPPORT.md 7).

Fixed tools (section 7.3) are FastMCP functions. Dynamic tools, one per
command of ``app.describe``, are served by overriding ``FastMCP.list_tools``
/ ``FastMCP.call_tool``, because FastMCP derives schemas from Python
signatures and cannot take a raw ``inputSchema``; their schema is the
command's ``params`` schema unchanged (see ``dynamic.py``).

Blocking socket work runs in worker threads; job progress is relayed to
Claude Code with ``ctx.report_progress``. App errors become MCP tool errors
carrying the code, message, hint and the next step to take.

Note: no ``from __future__ import annotations`` here, FastMCP 1.x inspects
real annotations to find the ``Context`` parameter.
"""

import inspect
import json
import logging
import os
import sys
import threading
import time
from collections import deque
from pathlib import Path
from typing import Annotated, Any, Awaitable, Callable, Iterable

import anyio
import anyio.from_thread
import anyio.to_thread
import mcp.types as types
from mcp.server.fastmcp import Context, FastMCP
from mcp.server.fastmcp.exceptions import ToolError
from mcp.server.lowlevel import NotificationOptions
from mcp.server.lowlevel.helper_types import ReadResourceContents
from pydantic import Field

from . import discovery, dynamic, launcher, pipe_probe
from .client import ControlClient
from .protocol import CLIENT_NAME, ControlError

log = logging.getLogger("spectrocraft_mcp")

JOB_WAIT_S = float(os.environ.get("SPECTROCRAFT_JOB_WAIT_S", "600"))
"""How long a tool waits for a job before returning "still running" (the job keeps going)."""

INSTRUCTIONS = """\
Drive the SpectroCraft audio/spectral editor that is open on this machine.
Start with sc_status (or sc_launch if nothing runs). sc_state lists open documents:
address them by their stable integer docId (`doc`), or by absolute path; omit `doc` for the active one.
Times are seconds, channels are 0-based, levels are dB with an explicit unit (dBFS or dB-SPL).
Edits go through sc_run_step (any of the 20 action-step types on an explicit start/end range and
channels) or the generated sc_<domain>_<command> tools; every edit is undoable (sc_undo, sc_history).
Long operations (model install, noise suppression, transcription, big files) are jobs: tools wait
and relay progress; sc_jobs / sc_cancel manage them. Destructive calls (overwrite, close unsaved,
paths outside the allowed roots) fail with confirm_required: ask the user, then retry with confirm=true.
sc_headless runs commands in a separate, windowless SpectroCraft (--json) when no app is open.
"""


# ---------------------------------------------------------------------------- bridge


class Bridge:
    """Connection to the selected instance + cached ``app.describe`` (thread-safe, blocking)."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.pid: int | None = None
        self.client: ControlClient | None = None
        self.describe: dict[str, Any] | None = None
        self._describe_identity: Any = None
        self._tools: dict[str, dynamic.DynamicTool] = {}
        self.reserved_names: set[str] = set()
        self.tools_dirty = False
        self._last_quick_attempt = 0.0

    # -- connection

    def select(self, pid: int | None) -> None:
        with self._lock:
            if pid != self.pid:
                self.pid = pid
                if self.client is not None:
                    self.client.close()
                    self.client = None

    def client_for(self, pid: int | None = None, *, allow_launch: bool = True) -> ControlClient:
        if pid is not None:
            self.select(pid)
        with self._lock:
            if self.client is None:
                self.client = ControlClient(locate=lambda: discovery.find_instance(self.pid), client_name=CLIENT_NAME)
            c = self.client
        try:
            c.connect()
        except ControlError as e:
            if e.code != "no_instance":
                raise
            if pipe_probe.probe_file_forward():
                raise launcher.control_disabled_error() from e
            if allow_launch and self.pid is None and launcher.launch_policy() == "auto":
                launcher.launch()
                c.connect()
            elif self.pid is not None:
                raise ControlError(
                    "no_instance",
                    f"no live SpectroCraft instance with pid {self.pid}",
                    {"hint": "call sc_status to list instances, or sc_run(..., pid=None) to use the most recent"},
                ) from e
            else:
                raise launcher.no_instance_error() from e
        self.refresh_describe(c)
        return c

    def quick_connect(self) -> None:
        """Best effort, no launch, no pipe probe: used by list_tools to load dynamic tools early."""
        now = time.monotonic()
        if self.describe is not None or now - self._last_quick_attempt < 10.0:
            return
        self._last_quick_attempt = now
        if not discovery.list_instances():
            return
        try:
            self.client_for(allow_launch=False)
        except ControlError as e:
            log.info("dynamic tools not loaded yet: %s", e)

    # -- dynamic tools

    def refresh_describe(self, c: ControlClient, force: bool = False) -> None:
        inst = c.instance
        ident = (inst.pid, inst.raw.get("startedAt")) if inst else None
        if not force and self.describe is not None and ident == self._describe_identity:
            return
        try:
            res = c.result("app.describe", {"withSchemas": True})
        except ControlError as e:
            log.warning("app.describe failed: %s", e)
            return
        if isinstance(res, dict):
            self.set_describe(res, ident)

    def set_describe(self, describe: dict[str, Any], identity: Any = None) -> bool:
        tools = {t.name: t for t in dynamic.build_tools(describe, self.reserved_names)}
        with self._lock:
            old = {n: (t.command, json.dumps(t.input_schema, sort_keys=True)) for n, t in self._tools.items()}
            new = {n: (t.command, json.dumps(t.input_schema, sort_keys=True)) for n, t in tools.items()}
            self.describe = describe
            self._describe_identity = identity
            self._tools = tools
            # Query commands are safe to resend after a lost connection.
            queries = {str(c.get("id")) for c in describe.get("commands") or []
                       if isinstance(c, dict) and "Query" in (c.get("flags") or [])}
            if self.client is not None:
                self.client.read_only_cmds |= queries
            changed = old != new
            if changed:
                self.tools_dirty = True
            return changed

    def dynamic_tools(self) -> list[dynamic.DynamicTool]:
        with self._lock:
            return list(self._tools.values())

    def dynamic_tool(self, name: str) -> dynamic.DynamicTool | None:
        with self._lock:
            return self._tools.get(name)


# ---------------------------------------------------------------------------- helpers


def _clean(**kw: Any) -> dict[str, Any]:
    """Drop None values (unset optional tool arguments are not sent to the app)."""
    return {k: v for k, v in kw.items() if v is not None}


def _envelope_text(reply: dict[str, Any], client: ControlClient | None) -> str:
    out = {k: v for k, v in reply.items() if k not in ("id", "ok")}
    if client is not None:
        own = "api:" + client.client_name
        others = [
            e
            for e in client.drain_events()
            if not str(e.get("event", "")).startswith("job.")
            and (e.get("data") or {}).get("source") not in (own, "api:" + client.client_name.split("/")[0])
        ]
        if others:
            # Changes made meanwhile by the user (menus) or by another client.
            out["eventsSinceLastCall"] = others[-20:]
    return json.dumps(out, ensure_ascii=False, indent=1, default=str)


async def _report(ctx: Context, progress: float | None, status: str | None) -> None:
    value = 0.0 if progress is None else float(progress)
    try:
        await ctx.report_progress(value, 1.0, status)
    except TypeError:  # mcp releases whose report_progress has no message argument
        await ctx.report_progress(value, 1.0)


class _Relay:
    """Progress callback usable from the worker thread."""

    def __init__(self, ctx: Context | None) -> None:
        self.ctx = ctx

    def __call__(self, progress: float | None, status: str | None) -> None:
        if self.ctx is None:
            return
        try:
            anyio.from_thread.run(_report, self.ctx, progress, status)
        except Exception:  # no progress token, client gone...
            log.debug("progress relay failed", exc_info=True)


def _make_runner(bridge: Bridge, notify: Callable[[Context | None], Any]):
    async def in_thread(ctx: Context | None, fn: Callable[[_Relay], str]) -> str:
        relay = _Relay(ctx)
        try:
            out = await anyio.to_thread.run_sync(lambda: fn(relay), abandon_on_cancel=True)
        except ControlError as e:
            await notify(ctx)
            raise ToolError(e.describe()) from e
        await notify(ctx)
        return out

    async def run(
        ctx: Context | None,
        cmd: str,
        params: dict[str, Any] | None = None,
        doc: Any = None,
        timeout_ms: int | None = None,
        wait: bool = True,
        pid: int | None = None,
    ) -> str:
        def work(relay: _Relay) -> str:
            c = bridge.client_for(pid)
            try:
                reply = c.run(cmd, params or {}, doc, timeout_ms, wait=wait, on_progress=relay, job_timeout=JOB_WAIT_S)
            except ControlError as e:
                if e.code == "timeout" and e.details.get("stillRunning"):
                    return json.dumps(
                        {"job": e.details.get("job"), "state": "running", "progress": e.details.get("progress"),
                         "note": f"still running after {JOB_WAIT_S:.0f} s; follow it with sc_jobs(job) or stop it with sc_cancel"},
                        indent=1,
                    )
                raise
            return _envelope_text(reply, c)

        return await in_thread(ctx, work)

    return in_thread, run


def _tool_kwargs(read_only: bool = False, destructive: bool = False, title: str | None = None) -> dict[str, Any]:
    params = inspect.signature(FastMCP.tool).parameters
    kw: dict[str, Any] = {}
    if "structured_output" in params:
        kw["structured_output"] = False
    if "annotations" in params and hasattr(types, "ToolAnnotations"):
        kw["annotations"] = types.ToolAnnotations(
            title=title, readOnlyHint=read_only, destructiveHint=destructive, openWorldHint=False
        )
    return kw


def _mcp_tool(t: dynamic.DynamicTool) -> types.Tool:
    kw: dict[str, Any] = {"name": t.name, "description": t.description, "inputSchema": t.input_schema}
    fields = getattr(types.Tool, "model_fields", {})
    if "annotations" in fields and hasattr(types, "ToolAnnotations"):
        kw["annotations"] = types.ToolAnnotations(**t.annotations())
    if "title" in fields:
        kw["title"] = t.title
    return types.Tool(**kw)


# ---------------------------------------------------------------------------- server


class SpectroCraftMCP(FastMCP):
    """FastMCP + dynamic tools generated from ``app.describe``."""

    def __init__(self, bridge: Bridge, **kw: Any) -> None:
        super().__init__(**kw)
        self.bridge = bridge
        self._run_dynamic: Callable[..., Any] | None = None
        # uri -> (name, description, mime type, async reader). Served here rather than with
        # @resource because older FastMCP 1.x releases do not await async resource functions.
        self.sc_resources: dict[str, tuple[str, str, str, Callable[[], Awaitable[str]]]] = {}

    def add_sc_resource(self, uri: str, name: str, description: str, mime_type: str, reader: Callable[[], Awaitable[str]]) -> None:
        self.sc_resources[uri] = (name, description, mime_type, reader)

    async def list_resources(self) -> list[types.Resource]:
        res = list(await super().list_resources())
        for uri, (name, description, mime, _reader) in self.sc_resources.items():
            res.append(types.Resource(uri=uri, name=name, description=description, mimeType=mime))
        return res

    async def read_resource(self, uri: Any) -> Iterable[ReadResourceContents]:
        entry = self.sc_resources.get(str(uri))
        if entry is None:
            return await super().read_resource(uri)
        return [ReadResourceContents(content=await entry[3](), mime_type=entry[2])]

    async def list_tools(self) -> list[types.Tool]:
        tools = await super().list_tools()
        self.bridge.reserved_names = {t.name for t in tools}
        if self.bridge.describe is None:
            try:
                with anyio.fail_after(5):
                    await anyio.to_thread.run_sync(self.bridge.quick_connect, abandon_on_cancel=True)
            except TimeoutError:
                pass
        fixed = {t.name for t in tools}
        tools.extend(_mcp_tool(t) for t in self.bridge.dynamic_tools() if t.name not in fixed)
        return tools

    async def call_tool(self, name: str, arguments: dict[str, Any]) -> Any:
        t = self.bridge.dynamic_tool(name)
        if t is None or self._run_dynamic is None:
            return await super().call_tool(name, arguments)
        params, doc, timeout_ms = t.split_arguments(arguments)
        text = await self._run_dynamic(self.get_context(), t.command, params, doc, timeout_ms)
        return [types.TextContent(type="text", text=text)]

    async def run_stdio_async(self) -> None:
        from mcp.server.stdio import stdio_server

        async with stdio_server() as (read_stream, write_stream):
            await self._mcp_server.run(
                read_stream,
                write_stream,
                self._mcp_server.create_initialization_options(NotificationOptions(tools_changed=True)),
            )


Doc = Annotated[
    int | str | None,
    Field(description="Target document: docId from sc_state or absolute path; omit for the active document"),
]
TimeoutMs = Annotated[int | None, Field(description="Request timeout in ms (default 30000; 0 = unlimited)")]
Wait = Annotated[bool, Field(description="Wait for the job to finish (progress is relayed); false returns the job id at once")]


def create_server(bridge: Bridge | None = None) -> SpectroCraftMCP:
    bridge = bridge or Bridge()
    mcp = SpectroCraftMCP(bridge, name="spectrocraft", instructions=INSTRUCTIONS)

    async def notify(ctx: Context | None) -> None:
        if ctx is None or not bridge.tools_dirty:
            return
        bridge.tools_dirty = False
        try:
            await ctx.session.send_tool_list_changed()
        except Exception:
            log.debug("tools/list_changed not sent", exc_info=True)

    in_thread, run = _make_runner(bridge, notify)
    mcp._run_dynamic = run

    # ------------------------------------------------------------------ instances

    @mcp.tool(**_tool_kwargs(read_only=True, title="SpectroCraft status"))
    async def sc_status(
        ctx: Context,
        pid: Annotated[int | None, Field(description="Select this instance for the following calls")] = None,
    ) -> str:
        """List running SpectroCraft instances (pid, version, open documents) and say which one the tools talk to.
        When none is found, explains why: AI control disabled in the open app, or nothing running."""

        def work(_relay: _Relay) -> str:
            if pid is not None:
                bridge.select(pid)
            insts = discovery.list_instances()
            selected = discovery.choose_instance(insts, bridge.pid)
            out: dict[str, Any] = {
                "controlDir": str(discovery.control_dir()),
                "launchPolicy": launcher.launch_policy(),
                "exe": os.environ.get("SPECTROCRAFT_EXE"),
                "instances": [],
            }
            for inst in insts:
                entry: dict[str, Any] = inst.summary()
                is_sel = selected is not None and inst.pid == selected.pid
                entry["selected"] = is_sel
                try:
                    if is_sel:
                        c = bridge.client_for(allow_launch=False)
                        hello = c.hello or {}
                        state = c.result("app.state")
                    else:
                        with ControlClient(inst) as tmp:
                            hello = tmp.connect()
                            state = tmp.result("app.state")
                    entry["app"] = {k: hello.get(k) for k in ("version", "protocol", "platform", "capabilities", "commands", "journalSeq")}
                    state = state if isinstance(state, dict) else {}
                    entry["activeDoc"] = state.get("activeDoc")
                    entry["docs"] = [
                        {k: d.get(k) for k in ("id", "name", "path", "durationSec", "channels", "sampleRate", "unsaved")}
                        for d in state.get("docs") or []
                        if isinstance(d, dict)
                    ]
                    entry["busy"] = state.get("busy")
                except ControlError as e:
                    entry["error"] = e.describe()
                out["instances"].append(entry)
            if not insts:
                answered = pipe_probe.probe_file_forward()
                out["fileForwardPipeAnswered"] = answered
                out["message"] = (launcher.control_disabled_error() if answered else launcher.no_instance_error()).describe()
            out["dynamicTools"] = len(bridge.dynamic_tools())
            return json.dumps(out, ensure_ascii=False, indent=1)

        return await in_thread(ctx, work)

    @mcp.tool(**_tool_kwargs(title="Launch SpectroCraft"))
    async def sc_launch(
        ctx: Context,
        files: Annotated[list[str] | None, Field(description="Absolute paths of audio files to open")] = None,
        force: Annotated[bool, Field(description="Start a new instance even if one is open (with or without AI control)")] = False,
    ) -> str:
        """Start SpectroCraft ($SPECTROCRAFT_EXE --control) and connect to it. If a controllable instance already
        runs, opens `files` in it instead. If an instance is open with AI control disabled, fails with a clear
        message unless force=true (then a second, controllable instance starts)."""

        def work(_relay: _Relay) -> str:
            live = discovery.list_instances()
            if live and not force:
                c = bridge.client_for(allow_launch=False)
                opened = [c.call("doc.open", {"path": f, "activate": True}).get("result") for f in files or []]
                return json.dumps({"launched": False, "reason": "already running", "pid": c.instance.pid if c.instance else None,
                                   "opened": opened}, ensure_ascii=False, indent=1)
            inst = launcher.launch(files or [], force=force)
            # forced: pin the new instance (an older one may coexist); otherwise "most recent" is it
            bridge.select(inst.pid if force else None)
            c = bridge.client_for()
            return json.dumps({"launched": True, "pid": inst.pid, "port": inst.port, "app": c.hello}, ensure_ascii=False, indent=1)

        return await in_thread(ctx, work)

    # ------------------------------------------------------------------ state / history

    @mcp.tool(**_tool_kwargs(read_only=True, title="Application state"))
    async def sc_state(
        ctx: Context, pid: Annotated[int | None, Field(description="Instance pid (default: selected / most recent)")] = None
    ) -> str:
        """Full application state: open documents (docId, path, duration, channels, rate, unsaved, undo depth),
        active document, selection, playhead, view, transport, AI settings, busy/modal flags, running jobs."""
        return await run(ctx, "app.state", {}, pid=pid)

    @mcp.tool(**_tool_kwargs(read_only=True, title="Edit history"))
    async def sc_history(
        ctx: Context,
        doc: Doc = None,
        include_journal: Annotated[bool, Field(description="Include the session journal entries")] = True,
        since: Annotated[int | None, Field(description="Only journal entries after this seq")] = None,
    ) -> str:
        """Undo history of a document (states with opId, params, timestamps, source) plus the session journal."""
        return await run(ctx, "history.list", _clean(includeJournal=include_journal, since=since), doc)

    @mcp.tool(**_tool_kwargs(title="Undo"))
    async def sc_undo(ctx: Context, steps: Annotated[int, Field(ge=1, le=64)] = 1, doc: Doc = None) -> str:
        """Undo the last `steps` edits of a document."""
        return await run(ctx, "history.undo", {"steps": steps}, doc)

    @mcp.tool(**_tool_kwargs(title="Redo"))
    async def sc_redo(ctx: Context, steps: Annotated[int, Field(ge=1, le=64)] = 1, doc: Doc = None) -> str:
        """Redo `steps` undone edits of a document."""
        return await run(ctx, "history.redo", {"steps": steps}, doc)

    @mcp.tool(**_tool_kwargs(title="Jump in history"))
    async def sc_jump(ctx: Context, state: Annotated[int, Field(ge=0, description="Target state index from sc_history")], doc: Doc = None) -> str:
        """Move to history state `state` (undo/redo as needed). Never destroys the redo stack; the next edit does."""
        return await run(ctx, "history.jumpTo", {"state": state}, doc)

    # ------------------------------------------------------------------ editing

    @mcp.tool(**_tool_kwargs(title="Run an action step"))
    async def sc_run_step(
        ctx: Context,
        type: Annotated[
            str,
            Field(description="Step type: silence, fadeIn, fadeOut, normalize, reverse, invert, changeGain, timeStretch, "
                              "catTimeScale, filterEq, envelope, resample, delete, trim, genSilence, genNoise, genTone, "
                              "noiseSuppress, transcribe, python"),
        ],
        params: Annotated[dict[str, Any] | None, Field(description='Step parameters, e.g. {"db": -3.0} for changeGain')] = None,
        start: Annotated[float | None, Field(ge=0, description="Range start in seconds (with end)")] = None,
        end: Annotated[float | None, Field(ge=0, description="Range end in seconds (with start)")] = None,
        select_all: Annotated[bool, Field(description="Apply to the whole file (ignored when start/end are given)")] = False,
        channels: Annotated[list[int] | None, Field(description="0-based channels; omit for the current/all channels")] = None,
        doc: Doc = None,
        wait: Wait = True,
        timeout_ms: TimeoutMs = None,
    ) -> str:
        """Apply one of the 20 action-step types on an explicit time range and channels (step.run).
        Without start/end/select_all the current selection is used. Undoable with sc_undo."""
        if (start is None) != (end is None):
            raise ToolError("give both start and end (seconds), or neither")
        p: dict[str, Any] = {"step": {"type": type, "params": params or {}}}
        if start is not None and end is not None:
            if end <= start:
                raise ToolError("end must be greater than start")
            p["select"] = {"start": start, "end": end}
        elif select_all:
            p["select"] = "all"
        if channels is not None:
            p["channels"] = channels
        return await run(ctx, "step.run", p, doc, timeout_ms, wait)

    @mcp.tool(**_tool_kwargs(title="Run any command"))
    async def sc_run(
        ctx: Context,
        cmd: Annotated[str, Field(description="Command id, e.g. selection.set, markers.add, view.zoom (see spectrocraft://commands)")],
        params: Annotated[dict[str, Any] | None, Field(description="Command parameters")] = None,
        doc: Doc = None,
        wait: Wait = True,
        timeout_ms: TimeoutMs = None,
        pid: Annotated[int | None, Field(description="Instance pid (default: selected / most recent)")] = None,
    ) -> str:
        """Send any SpectroCraft command (pass-through). Jobs are awaited with progress unless wait=false."""
        return await run(ctx, cmd, params or {}, doc, timeout_ms, wait, pid)

    # ------------------------------------------------------------------ documents

    @mcp.tool(**_tool_kwargs(title="Open a file"))
    async def sc_open(
        ctx: Context,
        path: Annotated[str, Field(description="Absolute path of the audio file")],
        activate: bool = True,
        confirm: Annotated[bool, Field(description="Only after the user agreed (path outside the allowed roots)")] = False,
    ) -> str:
        """Open an audio file; returns its docId. Files over 200 MB load as a job (progress relayed).
        `confirm` is required (after asking the user) for paths outside the allowed roots."""
        return await run(ctx, "doc.open", _clean(path=path, activate=activate, confirm=confirm or None))

    @mcp.tool(**_tool_kwargs(destructive=True, title="Save a document"))
    async def sc_save(
        ctx: Context,
        path: Annotated[str | None, Field(description="Absolute target path (doc.saveAs); omit to save in place (doc.save)")] = None,
        meta: Annotated[str | None, Field(description="Metadata storage: auto (keep), embedded (RIFF chunks) or sidecar (.meta.json)")] = None,
        confirm: Annotated[bool, Field(description="REQUIRED to overwrite an existing file: ask the user first")] = False,
        doc: Doc = None,
    ) -> str:
        """Save a document: in place (doc.save, always overwrites, so confirm is required) or to `path`
        (doc.saveAs, confirm required when the target exists). Without confirm the app answers
        confirm_required: ask the user, then call again with confirm=true."""
        if path is None:
            return await run(ctx, "doc.save", _clean(meta=meta, confirm=confirm or None), doc)
        return await run(ctx, "doc.saveAs", _clean(path=path, meta=meta, confirm=confirm or None), doc)

    @mcp.tool(**_tool_kwargs(destructive=True, title="Close a document"))
    async def sc_close(
        ctx: Context,
        doc: Doc = None,
        confirm: Annotated[bool, Field(description="REQUIRED to discard unsaved changes: ask the user first")] = False,
    ) -> str:
        """Close a document. With unsaved changes it fails with confirm_required: save first (sc_save) or,
        after asking the user, pass confirm=true to discard them."""
        return await run(ctx, "doc.close", _clean(confirm=confirm or None), doc)

    # ------------------------------------------------------------------ AI

    @mcp.tool(**_tool_kwargs(read_only=True, title="AI models"))
    async def sc_models(ctx: Context) -> str:
        """List AI models (noise suppression, Whisper, audio.cpp): installed, verified, size, backends, platforms."""
        return await run(ctx, "ai.models.list", {})

    @mcp.tool(**_tool_kwargs(title="Install an AI model"))
    async def sc_install_model(ctx: Context, model_id: Annotated[str, Field(description="Model id from sc_models")], wait: Wait = True) -> str:
        """Download and verify an AI model (job; progress relayed)."""
        return await run(ctx, "ai.models.install", {"modelId": model_id}, wait=wait)

    @mcp.tool(**_tool_kwargs(read_only=True, title="AI devices"))
    async def sc_devices(
        ctx: Context, init: Annotated[bool, Field(description="Initialise OpenVINO to list real devices (slow the first time)")] = False
    ) -> str:
        """Devices / backends really available per AI engine (OpenVINO, Whisper, audio.cpp)."""
        return await run(ctx, "ai.devices", {"init": init}, wait=True)

    @mcp.tool(**_tool_kwargs(title="Noise suppression"))
    async def sc_noise_suppress(
        ctx: Context,
        model_id: Annotated[str, Field(description="e.g. deepfilternet2, deepfilternet3")] = "deepfilternet2",
        device: Annotated[str, Field(description="auto or a device listed by sc_devices (CPU)")] = "auto",
        atten_lim_db: Annotated[float | None, Field(description="Attenuation limit in dB")] = None,
        df3_post_filter: bool | None = None,
        copy_noise: Annotated[bool | None, Field(description="Copy the removed noise to a new channel/document")] = None,
        auto_download: Annotated[bool | None, Field(description="Download the model if missing")] = None,
        doc: Doc = None,
        wait: Wait = True,
        timeout_ms: TimeoutMs = None,
    ) -> str:
        """AI noise suppression on the current selection (set it first with sc_selection_set or sc_run('selection.set')).
        Runs as a job with progress; undoable."""
        p = _clean(modelId=model_id, device=device, attenLimDb=atten_lim_db, df3PostFilter=df3_post_filter,
                   copyNoise=copy_noise, autoDownload=auto_download)
        return await run(ctx, "ai.noiseSuppress", p, doc, timeout_ms, wait)

    @mcp.tool(**_tool_kwargs(title="Transcribe"))
    async def sc_transcribe(
        ctx: Context,
        model_id: Annotated[str, Field(description="Whisper model id, e.g. whisper-base")] = "whisper-base",
        language: Annotated[str | None, Field(description="ISO code (fr, en...) or omit for auto")] = None,
        translate: bool | None = None,
        fast: bool | None = None,
        channels: Annotated[list[int] | None, Field(description="0-based channels")] = None,
        store_in_doc: Annotated[bool | None, Field(description="Store the transcription in the document")] = None,
        doc: Doc = None,
        wait: Wait = True,
        timeout_ms: TimeoutMs = None,
    ) -> str:
        """Whisper transcription (job with progress); returns segments with start/end seconds and text."""
        p = _clean(modelId=model_id, language=language, translate=translate, fast=fast, channels=channels, storeInDoc=store_in_doc)
        return await run(ctx, "ai.transcribe", p, doc, timeout_ms, wait)

    # ------------------------------------------------------------------ measures

    @mcp.tool(**_tool_kwargs(read_only=True, title="Levels"))
    async def sc_measure_levels(
        ctx: Context,
        start: Annotated[float, Field(ge=0, description="seconds")],
        end: Annotated[float, Field(ge=0, description="seconds")],
        channels: Annotated[list[int] | None, Field(description="0-based channels")] = None,
        doc: Doc = None,
    ) -> str:
        """RMS, peak (and its time), crest factor, DC offset, clipped samples per channel, in dBFS plus calibrated
        absolute levels (dB-SPL) when the document has a calibration."""
        return await run(ctx, "measure.levels", _clean(start=start, end=end, channels=channels), doc)

    @mcp.tool(**_tool_kwargs(read_only=True, title="Third-octave bands"))
    async def sc_third_octave(
        ctx: Context,
        start: Annotated[float, Field(ge=0, description="seconds")],
        end: Annotated[float, Field(ge=0, description="seconds")],
        channel: Annotated[int, Field(ge=0)] = 0,
        mode: Annotated[str, Field(description="reference (calibrated, as on screen) or iec (26 uncalibrated bands)")] = "reference",
        order: int | None = None,
        doc: Doc = None,
    ) -> str:
        """1/3-octave band levels of a region (relative dB and calibrated dB-SPL), identical to the app's view."""
        return await run(ctx, "measure.thirdOctave", _clean(start=start, end=end, channel=channel, mode=mode, order=order), doc)

    @mcp.tool(**_tool_kwargs(read_only=True, title="Spectrum"))
    async def sc_spectrum(
        ctx: Context,
        start: Annotated[float, Field(ge=0, description="seconds")],
        end: Annotated[float, Field(ge=0, description="seconds")],
        channels: Annotated[list[int] | None, Field(description="0-based channels")] = None,
        fft_size: int | None = None,
        window: Annotated[str | None, Field(description="e.g. hann")] = None,
        max_peaks: Annotated[int | None, Field(description="Also detect up to N spectral peaks")] = None,
        min_prominence_db: float | None = None,
        doc: Doc = None,
    ) -> str:
        """Averaged magnitude spectrum of a region, optionally with peak detection."""
        peaks = _clean(maxCount=max_peaks, minProminenceDb=min_prominence_db) or None
        p = _clean(start=start, end=end, channels=channels, fftSize=fft_size, window=window, peaks=peaks)
        return await run(ctx, "measure.spectrum", p, doc)

    @mcp.tool(**_tool_kwargs(title="Export spectrogram PNG"))
    async def sc_export_spectrogram(
        ctx: Context,
        path: Annotated[str, Field(description="Absolute .png path")],
        start: float | None = None,
        end: float | None = None,
        width: int | None = None,
        height: int | None = None,
        axes: bool | None = None,
        markers: bool | None = None,
        selections: bool | None = None,
        drawings: bool | None = None,
        confirm: Annotated[bool, Field(description="REQUIRED to overwrite an existing file: ask the user first")] = False,
        doc: Doc = None,
        wait: Wait = True,
    ) -> str:
        """Render the spectrogram to PNG (job). Overlays need the open app window."""
        overlays = _clean(axes=axes, markers=markers, selections=selections, drawings=drawings) or None
        p = _clean(path=path, start=start, end=end, width=width, height=height, overlays=overlays, confirm=confirm or None)
        return await run(ctx, "export.spectrogram", p, doc, None, wait)

    # ------------------------------------------------------------------ jobs

    @mcp.tool(**_tool_kwargs(read_only=True, title="Jobs"))
    async def sc_jobs(ctx: Context, job: Annotated[str | None, Field(description="Job id (j-N); omit to list jobs")] = None) -> str:
        """State, progress and result of one job, or the list of jobs."""
        if job is not None:
            return await run(ctx, "job.get", {"job": job})

        def work(_relay: _Relay) -> str:
            c = bridge.client_for()
            try:
                return _envelope_text(c.call("job.list", {}), c)
            except ControlError as e:
                if e.code != "unknown_cmd":
                    raise
                state = c.result("app.state") or {}
                return json.dumps({"jobs": state.get("jobs", []), "source": "app.state"}, ensure_ascii=False, indent=1)

        return await in_thread(ctx, work)

    @mcp.tool(**_tool_kwargs(title="Cancel a job"))
    async def sc_cancel(
        ctx: Context,
        job: Annotated[str, Field(description="Job id (j-N)")],
        force: Annotated[bool, Field(description="Cancel a job started by another client")] = False,
    ) -> str:
        """Ask SpectroCraft to cancel a job (it stops at the next safe point; nothing is committed)."""
        return await run(ctx, "job.cancel", _clean(job=job, force=force or None))

    # ------------------------------------------------------------------ headless

    @mcp.tool(**_tool_kwargs(title="Headless SpectroCraft"))
    async def sc_headless(
        ctx: Context,
        requests: Annotated[
            list[dict[str, Any]],
            Field(description='NDJSON requests, e.g. [{"cmd":"doc.open","params":{"path":"/abs/a.wav"}}, '
                              '{"cmd":"step.run","params":{"select":"all","step":{"type":"normalize","params":{}}}}, '
                              '{"cmd":"doc.saveAs","params":{"path":"/abs/b.wav"}}]'),
        ],
        continue_on_error: bool = False,
        timeout_s: Annotated[float, Field(gt=0, le=3600)] = 300.0,
    ) -> str:
        """Run commands in a separate, windowless SpectroCraft ($SPECTROCRAFT_EXE --json - --json-out <tmp>):
        no open app needed, no session.hello, jobs are awaited inline. Exit code 0 = all ok, 1 = an error, 2 = usage."""

        def work(_relay: _Relay) -> str:
            res = launcher.run_headless(requests, continue_on_error=continue_on_error, timeout_s=timeout_s)
            return json.dumps(res, ensure_ascii=False, indent=1)

        return await in_thread(ctx, work)

    # ------------------------------------------------------------------ resources

    async def commands_resource() -> str:
        def work(_relay: _Relay) -> str:
            c = bridge.client_for(allow_launch=False)
            res = c.result("app.describe", {"withSchemas": True})
            if isinstance(res, dict):
                inst = c.instance
                bridge.set_describe(res, (inst.pid, inst.raw.get("startedAt")) if inst else None)
            return json.dumps(res, ensure_ascii=False, indent=1)

        try:
            return await in_thread(None, work)
        except ToolError as e:
            return json.dumps({"error": str(e)}, ensure_ascii=False)

    async def journal_resource() -> str:
        def work(_relay: _Relay) -> str:
            return journal_tail(bridge)

        try:
            return await in_thread(None, work)
        except ToolError as e:
            return json.dumps({"error": str(e)}, ensure_ascii=False)

    mcp.add_sc_resource("spectrocraft://commands", "commands",
                        "app.describe of the connected SpectroCraft: every command with its JSON Schema",
                        "application/json", commands_resource)
    mcp.add_sc_resource("spectrocraft://journal/tail", "journal-tail",
                        "Last entries of the SpectroCraft session journal (JSON Lines)",
                        "application/x-ndjson", journal_resource)
    return mcp


def _tail_lines(path: Path, n: int) -> list[str]:
    try:
        with path.open("rb") as f:
            lines: deque[bytes] = deque(f, maxlen=n)
    except OSError:
        return []
    return [line.decode("utf-8", errors="replace").rstrip("\n") for line in lines]


def journal_tail(bridge: Bridge, n: int = 200) -> str:
    """Journal tail: the app's JSONL file if it has one (lot 5), else ``journal.read`` entries as JSONL."""
    pid = None
    try:
        c = bridge.client_for(allow_launch=False)
        pid = c.instance.pid if c.instance else None
        seq = (c.hello or {}).get("journalSeq") or 0
        try:
            res = c.result("journal.read", {"sinceSeq": max(0, int(seq) - n), "limit": n}) or {}
        except ControlError as e:
            if e.code != "unknown_cmd":
                raise
            res = {}
        f = res.get("file")
        if f and Path(f).is_file():
            return "\n".join(_tail_lines(Path(f), n))
        if res.get("entries") is not None:
            return "\n".join(json.dumps(e, ensure_ascii=False) for e in res["entries"])
    except ControlError as e:
        if e.code not in ("no_instance", "control_disabled"):
            raise
    files = sorted(discovery.journal_dir().glob(f"session-{pid}-*.jsonl" if pid else "session-*.jsonl"),
                   key=lambda p: p.stat().st_mtime)
    if files:
        return "\n".join(_tail_lines(files[-1], n))
    return json.dumps({"entries": [], "note": "no journal available (no instance connected and no journal file)"})


def main() -> None:
    logging.basicConfig(stream=sys.stderr, level=os.environ.get("SPECTROCRAFT_MCP_LOG", "WARNING").upper(),
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    create_server().run()


if __name__ == "__main__":
    main()
