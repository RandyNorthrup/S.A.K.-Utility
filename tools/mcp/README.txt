SAK MCP bundle layout
=====================

The Win32 desktop and browser automation MCP server is built into the app binary
(sak_utility.exe run in a headless stdio mode), not a separately bundled tool, so
there are no local runtime files to place here for it. The provider manifest
(data/ai/providers/providers.json) points the win32_mcp provider at sak_utility.exe.

Remote MCP providers do not need local runtime files:

  Microsoft Learn MCP: https://learn.microsoft.com/api/mcp
  Context7 MCP:        https://mcp.context7.com/mcp

Those endpoints still require network access at runtime. Microsoft Learn MCP
does not require authentication. Context7 can be used without an API key for
public documentation lookups.
