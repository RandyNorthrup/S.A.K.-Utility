SAK MCP bundle layout
=====================

Expected portable files:

  tools/mcp/win32-mcp-server/win32-mcp-server.exe

Build/package win32-mcp-server as a self-contained Windows executable before
shipping unattended GUI automation. SAK provider manifests already point to this
path and the build copies tools/ into the release directory.

Remote MCP providers do not need local runtime files:

  Microsoft Learn MCP: https://learn.microsoft.com/api/mcp
  Context7 MCP:        https://mcp.context7.com/mcp

Those endpoints still require network access at runtime. Microsoft Learn MCP
does not require authentication. Context7 can be used without an API key for
public documentation lookups.
