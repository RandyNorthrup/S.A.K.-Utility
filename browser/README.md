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

## Installing (headless, no "Load unpacked")

Modern stable Chrome ignores `--load-extension` and blocks off-store sideloading, so
the app installs the extension the enterprise way: it force-installs a signed CRX via
Chrome policy in the current user's registry (HKCU, no admin) and registers the native
messaging host in one step. This is version-independent (the policy path and CRX3 format
are stable across Chrome builds) and reversible.

- `dist/sak_browser_control.crx` -- the committed, signed extension package. Its RSA
  public key is pinned in `extension/manifest.json` ("key"), so the unpacked-load id and
  the CRX id are identical: `ofodhfbipljnhenjjjpbdaglkjdphoec`.
- `BrowserExtensionInstaller` (`src/win32mcp/browser_extension_installer.*`) writes an
  Omaha `update.xml` + the host manifest under `%LOCALAPPDATA%\SAK`, adds our single
  `ExtensionInstallForcelist` entry (never touching other policies), and registers the
  native host. `uninstall()` removes only our entry; `state()` reports installed/partial/
  not_installed/error. It is unit-tested against a throwaway HKCU key in
  `test_browser_extension_installer`.
- The assistant panel exposes Install/Uninstall, and the assistant has the
  `browser_extension_install` / `browser_extension_uninstall` / `browser_extension_status`
  tools (install/uninstall require a human confirmation in every access mode).
- `pack-extension.ps1` re-signs the CRX after any change under `extension/`. The private
  signing key is never committed; it lives at
  `%LOCALAPPDATA%\SAK\keys\sak_browser_control.signing-key.pem` and MUST be backed up
  (losing it changes the id and forces every machine to re-install).

The manual `native-host/register_native_host.ps1` + "Load unpacked" flow below remains
for developing an unpacked build.

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
`navigate` / `back` / `forward` / `reload`, `listTabs` / `selectTab` / `newTab` /
`closeTab`, the input actions (`click` / `type` / `pressKey` / `scroll`, unit 7), and
`screenshot` (a PNG of the active tab, unit 8).

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

`browser_click_at` (unit 9) is the coordinate fallback for targets with no ref (canvas,
image maps): it clicks at pixel coordinates the model read from the most recent
`browser_screenshot`. Those coordinates are screenshot (device) pixels, so the extension
divides them by the dpr captured WITH that screenshot before dispatching the CSS-pixel CDP
click. The screenshot is bound to a render fingerprint (tab, dpr, scroll offset, document
URL, and whether it was full-page); `browser_click_at` re-reads that fingerprint at click
time and fails closed if anything moved -- a different tab, a dpr/zoom change, a scroll, a
navigation, or a full-page (document-coordinate) screenshot -- asking for a fresh viewport
screenshot rather than clicking stale coordinates. It is an input tool and takes the same
hard confirmation gate as the other input actions.

Confirmation policy: every input action requires an explicit human confirmation in the
app in EVERY non-chat access mode, including Unattended (where read-only browser tools and
navigation may auto-run). The prompt is the app's trusted UI -- untrusted page content can
never grant permission. The classifier fails CLOSED: any win32 tool not on the read-only
allowlist requires confirmation, so a new automation tool cannot silently act as the user.

## Dialogs (unit 10)

A JavaScript dialog (`alert` / `confirm` / `prompt` / `beforeunload`) pauses the page until
CDP answers it, so with the Page domain attached the extension must respond or every later
command on the tab would wedge. An `onEvent` handler answers automatically by type: `alert`
and `beforeunload` are accepted (an alert only has OK; accepting `beforeunload` lets a
navigation the automation is driving proceed), while `confirm` and `prompt` are dismissed so
a page can never auto-confirm a destructive action. `browser_dialog` arms the response for
the NEXT dialog (one-shot) -- call it with `action:"accept"` (and optional `text` for a
prompt) just before the step that opens a `confirm`/`prompt` you want accepted -- and
reports the last dialog handled. It is gated as an input tool, so accepting a dialog takes
the same hard confirmation as the other input actions.

## Screenshot (unit 8)

`browser_screenshot` captures a PNG of the active tab via CDP `Page.captureScreenshot`
(`full_page:true` clips to the document height, capped at the Skia edge limit). It
rasterizes at the browser level -- no `getUserMedia` / display-capture prompt and no OS
screen grab, so it sees only the tab, never other windows or the desktop. It is classified
read-only (ungated). The PNG returns as an MCP `image` content block (not a base64 blob in
text); the bridge caps the payload size and fails closed on an empty capture.
