"""MCP tools generated from ``app.describe`` (RAPPORT.md 6.2 and 7.3).

Every command published by the app becomes one tool whose ``inputSchema`` is
the command's ``params`` JSON Schema, kept as is; the adapter only adds three
optional envelope-level properties: ``doc`` (target document), ``timeoutMs``
and ``confirm``. A command added on the C++ side therefore shows up in Claude
Code without touching the Python code.

Tool names: the report writes ``sc.<domain>.<command>``, but the Anthropic API
only accepts ``[a-zA-Z0-9_-]{1,64}`` and Claude Code prefixes MCP tools with
``mcp__spectrocraft__`` (19 chars), so ``app.state`` becomes ``sc_app_state``
and names are capped at 45 characters (hash suffix when truncated).

Standard library only.
"""

from __future__ import annotations

import copy
import hashlib
import re
from dataclasses import dataclass, field
from typing import Any, Iterable

TOOL_PREFIX = "sc_"
MAX_TOOL_NAME = 64 - len("mcp__spectrocraft__")
EXCLUDED_COMMANDS = frozenset({"session.hello"})  # handled by the client itself
EXCLUDED_CATEGORIES = frozenset({"session", "debug"})  # debug = Internal test helpers

ENVELOPE_PROPS: dict[str, dict[str, Any]] = {
    "doc": {
        "type": ["integer", "string"],
        "description": "Target document: stable docId from sc_state, or its absolute path. Omit for the active document.",
    },
    "timeoutMs": {
        "type": "integer",
        "minimum": 0,
        "description": "Request timeout in ms (default 30000; 0 = unlimited for long jobs).",
    },
}
CONFIRM_PROP: dict[str, Any] = {
    "type": "boolean",
    "description": "Set to true only after the user agreed to a destructive action (overwrite, unsaved changes, path outside the allowed roots).",
}


@dataclass
class DynamicTool:
    name: str
    command: str
    title: str
    description: str
    input_schema: dict[str, Any]
    flags: list[str]
    category: str
    # tool argument name -> envelope field ("doc" / "timeoutMs"), renamed on clash
    envelope_args: dict[str, str] = field(default_factory=dict)
    confirm_arg: str | None = None  # adapter-added confirm (absent when the schema has its own)
    wrapped: bool = False  # non-object params schema wrapped as {"params": <schema>}

    @property
    def read_only(self) -> bool:
        if "Query" in self.flags:  # the app's own "pure read" flag
            return True
        return not ({"Mutates", "Destructive", "LongRunning"} & set(self.flags))

    @property
    def destructive(self) -> bool:
        return "Destructive" in self.flags

    def annotations(self) -> dict[str, Any]:
        return {
            "title": self.title or self.command,
            "readOnlyHint": self.read_only,
            "destructiveHint": self.destructive,
            "openWorldHint": False,
        }

    def split_arguments(self, arguments: dict[str, Any] | None) -> tuple[dict[str, Any], Any, int | None]:
        """Tool arguments -> (params, doc, timeoutMs)."""
        args = dict(arguments or {})
        doc = None
        timeout_ms = None
        for arg, env in self.envelope_args.items():
            if arg in args:
                v = args.pop(arg)
                if env == "doc":
                    doc = v
                elif env == "timeoutMs" and v is not None:
                    timeout_ms = int(v)
        if self.confirm_arg is not None and self.confirm_arg in args:
            if args.pop(self.confirm_arg) is True:
                args["confirm"] = True
        if self.wrapped:
            params = args.get("params")
            return (params if params is not None else {}), doc, timeout_ms
        return args, doc, timeout_ms


def tool_name(command: str, taken: Iterable[str] = ()) -> str:
    base = TOOL_PREFIX + re.sub(r"[^A-Za-z0-9_-]", "_", command.replace(".", "_"))
    if len(base) > MAX_TOOL_NAME:
        digest = hashlib.sha1(command.encode()).hexdigest()[:8]
        base = base[: MAX_TOOL_NAME - 9] + "_" + digest
    taken = set(taken)
    name = base
    n = 2
    while name in taken:
        suffix = f"_{n}"
        name = base[: MAX_TOOL_NAME - len(suffix)] + suffix
        n += 1
    return name


def _object_schema(params: Any) -> tuple[dict[str, Any], bool]:
    """(object schema, wrapped). Never mutates the input."""
    if params is None or params == {}:
        return {"type": "object", "properties": {}}, False
    if not isinstance(params, dict):
        return {"type": "object", "properties": {}}, False
    schema = copy.deepcopy(params)
    t = schema.get("type")
    if t == "object" or (t is None and ("properties" in schema or "required" in schema)):
        schema.setdefault("type", "object")
        if not isinstance(schema.get("properties"), dict):
            schema["properties"] = {}
        return schema, False
    return {"type": "object", "properties": {"params": schema}, "required": ["params"], "additionalProperties": False}, True


def _description(cmd: dict[str, Any]) -> str:
    cid = str(cmd.get("id"))
    title = str(cmd.get("title") or cid)
    flags = [str(f) for f in cmd.get("flags") or []]
    parts = [f"{title.rstrip('.')}. SpectroCraft command `{cid}`"]
    if cmd.get("category"):
        parts[0] += f" (category {cmd['category']})"
    parts[0] += "."
    if cmd.get("description"):
        parts.append(str(cmd["description"]))
    notes = []
    if "Mutates" in flags:
        notes.append("Modifies the document; undo with sc_undo.")
    if "LongRunning" in flags:
        notes.append("Runs as a job: the adapter waits for it and relays progress.")
    if "Destructive" in flags:
        notes.append("Destructive: the app answers confirm_required unless confirm=true; ask the user before setting it.")
    if "NeedsUi" in flags:
        notes.append("Needs the open app window (not available headless).")
    if "AiBuildOnly" in flags:
        notes.append("AI builds only.")
    if "WindowsOnly" in flags:
        notes.append("Windows only.")
    aliases = [a for a in cmd.get("aliases") or [] if a]
    if aliases:
        notes.append("Aliases: " + ", ".join(map(str, aliases)) + ".")
    notes.append("Units: seconds, 0-based channels, dB.")
    return " ".join(parts + notes)


def build_tools(describe: dict[str, Any], reserved_names: Iterable[str] = ()) -> list[DynamicTool]:
    """Turn an ``app.describe`` result into tool definitions."""
    taken = set(reserved_names)
    tools: list[DynamicTool] = []
    for cmd in describe.get("commands") or []:
        if not isinstance(cmd, dict) or not cmd.get("id"):
            continue
        cid = str(cmd["id"])
        category = str(cmd.get("category") or cid.split(".")[0])
        flags = [str(f) for f in cmd.get("flags") or []]
        if cid in EXCLUDED_COMMANDS or category in EXCLUDED_CATEGORIES or "Internal" in flags:
            continue
        schema, wrapped = _object_schema(cmd.get("params"))
        props: dict[str, Any] = schema["properties"]
        envelope_args: dict[str, str] = {}
        for env, prop in ENVELOPE_PROPS.items():
            arg = env
            while arg in props:  # the command has a param of that name: keep it, rename ours
                arg = "sc_" + arg
            props[arg] = copy.deepcopy(prop)
            envelope_args[arg] = env
        confirm_arg = None
        if "confirm" not in props:
            props["confirm"] = copy.deepcopy(CONFIRM_PROP)
            confirm_arg = "confirm"
        name = tool_name(cid, taken)
        taken.add(name)
        tools.append(
            DynamicTool(
                name=name,
                command=cid,
                title=str(cmd.get("title") or cid),
                description=_description(cmd),
                input_schema=schema,
                flags=flags,
                category=category,
                envelope_args=envelope_args,
                confirm_arg=confirm_arg,
                wrapped=wrapped,
            )
        )
    return tools
