from __future__ import annotations

import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))  # tools/mcp: works without `pip install -e`
sys.path.insert(0, str(HERE))

from fake_server import FakeServer  # noqa: E402


@pytest.fixture
def control_dir(tmp_path, monkeypatch):
    d = tmp_path / "control"
    d.mkdir()
    monkeypatch.setenv("SPECTROCRAFT_CONTROL_DIR", str(d))
    monkeypatch.setenv("SPECTROCRAFT_JOURNAL_DIR", str(tmp_path / "journal"))
    # no FIFO there: the FileForward probe answers "nothing running" at once
    monkeypatch.setenv("SPECTROCRAFT_PIPE_BASE", str(tmp_path / "no-such-pipe"))
    for var in ("SPECTROCRAFT_PID", "SPECTROCRAFT_LAUNCH", "SPECTROCRAFT_EXE"):
        monkeypatch.delenv(var, raising=False)
    return d


@pytest.fixture
def fake(control_dir):
    srv = FakeServer()
    srv.write_discovery(control_dir)
    yield srv
    srv.close()


@pytest.fixture
def anyio_backend():
    return "asyncio"
