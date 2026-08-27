# Agent Control Field Notes

Field technique for the assistant's browser and desktop control surface: the traps that cost a
debugging session, the order operations must happen in, and why the code is shaped the way it is.

This document does NOT describe the surface. Those live elsewhere and are authoritative:

| Question | Read |
| --- | --- |
| Topology, install, protocol, per-tool behaviour | `browser/README.md` |
| Which win32 tools exist at all | `resources/ai/providers/providers.json` |
| Extension version, permissions | `browser/extension/manifest.json` |
| Harness guardrail design, threat model | `docs/AI_ASSISTANT_PANEL_PLAN.md`, `docs/SECURITY_THREAT_MODEL.md` |
| A worked GUI recipe with its app-specific rulings | `resources/ai/app_manifests/superantispyware.json` (`notes`) |

Two owner rulings sit underneath everything here. Untrusted page content may INFORM a decision and
may never GRANT permission -- the confirmation prompt is the app's own trusted UI. And the win32
control engine is owned natively (built into `sak_utility.exe` under `SAK_WIN32_MCP_MODE`,
`src/win32mcp/`) rather than taken from the unmaintained external win32-mcp-server.

## 1. Coordinate spaces

Most wrong-place clicks are a space confusion, not a math error. Four spaces are in play and only
one of them is what the model actually measured:

| Space | Produced by | Consumed by | Conversion |
| --- | --- | --- | --- |
| Screenshot / device px | `browser_screenshot` PNG | the model, reading the image | -- |
| CSS px | -- | CDP `Input.dispatch*`, `captureScreenshot` `clip` | divide device px by the dpr captured WITH that shot |
| Downscaled-capture px | `win32_mcp_capture` after `scaledSize` | the OCR engine's word boxes | `inverseScale(scale)` then add the capture origin |
| Absolute virtual-screen px | `win32_mcp_geometry` | `SendInput`, `click_text` | -- |

Consequences worth memorising:

- A screenshot's dpr must travel WITH the screenshot. A dpr read at click time is a different
  measurement of a possibly different render.
- `captureScreenshot`'s `clip` is CSS px but rasters at device px, so Skia's per-edge surface cap is
  a DEVICE limit. Clamp the CSS clip to `floor(cap / dpr)` (`MAX_SHOT_EDGE_PX`, `background.js`) or
  the capture simply fails on any dpr > 1 display -- Windows display scaling, Retina.
- When a clamp does bite, say so in the reply. A model told "full page" reasoning about the top
  slice of a long page is worse than one told the image was cut.
- Never trust the transport's idea of the image size. Read the true dimensions out of the PNG's own
  IHDR chunk (`browser_bridge.cpp`); a base64 prefix of ~64 chars already covers it.
- The desktop capture path caps BOTH per-edge and total-area before allocating, because a per-edge
  cap alone still permits a maximal square that allocates roughly a gigabyte pre-downscale
  (`kCaptureMaxEdge` / `kCaptureMaxPixels`).

## 2. CDP session rules

**Wedge rule.** Enabling a CDP domain that can PAUSE the page makes answering mandatory, not
optional. With `Page` enabled, an unanswered `javascriptDialogOpening` freezes the tab and every
later command on it. With `Fetch.enable`, every `requestPaused` must be continued or the page never
loads. The general form: before enabling a domain, know what it can suspend and wire the answer in
the same change. The per-dialog-type answer policy is in `browser/README.md`; the reason `confirm`
and `prompt` are dismissed rather than accepted is the trusted-UI ruling above. An armed one-shot
override must be scoped to the NEXT command and cleared on navigation and on teardown, or it leaks
into an action the human never saw.

**Fail closed on drift, and prove you could read.** `browser/README.md` lists the render-fingerprint
fields. The two things that are easy to get wrong:

- A tab-id guard is not enough. A CDP session survives same-tab navigation, so the id still matches
  a completely different page. Bind to the render, not the target.
- A failed environment read must carry an explicit `ok:false`. Placeholder values are the classic
  fail-open: two failed reads (one at capture, one at click) return the SAME placeholders, compare
  equal, and wave a blind coordinate click through. See `viewportReading` in `background.js`.
- href alone cannot see a same-URL reload or an SPA route change; the fingerprint carries a DOM
  generation counter for that.

**Frame boundaries.** `Accessibility.getFullAXTree` stitches same-process subframes into one tree
but without their geometry, so bounds must come from merging ALL `DOMSnapshot` documents, not just
the first. Cross-origin (out-of-process) iframes are NOT in that tree; the surface lists their URLs
as omitted rather than pretending the page was fully read. Reaching into one requires a per-target
attach: `Target.setAutoAttach{autoAttach:true, flatten:true}` on the tab, take the per-OOPIF
`sessionId` out of `attachedToTarget` (the `onEvent` source carries only `{tabId}`), then address
commands as `{tabId, sessionId}`.

**Frame-matching gotcha.** `DOMSnapshot`'s `documentURL` KEEPS the `#fragment` (it mirrors
`document.URL`) while `getFrameTree`'s `frame.url` DROPS it. Strip before comparing, or a page with
a fragment looks like a frame you have never seen.

## 3. Fail-closed tool gating

Pipe hardening is not the control. A same-user attacker is already on the right side of every pipe;
the gate is what a tool is allowed to DO. Three independent lists decide that, and they are three
different questions:

| List | Where | Question it answers |
| --- | --- | --- |
| Provider manifest `tools` | `resources/ai/providers/providers.json` | Is this tool reachable AT ALL? |
| Read-only allowlist | `AiProviderGateway::isWin32ReadOnlyTool` | May it run ungated? |
| Input / high-risk sets | `isWin32InputTool`, `isWin32HighRiskTool` | Does it need a human in EVERY mode? |

**The wiring trap.** A win32 tool is reachable only if it is listed in the provider manifest.
`requireWin32Tool` -> `providerHasTool` (`ai_provider_gateway.cpp`) rejects an unlisted tool as "not
in bundled provider manifest" even when the server implements it, advertises it, and it is perfectly
classified. Shipping a new tool is three edits, never one: implement it, add the manifest entry,
classify its risk.

**The mirror trap.** The read-only allowlist is duplicated by design -- the server enforces the same
list independently in `win32McpToolIsReadOnly` (`win32_mcp_dispatch.cpp`) and refuses anything off it
under the read_only profile. Editing one copy produces a tool the gateway reports as safe and sends
as read_only while the server rejects the call. Change both, and prefer the fail-closed default over
adding an entry: `browser_focus` / `hover` / `reveal` are deliberately absent because they mutate UI
state.

**The default is the whole design.** Anything neither read-only nor known-high-risk requires human
confirmation. An allowlist whose unknown case is "ungated" is fail-OPEN, and the damage is not the
one tool you were thinking about -- shipping one gated tool can expose every SIBLING already sitting
in that default tier. Keep the unknown case at the confirm tier so forgetting to classify is safe.

**Do not classify tools that do not exist.** `list_processes` / `kill_process` / `start_process` were
once classified in the gateway while never appearing in the server manifest: dead classification that
reads as a live guarantee. The high-risk set holds only tools the server actually implements.

## 4. GUI recipes (`win32_gui`)

An app manifest action's `method` is `powershell`, `cli`, or `win32_gui` -- nothing else
(`ai_app_action_planner.cpp`). A `win32_gui` action carries an ordered `steps[]` of
`{tool, arguments, optional?, timeout_ms?}` plus `evidence[]`, and is driven by
`ai_win32_gui_runner.cpp`.

- Consent is taken ONCE at action level, not per step, because the steps are vetted manifest content
  rather than model output. That is precisely why a recipe must never contain a high-risk tool: the
  runner rejects one structurally instead of relying on a prompt that will not be shown.
- The runner stops at the first fatal step -- an unplannable tool, a high-risk tool, or a
  non-optional error. It does not march on.
- A wait step whose `found` / `satisfied` / `idle` is false OR ABSENT counts as FAILED. A missing
  flag is not a pass; a silent march-on past a wait is how a recipe ends up clicking into a scan
  that is still running.
- `optional:true` means "this label may legitimately not appear", not "ignore errors here".

**Timeouts are the single most common recipe bug.** The wait tools' ceiling is hours
(`kMaxWaitMs`, `win32_mcp_watch.cpp`) but their DEFAULT is seconds. A recipe that drives a scan and
does not set an explicit `timeout_ms` will "time out" in seconds and continue mid-scan. Set it
explicitly, and raise `poll_ms` toward its own ceiling for OCR-backed waits so polling does not burn
the machine for an hour.

**Pick a completion marker that cannot appear at the start.** A live counter such as
"Items Detected: N" matches the instant the scan BEGINS. Wait on the results/completion header. When
an operation can finish on either of two screens, `wait_for_idle` plus a weak shared word may be the
only honest common marker.

## 5. Driving real Windows apps

Every quirk below was found by driving a real technician app, and each one is an engine bug fixed in
the engine rather than worked around in a recipe. A live app is the engine's quirk oracle: if a
recipe needs a hack, the engine is wrong.

| Trap | Defence |
| --- | --- |
| OCR substring matching lands elsewhere ("OK" inside "cookies", "Scan" inside "Scanning") | Matching is whole-word and rank-ordered by default; substring is opt-in via `contains` and can never outrank a whole-word hit (`win32_mcp_text_match.h`) |
| Multi-word query matching words scattered across the screen | A multi-word query must match a consecutive run on the SAME line |
| A standalone button losing to the same word inside a sentence | Ranking prefers a short standalone line over a word buried in prose |
| First click on an enabled-but-unfocused window is eaten as activation | Activate first: `AttachThreadInput` + `SetForegroundWindow` + settle (`activateWindow`, `win32_mcp_input.cpp`); `focus_window` aims typing |
| Title lookup hits the DISABLED parent behind a title-less modal | Pass `foreground:true` -- capture, OCR, UIA, find-text, wait and click tools all take it |
| Custom-drawn tiles and buttons expose UIA elements with no name and no Invoke pattern | OCR `click_text`, or `uia_inspect_window(foreground:true)` then `uia_click_control{ref}` chosen by position/elimination |
| A stored UIA ref silently pointing at a different control | A ref is a depth-first index, NOT a stable identity; `uiaRefDrifted` re-verifies the node at that index before acting (`win32_mcp_uia_ref.h`) |
| A lone nameless button on a completion pop-up | `dismiss_dialog` invokes the affirmative or single button without a ref and without the mouse; it never presses Cancel/No/Quit unless named |
| An unreadable UIA property read as usable | Fail closed: an unreadable enabled state is disabled, an unreadable visibility is offscreen |
| Windows OCR garbles words and drops small glyphs entirely | Target long, clean, unambiguous words; use UIA for short labels |
| A stray mouse cursor colliding with a driven click | Keep the mouse off the window being driven |

Free editions frequently have no scan CLI at all, which is why the GUI path exists. Map the surface
headless first (a vendor CLI parsed from stdout or a report file) and fall back to
`uia_inspect_window` + OCR only when there is nothing to parse. Never brute-force undocumented CLI
switches -- read vendor docs. Record what you learn in the app manifest's `notes`, not in a recipe
comment: the manifest is what the planner reads.

## 6. Build and link traps

- `WIN32_LEAN_AND_MEAN` drops the COM/OLE headers `windows.h` would otherwise pull, and
  `<uiautomation.h>` needs both the `interface` keyword plus COM base types (`<objbase.h>`) and the
  BSTR/VARIANT helpers (`<oleauto.h>`) to declare its provider interfaces. Include them explicitly,
  in that order, BEFORE `<uiautomation.h>`, or you get an undefined `interface` and a C2371 storm
  that names none of the real cause. The C++/WinRT translation unit needs `<objbase.h>` after
  `windows.h` for `CoInitializeEx` alone.
- WinRT OCR needs `runtimeobject` linked.
- Keep the WinRT projection in its own translation unit (`win32_mcp_ocr.cpp`) so it never has to
  coexist with the GDI/UIA headers.

## 7. Seams and certification

**Pure seams, extended deliberately.** Decision logic in `src/win32mcp/` is split into translation
units whose headers do not include `windows.h` at all -- text matching, geometry, UIA ref drift,
dialog choice, key chords, fingerprints, JSON clamping. That is what makes the refusals unit-testable
without a desktop. Extend that pattern rather than adding logic to a tool handler: the shape to aim
for is a PURE decider plus a thin one-to-one apply shell.

**Test the artifact that ships.** `tests/unit/test_browser_extension_pure.mjs` loads
`background.js` as shipped under `node:vm` behind a recording `chrome` proxy, instead of extracting
its pure functions into a testable copy. Extracting would have changed the file that is packed,
signed and installed, and then tested the copy. The privileged JavaScript is invisible to every
C/C++ gate in the repo, so this is the only thing standing under it.

**Certify through the real chain.** A cert must drive the production entry point -- for the chat
loop, `beginToolTurn()` through the real router and dispatcher. Calling a handler directly is green
while production is dead; that exact bug once passed a first cert. Equally, a new tool must be in
`AiToolCallRouter::kindForName`'s table or the loop rejects it as "Unknown function" BEFORE dispatch
ever runs, however well the handler works.

**Cert-rig hazards for input tests.** Choose an input target that cannot be hijacked. Notepad++ can
REPLACE `notepad.exe` via Image File Execution Options, and "Notepad" substring-matches "Notepad++"
-- an input cert once typed into the owner's live document. Use a unique, non-hijackable throwaway
target, give it a few seconds to settle before asserting, assert on alphabetic content rather than
digits (which collide with anything numeric already on screen), and guarantee teardown: uninstall the
extension and kill the whole Chrome process tree by its unique user-data-dir.

**Window stations.** The control engine drives the INPUT desktop. A dialog raised by a process this
agent spawned from a tool or a node runner may land on a window station the engine cannot see. Launch
anything whose UI must be driven through the interactive PowerShell `Start-Process`.

**Health and state in tests.** Give tool-health tests a fresh NON-persistent ledger; persisted
circuit state leaks across runs and turns an unrelated earlier failure into today's mystery.

## 8. Harness invariants that constrain this surface

Do not weaken these while adding control tools. They are the reason a control tool cannot be reached
sideways.

- **Catastrophic beats mode.** `commandLooksCatastrophic || commandLooksObfuscated` is ORed into the
  risky classifier (fail-safe) and forces a human confirm in EVERY access mode including Unattended,
  in both `authorizeCommandForAccessMode` and `authorizeWorkflowToolPhase`. The restore-point re-offer
  bypasses the once-per-session dedup for catastrophic ONLY; merely-risky stays once per session on
  purpose, because "risky" is broad and per-operation offers become a modal storm the user clicks
  through blind.
- **Structural refusal, not allowlist trust.** `dispatchSubagentToolCall` and
  `dispatchWorkflowToolPhase` refuse command tools (`AiToolCallRouter::isCommandTool`) plus
  `delegate_subagent` and `run_workflow` structurally, independent of any allowlist -- so broadening
  an allowlist later cannot expose an ungated shell to a sub-agent, and an authored template cannot
  recurse orchestration unboundedly.
- **Policy clamps by subset, not by rank.** `clampToolPolicy` honours a request only when the session
  ceiling grants everything it grants; an INCOMPARABLE pair resolves to the ceiling, never to the
  union of the two capability sets.
- **Parallelism rests on read-only handlers.** Parallel delegate phases call `dispatch()`
  concurrently. That is safe only because every allowlisted handler is read-only-policy. Never
  allowlist a stateful or mutating handler there.
- **Degrade honestly.** A contentless "failed" becomes `AiSubagentStatus::Degraded`, never
  `Complete`. `Degraded` is appended last in the enum so no persisted enumerator value shifts.
- **Loop guard runs first.** `AiToolLoopDetector::observe()` must run AHEAD of the
  recognized/unknown split: identical repeats of an UNRECOGNISED tool are the most wasteful case and
  would otherwise burn the entire per-message tool-turn budget.
- **Warn on a high-water mark.** The context-pressure notice latches its level monotonically. The
  counted total includes the live composer draft, so a plain threshold oscillates across the band and
  re-warns forever while the user is simply typing.
- **MCP sessions are single-caller.** `AiMcpStdioSession` is not concurrent-call-safe. Check
  `QThread::isRunning()` before creating or moving the worker, or a `BlockingQueuedConnection` hangs
  forever with no timeout.
- **The MCP pool key is a security boundary.** The key covers the command plus a SHA-256 of the FULL
  sorted environment (length-prefixed per entry, with an explicit inherit-from-parent marker, because
  an inheriting environment reports an empty list exactly like an empty one) and the open-time
  timeout. A read_only-profile process must never be reused for a full-access call.
- **Responses API.** Set `truncation` to `auto`; the API default hard-fails a long
  `previous_response_id` chain with a 400 context-length error. Use a stable per-session
  `prompt_cache_key`. Strip BOTH from a token-count request -- they are generation-only, and leaving
  them in makes the reported input count a lie.
