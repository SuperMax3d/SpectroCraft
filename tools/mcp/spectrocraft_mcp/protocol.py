"""Constants, error type and error hints of the SpectroCraft control protocol.

Reference: docs/ai-control/REPORT.md section 6 (envelope, closed error codes)
and section 5.3 (limits). Standard library only: this module is imported by
the dependency-free ``sc_client.py``.
"""

from __future__ import annotations

import json
from typing import Any

PROTOCOL_VERSION = 1
"""Protocol version this adapter speaks (``protocol`` field of session.hello)."""

MAX_LINE_BYTES = 1024 * 1024
"""NDJSON line limit (1 MiB), same on the socket and in ``--json`` mode."""

DEFAULT_TIMEOUT_MS = 30_000
"""Default per-request timeout used by the app (``timeoutMs``)."""

CLIENT_NAME = "spectrocraft-mcp/0.1"

# Closed list of error codes (section 6.1).
ERROR_CODES = frozenset(
    {
        "unauthorized",
        "unknown_cmd",
        "invalid_params",
        "no_document",
        "unknown_doc",
        "busy",
        "timeout",
        "cancelled",
        "confirm_required",
        "needs_ui",
        "unsupported_build",
        "unsupported_platform",
        "model_not_installed",
        "job_not_found",
        "shutting_down",
        "failed",
    }
)

# Adapter-side codes (never sent by the app).
LOCAL_CODES = frozenset({"connection_lost", "no_instance", "control_disabled", "protocol_mismatch", "launch_failed"})

# What Claude should do next, per code. Shown after the app's own message and hint.
NEXT_STEP: dict[str, str] = {
    "unauthorized": "The token in the discovery file was refused. SpectroCraft was probably restarted: run sc_status to rediscover it.",
    "unknown_cmd": "This build does not know that command. List the available ones with the spectrocraft://commands resource or sc_run('app.describe').",
    "invalid_params": "Fix the parameter named in details.path (expected type in details.expected) and retry.",
    "no_document": "No document is open. Open one with sc_open(path) first.",
    "unknown_doc": "That docId does not exist (documents are addressed by stable id, never by index). Call sc_state to list open documents.",
    "busy": "Another job or a modal dialog is active. Check sc_jobs, wait for it, or cancel it with sc_cancel; if details.modalActive is true, ask the user to close the dialog.",
    "timeout": "The app did not answer in time, usually because a modal dialog is open in SpectroCraft. Ask the user to close it, then retry.",
    "cancelled": "The job was cancelled; nothing was committed.",
    "confirm_required": "This is destructive (overwrite, unsaved changes, path outside the allowed roots). Ask the user, then resend the same call with confirm=true.",
    "needs_ui": "This command needs the open app window; it is not available in headless (--json) mode. Use the open app (sc_launch) instead of sc_headless.",
    "unsupported_build": "This SpectroCraft build was compiled without that feature (for example without AI).",
    "unsupported_platform": "This feature is not available on this operating system (audio.cpp publishes CLI builds for Windows x64, macOS and Linux x64 only).",
    "model_not_installed": "Install the model first with sc_install_model(model_id), then retry.",
    "job_not_found": "Unknown job id. List jobs with sc_jobs.",
    "shutting_down": "SpectroCraft is closing. Run sc_status to see whether another instance is available.",
    "failed": "The operation failed inside SpectroCraft; see the message and details.",
    "connection_lost": "The connection to SpectroCraft dropped while the request was in flight; it may or may not have been applied. Check sc_state / sc_history before retrying a mutating command.",
    "no_instance": "No controllable SpectroCraft instance was found.",
    "control_disabled": "SpectroCraft is open but AI control is disabled.",
    "protocol_mismatch": "The app speaks a different control protocol version than this adapter; update one of them.",
    "launch_failed": "SpectroCraft could not be started; check SPECTROCRAFT_EXE.",
}


class ControlError(Exception):
    """An error reply from SpectroCraft (``ok:false``) or an adapter-side failure."""

    def __init__(self, code: str, message: str, details: dict[str, Any] | None = None, reply: dict[str, Any] | None = None):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message
        self.details = details or {}
        self.reply = reply

    @classmethod
    def from_reply(cls, reply: dict[str, Any]) -> "ControlError":
        err = reply.get("error")
        if not isinstance(err, dict):
            return cls("failed", f"malformed error reply: {json.dumps(reply)[:300]}", reply=reply)
        details = err.get("details")
        return cls(
            str(err.get("code", "failed")),
            str(err.get("message", "")),
            details if isinstance(details, dict) else ({} if details is None else {"value": details}),
            reply,
        )

    @property
    def hint(self) -> str | None:
        hint = self.details.get("hint")
        return str(hint) if hint else None

    def describe(self) -> str:
        """Human/LLM-readable multi-line description: code, message, hint, next step, details."""
        lines = [f"SpectroCraft error [{self.code}]: {self.message}".rstrip()]
        if self.hint:
            lines.append(f"Hint: {self.hint}")
        step = NEXT_STEP.get(self.code)
        if step:
            lines.append(f"What to do: {step}")
        extra = {k: v for k, v in self.details.items() if k != "hint"}
        if extra:
            lines.append("Details: " + json.dumps(extra, ensure_ascii=False, default=str))
        return "\n".join(lines)


def encode_line(obj: dict[str, Any]) -> bytes:
    """Serialise one NDJSON line (UTF-8, compact, trailing newline). Enforces the 1 MiB limit."""
    data = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
    if len(data) > MAX_LINE_BYTES:
        raise ControlError(
            "invalid_params",
            f"request is {len(data)} bytes, over the {MAX_LINE_BYTES}-byte NDJSON line limit",
            {"hint": "split the request or pass large data through a file path"},
        )
    return data


def is_job_handle(cmd: str, result: Any) -> bool:
    """True when ``result`` is the handle of a job still to be awaited.

    In socket mode a LongRunning command (or a mutation on a streamed file)
    answers ``{"job":"j-7","state":"running"}`` (section 6.1); job.* commands
    themselves return job ids too and must not be awaited again.
    """
    if cmd.startswith("job.") or not isinstance(result, dict):
        return False
    job = result.get("job")
    if not isinstance(job, str) or not job:
        return False
    return result.get("state") in (None, "queued", "pending", "running", "started")
