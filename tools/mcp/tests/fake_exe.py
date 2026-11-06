"""Stand-in for the SpectroCraft executable (tests only).

``--json <file|-> [--json-out <f>]``: REPORT.md 6.9 line-by-line mode, no
session.hello, optional ``{"continueOnError":true}`` first line, exit code
0 / 1 / 2. ``--control``: starts a FakeServer and writes its discovery file
into ``$SPECTROCRAFT_CONTROL_DIR`` with this process's pid, then idles.
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from fake_server import AppError, FakeServer  # noqa: E402


class _Cli:
    authed = True
    client = "cli"
    kinds: list[str] = []


def json_mode(args: list[str]) -> int:
    src = args[args.index("--json") + 1]
    out_path = args[args.index("--json-out") + 1] if "--json-out" in args else None
    srv = FakeServer()
    inp = sys.stdin if src == "-" else open(src, encoding="utf-8")
    out = open(out_path, "w", encoding="utf-8") if out_path else sys.stdout
    rc = 0
    cont = False
    first = True
    try:
        for line in inp:
            line = line.strip()
            if not line:
                continue
            if len(line) > 1024 * 1024:
                return 2
            try:
                req = json.loads(line)
            except ValueError:
                return 2
            if first and "cmd" not in req and "continueOnError" in req:
                cont = bool(req["continueOnError"])
                first = False
                continue
            first = False
            try:
                reply = {"id": req.get("id"), "ok": True, **srv._handle(_Cli(), req)}
            except AppError as e:
                reply = {"id": req.get("id"), "ok": False, "error": {"code": e.code, "message": e.message, "details": e.details}}
            out.write(json.dumps(reply) + "\n")
            out.flush()
            if not reply["ok"]:
                rc = 1
                if not cont:
                    break
    finally:
        srv.close()
        if out is not sys.stdout:
            out.close()
    print("fake SpectroCraft done", file=sys.stderr)
    return rc


def control_mode() -> int:
    if os.environ.get("FAKE_EXE_EXIT"):
        print("cannot start", file=sys.stderr)
        return int(os.environ["FAKE_EXE_EXIT"])
    srv = FakeServer()
    srv.write_discovery(Path(os.environ["SPECTROCRAFT_CONTROL_DIR"]))
    try:
        time.sleep(float(os.environ.get("FAKE_EXE_LIFETIME", "30")))
    finally:
        srv.close()
    return 0


def main() -> int:
    args = sys.argv[1:]
    if "--json" in args:
        return json_mode(args)
    if "--control" in args:
        return control_mode()
    return 2


if __name__ == "__main__":
    sys.exit(main())
