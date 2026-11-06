"""The dependency-free CLI (tools/mcp/sc_client.py)."""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import sc_client

SC_CLIENT = Path(__file__).resolve().parent.parent / "sc_client.py"


def test_state(fake, capsys):
    assert sc_client.main(["app.state"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["result"]["activeDoc"] == 7


def test_flat_step_run_is_wrapped(fake, capsys):
    rc = sc_client.main(["--doc", "7", "step.run", '{"type":"fadeOut","select":{"start":1,"end":2.5}}'])
    assert rc == 0
    req = [r for r in fake.requests if r["cmd"] == "step.run"][-1]
    assert req["params"] == {"select": {"start": 1, "end": 2.5}, "step": {"type": "fadeOut", "params": {}}}
    assert req["doc"] == 7
    assert json.loads(capsys.readouterr().out)["undoDepth"] == 1
    assert sc_client.main(["history.undo"]) == 0


def test_job_waited_with_progress_on_stderr(fake, capsys):
    assert sc_client.main(["ai.models.install", '{"modelId":"whisper-base"}']) == 0
    cap = capsys.readouterr()
    assert json.loads(cap.out)["result"]["installed"] is True
    assert "[job]" in cap.err


def test_error_reply_exit_1(fake, capsys):
    assert sc_client.main(["doc.save", '{"path":"/tmp/x.wav"}']) == 1
    assert "confirm_required" in capsys.readouterr().err


def test_no_instance_exit_2(control_dir, capsys):
    assert sc_client.main(["app.state"]) == 2
    assert "no_instance" in capsys.readouterr().err
    assert sc_client.main(["--list"]) == 2


def test_bad_params_exit_2(fake):
    assert sc_client.main(["app.state", "[1,2]"]) == 2
    assert sc_client.main(["app.state", "{nope"]) == 2


def test_runs_as_a_script_without_mcp(fake, tmp_path):
    # -I: isolated mode, no site-packages => proves the CLI needs the standard library only
    env = {k: v for k, v in os.environ.items() if not k.startswith("PYTHON")}
    proc = subprocess.run([sys.executable, "-I", "-S", str(SC_CLIENT), "doc.list"], capture_output=True, text=True, env=env, timeout=30)
    assert proc.returncode == 0, proc.stderr
    assert json.loads(proc.stdout)["result"]["active"] == 7
