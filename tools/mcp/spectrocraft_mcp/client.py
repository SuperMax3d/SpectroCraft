"""Blocking NDJSON client for the SpectroCraft control server (RAPPORT.md 6).

- one TCP connection to 127.0.0.1:<port> read from the discovery file;
- first line ``session.hello {token, client, subscribe, sinceSeq}``;
- a reader thread routes replies to callers by ``id`` and queues events;
- ``call()`` sends one request and waits for its reply;
- ``wait_job()`` follows a job through ``job.progress`` / ``job.done`` events,
  polling ``job.get`` as a fallback (events may be missed or not emitted yet);
- silent reconnection: a dropped connection is re-established on the next
  call, and ``sinceSeq`` in the new hello asks the app to replay the events
  missed in between;
- NDJSON lines are limited to 1 MiB in both directions.

Standard library only (the dependency-free ``sc_client.py`` uses it).
"""

from __future__ import annotations

import collections
import itertools
import json
import logging
import socket
import threading
import time
from typing import Any, Callable, Iterable

from . import discovery
from .discovery import Instance
from .protocol import (
    CLIENT_NAME,
    DEFAULT_TIMEOUT_MS,
    MAX_LINE_BYTES,
    PROTOCOL_VERSION,
    ControlError,
    encode_line,
    is_job_handle,
)

log = logging.getLogger("spectrocraft_mcp.client")

DEFAULT_SUBSCRIBE = ("job", "history", "doc", "meta")
REPLY_GRACE_S = 10.0
"""Extra seconds the client waits beyond ``timeoutMs`` before giving up on a reply."""

# Commands that never mutate anything: safe to resend after a lost connection.
READ_ONLY_CMDS = frozenset(
    {
        "app.state",
        "app.describe",
        "doc.list",
        "doc.meta",
        "history.list",
        "journal.read",
        "job.get",
        "job.list",
        "step.validate",
        "ai.models.list",
        "ai.devices",
        "markers.list",
        "settings.get",
        "transcription.get",
        "measure.levels",
        "measure.thirdOctave",
        "measure.spectrum",
        "measure.harmonics",
        "measure.curve",
        "measure.probe",
    }
)

TERMINAL_JOB_STATES = frozenset({"done", "failed", "error", "cancelled"})

ProgressFn = Callable[[float | None, str | None], None]


class _Pending:
    __slots__ = ("event", "reply", "error")

    def __init__(self) -> None:
        self.event = threading.Event()
        self.reply: dict[str, Any] | None = None
        self.error: ControlError | None = None


class ControlClient:
    """Thread-safe client. Connects lazily; reconnects transparently."""

    def __init__(
        self,
        instance: Instance | None = None,
        *,
        pid: int | None = None,
        locate: Callable[[], Instance | None] | None = None,
        client_name: str = CLIENT_NAME,
        subscribe: Iterable[str] = DEFAULT_SUBSCRIBE,
        connect_timeout: float = 3.0,
        max_events: int = 1000,
    ) -> None:
        self._fixed = instance
        self._pid = pid
        self._locate_fn = locate
        self.client_name = client_name
        self.subscribe = list(subscribe)
        self.connect_timeout = connect_timeout

        self._conn_lock = threading.RLock()  # serialises connect(); never taken by the reader thread
        self._state_lock = threading.Lock()  # guards _sock / hello swaps
        self._send_lock = threading.Lock()
        self._sock: socket.socket | None = None
        self._reader: threading.Thread | None = None
        self._ids = itertools.count(1)
        self._pending: dict[str, _Pending] = {}
        self._pending_lock = threading.Lock()

        self._cv = threading.Condition()
        self._events: collections.deque[dict[str, Any]] = collections.deque(maxlen=max_events)
        self._unread: collections.deque[dict[str, Any]] = collections.deque(maxlen=max_events)
        self._jobs: dict[str, dict[str, Any]] = {}  # job id -> {"progress","status","done"}
        self._listeners: list[Callable[[dict[str, Any]], None]] = []
        self.last_seq: int | None = None
        self.read_only_cmds: set[str] = set(READ_ONLY_CMDS)  # extended with app.describe "Query" commands

        self.instance: Instance | None = None
        self.hello: dict[str, Any] | None = None
        self.connections = 0  # successful hellos (tests / diagnostics)
        self._closed = False

    # ------------------------------------------------------------------ connection

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def _locate(self) -> Instance | None:
        if self._locate_fn is not None:
            return self._locate_fn()
        if self._fixed is not None:
            return self._fixed
        return discovery.find_instance(self._pid)

    def connect(self) -> dict[str, Any]:
        """Connect and say hello if not connected yet. Returns the hello result."""
        with self._conn_lock:
            if self._sock is not None and self.hello is not None:
                return self.hello
            self._closed = False
            inst = self._locate()
            if inst is None:
                raise ControlError("no_instance", "no running SpectroCraft instance with AI control enabled was found")
            if self.instance is not None and _identity(self.instance) != _identity(inst):
                # Another process: its journal sequence is unrelated to ours.
                self.last_seq = None
                with self._cv:
                    self._jobs.clear()
            try:
                sock = socket.create_connection(("127.0.0.1", inst.port), timeout=self.connect_timeout)
            except OSError as e:
                raise ControlError(
                    "connection_lost",
                    f"cannot connect to SpectroCraft pid {inst.pid} on 127.0.0.1:{inst.port}: {e}",
                    {"pid": inst.pid, "port": inst.port},
                ) from e
            sock.settimeout(None)
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            with self._state_lock:
                self._sock = sock
            self.instance = inst
            self._reader = threading.Thread(target=self._read_loop, args=(sock,), name="sc-control-reader", daemon=True)
            self._reader.start()

            params: dict[str, Any] = {"token": inst.token, "client": self.client_name, "subscribe": self.subscribe}
            if self.last_seq is not None:
                params["sinceSeq"] = self.last_seq
            try:
                reply = self._roundtrip({"cmd": "session.hello", "params": params}, self.connect_timeout + REPLY_GRACE_S)
            except ControlError:
                self._drop(sock)
                raise
            result = reply.get("result") or {}
            proto = result.get("protocol")
            if proto is not None and proto != PROTOCOL_VERSION:
                self._drop(sock)
                raise ControlError(
                    "protocol_mismatch",
                    f"SpectroCraft speaks control protocol {proto}, this adapter speaks {PROTOCOL_VERSION}",
                    {"appVersion": result.get("version")},
                )
            if self.last_seq is None and isinstance(result.get("journalSeq"), int):
                with self._cv:
                    if self.last_seq is None:
                        self.last_seq = result["journalSeq"]
            with self._state_lock:
                if self._sock is not sock:
                    raise ControlError("connection_lost", "connection closed right after session.hello")
                self.hello = result
            self.connections += 1
            return result

    def close(self) -> None:
        self._closed = True
        sock = self._sock
        if sock is not None:
            self._drop(sock)

    def __enter__(self) -> "ControlClient":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()

    def _drop(self, sock: socket.socket) -> None:
        """Forget ``sock`` (if still current) and fail every pending call."""
        with self._state_lock:
            if self._sock is sock:
                self._sock = None
                self.hello = None
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            sock.close()
        except OSError:
            pass
        with self._pending_lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for p in pending:
            if not p.event.is_set():
                p.error = ControlError("connection_lost", "connection to SpectroCraft closed before the reply arrived")
                p.event.set()
        with self._cv:
            self._cv.notify_all()

    # ------------------------------------------------------------------ reader

    def _read_loop(self, sock: socket.socket) -> None:
        f = sock.makefile("rb")
        try:
            while True:
                try:
                    line = f.readline(MAX_LINE_BYTES + 1)
                except (OSError, ValueError):
                    break
                if not line:
                    break
                if len(line) > MAX_LINE_BYTES:
                    log.warning("dropping connection: incoming line over %d bytes", MAX_LINE_BYTES)
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line.decode("utf-8"))
                except (UnicodeDecodeError, ValueError):
                    log.warning("ignoring non-JSON line from SpectroCraft: %r", line[:200])
                    continue
                if isinstance(msg, dict):
                    self._dispatch(msg)
        finally:
            try:
                f.close()
            except OSError:
                pass
            self._drop(sock)

    def _dispatch(self, msg: dict[str, Any]) -> None:
        if "event" in msg:
            self._on_event(msg)
            return
        rid = msg.get("id")
        if rid is None:
            log.warning("reply without id: %s", json.dumps(msg)[:300])
            return
        with self._pending_lock:
            p = self._pending.pop(str(rid), None)
        if p is None:
            log.debug("reply for unknown id %r", rid)
            return
        p.reply = msg
        p.event.set()

    def _on_event(self, ev: dict[str, Any]) -> None:
        seq = ev.get("seq")
        data = ev.get("data") if isinstance(ev.get("data"), dict) else {}
        name = str(ev.get("event"))
        with self._cv:
            if isinstance(seq, int):
                if self.last_seq is not None and seq <= self.last_seq and name != "events.dropped":
                    return  # duplicate (replay overlap)
                self.last_seq = seq if self.last_seq is None else max(self.last_seq, seq)
            self._events.append(ev)
            self._unread.append(ev)
            job = data.get("job")
            if isinstance(job, str) and name.startswith("job."):
                st = self._jobs.setdefault(job, {})
                if name == "job.progress":
                    st["progress"] = data.get("progress")
                    st["status"] = data.get("status")
                elif name == "job.done":
                    st["done"] = data
                self._trim_jobs()
            self._cv.notify_all()
            listeners = list(self._listeners)
        for cb in listeners:
            try:
                cb(ev)
            except Exception:  # a listener must never kill the reader
                log.exception("event listener failed")

    def _trim_jobs(self) -> None:
        if len(self._jobs) > 256:
            for k in list(self._jobs)[: len(self._jobs) - 256]:
                del self._jobs[k]

    # ------------------------------------------------------------------ events

    def add_listener(self, cb: Callable[[dict[str, Any]], None]) -> None:
        with self._cv:
            self._listeners.append(cb)

    def remove_listener(self, cb: Callable[[dict[str, Any]], None]) -> None:
        with self._cv:
            if cb in self._listeners:
                self._listeners.remove(cb)

    def recent_events(self) -> list[dict[str, Any]]:
        with self._cv:
            return list(self._events)

    def drain_events(self) -> list[dict[str, Any]]:
        """Events received since the previous drain."""
        with self._cv:
            out = list(self._unread)
            self._unread.clear()
            return out

    def wait_event(self, predicate: Callable[[dict[str, Any]], bool], timeout: float) -> dict[str, Any] | None:
        """First buffered or future event matching ``predicate``, or None after ``timeout`` s."""
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                for ev in self._events:
                    if predicate(ev):
                        return ev
                left = deadline - time.monotonic()
                if left <= 0:
                    return None
                self._cv.wait(left)

    # ------------------------------------------------------------------ requests

    def _send(self, req: dict[str, Any]) -> None:
        data = encode_line(req)
        sock = self._sock
        if sock is None:
            raise ControlError("connection_lost", "not connected")
        with self._send_lock:
            try:
                sock.sendall(data)
            except OSError as e:
                self._drop(sock)
                raise ControlError("connection_lost", f"send failed: {e}", {"sent": False}) from e

    def _roundtrip(self, req: dict[str, Any], wait_s: float | None) -> dict[str, Any]:
        rid = f"c{next(self._ids)}"
        req = {"id": rid, **req}
        p = _Pending()
        with self._pending_lock:
            self._pending[rid] = p
        try:
            self._send(req)
        except ControlError:
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise
        if not p.event.wait(wait_s):
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise ControlError(
                "timeout",
                f"no reply to {req.get('cmd')} within {wait_s:.0f} s",
                {"hint": "a modal dialog is probably open in SpectroCraft"},
            )
        if p.error is not None:
            p.error.details.setdefault("sent", True)
            raise p.error
        reply = p.reply or {}
        if reply.get("ok") is False:
            raise ControlError.from_reply(reply)
        return reply

    def call(
        self,
        cmd: str,
        params: dict[str, Any] | None = None,
        doc: Any = None,
        timeout_ms: int | None = None,
        *,
        retry_if_lost: bool | None = None,
    ) -> dict[str, Any]:
        """Send one command; return the full reply envelope (``result`` + state fields).

        Raises :class:`ControlError` on ``ok:false`` or transport failure. A
        request that could not be sent is retried once after a reconnection;
        one whose connection dropped after sending is retried only when the
        command is read-only (``retry_if_lost`` overrides).
        """
        req: dict[str, Any] = {"cmd": cmd, "params": params or {}}
        if doc is not None:
            req["doc"] = doc
        if timeout_ms is not None:
            req["timeoutMs"] = int(timeout_ms)
        tmo = DEFAULT_TIMEOUT_MS if timeout_ms is None else int(timeout_ms)
        wait_s = None if tmo <= 0 else tmo / 1000.0 + REPLY_GRACE_S
        if retry_if_lost is None:
            retry_if_lost = cmd in self.read_only_cmds
        last: ControlError | None = None
        for _attempt in range(2):
            self.connect()
            try:
                return self._roundtrip(dict(req), wait_s)
            except ControlError as e:
                if e.code != "connection_lost":
                    raise
                last = e
                if e.details.get("sent") is False or retry_if_lost:
                    continue
                raise
        assert last is not None
        raise last

    def result(self, cmd: str, params: dict[str, Any] | None = None, doc: Any = None, timeout_ms: int | None = None) -> Any:
        """Like :meth:`call` but returns only ``result``."""
        return self.call(cmd, params, doc, timeout_ms).get("result")

    # ------------------------------------------------------------------ jobs

    def job_snapshot(self, job: str) -> dict[str, Any]:
        with self._cv:
            return dict(self._jobs.get(job, {}))

    def wait_job(
        self,
        job: str,
        on_progress: ProgressFn | None = None,
        timeout: float | None = None,
        poll_interval: float = 2.0,
    ) -> dict[str, Any]:
        """Block until ``job`` finishes. Returns the ``job.done`` data (``{job, ok, result, ...}``).

        Raises :class:`ControlError` with the job's error when it failed or was
        cancelled, and ``timeout`` (job left running) when ``timeout`` elapses.
        Progress comes from ``job.progress`` events and ``job.get`` polls.
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        last_poll = 0.0
        last_reported: tuple[Any, Any] | None = None
        while True:
            with self._cv:
                st = dict(self._jobs.get(job, {}))
            done = st.get("done")
            if done is None and time.monotonic() - last_poll >= poll_interval:
                last_poll = time.monotonic()
                try:
                    got = self.result("job.get", {"job": job})
                except ControlError as e:
                    if e.code in ("connection_lost", "no_instance"):
                        got = None  # reconnect on the next poll
                    elif e.code == "unknown_cmd":
                        got = None  # app without job.get: rely on events
                    else:
                        raise
                if isinstance(got, dict):
                    state = got.get("state")
                    if state in TERMINAL_JOB_STATES:
                        done = _done_from_job_get(job, got)
                        with self._cv:
                            self._jobs.setdefault(job, {})["done"] = done
                    else:
                        st["progress"] = got.get("progress", st.get("progress"))
                        st["status"] = got.get("status", st.get("status"))
            if on_progress is not None:
                cur = (st.get("progress"), st.get("status"))
                if cur != last_reported and (cur[0] is not None or cur[1] is not None):
                    last_reported = cur
                    try:
                        on_progress(cur[0], cur[1])
                    except Exception:
                        log.exception("progress callback failed")
            if done is not None:
                if done.get("ok") is False:
                    err = done.get("error") if isinstance(done.get("error"), dict) else {}
                    raise ControlError(
                        str(err.get("code", "failed")),
                        str(err.get("message", f"job {job} failed")),
                        {**(err.get("details") or {}), "job": job},
                        reply=done,
                    )
                return done
            if deadline is not None and time.monotonic() >= deadline:
                raise ControlError("timeout", f"job {job} still running", {"job": job, "progress": st.get("progress"), "stillRunning": True})
            wait = poll_interval if deadline is None else max(0.0, min(poll_interval, deadline - time.monotonic()))
            with self._cv:
                if "done" not in self._jobs.get(job, {}):
                    self._cv.wait(min(wait, 0.25) if self._sock is None else wait)

    def run(
        self,
        cmd: str,
        params: dict[str, Any] | None = None,
        doc: Any = None,
        timeout_ms: int | None = None,
        *,
        wait: bool = True,
        on_progress: ProgressFn | None = None,
        job_timeout: float | None = None,
    ) -> dict[str, Any]:
        """``call`` and, if the reply is a job handle and ``wait``, wait for the job.

        For an awaited job the returned envelope is the original reply with
        ``result`` replaced by the job's result and ``job`` / ``jobDone`` added.
        """
        reply = self.call(cmd, params, doc, timeout_ms)
        res = reply.get("result")
        if wait and is_job_handle(cmd, res):
            job = res["job"]
            done = self.wait_job(job, on_progress=on_progress, timeout=job_timeout)
            out = {k: v for k, v in reply.items() if k not in ("result",)}
            out["job"] = job
            out["result"] = done.get("result")
            for k, v in done.items():
                if k not in ("job", "ok", "result"):
                    out[k] = v
            return out
        return reply


def _identity(inst: Instance) -> tuple[int, Any, int]:
    return (inst.pid, inst.raw.get("startedAt"), inst.port)


def _done_from_job_get(job: str, got: dict[str, Any]) -> dict[str, Any]:
    ok = bool(got["ok"]) if isinstance(got.get("ok"), bool) else (got.get("state") == "done" and not got.get("error"))
    done: dict[str, Any] = {"job": job, "ok": ok}
    if ok:
        done["result"] = got.get("result")
    else:
        err = got.get("error")
        if not isinstance(err, dict):
            err = {"code": "cancelled" if got.get("state") == "cancelled" else "failed", "message": str(err or got.get("state"))}
        done["error"] = err
    return done
