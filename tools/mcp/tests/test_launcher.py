"""resolve_exe, launch (with the FileForward probe) and the --json headless runner."""

from __future__ import annotations

import os
import stat
import sys
from pathlib import Path

import pytest

from spectrocraft_mcp import discovery, launcher
from spectrocraft_mcp.protocol import ControlError

FAKE_EXE = Path(__file__).resolve().parent / "fake_exe.py"
posix_only = pytest.mark.skipif(sys.platform.startswith("win"), reason="shell wrapper")


@pytest.fixture
def exe(tmp_path, monkeypatch):
    w = tmp_path / "SpectroCraft"
    w.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{FAKE_EXE}" "$@"\n')
    w.chmod(w.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("SPECTROCRAFT_EXE", str(w))
    return w


def test_resolve_exe_errors(monkeypatch, tmp_path):
    monkeypatch.delenv("SPECTROCRAFT_EXE", raising=False)
    with pytest.raises(ControlError) as ei:
        launcher.resolve_exe()
    assert ei.value.code == "launch_failed" and "SPECTROCRAFT_EXE" in ei.value.describe()
    with pytest.raises(ControlError):
        launcher.resolve_exe(str(tmp_path / "missing"))


def test_resolve_app_bundle(tmp_path):
    binary = tmp_path / "SpectroCraft.app" / "Contents" / "MacOS" / "SpectroCraft"
    binary.parent.mkdir(parents=True)
    binary.write_text("")
    assert launcher.resolve_exe(str(tmp_path / "SpectroCraft.app")) == str(binary)


def test_launch_policy(monkeypatch):
    monkeypatch.delenv("SPECTROCRAFT_LAUNCH", raising=False)
    assert launcher.launch_policy() == "ask"
    monkeypatch.setenv("SPECTROCRAFT_LAUNCH", "AUTO")
    assert launcher.launch_policy() == "auto"
    monkeypatch.setenv("SPECTROCRAFT_LAUNCH", "bogus")
    assert launcher.launch_policy() == "ask"


@posix_only
def test_launch_waits_for_discovery_file(control_dir, exe, monkeypatch):
    monkeypatch.setenv("FAKE_EXE_LIFETIME", "10")
    inst = launcher.launch(wait_s=15)
    try:
        assert inst.port > 0
        assert discovery.find_instance(inst.pid) is not None
    finally:
        os.kill(inst.pid, 15)


@posix_only
def test_launch_reports_early_exit(control_dir, exe, monkeypatch):
    monkeypatch.setenv("FAKE_EXE_EXIT", "3")
    with pytest.raises(ControlError) as ei:
        launcher.launch(wait_s=10)
    assert ei.value.code == "launch_failed" and "code 3" in ei.value.message


def test_launch_refuses_when_open_without_control(control_dir, exe, monkeypatch):
    monkeypatch.setattr(launcher.pipe_probe, "probe_file_forward", lambda timeout=1.5: True)
    with pytest.raises(ControlError) as ei:
        launcher.launch(wait_s=1)
    assert ei.value.code == "control_disabled"
    assert "Allow AI control" in ei.value.describe()


@posix_only
def test_headless_runs_json_mode(exe):
    res = launcher.run_headless(
        [
            {"cmd": "app.describe", "params": {"withSchemas": True}},
            {"cmd": "step.run", "params": {"select": "all", "step": {"type": "normalize"}}},
        ]
    )
    assert res["exitCode"] == 0
    ids = [r["id"] for r in res["responses"]]
    assert ids == ["h1", "h2"]
    assert res["responses"][0]["result"]["commands"]
    assert res["responses"][1]["result"]["applied"] == "normalize"


@posix_only
def test_headless_error_exit_code_and_continue(exe):
    reqs = [{"cmd": "nope"}, {"cmd": "app.state"}]
    res = launcher.run_headless(reqs)
    assert res["exitCode"] == 1 and len(res["responses"]) == 1
    assert res["responses"][0]["error"]["code"] == "unknown_cmd"
    assert "fake SpectroCraft done" in res["stderrTail"]
    res = launcher.run_headless(reqs, continue_on_error=True)
    assert res["exitCode"] == 1 and len(res["responses"]) == 2 and res["responses"][1]["ok"]


def test_headless_validates_requests(exe):
    with pytest.raises(ControlError) as ei:
        launcher.run_headless([{"params": {}}])
    assert ei.value.code == "invalid_params"
