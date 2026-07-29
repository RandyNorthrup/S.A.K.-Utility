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

The message framing, the host message handler, and the model-facing DOM/tool
contract are implemented and unit-tested in C++ under `src/win32mcp/`
(`native_messaging`, `win32_mcp_native_host`, `browser_contract`).

## Try the handshake (unit 3)

1. Build the host: `cmake --build build --target sak_win32_mcp --config Debug`.
2. Load the extension: `chrome://extensions` -> enable Developer mode ->
   "Load unpacked" -> select `browser/extension`. Copy the extension id it
   shows.
3. Register the host (PowerShell):
   `./browser/native-host/register_native_host.ps1 -ExtensionId <id>`
   (add `-ExePath` for a non-Debug build).
4. Reload the extension, then click the "S.A.K. Browser Control" toolbar button.
   Open the extension's service-worker console (Inspect views on
   `chrome://extensions`) and look for `[SAK] host alive: sak-win32-mcp ...`.

To remove the host registration:
`./browser/native-host/register_native_host.ps1 -Unregister`.

## Protocol version

The extension (`BRIDGE_PROTOCOL` in `background.js`) and the native host
(`kBrowserBridgeProtocol` in `native_messaging.h`) must agree. A `pong` reports
the host's protocol; a mismatch is surfaced in the extension health state.
