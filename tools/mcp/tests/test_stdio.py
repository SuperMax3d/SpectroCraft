"""`python -m spectrocraft_mcp` over real stdio, as Claude Code runs it."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

import pytest

pytest.importorskip("mcp")

from mcp import ClientSession, StdioServerParameters  # noqa: E402
from mcp.client.stdio import stdio_client  # noqa: E402

pytestmark = pytest.mark.anyio
TOOLS_MCP = Path(__file__).resolve().parent.parent


async def test_stdio_entry_point(fake, control_dir):
    env = {**os.environ, "PYTHONPATH": str(TOOLS_MCP) + os.pathsep + os.environ.get("PYTHONPATH", "")}
    params = StdioServerParameters(command=sys.executable, args=["-m", "spectrocraft_mcp"], env=env)
    async with stdio_client(params) as (r, w):
        async with ClientSession(r, w) as cs:
            init = await cs.initialize()
            assert init.serverInfo.name == "spectrocraft"
            assert init.capabilities.tools is not None and init.capabilities.tools.listChanged is True
            names = {t.name for t in (await cs.list_tools()).tools}
            assert "sc_state" in names and "sc_effect_changeGain" in names  # dynamic tools loaded at list time
            res = await cs.call_tool("sc_state", {})
            assert not res.isError
            assert json.loads(res.content[0].text)["result"]["activeDoc"] == 7
