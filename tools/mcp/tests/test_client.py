from __future__ import annotations

import threading
import time

import pytest

from fake_server import FakeServer
from spectrocraft_mcp.client import ControlClient
from spectrocraft_mcp.protocol import MAX_LINE_BYTES, ControlError


@pytest.fixture
def client(fake):
    c = ControlClient(client_name="pytest/1")
    yield c
    c.close()


def test_hello_sends_token_client_subscribe(fake, client):
    hello = client.connect()
    assert hello["protocol"] == 1 and hello["app"] == "SpectroCraft"
    h = fake.hellos[-1]
    assert h["token"] == fake.token
    assert h["client"] == "pytest/1"
    assert set(h["subscribe"]) >= {"job", "history", "doc"}
    assert "sinceSeq" not in h  # first contact: nothing to replay
    assert client.last_seq == hello["journalSeq"]


def test_bad_token_is_unauthorized(control_dir):
    srv = FakeServer(token="right")
    srv.token = "right"
    srv.write_discovery(control_dir)
    srv.token = "changed"  # app restarted its session secret: the file is now wrong
    try:
        with pytest.raises(ControlError) as ei:
            ControlClient().connect()
        assert ei.value.code == "unauthorized"
        assert "sc_status" in ei.value.describe()
    finally:
        srv.close()


def test_no_instance(control_dir):
    with pytest.raises(ControlError) as ei:
        ControlClient().call("app.state")
    assert ei.value.code == "no_instance"


def test_call_returns_envelope(client):
    reply = client.call("step.run", {"select": {"start": 1.0, "end": 2.0}, "channels": [0],
                                     "step": {"type": "changeGain", "params": {"db": -3.0}}}, doc=7)
    assert reply["ok"] is True
    assert reply["result"]["applied"] == "changeGain"
    assert reply["undoDepth"] == 1 and reply["redoDepth"] == 0
    assert isinstance(reply["journalSeq"], int)


def test_routing_by_id_out_of_order(fake, client):
    fake.slow["app.state"] = 0.4
    out: dict[str, object] = {}

    def slow():
        out["slow"] = client.result("app.state")

    t = threading.Thread(target=slow)
    client.connect()
    t.start()
    time.sleep(0.05)
    t0 = time.monotonic()
    fast = client.result("doc.list")
    assert time.monotonic() - t0 < 0.3  # not stuck behind the slow reply
    t.join(2)
    assert fast["active"] == 7
    assert "docs" in out["slow"]


@pytest.mark.parametrize(
    "cmd, params, code, needle",
    [
        ("step.run", {"step": {"type": "changeGain", "params": {}}}, "invalid_params", "step.params.db"),
        ("step.run", {"step": {"type": "envelop"}}, "invalid_params", "step.type"),
        ("doc.save", {"path": "/tmp/x.wav"}, "confirm_required", "pass confirm:true"),
        ("nope.nothing", {}, "unknown_cmd", "spectrocraft://commands"),
        ("view.zoom", {"vertical": 2}, "needs_ui", "headless"),
        ("job.get", {"job": "j-999"}, "job_not_found", "sc_jobs"),
    ],
)
def test_error_codes_carry_details_and_hint(client, cmd, params, code, needle):
    with pytest.raises(ControlError) as ei:
        client.call(cmd, params)
    e = ei.value
    assert e.code == code
    assert needle in e.describe()
    assert e.reply is not None and e.reply["ok"] is False


def test_unknown_doc(client):
    with pytest.raises(ControlError) as ei:
        client.call("app.state")  # fine
        client.call("history.list", {}, doc=99)
    assert ei.value.code == "unknown_doc"


def test_events_are_queued_and_seq_tracked(fake, client):
    client.connect()
    before = client.last_seq
    client.call("step.run", {"select": "all", "step": {"type": "normalize"}})
    ev = client.wait_event(lambda e: e["event"] == "history.changed", 2)
    assert ev is not None and ev["data"]["doc"] == 7
    assert client.last_seq > before
    drained = client.drain_events()
    assert any(e["event"] == "history.changed" for e in drained)
    assert client.drain_events() == []


def test_wait_job_reports_progress(client):
    reply = client.call("ai.models.install", {"modelId": "whisper-base"})
    job = reply["result"]["job"]
    seen: list[tuple[float | None, str | None]] = []
    done = client.wait_job(job, on_progress=lambda p, s: seen.append((p, s)))
    assert done["ok"] is True and done["result"]["installed"] is True
    progresses = [p for p, _ in seen if p is not None]
    assert progresses and progresses == sorted(progresses)
    assert any(s and s.startswith("step") for _, s in seen)


def test_wait_job_polls_when_no_events(fake, client):
    fake.events_enabled = False  # an app that does not emit job events (yet)
    job = client.result("ai.models.install", {"modelId": "x"})["job"]
    done = client.wait_job(job, poll_interval=0.05, timeout=5)
    assert done["ok"] is True


def test_wait_job_failure_and_cancel(fake, client):
    job = client.result("ai.noiseSuppress", {"fail": True})["job"]
    with pytest.raises(ControlError) as ei:
        client.wait_job(job, timeout=5)
    assert ei.value.code == "failed" and ei.value.details["job"] == job

    fake.job_step_s = 0.2
    job = client.result("ai.noiseSuppress", {})["job"]
    client.call("job.cancel", {"job": job})
    with pytest.raises(ControlError) as ei:
        client.wait_job(job, timeout=5)
    assert ei.value.code == "cancelled"


def test_wait_job_timeout_leaves_job_running(fake, client):
    fake.job_step_s = 0.5
    job = client.result("test.longJob", {})["job"]
    with pytest.raises(ControlError) as ei:
        client.wait_job(job, timeout=0.3)
    assert ei.value.code == "timeout" and ei.value.details["stillRunning"] is True


def test_run_waits_for_job_handles(client):
    reply = client.run("ai.models.install", {"modelId": "whisper-base"})
    assert reply["job"].startswith("j-")
    assert reply["result"]["installed"] is True
    # job.* replies carry job ids too but are never awaited
    assert client.run("job.get", {"job": reply["job"]})["result"]["state"] == "done"


def test_silent_reconnect_and_since_seq_replay(fake, client):
    client.connect()
    client.call("step.run", {"select": "all", "step": {"type": "normalize"}})
    assert client.wait_event(lambda e: e["event"] == "history.changed", 2)
    seen_seq = client.last_seq

    fake.drop_clients()
    deadline = time.monotonic() + 2
    while client.connected and time.monotonic() < deadline:
        time.sleep(0.01)
    assert not client.connected
    # things happen in the app while we are away (e.g. the user edits with the mouse)
    missed = fake.emit("history.changed", {"doc": 7, "undoDepth": 2, "source": "menu"})

    state = client.result("app.state")  # reconnects transparently
    assert state["activeDoc"] == 7
    assert client.connections == 2
    assert fake.hellos[-1]["sinceSeq"] == seen_seq
    ev = client.wait_event(lambda e: e.get("seq") == missed["seq"], 2)
    assert ev is not None and ev["data"]["source"] == "menu"


def test_reconnect_to_restarted_app_resets_seq(fake, control_dir, client):
    client.connect()
    fake.close()
    (control_dir / f"{fake.pid}.json").unlink()
    srv2 = FakeServer(token="new-token")
    srv2.seq = 5  # fresh journal
    srv2.write_discovery(control_dir, started_at=time.time() + 60)  # new process start
    try:
        deadline = time.monotonic() + 2
        while client.connected and time.monotonic() < deadline:
            time.sleep(0.01)
        assert client.result("doc.list")["active"] == 7
        assert "sinceSeq" not in srv2.hellos[-1]
        assert client.last_seq == 5
    finally:
        srv2.close()


def test_in_flight_mutation_not_retried(fake, client):
    client.connect()
    fake.slow["step.run"] = 0.5
    threading.Timer(0.1, fake.drop_clients).start()
    with pytest.raises(ControlError) as ei:
        client.call("step.run", {"select": "all", "step": {"type": "normalize"}})
    assert ei.value.code == "connection_lost"
    assert "may or may not" in ei.value.describe()


def test_in_flight_read_only_retried(fake, client):
    client.connect()
    fake.slow["app.state"] = 0.5
    threading.Timer(0.1, fake.drop_clients).start()
    t = threading.Timer(0.3, lambda: fake.slow.pop("app.state", None))
    t.start()
    assert client.result("app.state")["activeDoc"] == 7
    assert client.connections == 2


def test_wait_job_survives_reconnection(fake, client):
    fake.job_step_s = 0.1
    job = client.result("ai.models.install", {"modelId": "x"})["job"]
    threading.Timer(0.05, fake.drop_clients).start()
    done = client.wait_job(job, poll_interval=0.1, timeout=10)
    assert done["ok"] is True


def test_outgoing_line_limit(client):
    with pytest.raises(ControlError) as ei:
        client.call("markers.add", {"name": "x" * MAX_LINE_BYTES})
    assert ei.value.code == "invalid_params"


def test_incoming_line_limit_drops_connection(client):
    with pytest.raises(ControlError) as ei:
        client.call("test.big", {"size": MAX_LINE_BYTES + 10})
    assert ei.value.code == "connection_lost"
    # and the client recovers on the next call
    assert client.result("app.state")["activeDoc"] == 7


def test_large_but_legal_reply(client):
    blob = client.result("test.big", {"size": MAX_LINE_BYTES - 200})["blob"]
    assert len(blob) == MAX_LINE_BYTES - 200
