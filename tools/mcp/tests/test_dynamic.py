from __future__ import annotations

import copy
import re

import jsonschema
import pytest

from fake_server import load_describe
from spectrocraft_mcp import dynamic

NAME_RE = re.compile(r"^[a-zA-Z0-9_-]{1,%d}$" % dynamic.MAX_TOOL_NAME)


@pytest.fixture
def describe():
    return load_describe()


@pytest.fixture
def tools(describe):
    return dynamic.build_tools(describe, reserved_names={"sc_state", "sc_run"})


def test_every_input_schema_is_valid_json_schema(tools):
    assert tools
    for t in tools:
        jsonschema.Draft202012Validator.check_schema(t.input_schema)
        assert t.input_schema["type"] == "object", t.name


def test_sample_params_schemas_are_valid_too(describe):
    for c in describe["commands"]:
        if "params" in c:
            jsonschema.Draft202012Validator.check_schema(c["params"])


def test_params_schema_kept_unchanged_apart_from_envelope_props(describe, tools):
    original = {c["id"]: c.get("params") for c in describe["commands"]}
    pristine = copy.deepcopy(original)
    for t in tools:
        src = original[t.command]
        if src is None:
            continue
        schema = copy.deepcopy(t.input_schema)
        added = set(t.envelope_args) | ({t.confirm_arg} if t.confirm_arg else set())
        for k in added:
            del schema["properties"][k]
        assert schema == src, t.command
    assert original == pristine  # build_tools never mutates the describe


def test_names_are_api_safe_unique_and_not_reserved(tools):
    names = [t.name for t in tools]
    assert len(names) == len(set(names))
    for n in names:
        assert NAME_RE.match(n), n
        assert n not in {"sc_state", "sc_run"}
    by_cmd = {t.command: t.name for t in tools}
    assert by_cmd["app.state"] == "sc_app_state"
    assert by_cmd["effect.changeGain"] == "sc_effect_changeGain"
    assert by_cmd["ai.models.install"] == "sc_ai_models_install"
    long = [t for t in tools if t.command.startswith("ai.transcribeWithAVeryLong")][0]
    assert len(long.name) <= dynamic.MAX_TOOL_NAME


def test_session_commands_excluded(tools):
    assert "session.hello" not in {t.command for t in tools}


def test_envelope_and_confirm_props_added(tools):
    t = next(t for t in tools if t.command == "effect.changeGain")
    props = t.input_schema["properties"]
    assert {"doc", "timeoutMs", "confirm"} <= set(props)
    assert t.input_schema["required"] == ["db"]
    params, doc, tmo = t.split_arguments({"db": 6.0, "doc": 7, "timeoutMs": 5000, "confirm": False})
    assert params == {"db": 6.0} and doc == 7 and tmo == 5000
    params, _, _ = t.split_arguments({"db": 6.0, "confirm": True})
    assert params == {"db": 6.0, "confirm": True}


def test_command_own_confirm_param_untouched(tools):
    t = next(t for t in tools if t.command == "doc.save")
    assert t.confirm_arg is None
    assert t.input_schema["properties"]["confirm"]["description"] == "Required to overwrite"
    params, _, _ = t.split_arguments({"path": "/x.wav", "confirm": False})
    assert params == {"path": "/x.wav", "confirm": False}
    assert t.destructive and not t.read_only


def test_clash_with_envelope_name_renames_adapter_prop(tools):
    t = next(t for t in tools if t.command == "markers.add")
    props = t.input_schema["properties"]
    assert props["doc"]["description"].startswith("a param")  # the command's own
    assert "sc_doc" in props
    params, doc, _ = t.split_arguments({"name": "slam", "start": 4.1, "doc": "abc", "sc_doc": 8})
    assert params == {"name": "slam", "start": 4.1, "doc": "abc"} and doc == 8


def test_command_without_schema_gets_empty_object(tools):
    t = next(t for t in tools if t.command == "app.ping")
    assert t.input_schema["type"] == "object"
    params, _, _ = t.split_arguments({})
    assert params == {}


def test_non_object_schema_is_wrapped():
    tools = dynamic.build_tools({"commands": [{"id": "x.list", "params": {"type": "array", "items": {"type": "number"}}}]})
    t = tools[0]
    assert t.wrapped
    jsonschema.Draft202012Validator.check_schema(t.input_schema)
    params, _, _ = t.split_arguments({"params": [1, 2]})
    assert params == [1, 2]


def test_generated_schema_validates_arguments(tools):
    t = next(t for t in tools if t.command == "effect.changeGain")
    v = jsonschema.Draft202012Validator(t.input_schema)
    v.validate({"db": 6, "select": {"start": 1, "end": 2}, "channels": [0, 1], "doc": 7, "confirm": False})
    v.validate({"db": -3, "select": "all", "doc": "/abs/file.wav"})
    with pytest.raises(jsonschema.ValidationError):
        v.validate({"db": "loud"})
    with pytest.raises(jsonschema.ValidationError):
        v.validate({"db": 1, "unexpected": True})  # additionalProperties:false kept
    with pytest.raises(jsonschema.ValidationError):
        v.validate({})


def test_annotations_follow_flags(tools):
    by = {t.command: t for t in tools}
    assert by["app.state"].annotations()["readOnlyHint"] is True
    assert by["effect.changeGain"].annotations()["readOnlyHint"] is False
    assert by["doc.close"].annotations()["destructiveHint"] is True
    assert "confirm_required" in by["doc.close"].description
    assert "job" in by["ai.models.install"].description
