"""SpectroCraft MCP adapter.

``discovery``, ``client``, ``protocol``, ``dynamic``, ``pipe_probe`` and
``launcher`` use the standard library only; ``server`` needs the ``mcp`` SDK.
Do not import ``server`` from here: ``sc_client.py`` must work without ``mcp``.
"""

__version__ = "0.1.0"
