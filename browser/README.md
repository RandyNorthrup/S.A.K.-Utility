# S.A.K. Utility Browser Control

The assistant's browser-control surface, modeled on how OpenAI Codex drives
Chrome: the model works off a filtered DOM (interactable elements with stable
ids) and acts on elements by id, not screen pixels. Actions are injected at the
DevTools-protocol level, so the user's own mouse and keyboard are never taken
over -- the agent gets its own cursor.

## Pieces

- `extension/` -- the Chrome MV3 extension. It runs in the user's own Chrome
  profile (so it inherits their logins), connects to the native host over Chrome
  native messaging, and (in later units) attaches the DevTools protocol to the
  active tab and draws the agent cursor.
- `native-host/` -- the native messaging host manifest and a registration
  script. The host itself is the `sak_win32_mcp` executable: when Chrome launches
  it with the calling extension's `chrome-extension://<id>/` origin as an
  argument, it runs in native-host mode (length-prefixed frames) instead of its
  MCP JSON-RPC mode.

The message framing, the DOM/tool contract, the pipe transport, and the relay are
implemented and unit-tested in C++ under `src/win32mcp/` (`native_messaging`,
`browser_contract`, `browser_bridge`, `browser_bridge_pipe`, `browser_bridge_relay`,
`browser_control`). The relay <-> pipe <-> session loop is covered end to end by
`test_browser_bridge_relay` (a fake extension). This extension is the real other end.

## Topology

Two processes speak on each side of a hardened named pipe:

- The long-lived `sak_win32_mcp` (the assistant's MCP server) owns the browser
  authority: it creates the pipe, publishes a rendezvous record, and turns a
  `browser_*` tool call into a command frame.
- Chrome launches a SECOND `sak_win32_mcp` in relay mode when the extension opens the
  native port. The relay discovers the pipe from the rendezvous record, verifies the
  server's code identity, and pumps frames between Chrome and the pipe.

The extension answers commands off the active tab: `snapshot` (accessibility roles +
names joined with DOM-snapshot geometry by backendNodeId), `read` (text/html),
`navigate` / `back` / `forward` / `reload`, and `listTabs` / `selectTab` / `newTab` /
`closeTab`. Input actions and screenshots are later units.

## Protocol

Strict request/reply. For every `{type:"command", id, cmd, ...}` frame the extension
replies exactly once with `{type:"result", id, cmd, payload}` or
`{type:"error", id, cmd, error}`, and never sends an unsolicited frame. The relay's
`{type:"bridge_ready"|"bridge_unavailable"}` frames are informational. The extension
(`BRIDGE_PROTOCOL` in `background.js`) and the native side (`kBrowserBridgeProtocol` in
`native_messaging.h`) must agree; a mismatch is surfaced in the extension health state.

## Try the read path (unit 6)

1. Build the host: `cmake --build build --target sak_win32_mcp --config Debug`.
2. Load the extension: `chrome://extensions` -> enable Developer mode -> "Load
   unpacked" -> select `browser/extension`. Copy the extension id it shows.
3. Register the host (PowerShell):
   `./browser/native-host/register_native_host.ps1 -ExtensionId <id>`
   (add `-ExePath` for a non-Debug build).
4. Start the MCP server so the pipe + rendezvous exist (normally the app does this; for a
   standalone smoke, run `build/Debug/sak_win32_mcp.exe` and leave it reading stdin).
5. Reload the extension. Open its service-worker console (Inspect views on
   `chrome://extensions`) and confirm `[SAK] bridge ready, protocol 1`.
6. Drive a snapshot: paste one JSON-RPC line into the server's stdin (from step 4):
   `{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"browser_snapshot","arguments":{}}}`
   The reply's text is the filtered DOM outline of your active tab (roles, names, and
   `[ref=eN]` handles). `browser_read` and `browser_navigate` work the same way.

To remove the host registration:
`./browser/native-host/register_native_host.ps1 -Unregister`.

## Input (unit 7)

The assistant can also act on the page: `browser_click`, `browser_type`,
`browser_press_key`, and `browser_scroll`. These are injected at the DevTools-protocol
level (CDP `Input`), so the user's real mouse and keyboard are never taken over, and the
agent draws its OWN cursor on the page (a small blue dot) so the user can see where it is
acting. Targets come only from the latest snapshot's `[ref=eN]` handles (validated by the
bridge), never from page content; `browser_type` requires a ref so page-controlled focus
cannot redirect typed text.

Confirmation policy: every input action requires an explicit human confirmation in the
app in EVERY non-chat access mode, including Unattended (where read-only browser tools and
navigation may auto-run). The prompt is the app's trusted UI -- untrusted page content can
never grant permission. The classifier fails CLOSED: any win32 tool not on the read-only
allowlist requires confirmation, so a new automation tool cannot silently act as the user.
