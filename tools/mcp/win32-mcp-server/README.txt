Portable win32-mcp-server bundle
================================

Executable:
  tools/mcp/win32-mcp-server/win32-mcp-server.exe

Source:
  local source checkout selected by scripts/bundle_mcp_servers.ps1

Smoke:
  2.6.1

SAK uses this stdio MCP server for manifest-gated Windows desktop observation
and automation. It is not a general permission bypass: mutating actions must
be routed through SAK policy and app-control manifests.
