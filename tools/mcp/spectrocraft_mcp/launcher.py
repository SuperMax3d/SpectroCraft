"""Locate and start the SpectroCraft executable; run its ``--json`` headless mode.

Standard library only.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any, Iterable, Sequence

from . import discovery, pipe_probe
from .protocol import ControlError, encode_line

LAUNCH_WAIT_S = 20.0


def resolve_exe(exe: str | None = None) -> str:
    """``exe`` or ``$SPECTROCRAFT_EXE``; a macOS ``.app`` bundle resolves to its binary."""
    raw = exe or os.environ.get("SPECTROCRAFT_EXE")
    if not raw:
        raise ControlError(
            "launch_failed",
            "SPECTROCRAFT_EXE is not set",
            {"hint": "set SPECTROCRAFT_EXE in .mcp.json (env) to the SpectroCraft executable"},
        )
    p = Path(os.path.expanduser(raw))
    if p.suffix == ".app" and p.is_dir():
        p = p / "Contents" / "MacOS" / p.stem
    if not p.is_file():
        raise ControlError("launch_failed", f"SpectroCraft executable not found: {p}", {"hint": "fix SPECTROCRAFT_EXE"})
    return str(p)


def launch_policy() -> str:
    v = (os.environ.get("SPECTROCRAFT_LAUNCH") or "ask").strip().lower()
    return v if v in ("ask", "auto", "never") else "ask"


def control_disabled_error() -> ControlError:
    return ControlError(
        "control_disabled",
        "SpectroCraft is open but its AI control server is off (the FileForward pipe answered, no live discovery file)",
        {
            "hint": "in SpectroCraft enable Window > Allow AI control (localhost), or call sc_launch(force=true) "
            "to start a second instance with --control",
            "controlDir": str(discovery.control_dir()),
        },
    )


def no_instance_error() -> ControlError:
    policy = launch_policy()
    if policy == "never":
        hint = "SPECTROCRAFT_LAUNCH=never: use sc_headless for batch work, or start SpectroCraft yourself with --control"
    else:
        hint = "call sc_launch to start SpectroCraft with --control (needs SPECTROCRAFT_EXE), or sc_headless for batch work"
    return ControlError(
        "no_instance",
        "no SpectroCraft instance is running",
        {"hint": hint, "controlDir": str(discovery.control_dir()), "launchPolicy": policy},
    )


def launch(
    files: Sequence[str] = (),
    *,
    force: bool = False,
    exe: str | None = None,
    wait_s: float = LAUNCH_WAIT_S,
    probe_timeout: float = 1.5,
) -> discovery.Instance:
    """Start ``SPECTROCRAFT_EXE --control [files]`` and wait for its discovery file.

    Before launching (unless ``force``), probe the FileForward pipe: an
    instance that answers but has no live discovery file has control
    disabled, and starting another one would only hide that.
    """
    if not force and not discovery.list_instances() and pipe_probe.probe_file_forward(probe_timeout):
        raise control_disabled_error()
    path = resolve_exe(exe)
    args = [path, "--control", *[str(f) for f in files]]
    kwargs: dict[str, Any] = {
        "stdin": subprocess.DEVNULL,
        "stdout": subprocess.DEVNULL,
        "stderr": subprocess.DEVNULL,
        "close_fds": True,
    }
    if sys.platform.startswith("win"):
        kwargs["creationflags"] = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    else:
        kwargs["start_new_session"] = True
    try:
        proc = subprocess.Popen(args, **kwargs)
    except OSError as e:
        raise ControlError("launch_failed", f"cannot start {path}: {e}") from e
    deadline = time.monotonic() + wait_s
    while time.monotonic() < deadline:
        for inst in discovery.list_instances():
            if inst.pid == proc.pid:
                return inst
        rc = proc.poll()
        if rc is not None:
            raise ControlError(
                "launch_failed",
                f"SpectroCraft exited with code {rc} before its control server started",
                {"exe": path, "hint": "run it by hand with --control to see why"},
            )
        time.sleep(0.2)
    raise ControlError(
        "launch_failed",
        f"SpectroCraft (pid {proc.pid}) did not write its discovery file within {wait_s:.0f} s",
        {"pid": proc.pid, "controlDir": str(discovery.control_dir()),
         "hint": "is this build recent enough to support --control (lot 1)?"},
    )


def run_headless(
    requests: Iterable[dict[str, Any]],
    *,
    exe: str | None = None,
    continue_on_error: bool = False,
    timeout_s: float = 300.0,
    extra_args: Sequence[str] = (),
) -> dict[str, Any]:
    """Run ``SPECTROCRAFT_EXE --json - --json-out <tmp>`` (REPORT.md 6.9).

    No ``session.hello`` in this mode. Requests without ``id`` get ``h1``,
    ``h2``... Returns ``{"exitCode", "responses", "stderrTail"?}``.
    """
    path = resolve_exe(exe)
    lines: list[bytes] = []
    if continue_on_error:
        lines.append(encode_line({"continueOnError": True}))
    for i, r in enumerate(requests, 1):
        if not isinstance(r, dict) or not isinstance(r.get("cmd"), str):
            raise ControlError("invalid_params", f"request {i} must be an object with a string 'cmd'", {"path": f"requests[{i - 1}].cmd"})
        req = dict(r)
        req.setdefault("id", f"h{i}")
        req.setdefault("params", {})
        lines.append(encode_line(req))
    fd, out_path = tempfile.mkstemp(prefix="sc-json-", suffix=".ndjson")
    os.close(fd)
    env = {**os.environ, "SC_NO_AUDIO_DEVICE": "1"}
    kwargs: dict[str, Any] = {}
    if sys.platform.startswith("win"):
        kwargs["creationflags"] = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    try:
        try:
            proc = subprocess.run(
                [path, "--json", "-", "--json-out", out_path, *extra_args],
                input=b"".join(lines),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=timeout_s,
                close_fds=True,
                env=env,
                **kwargs,
            )
        except subprocess.TimeoutExpired as e:
            raise ControlError("timeout", f"SpectroCraft --json did not finish within {timeout_s:.0f} s", {"exe": path}) from e
        except OSError as e:
            raise ControlError("launch_failed", f"cannot start {path}: {e}") from e
        text = Path(out_path).read_text(encoding="utf-8", errors="replace")
        if not text.strip():
            text = proc.stdout.decode("utf-8", errors="replace")  # builds that ignore --json-out
    finally:
        try:
            os.unlink(out_path)
        except OSError:
            pass
    responses: list[Any] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            responses.append(json.loads(line))
        except ValueError:
            responses.append({"unparsed": line[:500]})
    out: dict[str, Any] = {"exitCode": proc.returncode, "responses": responses}
    if proc.returncode != 0 or not responses:
        err = proc.stderr.decode("utf-8", errors="replace")
        if err.strip():
            out["stderrTail"] = err[-2000:]
    return out
