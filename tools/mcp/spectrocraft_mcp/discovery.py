"""Find running SpectroCraft instances through their discovery files.

Each instance whose control server is on writes
``<userApplicationDataDirectory>/SpectroCraft/control/<pid>.json`` =
``{pid, port, token, version, startedAt, exe, protocol}`` (REPORT.md 5.2).
Real paths (JUCE ``userApplicationDataDirectory``):

- macOS   ``~/Library/SpectroCraft/control``   (not "Application Support")
- Linux   ``$XDG_CONFIG_HOME/SpectroCraft/control`` or ``~/.config/SpectroCraft/control``
- Windows ``%APPDATA%\\SpectroCraft\\control``

``SPECTROCRAFT_CONTROL_DIR`` overrides the directory (tests, unusual setups).
Standard library only.
"""

from __future__ import annotations

import ctypes
import json
import os
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

PID_REUSE_TOLERANCE_S = 5.0
"""A live process that started more than this after ``startedAt`` is not the writer (PID reused)."""


@dataclass(frozen=True)
class Instance:
    pid: int
    port: int
    token: str
    path: Path
    version: str | None = None
    protocol: int | None = None
    exe: str | None = None
    started_at: float | None = None  # epoch seconds
    raw: dict[str, Any] = field(default_factory=dict, compare=False, repr=False)

    def summary(self) -> dict[str, Any]:
        """Public view (never includes the token)."""
        return {
            "pid": self.pid,
            "port": self.port,
            "version": self.version,
            "protocol": self.protocol,
            "exe": self.exe,
            "startedAt": self.raw.get("startedAt"),
            "file": str(self.path),
        }


def app_data_dir() -> Path:
    """JUCE ``userApplicationDataDirectory`` for the current OS."""
    if sys.platform == "darwin":
        return Path.home() / "Library"
    if sys.platform.startswith("win"):
        appdata = os.environ.get("APPDATA")
        return Path(appdata) if appdata else Path.home() / "AppData" / "Roaming"
    xdg = os.environ.get("XDG_CONFIG_HOME")
    return Path(xdg) if xdg else Path.home() / ".config"


def control_dir() -> Path:
    override = os.environ.get("SPECTROCRAFT_CONTROL_DIR")
    if override:
        return Path(override)
    return app_data_dir() / "SpectroCraft" / "control"


def journal_dir() -> Path:
    """Where the app writes its JSONL session journals (lot 5)."""
    override = os.environ.get("SPECTROCRAFT_JOURNAL_DIR")
    if override:
        return Path(override)
    return control_dir().parent / "journal"


# --------------------------------------------------------------------------- processes


def pid_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    if sys.platform.startswith("win"):
        return _win_pid_alive(pid)
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, owned by someone else
    except OSError:
        return False
    return True


def _win_pid_alive(pid: int) -> bool:  # pragma: no cover - Windows only
    kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    STILL_ACTIVE = 259
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return False
        return code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def process_start_time(pid: int) -> float | None:
    """Creation time of ``pid`` in epoch seconds, or None when it cannot be determined."""
    try:
        if sys.platform.startswith("linux"):
            return _linux_start_time(pid)
        if sys.platform.startswith("win"):
            return _win_start_time(pid)
        return _ps_start_time(pid)
    except Exception:
        return None


def _linux_start_time(pid: int) -> float | None:
    stat = Path(f"/proc/{pid}/stat").read_text()
    # comm (field 2) may contain spaces and parentheses: split after the last ')'.
    fields = stat[stat.rindex(")") + 2 :].split()
    start_ticks = int(fields[19])  # field 22 overall
    btime = None
    for line in Path("/proc/stat").read_text().splitlines():
        if line.startswith("btime "):
            btime = int(line.split()[1])
            break
    if btime is None:
        return None
    return btime + start_ticks / os.sysconf("SC_CLK_TCK")


def _ps_start_time(pid: int) -> float | None:
    out = subprocess.run(
        ["ps", "-p", str(pid), "-o", "lstart="],
        capture_output=True,
        text=True,
        timeout=5,
        env={**os.environ, "LC_ALL": "C", "LANG": "C"},
    )
    text = " ".join(out.stdout.split())
    if out.returncode != 0 or not text:
        return None
    return time.mktime(time.strptime(text, "%a %b %d %H:%M:%S %Y"))


def _win_start_time(pid: int) -> float | None:  # pragma: no cover - Windows only
    kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
    handle = kernel32.OpenProcess(0x1000, False, pid)
    if not handle:
        return None
    try:
        ft = [ctypes.c_ulonglong() for _ in range(4)]
        if not kernel32.GetProcessTimes(handle, *[ctypes.byref(x) for x in ft]):
            return None
        # FILETIME: 100 ns intervals since 1601-01-01.
        return ft[0].value / 10_000_000 - 11_644_473_600
    finally:
        kernel32.CloseHandle(handle)


# --------------------------------------------------------------------------- files


def parse_started_at(value: Any) -> float | None:
    """``startedAt`` as epoch seconds. Accepts epoch ms, epoch s, or an ISO-8601 string."""
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        v = float(value)
        return v / 1000.0 if v > 1e11 else v
    if isinstance(value, str) and value:
        s = value.strip()
        try:
            return parse_started_at(float(s))
        except ValueError:
            pass
        try:
            dt = datetime.fromisoformat(s.replace("Z", "+00:00"))
        except ValueError:
            return None
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.timestamp()
    return None


def read_instance_file(path: Path) -> Instance | None:
    """Parse one discovery file; None if unreadable or incomplete (possibly being written)."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    if not isinstance(data, dict):
        return None
    try:
        pid = int(data.get("pid") or path.stem)
        port = int(data["port"])
        token = str(data["token"])
    except (KeyError, TypeError, ValueError):
        return None
    if not (0 < port < 65536):
        return None
    proto = data.get("protocol")
    return Instance(
        pid=pid,
        port=port,
        token=token,
        path=path,
        version=None if data.get("version") is None else str(data.get("version")),
        protocol=proto if isinstance(proto, int) else None,
        exe=data.get("exe"),
        started_at=parse_started_at(data.get("startedAt")),
        raw=data,
    )


def is_stale(inst: Instance) -> tuple[bool, str]:
    """(stale, reason). Stale = writer process gone, or PID reused by a newer process."""
    if not pid_alive(inst.pid):
        return True, "pid not running"
    if inst.started_at is not None:
        proc_start = process_start_time(inst.pid)
        if proc_start is not None and proc_start > inst.started_at + PID_REUSE_TOLERANCE_S:
            return True, "pid reused by a newer process"
    return False, ""


def list_instances(directory: Path | None = None, cleanup: bool = True) -> list[Instance]:
    """Live instances, most recent first. Stale files are deleted when ``cleanup``."""
    d = directory or control_dir()
    try:
        files = sorted(d.glob("*.json"))
    except OSError:
        return []
    live: list[Instance] = []
    for f in files:
        inst = read_instance_file(f)
        if inst is None:
            continue
        stale, _ = is_stale(inst)
        if stale:
            if cleanup:
                try:
                    f.unlink()
                except OSError:
                    pass
            continue
        live.append(inst)
    live.sort(key=_recency, reverse=True)
    return live


def _recency(inst: Instance) -> float:
    if inst.started_at is not None:
        return inst.started_at
    try:
        return inst.path.stat().st_mtime
    except OSError:
        return 0.0


def choose_instance(instances: Iterable[Instance], pid: int | None = None) -> Instance | None:
    """``pid`` if given, else ``$SPECTROCRAFT_PID``, else the most recent instance."""
    items = list(instances)
    if pid is None:
        env = os.environ.get("SPECTROCRAFT_PID", "").strip()
        if env:
            try:
                pid = int(env)
            except ValueError:
                pid = None
    if pid is not None:
        return next((i for i in items if i.pid == pid), None)
    return max(items, key=_recency) if items else None


def find_instance(pid: int | None = None, directory: Path | None = None) -> Instance | None:
    return choose_instance(list_instances(directory), pid)
