"""Probe the ``SpectroCraft.FileForward`` single-instance pipe (REPORT.md 7.3).

Every running SpectroCraft hosts this pipe, with or without AI control
(``src/SingleInstance.cpp``). Sending a message with zero paths (``int32 0``)
makes the server answer the acknowledgement byte ``0x01`` and do nothing
else, so the probe is harmless. It tells "an instance is open but its
control server is off" apart from "nothing is running".

JUCE ``NamedPipe`` mapping:

- POSIX: two FIFOs ``/tmp/SpectroCraft.FileForward_in`` (server reads) and
  ``/tmp/SpectroCraft.FileForward_out`` (server writes); stale FIFOs left by a
  crash have no reader, so opening ``_in`` for writing fails with ENXIO.
- Windows: ``\\\\.\\pipe\\SpectroCraft.FileForward`` (duplex, byte mode).

Standard library only.
"""

from __future__ import annotations

import errno
import os
import select
import struct
import sys
import threading
import time

PIPE_NAME = "SpectroCraft.FileForward"


def _posix_base() -> str:
    return os.environ.get("SPECTROCRAFT_PIPE_BASE") or ("/tmp/" + PIPE_NAME)


def probe_file_forward(timeout: float = 1.5, base: str | None = None) -> bool:
    """True when a SpectroCraft instance acknowledged an empty file list."""
    try:
        if sys.platform.startswith("win"):
            return _probe_windows(timeout, base)
        return _probe_posix(timeout, base or _posix_base())
    except Exception:
        return False


def _probe_posix(timeout: float, base: str) -> bool:
    to_server = base + "_in"
    from_server = base + "_out"
    deadline = time.monotonic() + timeout
    if not (os.path.exists(to_server) or os.path.exists(from_server)):
        return False  # no FIFO at all: no instance
    # The server recreates its FIFOs after each client: wait briefly if they are being swapped.
    while not (os.path.exists(to_server) and os.path.exists(from_server)):
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.01)
    rfd = wfd = -1
    try:
        rfd = os.open(from_server, os.O_RDWR | os.O_NONBLOCK)
        while True:
            try:
                wfd = os.open(to_server, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError as e:
                if e.errno not in (errno.ENXIO, errno.ENOENT) or time.monotonic() >= deadline:
                    return False  # ENXIO until the deadline: stale FIFO, nobody reading
                time.sleep(0.01)
        os.write(wfd, struct.pack("=i", 0))
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            ready, _, _ = select.select([rfd], [], [], left)
            if not ready:
                continue
            try:
                b = os.read(rfd, 1)
            except BlockingIOError:
                continue
            return b == b"\x01"
    except OSError:
        return False
    finally:
        for fd in (wfd, rfd):
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass


def _probe_windows(timeout: float, base: str | None) -> bool:  # pragma: no cover - Windows only
    path = base or (r"\\.\pipe" + "\\" + PIPE_NAME)
    try:
        f = open(path, "r+b", buffering=0)
    except OSError:
        return False
    result: list[bytes] = []

    def talk() -> None:
        try:
            f.write(struct.pack("=i", 0))
            result.append(f.read(1) or b"")
        except OSError:
            result.append(b"")

    t = threading.Thread(target=talk, daemon=True)
    t.start()
    t.join(timeout)
    try:
        f.close()
    except OSError:
        pass
    return bool(result) and result[0] == b"\x01"
