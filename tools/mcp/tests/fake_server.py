"""In-process fake of the SpectroCraft control server (RAPPORT.md section 6).

Implements the NDJSON envelope for the commands the adapter calls: token
check in session.hello, routing by id (replies may be out of order), events
with a monotonic ``seq`` and ``sinceSeq`` replay, jobs with progress, the
closed error codes, plus test hooks (drop connections, mute events, slow
commands).
"""

from __future__ import annotations

import json
import os
import socket
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

DATA = Path(__file__).parent / "data"
ACTION_TYPES = [
    "silence", "fadeIn", "fadeOut", "normalize", "reverse", "invert", "changeGain", "timeStretch",
    "catTimeScale", "filterEq", "envelope", "resample", "delete", "trim", "genSilence", "genNoise",
    "genTone", "noiseSuppress", "transcribe", "python",
]


def load_describe() -> dict[str, Any]:
    return json.loads((DATA / "describe_sample.json").read_text(encoding="utf-8"))


def _now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


class AppError(Exception):
    def __init__(self, code: str, message: str, details: dict[str, Any] | None = None):
        super().__init__(message)
        self.code, self.message, self.details = code, message, details or {}


class _Session:
    def __init__(self, server: "FakeServer", sock: socket.socket):
        self.server, self.sock = server, sock
        self.authed = False
        self.client = "?"
        self.kinds: list[str] = []
        self.wlock = threading.Lock()
        self.alive = True

    def send(self, obj: dict[str, Any]) -> None:
        data = json.dumps(obj, separators=(",", ":")).encode() + b"\n"
        with self.wlock:
            try:
                self.sock.sendall(data)
            except OSError:
                self.alive = False

    def wants(self, event: str) -> bool:
        return self.authed and (not self.kinds or event.split(".")[0] in self.kinds or event == "events.dropped")


class FakeServer:
    def __init__(self, token: str = "tok-123", *, pid: int | None = None, events_enabled: bool = True):
        self.token = token
        self.pid = pid or os.getpid()
        self.events_enabled = events_enabled
        self.lock = threading.RLock()
        self.seq = 400
        self.log: list[dict[str, Any]] = []
        self.sessions: list[_Session] = []
        self.requests: list[dict[str, Any]] = []
        self.hellos: list[dict[str, Any]] = []
        self.slow: dict[str, float] = {}
        self.extra_handlers: dict[str, Callable[[dict[str, Any]], Any]] = {}
        self.job_steps = 4
        self.job_step_s = 0.05
        self.jobs: dict[str, dict[str, Any]] = {}
        self.next_job = 1
        self.docs: list[dict[str, Any]] = [
            {"id": 7, "path": "/tmp/recording_a.wav", "name": "recording_a.wav", "durationSec": 12.345, "channels": 2,
             "sampleRate": 48000, "unsaved": False, "fullyInMemory": True}
        ]
        self.active = 7
        self.undo: dict[int, list[dict[str, Any]]] = {7: []}
        self.redo: dict[int, list[dict[str, Any]]] = {7: []}
        self.content_version = 40
        self.describe = load_describe()

        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self._stop = False
        self._thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._thread.start()

    # ------------------------------------------------------------------ lifecycle

    def discovery_json(self, started_at: Any = None) -> dict[str, Any]:
        return {
            "pid": self.pid,
            "port": self.port,
            "token": self.token,
            "version": "1.0.72",
            "startedAt": started_at if started_at is not None else _now(),
            "exe": "/Applications/SpectroCraft.app/Contents/MacOS/SpectroCraft",
            "protocol": 1,
        }

    def write_discovery(self, directory: Path, started_at: Any = None) -> Path:
        directory.mkdir(parents=True, exist_ok=True)
        p = directory / f"{self.pid}.json"
        p.write_text(json.dumps(self.discovery_json(started_at)), encoding="utf-8")
        return p

    def close(self) -> None:
        self._stop = True
        try:
            self.listener.close()
        except OSError:
            pass
        self.drop_clients()

    def drop_clients(self) -> None:
        with self.lock:
            sessions, self.sessions = self.sessions, []
        for s in sessions:
            s.alive = False
            try:
                s.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            s.sock.close()

    def _accept_loop(self) -> None:
        while not self._stop:
            try:
                sock, _ = self.listener.accept()
            except OSError:
                return
            s = _Session(self, sock)
            with self.lock:
                self.sessions.append(s)
            threading.Thread(target=self._serve, args=(s,), daemon=True).start()

    def _serve(self, s: _Session) -> None:
        f = s.sock.makefile("rb")
        try:
            while s.alive:
                line = f.readline(1024 * 1024 + 1)
                if not line:
                    break
                if len(line) > 1024 * 1024:
                    s.send({"id": None, "ok": False, "error": {"code": "invalid_params", "message": "line over 1 MiB"}})
                    break
                try:
                    req = json.loads(line)
                except ValueError:
                    s.send({"id": None, "ok": False, "error": {"code": "invalid_params", "message": "malformed JSON"}})
                    continue
                with self.lock:
                    self.requests.append(req)
                delay = self.slow.get(req.get("cmd", ""))
                if delay:
                    # answer asynchronously so replies can arrive out of order
                    threading.Thread(target=self._answer, args=(s, req, delay), daemon=True).start()
                else:
                    self._answer(s, req, 0)
        except OSError:
            pass
        finally:
            s.alive = False
            with self.lock:
                if s in self.sessions:
                    self.sessions.remove(s)
            try:
                s.sock.close()
            except OSError:
                pass

    def _answer(self, s: _Session, req: dict[str, Any], delay: float) -> None:
        if delay:
            time.sleep(delay)
        rid = req.get("id")
        try:
            reply = self._handle(s, req)
            s.send({"id": rid, "ok": True, **reply})
        except AppError as e:
            s.send({"id": rid, "ok": False, "error": {"code": e.code, "message": e.message, "details": e.details}})
            if e.code == "unauthorized":
                s.alive = False
                try:
                    s.sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    # ------------------------------------------------------------------ events

    def emit(self, event: str, data: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            self.seq += 1
            ev = {"event": event, "seq": self.seq, "ts": _now(), "data": data}
            self.log.append(ev)
            targets = [x for x in self.sessions if x.wants(event)] if self.events_enabled else []
        for t in targets:
            t.send(ev)
        return ev

    # ------------------------------------------------------------------ commands

    def _doc(self, req: dict[str, Any]) -> dict[str, Any]:
        d = req.get("doc", self.active)
        if d is None:
            raise AppError("no_document", "No document is open")
        for doc in self.docs:
            if doc["id"] == d or doc["path"] == d:
                return doc
        raise AppError("unknown_doc", f"unknown doc {d!r}", {"doc": d})

    def _state_fields(self, doc: dict[str, Any]) -> dict[str, Any]:
        return {
            "undoDepth": len(self.undo[doc["id"]]),
            "redoDepth": len(self.redo[doc["id"]]),
            "redoDiscarded": 0,
            "contentVersion": self.content_version,
            "journalSeq": self.seq,
        }

    def _handle(self, s: _Session, req: dict[str, Any]) -> dict[str, Any]:
        cmd = req.get("cmd")
        p = req.get("params") or {}
        if cmd == "session.hello":
            return self._hello(s, p)
        if not s.authed:
            raise AppError("unauthorized", "session.hello required first")
        with self.lock:
            if cmd in self.extra_handlers:
                return {"result": self.extra_handlers[cmd](req)}
            if cmd == "app.state":
                return {"result": {"docs": self.docs, "activeDoc": self.active, "busy": False, "modalActive": False,
                                   "jobs": [j["public"] for j in self.jobs.values() if j["public"]["state"] == "running"]}}
            if cmd == "app.describe":
                cat = p.get("category")
                cmds = [c for c in self.describe["commands"] if not cat or c["category"] == cat]
                if p.get("withSchemas") is False:
                    cmds = [{k: v for k, v in c.items() if k not in ("params", "result")} for c in cmds]
                return {"result": {"protocol": 1, "commands": cmds}}
            if cmd == "doc.list":
                return {"result": {"active": self.active, "docs": self.docs}}
            if cmd == "doc.open":
                path = p.get("path")
                if not isinstance(path, str):
                    raise AppError("invalid_params", "missing path", {"path": "path", "expected": "string"})
                nid = max([d["id"] for d in self.docs] + [7]) + 1
                doc = {"id": nid, "path": path, "name": path.rsplit("/", 1)[-1], "durationSec": 30.0, "channels": 4,
                       "sampleRate": 48000, "unsaved": False, "fullyInMemory": True}
                self.docs.append(doc)
                self.undo[nid], self.redo[nid] = [], []
                if p.get("activate", True):
                    self.active = nid
                self.emit("doc.opened", {"doc": nid, "path": path})
                return {"result": {"doc": nid, "durationSec": 30.0, "channels": 4, "sampleRate": 48000, "fullyInMemory": True}}
            if cmd == "doc.save":
                doc = self._doc(req)
                if not p.get("confirm"):
                    raise AppError("confirm_required", "Target exists; resend with params.confirm=true",
                                   {"hint": "pass confirm:true, or choose a new path"})
                doc["unsaved"] = False
                return {"result": {"path": p.get("path", doc["path"]), "unsaved": False}}
            if cmd == "doc.close":
                doc = self._doc(req)
                if doc["unsaved"] and not p.get("confirm"):
                    raise AppError("confirm_required", "Document has unsaved changes",
                                   {"unsaved": True, "hint": "call doc.save first or pass confirm:true"})
                self.docs.remove(doc)
                self.active = self.docs[0]["id"] if self.docs else None
                return {"result": {"closed": doc["id"]}}
            if cmd == "step.run":
                return self._step_run(req, p)
            if cmd in ("history.undo", "history.redo"):
                doc = self._doc(req)
                src, dst = (self.undo, self.redo) if cmd == "history.undo" else (self.redo, self.undo)
                n = int(p.get("steps", 1))
                moved = 0
                desc = None
                for _ in range(n):
                    if not src[doc["id"]]:
                        break
                    e = src[doc["id"]].pop()
                    dst[doc["id"]].append(e)
                    desc = e["description"]
                    moved += 1
                self.content_version += 1
                self.emit("history.changed", {"doc": doc["id"], "undoDepth": len(self.undo[doc["id"]]), "source": "api:" + s.client})
                key = "undone" if cmd == "history.undo" else "redone"
                return {"result": {"currentState": len(self.undo[doc["id"]]), key: moved, "description": desc},
                        **self._state_fields(doc)}
            if cmd == "history.list":
                doc = self._doc(req)
                states = [{"state": 0, "label": "Loaded " + doc["name"]}]
                for i, e in enumerate(self.undo[doc["id"]], 1):
                    states.append({"state": i, **e, "future": False})
                for j, e in enumerate(reversed(self.redo[doc["id"]]), len(states)):
                    states.append({"state": j, **e, "future": True})
                return {"result": {"docId": doc["id"], "currentState": len(self.undo[doc["id"]]), "maxLevels": 64, "states": states}}
            if cmd == "history.jumpTo":
                doc = self._doc(req)
                target = int(p["state"])
                while len(self.undo[doc["id"]]) > target:
                    self.redo[doc["id"]].append(self.undo[doc["id"]].pop())
                while len(self.undo[doc["id"]]) < target and self.redo[doc["id"]]:
                    self.undo[doc["id"]].append(self.redo[doc["id"]].pop())
                return {"result": {"currentState": len(self.undo[doc["id"]])}, **self._state_fields(doc)}
            if cmd in ("ai.models.install", "ai.noiseSuppress", "test.longJob"):
                return {"result": self._start_job(cmd, p, s.client, fail=p.get("fail"))}
            if cmd == "job.get":
                j = self.jobs.get(p.get("job"))
                if j is None:
                    raise AppError("job_not_found", f"unknown job {p.get('job')}")
                return {"result": j["public"]}
            if cmd == "job.list":
                return {"result": {"jobs": [j["public"] for j in self.jobs.values()]}}
            if cmd == "job.cancel":
                j = self.jobs.get(p.get("job"))
                if j is None:
                    raise AppError("job_not_found", f"unknown job {p.get('job')}")
                j["cancel"] = True
                return {"result": {"job": p.get("job"), "state": "cancelling"}}
            if cmd == "measure.levels":
                if "start" not in p or "end" not in p:
                    raise AppError("invalid_params", "start/end required", {"path": "start", "expected": "number"})
                return {"result": {"unit": "dBFS", "channels": [{"ch": c, "rmsDb": -18.3, "peakDb": -6.1} for c in p.get("channels", [0])]}}
            if cmd == "view.zoom":
                raise AppError("needs_ui", "view.zoom {vertical} needs the open window")
            if cmd == "journal.read":
                since = int(p.get("sinceSeq", 0))
                entries = [{"seq": e["seq"], "kind": e["event"].split(".")[0], "what": e["event"]} for e in self.log if e["seq"] > since]
                return {"result": {"entries": entries[-int(p.get("limit", 200)):], "nextSeq": self.seq + 1, "file": None}}
            if cmd == "test.big":
                return {"result": {"blob": "x" * int(p.get("size", 10))}}
        raise AppError("unknown_cmd", f"unknown command {cmd!r}")

    def _hello(self, s: _Session, p: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            self.hellos.append(dict(p))
        if p.get("token") != self.token:
            raise AppError("unauthorized", "bad token")
        s.authed = True
        s.client = str(p.get("client", "?"))
        s.kinds = list(p.get("subscribe") or [])
        since = p.get("sinceSeq")
        if isinstance(since, int) and self.events_enabled:
            with self.lock:
                replay = [e for e in self.log if e["seq"] > since and s.wants(e["event"])]
            # replayed events are written before the hello reply, as a real
            # server flushing its ring buffer would
            for e in replay:
                s.send(e)
        return {"result": {"app": "SpectroCraft", "version": "1.0.72", "pid": self.pid, "protocol": 1, "platform": "linux",
                           "capabilities": {"ai": True, "audiocpp": False, "python": True, "ui": True},
                           "commands": len(self.describe["commands"]), "journalSeq": self.seq}}

    def _step_run(self, req: dict[str, Any], p: dict[str, Any]) -> dict[str, Any]:
        doc = self._doc(req)
        step = p.get("step")
        if not isinstance(step, dict):
            raise AppError("invalid_params", "missing step", {"path": "step", "expected": "object"})
        t = step.get("type")
        if t not in ACTION_TYPES:
            raise AppError("invalid_params", f'unknown step type "{t}"', {"path": "step.type", "expected": "one of actionTypes"})
        sp = step.get("params") or {}
        if t == "changeGain" and not isinstance(sp.get("db"), (int, float)):
            raise AppError("invalid_params", 'changeGain: missing numeric param "db"', {"path": "step.params.db", "expected": "number"})
        entry = {"opId": "step." + t, "params": sp, "description": f"{t} applied", "select": p.get("select"), "channels": p.get("channels")}
        self.undo[doc["id"]].append(entry)
        discarded = len(self.redo[doc["id"]])
        self.redo[doc["id"]] = []
        self.content_version += 1
        doc["unsaved"] = True
        self.emit("history.changed", {"doc": doc["id"], "undoDepth": len(self.undo[doc["id"]]), "opId": "step." + t, "source": "api"})
        fields = self._state_fields(doc)
        fields["redoDiscarded"] = discarded
        return {"result": {"applied": t, "patchSeq": 100 + len(self.undo[doc["id"]]), "description": entry["description"]},
                "alerts": [], **fields}

    def _start_job(self, cmd: str, p: dict[str, Any], client: str, fail: Any = None) -> dict[str, Any]:
        jid = f"j-{self.next_job}"
        self.next_job += 1
        job = {"public": {"job": jid, "cmd": cmd, "state": "running", "progress": 0.0}, "cancel": False}
        self.jobs[jid] = job
        threading.Thread(target=self._run_job, args=(jid, fail), daemon=True).start()
        return {"job": jid, "state": "running"}

    def _run_job(self, jid: str, fail: Any) -> None:
        job = self.jobs[jid]
        for i in range(1, self.job_steps + 1):
            time.sleep(self.job_step_s)
            if job["cancel"]:
                with self.lock:
                    job["public"].update(state="cancelled", error={"code": "cancelled", "message": "cancelled by client"})
                self.emit("job.done", {"job": jid, "ok": False, "error": {"code": "cancelled", "message": "cancelled by client"}})
                return
            prog = i / (self.job_steps + 1)
            with self.lock:
                job["public"]["progress"] = prog
            self.emit("job.progress", {"job": jid, "progress": prog, "status": f"step {i}/{self.job_steps}"})
        time.sleep(self.job_step_s)
        if fail:
            err = {"code": "failed", "message": "AI processing failed on CPU", "details": {"manualInstall": "see docs"}}
            with self.lock:
                job["public"].update(state="failed", error=err)
            self.emit("job.done", {"job": jid, "ok": False, "error": err})
            return
        result = {"done": True, "modelId": "whisper-base", "installed": True}
        with self.lock:
            job["public"].update(state="done", progress=1.0, result=result)
        self.emit("job.done", {"job": jid, "ok": True, "result": result, "undoDepth": 0})
