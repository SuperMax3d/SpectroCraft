"""FileForward pipe probe against a fake JUCE-style FIFO pair (POSIX only)."""

from __future__ import annotations

import os
import struct
import sys
import threading
import time

import pytest

from spectrocraft_mcp import pipe_probe

pytestmark = pytest.mark.skipif(sys.platform.startswith("win"), reason="POSIX FIFOs")


def _serve_once(base: str, answer: bytes, got: list[int], stop: threading.Event) -> None:
    """Mimics sc::singleinstance::Coordinator: read int32 count from _in, answer on _out."""
    rfd = os.open(base + "_in", os.O_RDWR | os.O_NONBLOCK)
    try:
        buf = b""
        deadline = time.monotonic() + 3
        while len(buf) < 4 and time.monotonic() < deadline and not stop.is_set():
            try:
                buf += os.read(rfd, 4 - len(buf))
            except BlockingIOError:
                time.sleep(0.005)
        if len(buf) == 4:
            got.append(struct.unpack("=i", buf)[0])
            wfd = os.open(base + "_out", os.O_WRONLY | os.O_NONBLOCK)
            os.write(wfd, answer)
            os.close(wfd)
    finally:
        os.close(rfd)


def _fifos(tmp_path):
    base = str(tmp_path / "SpectroCraft.FileForward")
    os.mkfifo(base + "_in")
    os.mkfifo(base + "_out")
    return base


def test_no_fifo_means_no_instance(tmp_path):
    t0 = time.monotonic()
    assert pipe_probe.probe_file_forward(1.0, base=str(tmp_path / "none")) is False
    assert time.monotonic() - t0 < 0.5


def test_live_instance_acknowledges_empty_list(tmp_path):
    base = _fifos(tmp_path)
    got: list[int] = []
    stop = threading.Event()
    th = threading.Thread(target=_serve_once, args=(base, b"\x01", got, stop), daemon=True)
    th.start()
    time.sleep(0.05)  # server has its read end open, like the real one
    try:
        assert pipe_probe.probe_file_forward(2.0, base=base) is True
    finally:
        stop.set()
        th.join(2)
    assert got == [0]  # zero paths: harmless for the app


def test_stale_fifo_without_reader(tmp_path):
    base = _fifos(tmp_path)  # left over by a crash: nobody reads _in
    t0 = time.monotonic()
    assert pipe_probe.probe_file_forward(0.5, base=base) is False
    assert time.monotonic() - t0 < 1.5


def test_wrong_ack_is_not_an_instance(tmp_path):
    base = _fifos(tmp_path)
    got: list[int] = []
    stop = threading.Event()
    th = threading.Thread(target=_serve_once, args=(base, b"\x00", got, stop), daemon=True)
    th.start()
    time.sleep(0.05)
    try:
        assert pipe_probe.probe_file_forward(1.0, base=base) is False
    finally:
        stop.set()
        th.join(2)


def test_env_override(tmp_path, monkeypatch):
    monkeypatch.setenv("SPECTROCRAFT_PIPE_BASE", str(tmp_path / "x"))
    assert pipe_probe._posix_base() == str(tmp_path / "x")
    monkeypatch.delenv("SPECTROCRAFT_PIPE_BASE")
    assert pipe_probe._posix_base() == "/tmp/SpectroCraft.FileForward"
