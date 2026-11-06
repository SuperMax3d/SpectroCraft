#!/usr/bin/env python3
"""Dependency-free command-line client for an open SpectroCraft (standard library only).

Diagnoses the control channel when the ``mcp`` package is missing, and lets a
terminal or a Bash tool drive the app:

    python3 tools/mcp/sc_client.py --list
    python3 tools/mcp/sc_client.py app.state
    python3 tools/mcp/sc_client.py --doc 7 step.run '{"type":"fadeOut","select":{"start":1,"end":2.5}}'
    python3 tools/mcp/sc_client.py step.run '{"select":"all","step":{"type":"normalize","params":{}}}'
    python3 tools/mcp/sc_client.py history.undo
    python3 tools/mcp/sc_client.py ai.models.install '{"modelId":"whisper-base"}'   # waits, progress on stderr
    python3 tools/mcp/sc_client.py --events 10      # print events for 10 s

``params`` is a JSON object, ``@file.json``, or ``-`` (stdin). For ``step.run``
a flat form ``{"type":..., "params":..., "select":..., "channels":...}`` is
accepted and wrapped into ``{"step": {...}}``.

Exit codes: 0 ok, 1 error reply from the app, 2 usage / no instance / transport error.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from spectrocraft_mcp import discovery, pipe_probe  # noqa: E402
from spectrocraft_mcp.client import ControlClient  # noqa: E402
from spectrocraft_mcp.protocol import ControlError, is_job_handle  # noqa: E402

CLIENT = "sc_client/0.1"


def parse_params(raw: str | None) -> dict:
    if raw is None or raw == "":
        return {}
    if raw == "-":
        raw = sys.stdin.read()
    elif raw.startswith("@"):
        with open(raw[1:], encoding="utf-8") as f:
            raw = f.read()
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("params must be a JSON object")
    return value


def normalise_step(cmd: str, params: dict) -> dict:
    """Accept ``step.run {"type":..}`` as shorthand for ``step.run {"step":{"type":..}}``."""
    if cmd == "step.run" and "type" in params and "step" not in params:
        step = {"type": params.pop("type"), "params": params.pop("params", {})}
        params = {**params, "step": step}
    return params


def _print(obj: object) -> None:
    print(json.dumps(obj, ensure_ascii=False, indent=2))


def cmd_list() -> int:
    insts = discovery.list_instances()
    out = {"controlDir": str(discovery.control_dir()), "instances": [i.summary() for i in insts]}
    if not insts:
        out["fileForwardPipeAnswered"] = pipe_probe.probe_file_forward()
        out["message"] = (
            "SpectroCraft is open but AI control is off: Window > Allow AI control (localhost), or relaunch with --control"
            if out["fileForwardPipeAnswered"]
            else "no SpectroCraft instance is running"
        )
    _print(out)
    return 0 if insts else 2


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="SpectroCraft control client (NDJSON over TCP loopback)")
    ap.add_argument("cmd", nargs="?", help="command id, e.g. app.state, step.run, history.undo")
    ap.add_argument("params", nargs="?", help="JSON object, @file.json or - for stdin")
    ap.add_argument("--pid", type=int, help="instance pid (default $SPECTROCRAFT_PID, else the most recent)")
    ap.add_argument("--doc", help="target docId (integer) or absolute path")
    ap.add_argument("--timeout-ms", type=int, help="request timeoutMs")
    ap.add_argument("--no-wait", action="store_true", help="do not wait for jobs")
    ap.add_argument("--list", action="store_true", help="list discovered instances and exit")
    ap.add_argument("--events", type=float, metavar="SECONDS", help="print events for SECONDS and exit")
    ap.add_argument("--raw", action="store_true", help="print the whole reply envelope (default: result + state fields)")
    a = ap.parse_args(argv)

    if a.list:
        return cmd_list()
    if not a.cmd and a.events is None:
        ap.print_usage(sys.stderr)
        return 2

    doc: object = None
    if a.doc is not None:
        doc = int(a.doc) if a.doc.lstrip("-").isdigit() else a.doc

    try:
        params = normalise_step(a.cmd or "", parse_params(a.params))
    except (OSError, ValueError) as e:
        print(f"bad params: {e}", file=sys.stderr)
        return 2

    client = ControlClient(pid=a.pid, client_name=CLIENT)
    try:
        if a.events is not None:
            client.connect()
            client.add_listener(lambda ev: print(json.dumps(ev, ensure_ascii=False), flush=True))
            if a.cmd:
                _print(client.call(a.cmd, params, doc, a.timeout_ms))
            time.sleep(a.events)
            return 0

        def progress(p: float | None, status: str | None) -> None:
            pct = "" if p is None else f"{p * 100:5.1f}% "
            print(f"[job] {pct}{status or ''}", file=sys.stderr, flush=True)

        reply = client.call(a.cmd, params, doc, a.timeout_ms)
        res = reply.get("result")
        if not a.no_wait and is_job_handle(a.cmd, res):
            print(f"[job] {res['job']} started", file=sys.stderr, flush=True)
            done = client.wait_job(res["job"], on_progress=progress)
            reply = {**reply, "result": done.get("result"), "job": res["job"],
                     **{k: v for k, v in done.items() if k not in ("job", "ok", "result")}}
        if a.raw:
            _print(reply)
        else:
            _print({k: v for k, v in reply.items() if k not in ("id", "ok")})
        return 0
    except ControlError as e:
        print(e.describe(), file=sys.stderr)
        return 1 if e.reply is not None else 2
    except KeyboardInterrupt:
        return 130
    finally:
        client.close()


if __name__ == "__main__":
    sys.exit(main())
