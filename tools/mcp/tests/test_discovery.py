from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import pytest

from spectrocraft_mcp import discovery


def _write(d: Path, name: str, **fields) -> Path:
    data = {"pid": os.getpid(), "port": 50000, "token": "t", "version": "1.0.72", "protocol": 1,
            "startedAt": datetime.now(timezone.utc).isoformat(), **fields}
    p = d / name
    p.write_text(json.dumps(data))
    return p


def _dead_pid() -> int:
    proc = subprocess.Popen([sys.executable, "-c", "pass"])
    proc.wait()
    return proc.pid


@pytest.mark.parametrize(
    "platform, env, expected",
    [
        ("darwin", {}, Path.home() / "Library" / "SpectroCraft" / "control"),
        ("linux", {"XDG_CONFIG_HOME": "/xdg"}, Path("/xdg") / "SpectroCraft" / "control"),
        ("linux", {}, Path.home() / ".config" / "SpectroCraft" / "control"),
        ("win32", {"APPDATA": "C:/Users/n/AppData/Roaming"}, Path("C:/Users/n/AppData/Roaming") / "SpectroCraft" / "control"),
    ],
)
def test_real_per_os_paths(monkeypatch, platform, env, expected):
    monkeypatch.delenv("SPECTROCRAFT_CONTROL_DIR", raising=False)
    monkeypatch.delenv("XDG_CONFIG_HOME", raising=False)
    monkeypatch.delenv("APPDATA", raising=False)
    for k, v in env.items():
        monkeypatch.setenv(k, v)
    monkeypatch.setattr(discovery.sys, "platform", platform)
    assert discovery.control_dir() == expected


def test_override_dir(control_dir):
    assert discovery.control_dir() == control_dir


def test_live_instance_listed(control_dir):
    _write(control_dir, f"{os.getpid()}.json")
    insts = discovery.list_instances()
    assert [i.pid for i in insts] == [os.getpid()]
    assert insts[0].port == 50000 and insts[0].protocol == 1
    assert "token" not in json.dumps(insts[0].summary())


def test_dead_pid_file_is_removed(control_dir):
    pid = _dead_pid()
    f = _write(control_dir, f"{pid}.json", pid=pid)
    assert discovery.list_instances() == []
    assert not f.exists()


def test_dead_pid_kept_without_cleanup(control_dir):
    pid = _dead_pid()
    f = _write(control_dir, f"{pid}.json", pid=pid)
    assert discovery.list_instances(cleanup=False) == []
    assert f.exists()


@pytest.mark.skipif(sys.platform.startswith("win"), reason="process start time via ps / procfs")
def test_pid_reuse_detected_with_started_at(control_dir):
    # Our PID is alive, but this file claims the process started in 2001:
    # the live process is newer than the file, so the PID was reused.
    f = _write(control_dir, f"{os.getpid()}.json", startedAt="2001-01-01T00:00:00Z")
    assert discovery.list_instances() == []
    assert not f.exists()


def test_started_at_epoch_ms_accepted(control_dir):
    _write(control_dir, "a.json", startedAt=int(time.time() * 1000))
    assert len(discovery.list_instances()) == 1


def test_started_at_after_process_start_is_fine(control_dir):
    # control enabled long after the app started: startedAt > process start
    _write(control_dir, "a.json", startedAt=time.time() + 3600)
    assert len(discovery.list_instances()) == 1


def test_malformed_files_skipped_not_deleted(control_dir):
    bad = control_dir / "123.json"
    bad.write_text("{not json")
    partial = control_dir / "124.json"
    partial.write_text(json.dumps({"pid": os.getpid()}))  # no port/token yet
    assert discovery.list_instances() == []
    assert bad.exists() and partial.exists()


def test_choose_most_recent_or_env(control_dir, monkeypatch):
    now = time.time()
    _write(control_dir, "old.json", pid=os.getppid(), port=50001, startedAt=now + 100)
    _write(control_dir, "new.json", pid=os.getpid(), port=50002, startedAt=now + 200)
    insts = discovery.list_instances()
    assert [i.port for i in insts] == [50002, 50001]
    assert discovery.choose_instance(insts).port == 50002
    monkeypatch.setenv("SPECTROCRAFT_PID", str(os.getppid()))
    assert discovery.choose_instance(insts).port == 50001
    assert discovery.choose_instance(insts, pid=os.getpid()).port == 50002
    assert discovery.choose_instance(insts, pid=1) is None


@pytest.mark.parametrize(
    "value, expected",
    [
        (1730816469, 1730816469.0),
        (1730816469123, 1730816469.123),
        ("1730816469", 1730816469.0),
        ("2024-11-05T14:21:09Z", datetime(2024, 11, 5, 14, 21, 9, tzinfo=timezone.utc).timestamp()),
        ("2024-11-05T14:21:09.002+00:00", datetime(2024, 11, 5, 14, 21, 9, 2000, tzinfo=timezone.utc).timestamp()),
        (None, None),
        ("garbage", None),
    ],
)
def test_parse_started_at(value, expected):
    got = discovery.parse_started_at(value)
    assert got == pytest.approx(expected) if expected is not None else got is None


def test_pid_alive_and_start_time():
    assert discovery.pid_alive(os.getpid())
    assert not discovery.pid_alive(_dead_pid())
    assert not discovery.pid_alive(0)
    t = discovery.process_start_time(os.getpid())
    if sys.platform.startswith(("linux", "darwin")):
        assert t is not None and t <= time.time() + 2
