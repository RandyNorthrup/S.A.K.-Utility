// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// S.A.K. Utility Browser Control -- background service worker.
//
// Unit 6 scope: the DOM READ path. The extension connects to the native host over
// Chrome native messaging (the sak_win32_mcp relay), waits for the bridge to come up,
// then answers command frames forwarded from the S.A.K. assistant:
//   - snapshot  : attach the DevTools protocol to the active tab and build the filtered
//                 DOM capture the model reads (roles/names from the accessibility tree,
//                 geometry from a DOM snapshot, joined by backendNodeId).
//   - read      : return the active tab's text or html.
//   - navigate / back / forward / reload : drive history via chrome.tabs.
//   - listTabs / selectTab / newTab / closeTab : tab management via chrome.tabs.
//   - click / type / pressKey / scroll : input injected over CDP Input (unit 7).
//   - screenshot : a PNG of the active tab via CDP Page.captureScreenshot (unit 8).
//   - clickAt : coordinate click at screenshot pixels, dpr-converted to CSS px (unit 9).
//   - dialog : arm the next JS dialog's response + report the last one (unit 10); dialogs
//              are auto-answered by an onEvent handler so the page never wedges.
//
// PROTOCOL: the native host is a thin relay in strict request/reply. For every
// {type:"command", id, cmd, ...} frame this worker replies EXACTLY ONCE with
// {type:"result", id, cmd, payload} or {type:"error", id, cmd, error}. It NEVER sends
// an unsolicited frame (that would desync the relay's one-op pump). The relay's own
// {type:"bridge_ready"|"bridge_unavailable"} frames are informational. A {type:"cancel"} frame
// retires whatever command is in flight -- the polling handlers stop and that command answers
// with its own error -- and is itself never replied to, so the pump stays in step.
//
// SECURITY: page content is untrusted DATA. The worker never eval()s page-controlled
// strings, never lets a page decide what is permitted, and only navigates to http(s).
// The real control against a hostile page is the assistant-side confirmation gate.

const HOST_NAME = "com.sak.browsercontrol";

// Bridge protocol version the extension speaks; must match kBrowserBridgeProtocol on
// the native side. A mismatch is surfaced rather than silently tolerated.
const BRIDGE_PROTOCOL = 1;

// The CDP roles we treat as actionable (get a ref the model can act on). Kept lower-cased
// for a case-insensitive match against the accessibility tree's role values.
const INTERACTABLE_ROLES = new Set([
  "button", "link", "textbox", "searchbox", "checkbox", "radio", "combobox",
  "listbox", "menuitem", "menuitemcheckbox", "menuitemradio", "option", "tab",
  "switch", "slider", "spinbutton", "treeitem", "gridcell", "scrollbar",
]);

// Roles that are pure structure -- not interactable on their own even when focusable.
const STRUCTURAL_ROLES = new Set([
  "generic", "none", "presentation", "group", "genericcontainer",
]);

// Hard caps so a hostile or enormous page cannot blow up the worker or the frame.
const MAX_CAPTURE_NODES = 2000;
const MAX_READ_CHARS = 200000;
const NAV_TIMEOUT_MS = 15000;
const DEFAULT_SCROLL_PX = 400;
// How many omitted cross-origin frame URLs a reply LISTS; a hostile page could otherwise spawn
// thousands of iframes. A page hiding fifty frames must not read as one hiding twenty, so when the
// cap bites the reply carries omittedFramesTruncated alongside the list. That flag is what the
// renderer reports, rather than inferring truncation from the array's length: a length convention
// silently couples this constant to the renderer's own cap, and raising either one alone would
// turn a cut list back into an apparently complete one.
const MAX_OMITTED_FRAMES = 20;
// How much of a control's live value the accessibility capture carries. A value longer than
// this is cut and flagged with value_truncated rather than silently shortened, so a clipped
// entry is never mistaken for the control's full contents.
const AX_VALUE_MAX_CHARS = 80;
// Skia caps a single raster surface at 16384 DEVICE px per edge; a full-page clip past
// that fails the capture. The clip is expressed in CSS px and rasterized at the display's
// devicePixelRatio, so the CSS clip is clamped to this cap divided by dpr (below).
const MAX_SHOT_EDGE_PX = 16384;
// The largest base64 image/PDF payload a reply may carry. These mirror kMaxScreenshotBase64 and
// kMaxPdfBase64 on the bridge side: past them the bridge refuses the payload, and past the
// relay's own frame cap the frame does not parse at all and the connection is torn down -- the
// caller then sees a transport reset rather than a size error it could act on. Refusing here
// keeps an oversized capture an honest, recoverable error.
const MAX_SHOT_BASE64 = 16 * 1024 * 1024;
const MAX_PDF_BASE64 = 24 * 1024 * 1024;

// CDP Input.dispatchKeyEvent modifier bitmask (Alt=1, Control=2, Meta=4, Shift=8).
// These tables are indexed by a caller-supplied token, so they carry NO prototype: a plain object
// literal answers "constructor"/"toString"/"valueOf" with an inherited member, and a lookup that
// treats a truthy hit as a real definition would accept a modifier or key that was never declared.
const MODIFIER_BITS = Object.assign(Object.create(null),
  { alt: 1, control: 2, ctrl: 2, meta: 4, command: 4, cmd: 4, shift: 8 });
// Named non-printable keys the model may press, mapped to their DOM code + legacy
// keyCode (needed so browser shortcuts like Control+A actually register).
const KEY_DEFS = Object.assign(Object.create(null), {
  enter: { key: "Enter", code: "Enter", keyCode: 13, text: "\r" },
  tab: { key: "Tab", code: "Tab", keyCode: 9 },
  escape: { key: "Escape", code: "Escape", keyCode: 27 },
  esc: { key: "Escape", code: "Escape", keyCode: 27 },
  backspace: { key: "Backspace", code: "Backspace", keyCode: 8 },
  delete: { key: "Delete", code: "Delete", keyCode: 46 },
  space: { key: " ", code: "Space", keyCode: 32, text: " " },
  arrowup: { key: "ArrowUp", code: "ArrowUp", keyCode: 38 },
  arrowdown: { key: "ArrowDown", code: "ArrowDown", keyCode: 40 },
  arrowleft: { key: "ArrowLeft", code: "ArrowLeft", keyCode: 37 },
  arrowright: { key: "ArrowRight", code: "ArrowRight", keyCode: 39 },
  home: { key: "Home", code: "Home", keyCode: 36 },
  end: { key: "End", code: "End", keyCode: 35 },
  pageup: { key: "PageUp", code: "PageUp", keyCode: 33 },
  pagedown: { key: "PageDown", code: "PageDown", keyCode: 34 },
});
// OEM punctuation -> [DOM code, Windows virtual key code]. ASCII charCodeAt is NOT the
// virtual key (e.g. '.' is 46 = VK_DELETE), so a chord like Control+/ needs the real VK.
const OEM_KEYS = Object.assign(Object.create(null), {
  ";": ["Semicolon", 186], ":": ["Semicolon", 186],
  "=": ["Equal", 187], "+": ["Equal", 187],
  ",": ["Comma", 188], "<": ["Comma", 188],
  "-": ["Minus", 189], "_": ["Minus", 189],
  ".": ["Period", 190], ">": ["Period", 190],
  "/": ["Slash", 191], "?": ["Slash", 191],
  "`": ["Backquote", 192], "~": ["Backquote", 192],
  "[": ["BracketLeft", 219], "{": ["BracketLeft", 219],
  "\\": ["Backslash", 220], "|": ["Backslash", 220],
  "]": ["BracketRight", 221], "}": ["BracketRight", 221],
  "'": ["Quote", 222], '"': ["Quote", 222],
});
// The character a printable key produces while Shift is HELD (US layout). A chord dispatches the
// unshifted token as its `text` otherwise, so "Shift+1" would insert "1" where the caller asked
// for "!" -- the wrong character, reported as the chord that was sent.
const SHIFTED_KEYS = Object.assign(Object.create(null), {
  "1": "!", "2": "@", "3": "#", "4": "$", "5": "%",
  "6": "^", "7": "&", "8": "*", "9": "(", "0": ")",
  ";": ":", "=": "+", ",": "<", "-": "_", ".": ">", "/": "?",
  "`": "~", "[": "{", "\\": "|", "]": "}", "'": '"',
});
// The assistant's on-page CONTROL PRESENCE: a page-injected overlay so the user SEES that the
// assistant is driving this tab and where its pointer is -- distinct from their untouched OS
// cursor. It is a prominent neon-pink pointer (with a pulsing halo), a viewport frame, and an
// "AI CONTROL" badge. Cosmetic only (never a security boundary): a hostile page can hide it but
// cannot use it to drive input. It is auto-hidden while a screenshot is captured so it never
// bleeds into the image the model reads (and does not obscure page media in a capture).
const AGENT_CURSOR_ID = "__sak_agent_cursor__";
const CONTROL_STYLE_ID = "__sak_control_style__";
const CONTROL_FRAME_ID = "__sak_control_frame__";
const CONTROL_BADGE_ID = "__sak_control_badge__";
// The ids the presence overlay owns, for bulk hide/remove.
const PRESENCE_IDS = [CONTROL_STYLE_ID, CONTROL_FRAME_ID, CONTROL_BADGE_ID, AGENT_CURSOR_ID];
// Where the parked cursor sits until the first real pointer action moves it, and the last point
// it moved to (so a presence refresh after a navigation re-parks it where it was).
let lastCursorPoint = { x: 24, y: 24 };
// Live network-activity tracking for browser_wait_for network_idle: the IDS of the in-flight
// requests on the attached tab and the timestamp of the last request start/finish. Fed by the
// CDP Network domain events; reset on navigation/detach so a stalled request cannot wedge idle.
// Ids rather than a counter, because a redirect re-fires Network.requestWillBeSent for the SAME
// requestId and still reports only one completion: counting events ratchets the total upward, and
// a single redirected subresource would keep network_idle from ever holding again.
const inflightRequests = new Set();
let lastNetworkActivityMs = 0;
// True only while the Network domain is actually instrumenting the attached tab. The idle
// predicate is read entirely off the events above, so without them it reports a quiet page having
// observed nothing at all -- a statement about the page made from no evidence.
let networkInstrumented = false;
// The tab's real User-Agent, captured before any browser_emulate override so reset can restore
// it. Null until first captured; cleared on detach (a new session recaptures its own).
let originalUserAgent = null;
// Credentials armed for HTTP auth challenges via browser_http_auth. Null = disarmed (no Fetch
// interception). When set, it is { username, password, origin } and the Fetch domain is enabled;
// the onEvent handler answers an auth challenge with these ONLY when the challenge origin matches
// the armed origin. Cleared on detach and on top-frame navigation (Fetch is per-session, so it
// auto-disables on detach too).
let httpAuthCreds = null;
// The last failure answering a request Fetch paused, or null when none has happened this session.
// A urlPattern:"*" interception pauses EVERY request, so a continue/continueWithAuth that never
// lands leaves that resource stalled and the page hanging on it. The handler is an event callback
// with no caller to fail, so the failure is recorded instead of discarded: browser_http_auth
// reports it back, which is the only way a stalled interception is visible at all.
let lastFetchError = null;

// True when two origins are the same scheme://host:port. Both inputs are normalized through URL
// so "https://host" and "https://host:443" compare equal; falls back to strict string equality
// if either is not a parseable absolute origin (in which case a non-match fails closed).
function originsMatch(a, b) {
  if (!a || !b) {
    return false;
  }
  try {
    return new URL(a).origin === new URL(b).origin;
  } catch (_e) {
    return a === b;
  }
}

// The real origin of a document URL, or null when it has none to compare (unparseable, or an
// opaque origin such as file:/sandboxed, which serializes to the string "null" and would
// otherwise compare EQUAL to every other opaque origin).
function originOf(url) {
  try {
    const origin = new URL(url).origin;
    return origin && origin !== "null" ? origin : null;
  } catch (_e) {
    return null;
  }
}

let port = null;
let health = { connected: false, bridge: null, error: null };
// True only while the host has completed the bridge_ready handshake AND declared the exact
// protocol this worker speaks. Command frames are privileged (input injection, cookies, web
// storage, permissions), so they run only against a bridge that announced itself and agrees on
// the frame shape: a detected protocol skew has to STOP something to be a check at all, and an
// unannounced host must not get a command executed on its say-so. Cleared on
// bridge_unavailable and on port loss.
let bridgeReady = false;
let attachedTabId = null;
// A monotonic DOM-generation counter stamped on every reply (domEpoch). It increments whenever
// the DOM the bridge's ref_index was captured against goes away out from under us -- a top-frame
// navigation, an in-document (SPA) route change, or a CDP detach (tab close / DevTools). The
// bridge compares it across replies and invalidates element refs when it moves, so an external
// navigation cannot leave a stale ref addressable. (The relay is strict request/reply, so this
// rides on normal replies rather than an unsolicited event.)
let domEpoch = 0;
// The tab whose nodes populated the current ref_index. An element ref (backendNodeId) is
// only valid against THIS tab: if the active tab changed since the snapshot, applying the
// ref elsewhere could click/type a node the user never saw, so ref actions refuse.
let lastSnapshotTabId = null;
// The domEpoch the snapshot above was captured at. The tab id alone does not pin a ref: the
// SAME tab can navigate under us, and Blink allocates backendNodeIds from a per-renderer
// counter, so after a cross-site navigation a live id names an arbitrary node of the NEW
// document. The bridge's epoch check is post-hoc by construction -- it reads the marker off
// the REPLY, i.e. after the click already landed -- so the generation is pinned here too and
// checked BEFORE dispatching.
let lastSnapshotEpoch = null;
// Fingerprint of the render the most recent screenshot captured: {tabId, fullPage, dpr,
// scrollX, scrollY, href}. A coordinate click (browser_click_at) is meaningful only against
// that exact image, so it converts device->CSS with THIS dpr (not a re-read) and refuses if
// the tab, dpr, scroll, or document changed since -- a full-page shot is document-space, not
// click-space. null when no valid screenshot is outstanding.
let lastShot = null;
// The tab listing browser_tabs last returned: the window it was taken from and the url/title at
// each index. A tab index is POSITIONAL -- it shifts whenever a tab opens, closes, or moves, and
// "the last focused window" can resolve to a DIFFERENT window than the one that was listed -- so
// an index names a tab only while it still names the tab the model read at that index. Null when
// no listing is outstanding (nothing was listed, or an action shifted the positions).
let lastTabListing = null;
// The window ids browser_windows last reported (and the ones this session opened itself), as a
// Set. A window id is a bare small integer with no shape of its own, so a stale id from an earlier
// listing -- or one the model simply guessed -- names a live window just as well as a real one,
// and focus/close would then act on a window the operator never saw listed. Null until a listing
// is taken.
let lastWindowListing = null;
// A one-shot response armed for the NEXT JavaScript dialog by browser_dialog, e.g.
// {accept:true, text:"..."}. It is scoped to the single command that follows the arm (see
// runCommand): consumed if that command's action opens a dialog, otherwise dropped when the
// command finishes -- so an armed "accept" can never linger and auto-confirm an unrelated
// later dialog. Also cleared on navigation and teardown.
let pendingDialogPolicy = null;
// True only while a command that could open a dialog is executing. Clearing the arm when that
// command finishes still leaves it live through the gap BEFORE the command arrives -- an
// arbitrary idle window in which a page's own setTimeout confirm() consumes the accept meant for
// the next tool call, and the dialog the operator armed for then gets the safe default instead.
// The arm is only visible to the handler while a dispatch is in flight.
let dialogArmActive = false;
// The most recent dialog the extension handled: {type, message, accepted}. Reported by
// browser_dialog so the model can see what an auto-dismissed alert/confirm said.
let lastDialog = null;
// Monotonic counter of command frames accepted. A handler that polls (browser_wait_for) holds
// the generation it started under; the moment a newer command arrives -- or the session is torn
// down -- its generation is stale and the loop must abandon rather than keep driving the page
// and eventually post a reply into a relay that has already been reset.
let commandGeneration = 0;
let reconnectTimer = null;

// -- Native messaging port ---------------------------------------------------

function scheduleReconnect() {
  if (reconnectTimer) {
    return;
  }
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connect();
  }, 2000);
}

function onDisconnect() {
  const err = chrome.runtime.lastError;
  health.connected = false;
  health.error = err ? err.message : "port closed";
  bridgeReady = false;  // the next host must handshake again before it can command us
  port = null;
  console.warn("[SAK] host disconnected:", health.error);
  // The bridge (and thus any CDP session it drove) is gone; drop our attachment.
  detachAll("bridge disconnected");
  scheduleReconnect();
}

function connect() {
  if (port) {
    return port;
  }
  try {
    port = chrome.runtime.connectNative(HOST_NAME);
  } catch (e) {
    health.connected = false;
    health.error = String(e);
    console.error("[SAK] connectNative threw:", e);
    scheduleReconnect();
    return null;
  }
  port.onMessage.addListener(onHostMessage);
  port.onDisconnect.addListener(onDisconnect);
  return port;
}

function send(reply) {
  if (!port) {
    console.warn("[SAK] no port to reply on; dropping", reply && reply.id);
    return;
  }
  try {
    port.postMessage(reply);
  } catch (e) {
    // The reply IS the exchange: a frame that never reaches the relay leaves the app waiting out
    // its whole I/O deadline and then reporting a transport reset for a delivery failure already
    // known here. Posting a second frame down the same port cannot work either, so the port is
    // torn down at once -- the app fails in milliseconds with the connection gone, and the next
    // command arrives only after a fresh handshake.
    console.error("[SAK] postMessage threw:", e);
    const dead = port;
    port = null;
    try {
      dead.disconnect();
    } catch (_e) {
      // Already gone; the local state below is what matters.
    }
    health.connected = false;
    health.error = "reply could not be delivered: " + (e && e.message ? e.message : String(e));
    bridgeReady = false;  // a reconnected host must handshake again before it can command us
    detachAll("reply delivery failed");
    scheduleReconnect();
  }
}

// -- Frame dispatch ----------------------------------------------------------

function onHostMessage(msg) {
  if (!msg || typeof msg !== "object") {
    return;
  }
  if (msg.type === "bridge_ready") {
    health.connected = true;
    health.bridge = "ready";
    health.error = null;
    bridgeReady = msg.protocol === BRIDGE_PROTOCOL;
    if (!bridgeReady) {
      health.error =
        "Bridge protocol mismatch: host " + msg.protocol + ", extension " + BRIDGE_PROTOCOL;
    }
    console.info("[SAK] bridge ready, protocol", msg.protocol);
    return;
  }
  if (msg.type === "bridge_unavailable") {
    health.connected = false;
    health.bridge = "unavailable";
    health.error = msg.error || "bridge unavailable";
    bridgeReady = false;
    console.warn("[SAK] bridge unavailable:", health.error);
    return;
  }
  if (msg.type === "cancel") {
    // The host abandoning an exchange (its own I/O deadline elapsed) has no other way to stop a
    // handler that POLLS: browser_wait_for and browser_download keep driving the browser for as
    // long as their own timeout allows, then post a reply into a relay that has already been
    // reset. Retiring the generation is the signal those loops watch; the command they belong to
    // then fails with its own error, so the one-reply-per-command rule still holds. A cancel is
    // not a command frame and is never replied to.
    commandGeneration++;
    return;
  }
  if (msg.type === "command") {
    // A command frame has to be addressable and dispatchable before any of it runs: the id is
    // what correlates the single reply, the cmd is what selects the handler, and neither can be
    // inferred from the rest of the frame. A frame with no usable id cannot even be told so.
    if (typeof msg.id !== "string" || msg.id.length === 0) {
      console.warn("[SAK] command frame with no usable id; dropping");
      return;
    }
    if (typeof msg.cmd !== "string" || msg.cmd.length === 0) {
      send({ type: "error", id: msg.id, cmd: "",
             error: "The command frame carries no command name.", domEpoch });
      return;
    }
    // The relay writes bridge_ready before it pumps a single command, so a command that
    // arrives without one comes from a host that never handshook -- or from one whose
    // protocol we already know we do not speak. Refuse it (still exactly one reply per
    // command frame, so the relay's one-op pump stays in step).
    if (!bridgeReady) {
      send({ type: "error", id: msg.id, cmd: msg.cmd,
             error: health.error || "The bridge has not completed its readiness handshake.",
             domEpoch });
      return;
    }
    handleCommand(msg);
    return;
  }
  console.warn("[SAK] unexpected frame type:", msg.type);
}

async function handleCommand(msg) {
  const id = msg.id;
  const cmd = msg.cmd;
  // Every arriving frame retires whatever came before it. A polling handler captures this value
  // and re-reads it each iteration, so a loop whose command is no longer the live one stops
  // instead of running on against a page (and a relay) that has moved on without it.
  commandGeneration++;
  try {
    const payload = await runCommand(cmd, msg);
    send({ type: "result", id, cmd, payload, domEpoch });
  } catch (e) {
    send({ type: "error", id, cmd, error: e && e.message ? e.message : String(e), domEpoch });
  }
}

async function runCommand(cmd, args) {
  // Scope an armed dialog response to the SINGLE command meant to trigger the dialog: if that
  // command runs without the dialog firing (which would consume the arm in the onEvent
  // handler), drop the arm so it can never persist and auto-answer an unrelated later dialog.
  // The arming command itself ("dialog") is excluded so it can set the policy. Capture the
  // exact policy object and clear only if it is STILL that object -- so we never clobber a
  // fresh arm placed by a later (e.g. overlapping) command, only the one we scoped.
  const scopedPolicy = cmd !== "dialog" ? pendingDialogPolicy : null;
  // Expose the arm to the dialog handler only for the duration of this dispatch, so a dialog the
  // page fires on its own schedule (between tool calls) cannot consume it.
  if (scopedPolicy !== null) {
    dialogArmActive = true;
  }
  try {
    return await dispatchCommand(cmd, args);
  } finally {
    dialogArmActive = false;
    if (scopedPolicy !== null && pendingDialogPolicy === scopedPolicy) {
      pendingDialogPolicy = null;
    }
    // The handles this command resolved end with it; nothing outlives the dispatch that made them.
    await releaseCdpObjects();
  }
}

// The command table. Each entry declares what its handler needs rather than repeating a
// call shape 43 times: `tab` means the handler is given the active tab id, `args` means it
// is given the command's arguments.
//
// This is a Map, NOT an object literal, and that is a security property rather than a
// style choice. `cmd` arrives from the native-messaging relay, so an object literal would
// resolve inherited names -- COMMAND_TABLE["constructor"] and ["toString"] are truthy on
// any plain object, so a frame naming one would pass the "is this a known command?" test
// and then be called. A Map has no prototype chain to walk, so only keys put here match.
const COMMAND_TABLE = new Map([
  ["snapshot", { fn: captureSnapshot, tab: true, args: false }],
  ["read", { fn: handleRead, tab: true, args: true }],
  ["navigate", { fn: handleNavigate, tab: true, args: true }],
  ["back", { fn: () => handleHistory("back"), tab: false, args: false }],
  ["forward", { fn: () => handleHistory("forward"), tab: false, args: false }],
  ["reload", { fn: handleReload, tab: false, args: false }],
  ["listTabs", { fn: handleListTabs, tab: false, args: false }],
  ["selectTab", { fn: handleSelectTab, tab: false, args: true }],
  ["newTab", { fn: handleNewTab, tab: false, args: true }],
  ["closeTab", { fn: handleCloseTab, tab: false, args: true }],
  ["click", { fn: handleClick, tab: true, args: true }],
  ["clickAt", { fn: handleClickAt, tab: true, args: true }],
  ["hover", { fn: handleHover, tab: true, args: true }],
  ["drag", { fn: handleDrag, tab: true, args: true }],
  ["dialog", { fn: handleDialog, tab: true, args: true }],
  ["type", { fn: handleType, tab: true, args: true }],
  ["select", { fn: handleSelect, tab: true, args: true }],
  ["setValue", { fn: handleSetValue, tab: true, args: true }],
  ["media", { fn: handleMedia, tab: true, args: true }],
  ["pressKey", { fn: handlePressKey, tab: true, args: true }],
  ["scroll", { fn: handleScroll, tab: true, args: true }],
  ["screenshot", { fn: handleScreenshot, tab: true, args: true }],
  ["groupTabs", { fn: handleGroupTabs, tab: false, args: true }],
  ["ungroupTabs", { fn: handleUngroupTabs, tab: false, args: true }],
  ["waitFor", { fn: handleWaitFor, tab: true, args: true }],
  ["getValue", { fn: handleGetValue, tab: true, args: true }],
  ["getAttribute", { fn: handleGetAttribute, tab: true, args: true }],
  ["box", { fn: handleBox, tab: true, args: true }],
  ["focus", { fn: handleFocus, tab: true, args: true }],
  ["reveal", { fn: handleReveal, tab: true, args: true }],
  ["jsClick", { fn: handleJsClick, tab: true, args: true }],
  ["listWindows", { fn: handleListWindows, tab: false, args: false }],
  ["window", { fn: handleWindow, tab: false, args: true }],
  ["emulate", { fn: handleEmulate, tab: true, args: true }],
  ["print", { fn: handlePrint, tab: true, args: true }],
  ["permission", { fn: handlePermission, tab: false, args: true }],
  ["storage", { fn: handleStorage, tab: true, args: true }],
  ["cookies", { fn: handleCookies, tab: false, args: true }],
  ["download", { fn: handleDownload, tab: false, args: true }],
  ["httpAuth", { fn: handleHttpAuth, tab: true, args: true }],
]);

async function dispatchCommand(cmd, args) {
  const entry = COMMAND_TABLE.get(cmd);
  if (!entry) {
    throw new Error("Unknown command: " + cmd);
  }
  const params = [];
  if (entry.tab) { params.push(await activeTabId()); }
  if (entry.args) { params.push(args); }
  return await entry.fn(...params);
}

// -- Active tab helpers ------------------------------------------------------

async function activeTab() {
  const tabs = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
  if (!tabs || !tabs.length) {
    throw new Error("No active tab.");
  }
  return tabs[0];
}

async function activeTabId() {
  return (await activeTab()).id;
}

// The url/title nearly every reply in this file names its target by. A url the browser did not
// give us is an unidentified page, and reporting it as "" would put that page's snapshot, read,
// print, or navigation on record as a page with no address -- an observation nobody made. The
// title is genuinely optional (a loading tab has none), so only the url fails the command.
async function tabInfo(tabId) {
  const tab = await chrome.tabs.get(tabId);
  if (typeof tab.url !== "string") {
    throw new Error(
      "Could not read the tab's URL (the tab is gone, or the host permission for it is absent).");
  }
  return { url: tab.url, title: typeof tab.title === "string" ? tab.title : "" };
}

// -- CDP attach lifecycle ----------------------------------------------------

function sendCdp(tabId, method, params) {
  return chrome.debugger.sendCommand({ tabId }, method, params || {});
}

// Every objectId CDP hands back is a RETAINED handle: it pins the JS wrapper -- and the DOM node
// it names, detached or not -- in the renderer until the execution context is destroyed. The
// handles here are all short-lived (one command resolves a ref, hit-tests a point, or polls a
// selector and is done), so they are tagged with one group name and the whole group is released
// when the command ends. Without that a single wait_for polling every 250ms, or a long session of
// clicks each running an occlusion test, accumulates thousands of handles on one document.
const CDP_OBJECT_GROUP = "sak";

// Drop the handles the command that just finished resolved. This is cleanup AFTER the answer is
// computed: the two ways it can fail (the session detached, the context was destroyed) have both
// already freed every handle in the group, so failing the completed command over it would report
// an error about work that succeeded. It is logged rather than swallowed.
async function releaseCdpObjects() {
  const tabId = attachedTabId;
  if (typeof tabId !== "number") {
    return;
  }
  await sendCdp(tabId, "Runtime.releaseObjectGroup", { objectGroup: CDP_OBJECT_GROUP })
    .catch((e) => { console.warn("[SAK] releaseObjectGroup failed:", e); });
}

// The domains a controlled session cannot run without. getFullAXTree needs DOM + Accessibility;
// Page backs getFrameTree, the javascriptDialogOpening auto-answer (without it a confirm() PAUSES
// the tab and every later command blocks to the transport deadline), and the navigation events
// that raise domEpoch -- the sole signal that retires a stale ref_index. Network backs the
// in-flight tracking browser_wait_for reads for network_idle. A domain that failed to enable is
// a safety mechanism silently switched off, so it takes the whole attach down rather than leaving
// a session that looks controlled and is not. enable is idempotent.
async function enableSessionDomains(tabId) {
  networkInstrumented = false;
  await sendCdp(tabId, "DOM.enable");
  await sendCdp(tabId, "Accessibility.enable");
  await sendCdp(tabId, "Page.enable");
  await sendCdp(tabId, "Network.enable");
  networkInstrumented = true;
  inflightRequests.clear();
  lastNetworkActivityMs = Date.now();
}

async function ensureAttached(tabId) {
  if (attachedTabId !== tabId) {
    if (attachedTabId !== null) {
      await detachAll("switching tabs");
    }
    try {
      await chrome.debugger.attach({ tabId }, "1.3");
    } catch (e) {
      const m = String(e && e.message ? e.message : e);
      if (m.includes("Another debugger")) {
        throw new Error("Cannot inspect the tab: DevTools (or another debugger) is attached.");
      }
      throw new Error("Cannot attach to the tab: " + m);
    }
    // Claim the tab only once initialization has fully succeeded. Committing attachedTabId first
    // makes a failed enable PERMANENT: every later command finds the tab already attached, skips
    // this block, and never retries the domain -- so the session runs on with its dialog
    // auto-answer and its epoch invalidation quietly absent, reporting success throughout.
    try {
      await enableSessionDomains(tabId);
    } catch (e) {
      await chrome.debugger.detach({ tabId }).catch(() => {});
      throw new Error(
        "Cannot prepare the tab for control: " + String(e && e.message ? e.message : e));
    }
    attachedTabId = tabId;
  }
  // Keep the control presence visible on every action (and re-created after a navigation wiped
  // it); cheap + idempotent. This is what makes the assistant's control persistently apparent,
  // not only during a pointer action.
  await refreshControlPresence(tabId);
}

async function detachAll(_reason) {
  commandGeneration++;        // the session a polling command was watching is over; retire it
  // A tab switch WE initiate ends the DOM every outstanding ref was captured against just as a
  // navigation does, and the onDetach listener cannot see it: this clears attachedTabId before
  // calling detach, so its source check never matches. Raise the generation here so the bridge
  // invalidates its ref_index instead of holding refs that now name nodes in another renderer.
  domEpoch++;
  lastSnapshotTabId = null;  // any ref_index is now unverifiable against a live tab
  lastSnapshotEpoch = null;  // and its DOM generation no longer names a live document
  lastShot = null;           // and any screenshot coordinates are against a dead render
  pendingDialogPolicy = null;  // an armed dialog response does not carry across sessions
  lastDialog = null;         // nor does a prior page's dialog report
  inflightRequests.clear();  // network-idle tracking does not carry across sessions
  networkInstrumented = false;  // nor does the Network domain: the next session enables its own
  originalUserAgent = null;  // emulation overrides are per-session; recapture on the next tab
  httpAuthCreds = null;      // armed HTTP-auth credentials do not carry across sessions
  lastFetchError = null;     // nor does an interception failure from a page we no longer drive
  if (attachedTabId === null) {
    return;
  }
  const tabId = attachedTabId;
  attachedTabId = null;
  // Control of this tab is ending: clear the on-page presence + toolbar badge while we can still
  // reach the tab (best-effort; a closed tab just fails silently).
  await removeControlPresence(tabId);
  try {
    await chrome.debugger.detach({ tabId });
  } catch (_e) {
    // Already gone (tab closed / user detached); nothing to do.
  }
}

// The user opening DevTools, or the tab closing, force-detaches our session.
chrome.debugger.onDetach.addListener((source) => {
  if (source && source.tabId === attachedTabId) {
    domEpoch++;  // the DOM our refs were captured against is gone
    setTabControlBadge(source.tabId, false);  // control ended; clear the toolbar badge
    attachedTabId = null;
    lastSnapshotTabId = null;
    lastSnapshotEpoch = null;
    lastShot = null;
    pendingDialogPolicy = null;
    lastDialog = null;
    inflightRequests.clear();
    networkInstrumented = false;
    httpAuthCreds = null;
    lastFetchError = null;
  }
});

// A JavaScript dialog (alert/confirm/prompt/beforeunload) PAUSES the page until CDP answers
// it; with the Page domain enabled we must respond or every later command on the tab wedges.
// Default by type: alert only has OK (accept just closes it), and beforeunload accepts so a
// navigation the automation is driving is not blocked; confirm/prompt DISMISS (cancel) so a
// page can never auto-confirm a destructive action -- the model opts into acceptance per
// dialog via browser_dialog. A page cannot escalate through this: it only ever gets its own
// default, or an accept the model explicitly armed (an input-gated tool).
function defaultDialogAccept(type) {
  return type === "alert" || type === "beforeunload";
}

// A paused request that could not be resumed stalls the page on that resource. Keep the newest
// such failure so browser_http_auth can surface it; discarding it leaves a hung page with nothing
// anywhere that explains why.
function recordFetchError(e) {
  lastFetchError = String(e && e.message ? e.message : e);
}

// The dialog kinds Chrome reports on Page.javascriptDialogOpening. A value outside this set is
// metadata we could not read, and an unreadable dialog must not be answered as an "alert" --
// that default ACCEPTS. Anything unrecognized is normalized to a type that dismisses.
const DIALOG_TYPES = new Set(["alert", "confirm", "prompt", "beforeunload"]);

chrome.debugger.onEvent.addListener((source, method, params) => {
  if (!source || source.tabId !== attachedTabId) {
    return;
  }
  // Network-activity tracking for browser_wait_for network_idle. A request starting records its
  // id; finishing/failing drops it. Set semantics are what make a REDIRECT harmless: the hop
  // re-sends requestWillBeSent under the same requestId and only one completion ever arrives, so
  // a count would keep a phantom request in flight for the life of the document. An event without
  // an id is not recorded -- an entry nothing can ever remove would wedge idle permanently.
  if (method === "Network.requestWillBeSent") {
    if (params && params.requestId) { inflightRequests.add(params.requestId); }
    lastNetworkActivityMs = Date.now();
    return;
  }
  if (method === "Network.loadingFinished" || method === "Network.loadingFailed") {
    if (params && params.requestId) { inflightRequests.delete(params.requestId); }
    lastNetworkActivityMs = Date.now();
    return;
  }
  // HTTP-auth interception (browser_http_auth). While Fetch is enabled we MUST answer every
  // paused request or the page wedges: an auth challenge gets the armed credentials (or the
  // browser default when disarmed), and every other paused request is continued untouched.
  if (method === "Fetch.authRequired") {
    // Only hand the armed credentials to a challenge from the SAME origin they were
    // armed for. A bare urlPattern:"*" makes Fetch pause every request, so without
    // this an armed 401 answer would also be sent to a cross-origin subresource or a
    // redirected challenger and silently leak the password. Every other challenger
    // gets the browser default (native dialog / cancel).
    const challengeOrigin =
      params && params.authChallenge ? params.authChallenge.origin : null;
    const originMatches =
      httpAuthCreds && originsMatch(challengeOrigin, httpAuthCreds.origin);
    const response = originMatches
      ? { response: "ProvideCredentials", username: httpAuthCreds.username,
          password: httpAuthCreds.password }
      : { response: "Default" };
    sendCdp(source.tabId, "Fetch.continueWithAuth",
      { requestId: params.requestId, authChallengeResponse: response }).catch(recordFetchError);
    return;
  }
  if (method === "Fetch.requestPaused") {
    sendCdp(source.tabId, "Fetch.continueRequest", { requestId: params.requestId })
      .catch(recordFetchError);
    return;
  }
  // A navigation ends the page an armed response was meant for; drop it so an accept armed
  // for page A cannot auto-confirm page B's first dialog. Cover both a real document swap
  // (top-frame Page.frameNavigated) and a client-side/SPA route change
  // (Page.navigatedWithinDocument). A beforeunload for the navigation itself fires BEFORE the
  // commit, so an arm meant for it is still honored.
  const topFrameNav =
    method === "Page.frameNavigated" && params && params.frame && !params.frame.parentId;
  if (topFrameNav || method === "Page.navigatedWithinDocument") {
    domEpoch++;  // the document/route changed: any prior ref_index is stale
    pendingDialogPolicy = null;
    if (topFrameNav) {
      // A new document: any in-flight request from the old page is moot -- reset so a request
      // that never reported completion cannot keep network_idle from ever resolving.
      inflightRequests.clear();
      lastNetworkActivityMs = Date.now();
      // Armed HTTP-auth credentials were meant for the page being left; disarm them so a
      // navigation (or an attacker-driven redirect) cannot carry them into a new document.
      httpAuthCreds = null;
    }
    // A navigation wiped the injected overlay; re-assert the control presence on the new
    // document so the assistant's control stays visible without waiting for the next action.
    refreshControlPresence(source.tabId);
    return;
  }
  if (method !== "Page.javascriptDialogOpening") {
    return;
  }
  const rawType = params && typeof params.type === "string" ? params.type : "";
  const type = DIALOG_TYPES.has(rawType) ? rawType : "unknown";
  const message = params && typeof params.message === "string" ? params.message : "";
  const frameUrl = params && typeof params.url === "string" ? params.url : "";
  let accept = defaultDialogAccept(type);
  let promptText;
  // An arm belongs to the document it was armed against, and to the command it was armed for.
  // Page.javascriptDialogOpening reports the frame that opened the dialog, and a page can embed a
  // third-party iframe -- or simply wait for the gap between tool calls -- to fire a confirm()
  // that would otherwise consume an accept armed for the top document's next action. A dialog
  // that matches neither gets the type default and leaves the arm in place for its own dialog.
  if (pendingDialogPolicy && dialogArmActive &&
      dialogSourceMatches(frameUrl, pendingDialogPolicy.origin)) {
    accept = pendingDialogPolicy.accept;
    promptText = pendingDialogPolicy.text;
    pendingDialogPolicy = null;  // one-shot: never carry an armed response to a later dialog
  }
  const answer = { accept };
  if (accept && typeof promptText === "string") {
    answer.promptText = promptText;
  }
  // The record follows the ANSWER, never the intent. Page.handleJavaScriptDialog can reject (the
  // dialog was already gone, the session detached mid-answer), and the page then stays PAUSED --
  // every later command on the tab blocks to the transport deadline. Reporting that dialog as
  // handled would hide the one fact that explains it, and the one-shot arm is already spent.
  sendCdp(source.tabId, "Page.handleJavaScriptDialog", answer).then(
    () => {
      lastDialog = { type, message, accepted: accept, url: frameUrl };
    },
    (e) => {
      lastDialog = { type, message, accepted: null, url: frameUrl,
        answer_failed: String(e && e.message ? e.message : e) };
    });
});

// Refs are valid only on the tab the snapshot was taken from AND only against the DOM
// generation it was captured at; refuse a ref action when either moved (a tab switch, a
// model-opened foreground tab, or a navigation the page itself drove under us).
function requireSnapshotTab(tabId) {
  if (lastSnapshotTabId === null || tabId !== lastSnapshotTabId) {
    throw new Error(
      "The active tab changed since the last snapshot; call browser_snapshot on the " +
        "current tab before acting on an element.");
  }
  if (lastSnapshotEpoch === null || domEpoch !== lastSnapshotEpoch) {
    throw new Error(
      "The page changed since the last snapshot; call browser_snapshot again before " +
        "acting on an element.");
  }
}

// -- snapshot: accessibility roles/names joined with DOM-snapshot geometry ----

function axValue(v) {
  return v && v.value !== undefined && v.value !== null ? String(v.value) : "";
}

// Null-prototype, because the keys are property NAMES arriving from the accessibility tree
// and the values are read back by name a few lines later. On a plain object literal, a
// property named "__proto__" would set this map's prototype instead of becoming an own key,
// and every later lookup for a state the page never set would resolve through an object the
// page chose. Chrome populates these names from its own AXPropertyName enum rather than from
// page strings, so this is defence in depth rather than a live hole -- but a null prototype
// costs nothing and removes the question.
function indexProps(properties) {
  const map = Object.create(null);
  for (const p of properties || []) {
    if (p && p.name && p.value) {
      map[p.name] = p.value.value;
    }
  }
  return map;
}

// One DOMSnapshot gives every node's backendNodeId + layout bounds in a single call,
// so geometry is a map lookup instead of an N-round-trip DOM.getBoxModel per node.
// captureSnapshot returns ONE document per same-process frame (main at [0], same-origin
// iframes at [1..]); getFullAXTree stitches those same-process subframes into one tree,
// so their nodes need geometry too. Merge EVERY document -- backendNodeId is unique
// tab-wide, so a single map keyed by it has no cross-document collisions.
function buildBoundsMap(snapshot) {
  const map = new Map();
  const documents = (snapshot && snapshot.documents) || [];
  for (const doc of documents) {
    if (!doc || !doc.nodes || !doc.layout) {
      continue;
    }
    const backendIds = doc.nodes.backendNodeId || [];
    const layout = doc.layout;
    const nodeIndex = layout.nodeIndex || [];
    const bounds = layout.bounds || [];
    for (let i = 0; i < nodeIndex.length; i++) {
      const backend = backendIds[nodeIndex[i]];
      const b = bounds[i];
      if (backend === undefined || !b) {
        continue;
      }
      map.set(backend, {
        x: Math.round(b[0]),
        y: Math.round(b[1]),
        width: Math.round(b[2]),
        height: Math.round(b[3]),
      });
    }
  }
  return map;
}

function isEditableRole(role) {
  return role === "textbox" || role === "searchbox";
}

// An ARIA state that is present but false is not the same fact as one that is absent, so
// these helpers only ever ADD a field. A caller reading a missing field learns "the page did
// not say", which is the honest answer, rather than a fabricated false.
const axTruthy = (v) => v === true || v === "true" || v === "mixed";

// ARIA `checked` is TRI-state. "mixed" is a partially-checked control (the "select all" whose
// children are split), which is neither checked nor unchecked: collapsing it either way states
// something about the control that is not true, so it is reported as its own field and the
// boolean is left absent -- the outline then makes no claim it cannot support.
function axApplyCheckedState(props, rec) {
  if (props.checked === "mixed") {
    rec.mixed = true;
  } else if (props.checked !== undefined) {
    rec.checked = props.checked === true || props.checked === "true";
  }
}

// The states that are simply "present and true" become one row each. Written as data rather
// than as a branch per state so that adding an ARIA state is a table entry, and so the shape
// of the check is visible side by side instead of spread over fifteen conditionals.
const AX_STATE_FLAGS = [
  { prop: "disabled", field: "disabled", when: (v) => v === true },
  { prop: "readonly", field: "readonly", when: (v) => v === true },
  { prop: "required", field: "required", when: (v) => v === true },
  { prop: "busy", field: "busy", when: (v) => v === true },
  { prop: "selected", field: "selected", when: axTruthy },
  { prop: "pressed", field: "pressed", when: axTruthy },
  // "invalid" carries a reason string ("spelling", "grammar"), so anything that is not
  // absent and not an explicit false counts as invalid.
  { prop: "invalid", field: "invalid",
    when: (v) => v !== undefined && v !== false && v !== "false" },
];

// ARIA state, so the model can read expanded/selected/pressed/validity without a round-trip.
function axApplyStateFlags(props, role, rec) {
  for (const flag of AX_STATE_FLAGS) {
    if (flag.when(props[flag.prop])) {
      rec[flag.field] = true;
    }
  }
  if (isEditableRole(role) || (props.editable !== undefined && props.editable !== false)) {
    rec.editable = true;
  }
  axApplyCheckedState(props, rec);
  // expanded is reported as a real boolean rather than only when true, because a collapsed
  // control and a control with no expanded state are different facts to a caller.
  if (props.expanded !== undefined && props.expanded !== "undefined") {
    rec.expanded = axTruthy(props.expanded);
  }
}

// The live value of inputs, sliders and spinbuttons.
function axApplyValueFields(node, props, rec) {
  const currentValue = node.value && node.value.value;
  if (currentValue !== undefined && currentValue !== null && currentValue !== "") {
    // A silently sliced value reads as the whole value; mark the cut like every other capped
    // field in this file so a longer entry is not mistaken for the control's full contents.
    const raw = String(currentValue);
    rec.value = raw.slice(0, AX_VALUE_MAX_CHARS);
    if (raw.length > AX_VALUE_MAX_CHARS) {
      rec.value_truncated = true;
    }
  }
  if (typeof props.valuemin === "number" && typeof props.valuemax === "number") {
    rec.valuemin = props.valuemin;
    rec.valuemax = props.valuemax;
  }
}

function axApplyGeometry(node, boundsByBackend, rec) {
  if (typeof node.backendDOMNodeId !== "number") {
    return;
  }
  rec.backendNodeId = node.backendDOMNodeId;
  const b = boundsByBackend.get(node.backendDOMNodeId);
  if (b) {
    rec.bounds = b;
  }
}

function axNodeToCapture(node, depth, boundsByBackend) {
  if (node.ignored) {
    return null;
  }
  const role = axValue(node.role).toLowerCase();
  const name = axValue(node.name);
  const props = indexProps(node.properties);
  const focusable = props.focusable === true;
  const interactable = INTERACTABLE_ROLES.has(role) || (focusable && !STRUCTURAL_ROLES.has(role));
  // Structural, unnamed, non-interactable filler is dropped by the C++ renderer anyway
  // (roleIsStructuralNoise); not emitting it here keeps the node budget for content the
  // model can actually see or act on.
  if (!interactable && !name && STRUCTURAL_ROLES.has(role)) {
    return null;
  }
  const rec = { role, name, depth, interactable, visible: props.hidden !== true };
  axApplyGeometry(node, boundsByBackend, rec);
  axApplyStateFlags(props, role, rec);
  axApplyValueFields(node, props, rec);
  return rec;
}

// Walk the AX tree in document (pre-order) order, tracking depth, emitting the capture
// nodes the C++ renderSnapshot consumes.
function buildNodes(axTree, boundsByBackend) {
  const axNodes = (axTree && axTree.nodes) || [];
  const byId = new Map(axNodes.map((n) => [n.nodeId, n]));
  const childOf = new Set();
  for (const n of axNodes) {
    for (const c of n.childIds || []) {
      childOf.add(c);
    }
  }
  const roots = axNodes.filter((n) => !childOf.has(n.nodeId));
  const out = [];
  const seen = new Set();
  // pop() is LIFO, so the roots go on in REVERSE to come off in document order -- the same reason
  // the child push below counts down. Pushed forward, the LAST root is walked first, and its
  // subtree can spend the whole node budget and truncate the MAIN document out of the outline
  // while `truncated` gives no hint that the missing part is the page itself.
  const stack = [];
  for (let i = roots.length - 1; i >= 0; i--) {
    stack.push({ node: roots[i], depth: 0 });
  }
  // The emitted-record cap does not bound the WALK: axNodeToCapture drops unnamed structural
  // filler, so a tree of a million <div>s is traversed in full while `out` stays tiny -- a
  // page-controlled way to hold the single command channel busy past the transport deadline.
  // Bound the visits too; either bound biting leaves the stack non-empty, which reports the
  // outline as partial rather than whole.
  let visited = 0;
  const maxVisits = 20 * MAX_CAPTURE_NODES;
  while (stack.length && out.length < MAX_CAPTURE_NODES && visited < maxVisits) {
    visited++;
    const { node, depth } = stack.pop();
    if (!node || seen.has(node.nodeId)) {
      continue;
    }
    seen.add(node.nodeId);
    const rec = axNodeToCapture(node, depth, boundsByBackend);
    if (rec) {
      out.push(rec);
    }
    const kids = (node.childIds || []).map((cid) => byId.get(cid)).filter(Boolean);
    for (let i = kids.length - 1; i >= 0; i--) {
      stack.push({ node: kids[i], depth: depth + 1 });
    }
  }
  // A non-empty stack means we hit the node cap and dropped the rest of the tree; the
  // caller must tell the model the outline is partial rather than present it as whole.
  return { nodes: out, truncated: stack.length > 0 };
}

// Count every frame in the tree Chrome reports for the tab.
function countFrames(frameTree) {
  if (!frameTree) {
    return 0;
  }
  let total = 1;
  for (const child of frameTree.childFrames || []) {
    total += countFrames(child);
  }
  return total;
}

// DOMSnapshot's documentURL keeps the "#fragment" (it mirrors document.URL), but
// Page.getFrameTree's frame.url drops it (the fragment is a separate frame.urlFragment we
// never read). Compare both sides fragment-insensitively, or a page at ".../docs#intro"
// with no iframes would see its own main frame falsely listed as an omitted cross-origin one.
function stripFragment(url) {
  return String(url).split("#")[0];
}

// The document URLs the single capture DID reach (main frame + same-process subframes),
// fragment-stripped. DOMSnapshot stores documentURL as an index into the shared string table.
function sameProcessUrls(snapshot) {
  const urls = new Set();
  const strings = (snapshot && snapshot.strings) || [];
  for (const doc of (snapshot && snapshot.documents) || []) {
    if (doc && typeof doc.documentURL === "number" && strings[doc.documentURL] != null) {
      urls.add(stripFragment(strings[doc.documentURL]));
    }
  }
  return urls;
}

function collectFrameUrls(frameTree, out) {
  if (!frameTree || !frameTree.frame) {
    return;
  }
  if (frameTree.frame.url) {
    out.push(frameTree.frame.url);
  }
  for (const child of frameTree.childFrames || []) {
    collectFrameUrls(child, out);
  }
}

// Deduplicate http(s) frame URLs into a capped list, reporting whether the cap cut it. Only
// http(s), so the model gets frames it can actually navigate to. Scanning continues past the cap
// instead of breaking, so truncated reflects frames genuinely left out rather than every
// duplicate or non-http entry that happened to trail the list.
function capFrameUrls(urls, covered) {
  const omitted = [];
  const seen = new Set();
  let truncated = false;
  for (const url of urls) {
    const key = stripFragment(url);
    if (!/^https?:\/\//i.test(url) || (covered && covered.has(key)) || seen.has(key)) {
      continue;
    }
    seen.add(key);
    if (omitted.length >= MAX_OMITTED_FRAMES) {
      truncated = true;
      continue;
    }
    omitted.push(url);  // the original (already fragmentless) URL, navigable as-is
  }
  return { frames: omitted, truncated };
}

// The http(s) frames present in the tab but NOT covered by the same-process capture, i.e.
// the cross-origin (out-of-process) iframes whose content is missing.
function collectOmittedFrames(snapshot, frameTree) {
  const covered = sameProcessUrls(snapshot);
  const all = [];
  collectFrameUrls(frameTree, all);
  return capFrameUrls(all, covered);
}

// <audio>/<video> elements are frequently absent or unnamed in the AX tree (no native
// controls), so the model gets no ref to drive browser_media. Surface them directly from
// the DOM (by tag) as ref'd capture nodes, deduped against what the AX pass already emitted.
// Each element costs its own serial DOM.describeNode round trip and the list is page-controlled,
// so it is capped twice: at a fixed ceiling (a page can trivially carry 50,000 <video> tags, and
// 50,000 round trips outlive the bridge's I/O deadline and tear the connection down) and at
// whatever is left of the snapshot's node budget. A cap that bit is reported, never hidden.
const MAX_MEDIA_NODES = 100;

async function collectMediaNodes(tabId, existing) {
  const have = new Set(
    existing.filter((n) => typeof n.backendNodeId === "number").map((n) => n.backendNodeId));
  const out = [];
  // A scan that FAILED and a page that genuinely has no media produce the same empty list, and
  // the model reads the empty one as an absence of <audio>/<video>. The scan reads therefore
  // propagate: an unusable media pass fails the snapshot, so the caller re-takes it, rather than
  // stating an absence nothing observed.
  const doc = await sendCdp(tabId, "DOM.getDocument", { depth: 0 });
  const root = doc && doc.root && doc.root.nodeId;
  if (!root) {
    throw new Error("The page exposed no document node to scan for media elements.");
  }
  const found = await sendCdp(tabId, "DOM.querySelectorAll", { nodeId: root, selector: "audio,video" });
  const all = (found && found.nodeIds) || [];
  const budget = Math.max(0, Math.min(MAX_MEDIA_NODES, MAX_CAPTURE_NODES - existing.length));
  let truncated = all.length > budget;
  for (const nodeId of all.slice(0, budget)) {
    // One element that went away between the query and the describe is a node this capture could
    // not cover -- not a failed scan. It counts as partiality, which the snapshot already reports,
    // instead of silently shortening the list.
    const d = await sendCdp(tabId, "DOM.describeNode", { nodeId }).catch(() => null);
    const node = d && d.node;
    if (!node) { truncated = true; continue; }
    if (typeof node.backendNodeId !== "number" || have.has(node.backendNodeId)) { continue; }
    const attrs = {};
    const a = node.attributes || [];
    for (let i = 0; i + 1 < a.length; i += 2) { attrs[a[i]] = a[i + 1]; }
    const role = (node.nodeName || "").toLowerCase() === "video" ? "video" : "audio";
    const name = attrs["aria-label"] || attrs["title"] || role;
    out.push({ role, name, depth: 0, interactable: true, visible: true, backendNodeId: node.backendNodeId });
    have.add(node.backendNodeId);
  }
  return { nodes: out, truncated };
}

async function captureSnapshot(tabId) {
  await ensureAttached(tabId);
  // The capture is five sequential CDP reads. A top-frame navigation part way through does not
  // throw -- it silently leaves geometry and AX nodes from document A joined to the url/title
  // (and stamped with the epoch) of document B, and backendNodeIds restart across a cross-process
  // navigation, so a ref minted from A can resolve to an unrelated element in B. The bridge
  // treats a snapshot reply as a NEW baseline rather than a check, so it would adopt that mixed
  // capture as the ref_index of record: the comparison has to happen here.
  const startEpoch = domEpoch;
  const snapshot = await sendCdp(tabId, "DOMSnapshot.captureSnapshot", { computedStyles: [] });
  const boundsByBackend = buildBoundsMap(snapshot);
  const axTree = await sendCdp(tabId, "Accessibility.getFullAXTree", {});
  const built = buildNodes(axTree, boundsByBackend);
  const media = await collectMediaNodes(tabId, built.nodes);
  const nodes = built.nodes.concat(media.nodes);
  const truncated = built.truncated || media.truncated;
  const info = await tabInfo(tabId);
  // getFullAXTree + DOMSnapshot cover the main frame and its SAME-process subframes.
  // Cross-origin (out-of-process) iframes live in separate targets this single pass does
  // not reach, so their content is absent. Detect that (more frames exist than
  // same-process documents) and flag it, so the model is told the capture is partial
  // instead of concluding those elements do not exist.
  let iframesOmitted = false;
  let omittedFrames = [];
  let omittedFramesTruncated = false;
  try {
    const tree = await sendCdp(tabId, "Page.getFrameTree", {});
    const cut = collectOmittedFrames(snapshot, tree && tree.frameTree);
    omittedFrames = cut.frames;
    omittedFramesTruncated = cut.truncated;
    const totalFrames = countFrames(tree && tree.frameTree);
    const sameProcessDocs = ((snapshot && snapshot.documents) || []).length;
    // Flag omission from either signal: the named http(s) frames, or a frame-count excess
    // (covers non-http frames the URL list intentionally skips).
    iframesOmitted = omittedFrames.length > 0 || totalFrames > sameProcessDocs;
  } catch (_e) {
    // If the frame tree is unavailable, do not claim completeness we cannot verify.
    iframesOmitted = true;
  }
  if (domEpoch !== startEpoch) {
    throw new Error(
      "The page navigated while the snapshot was being taken; take another snapshot.");
  }
  // Mark this tab as the snapshot-of-record ONLY after the capture fully succeeded: a throw in
  // any await above (DOMSnapshot/getFullAXTree/tabInfo) or the generation check just made leaves
  // lastSnapshotTabId untouched, so requireSnapshotTab keeps failing closed on refs the model
  // never received rather than trusting a stale/aborted tab id.
  lastSnapshotTabId = tabId;  // refs from this snapshot are valid only against this tab
  lastSnapshotEpoch = domEpoch;  // and only against the DOM generation it was captured at
  return { url: info.url, title: info.title, nodes, truncated, iframesOmitted, omittedFrames,
           omittedFramesTruncated };
}

// -- read --------------------------------------------------------------------

// The read format the caller asked for. An unrecognized value is not a request for text: mapping
// "markdown" or "HTML" onto text silently answers a different question than the one asked.
function readFormat(args) {
  if (!args || args.format === undefined || args.format === null) {
    return "text";
  }
  if (args.format !== "text" && args.format !== "html") {
    throw new Error("browser_read format must be \"text\" or \"html\".");
  }
  return args.format;
}

// The subframes the read did NOT cover. innerText/outerHTML are properties of the MAIN document,
// so an iframe's content is absent from both -- and for embedded apps (docs viewers, chat
// widgets, payment frames) that is where the content lives. Reporting the omission is what keeps
// a partial read from being read as "the page has nothing on it". http(s) frames are named so the
// model can navigate to one; a frame tree we could not read claims no completeness at all.
async function readOmittedFrames(tabId) {
  let tree;
  try {
    tree = await sendCdp(tabId, "Page.getFrameTree", {});
  } catch (_e) {
    return { iframesOmitted: true, omittedFrames: [], omittedFramesTruncated: false };
  }
  const urls = [];
  // Count the frames structurally rather than inferring from the URL list: a frame with no
  // reportable url (about:blank, srcdoc) still holds content this read did not cover.
  let subframes = 0;
  for (const child of (tree && tree.frameTree && tree.frameTree.childFrames) || []) {
    subframes += countFrames(child);
    collectFrameUrls(child, urls);
  }
  // No same-process document covers a read, so every subframe URL is omitted from it.
  const cut = capFrameUrls(urls, null);
  return { iframesOmitted: subframes > 0, omittedFrames: cut.frames,
           omittedFramesTruncated: cut.truncated };
}

async function handleRead(tabId, args) {
  const format = readFormat(args);
  await ensureAttached(tabId);
  const read =
    format === "html"
      ? "document.documentElement ? document.documentElement.outerHTML : ''"
      : "document.body ? document.body.innerText : " +
        "(document.documentElement ? document.documentElement.innerText : '')";
  const res = await sendCdp(tabId, "Runtime.evaluate",
    { expression: presenceFreeReadScript(read), returnByValue: true });
  // An evaluation that threw (a page can redefine HTMLElement.prototype.innerText, or the read
  // can land while document.body is null) has established nothing about the page. Reporting it as
  // empty content with ok tells the model the page has no text, which is a claim about the page
  // rather than about the failure.
  if (!res || res.exceptionDetails || !res.result || typeof res.result.value !== "string") {
    throw new Error("Could not read the page content; take a fresh snapshot and try again.");
  }
  let content = res.result.value;
  let truncated = false;
  if (content.length > MAX_READ_CHARS) {
    content = content.slice(0, MAX_READ_CHARS);
    truncated = true;
  }
  const info = await tabInfo(tabId);
  const frames = await readOmittedFrames(tabId);
  return { format, content, truncated, url: info.url, title: info.title,
           iframesOmitted: frames.iframesOmitted, omittedFrames: frames.omittedFrames };
}

// -- screenshot (vision): a PNG of the active tab over CDP --------------------
//
// Page.captureScreenshot rasterizes at the browser level (no getUserMedia / display
// prompt, no OS screen capture), so it sees only the tab -- never other windows or the
// desktop. full_page uses captureBeyondViewport with a clip sized to the document, capped
// at the Skia edge limit. The base64 PNG rides back in the reply payload; the bridge caps
// its size and returns it as an MCP image content block.
// The live tab's render fingerprint in one evaluate: devicePixelRatio (>= 1), scroll offset,
// document URL, and the layout viewport size. dpr bounds the screenshot clip in the DEVICE
// pixels Skia rasters, and the whole tuple binds a screenshot to the exact render a later
// coordinate click must match. The viewport size is part of it because browser_emulate can
// relay the whole page out at a new width/height without moving href, dpr, or scroll.
// `ok` is false when the page could not be read: callers must fail closed on it rather than
// trust the placeholder values, so that two failed reads (shot + click) can never compare
// equal and wave a blind coordinate click through.
async function viewportState(tabId) {
  const res = await sendCdp(tabId, "Runtime.evaluate", {
    expression:
      "({dpr: window.devicePixelRatio, sx: window.scrollX, sy: window.scrollY, href: location.href," +
      " iw: window.innerWidth, ih: window.innerHeight})",
    returnByValue: true,
  }).catch(() => null);
  const v = res && res.result && res.result.value ? res.result.value : null;
  if (!v || typeof v.href !== "string" || !Number.isFinite(v.dpr) || v.dpr <= 0 ||
      !Number.isFinite(v.iw) || !Number.isFinite(v.ih) || v.iw <= 0 || v.ih <= 0) {
    return { ok: false, dpr: 1, scrollX: 0, scrollY: 0, href: "", width: 0, height: 0 };
  }
  return {
    ok: true,
    dpr: v.dpr,
    scrollX: Number.isFinite(v.sx) ? Math.round(v.sx) : 0,
    scrollY: Number.isFinite(v.sy) ? Math.round(v.sy) : 0,
    href: v.href,
    width: Math.round(v.iw),
    height: Math.round(v.ih),
  };
}

// Does the live render still match the one a screenshot was taken against? Every field here
// moves pixels under a coordinate the model measured off that image: a scroll or zoom, a
// navigation, a same-URL reload or SPA route change (which href alone cannot see -- hence the
// DOM generation), and a viewport resize.
function shotMatchesRender(shot, now) {
  return now.ok && now.dpr === shot.dpr && now.href === shot.href &&
    now.scrollX === shot.scrollX && now.scrollY === shot.scrollY &&
    now.width === shot.width && now.height === shot.height && domEpoch === shot.epoch;
}

async function handleScreenshot(tabId, args) {
  await ensureAttached(tabId);
  const fullPage = !!(args && args.full_page === true);
  const params = { format: "png", captureBeyondViewport: fullPage };
  const metrics = await sendCdp(tabId, "Page.getLayoutMetrics", {}).catch(() => null);
  const state = await viewportState(tabId);
  // Bound the CSS clip so the DEVICE raster (css x dpr) stays within Skia's 16384 cap; a
  // plain CSS clamp would let a tall page overflow the surface and fail the capture on any
  // dpr > 1 display (Windows scaling, Retina).
  const maxCssEdge = Math.max(1, Math.floor(MAX_SHOT_EDGE_PX / state.dpr));
  const clipped = { applied: false };
  if (fullPage) {
    const content = metrics && (metrics.cssContentSize || metrics.contentSize);
    // Without the metrics there is no document-sized clip, and the capture would silently fall
    // back to captureBeyondViewport with no bounds -- a different image than the full page that
    // was asked for, returned as though it were it.
    if (!content) {
      throw new Error(
        "Cannot capture the full page: the browser did not report the document's layout metrics.");
    }
    const rawWidth = Math.round(content.width);
    const rawHeight = Math.round(content.height);
    params.clip = {
      x: 0,
      y: 0,
      width: Math.min(rawWidth, maxCssEdge),
      height: Math.min(rawHeight, maxCssEdge),
      scale: 1,
    };
    // A document taller (or wider) than Skia's raster cap is CLIPPED, and the image then shows
    // the top slice of a long page. Say so: the model otherwise reasons about "the full page"
    // from a fraction of it.
    clipped.applied = rawWidth > maxCssEdge || rawHeight > maxCssEdge;
    clipped.content_width = rawWidth;
    clipped.content_height = rawHeight;
    clipped.captured_width = params.clip.width;
    clipped.captured_height = params.clip.height;
  }
  // Hide the control presence for the capture so it never bleeds into the image the model reads
  // or obscures page media (video/pictures); restore it right after.
  await setPresenceVisible(tabId, false);
  let res;
  try {
    res = await sendCdp(tabId, "Page.captureScreenshot", params);
  } finally {
    await setPresenceVisible(tabId, true);
  }
  const data = res && typeof res.data === "string" ? res.data : "";
  if (!data) {
    throw new Error("Screenshot capture returned no image data.");
  }
  if (data.length > MAX_SHOT_BASE64) {
    throw new Error(
      "The captured image is too large to return; capture the viewport instead of the full page.");
  }
  // Bind this image to the exact render so a later browser_click_at can convert with the
  // same dpr and refuse if the tab, dpr, scroll, or document moved. Only bind when the
  // fingerprint was read successfully; otherwise leave no binding so a coordinate click
  // fails closed ("no current screenshot") rather than trusting placeholder values.
  lastShot = state.ok
    ? { tabId, fullPage, dpr: state.dpr, scrollX: state.scrollX, scrollY: state.scrollY,
        href: state.href, width: state.width, height: state.height, epoch: domEpoch }
    : null;
  const info = await tabInfo(tabId);
  // Dimensions are read authoritatively from the PNG header on the bridge side, so they
  // reflect the real device pixels regardless of dpr/clip. What the header cannot show is that a
  // full-page capture was CUT to the raster cap -- the image looks whole -- so the document's own
  // size rides along whenever the clamp bit.
  const payload = { data, mimeType: "image/png", url: info.url, title: info.title };
  if (clipped.applied) {
    payload.clipped = true;
    payload.content_width = clipped.content_width;
    payload.content_height = clipped.content_height;
    payload.captured_width = clipped.captured_width;
    payload.captured_height = clipped.captured_height;
  }
  return payload;
}

// -- input: browser-level injection (the user's OS cursor is never touched) ---
//
// Every action here is driven by the assistant through the code-verified bridge and
// gated by the app's confirmation policy before it arrives; a page can neither initiate
// input nor supply the target (backendNodeId comes from our own validated snapshot,
// text/keys from the model). Actions are dispatched over CDP Input, so they land in the
// page at the browser level and the user's real mouse/keyboard are untouched.

// Center of a CDP content-box quad [x1,y1,x2,y2,x3,y3,x4,y4], in the CSS-pixel viewport
// space Input.dispatchMouseEvent expects.
function quadCenter(quad) {
  const xs = [quad[0], quad[2], quad[4], quad[6]];
  const ys = [quad[1], quad[3], quad[5], quad[7]];
  const avg = (a) => a.reduce((s, v) => s + v, 0) / a.length;
  return { x: avg(xs), y: avg(ys) };
}

async function resolveActionPoint(tabId, backendNodeId) {
  if (typeof backendNodeId !== "number") {
    throw new Error("This action needs a valid element ref from the latest snapshot.");
  }
  await sendCdp(tabId, "DOM.scrollIntoViewIfNeeded", { backendNodeId }).catch(() => {});
  let model;
  try {
    model = await sendCdp(tabId, "DOM.getBoxModel", { backendNodeId });
  } catch (_e) {
    throw new Error("The element is not laid out (hidden or gone); take a fresh snapshot.");
  }
  const quad = model && model.model && model.model.content;
  if (!quad || quad.length < 8) {
    throw new Error("The element has no visible box; take a fresh snapshot.");
  }
  return quadCenter(quad);
}

// Build the control-presence overlay (frame + badge + prominent pulsing cursor) and position the
// cursor at (x,y). The whole markup is CONSTANT; only x/y are interpolated, coerced to finite
// integers -- nothing page- or model-controlled is ever injected as code. Every element is
// pointer-events:none (cosmetic; never intercepts input or hit-testing) and idempotent (reuses
// an existing node), so it is cheap to call on every action and after a navigation.
// Every element is also aria-hidden: this overlay is the assistant's own furniture, not something
// the site rendered, and an accessibility node for it would reach the model as an element of the
// PAGE -- the badge as an "AI CONTROL" static text it could reason from or act on. aria-hidden
// marks the whole subtree ignored, and an ignored node is what axNodeToCapture drops.
function controlPresenceScript(x, y) {
  // The arrow's tip is at (2,1) in the 24-unit viewBox rendered at 32px (scale ~1.33); offset
  // the translate so the tip lands on the action point.
  const px = (Number.isFinite(x) ? Math.round(x) : 0) - 3;
  const py = (Number.isFinite(y) ? Math.round(y) : 0) - 1;
  return (
    "(function(){" +
    "function mk(id,tag){var e=document.getElementById(id);if(!e){e=document.createElement(tag);" +
    "e.id=id;(document.body||document.documentElement).appendChild(e);}" +
    "e.setAttribute('aria-hidden','true');e.setAttribute('role','presentation');return e;}" +
    // Keyframes for the cursor halo pulse (injected once).
    "var st=document.getElementById('" + CONTROL_STYLE_ID + "');" +
    "if(!st){st=document.createElement('style');st.id='" + CONTROL_STYLE_ID + "';" +
    "st.textContent='@keyframes sakPulse{0%{transform:scale(.6);opacity:.85}" +
    "70%{transform:scale(1.6);opacity:0}100%{transform:scale(1.6);opacity:0}}';" +
    "(document.head||document.documentElement).appendChild(st);}" +
    // Viewport frame: thin neon border, hollow + pointer-events:none so it never covers content
    // or intercepts a hit-test.
    "var f=mk('" + CONTROL_FRAME_ID + "','div');" +
    "f.style.cssText='position:fixed;inset:0;z-index:2147483644;pointer-events:none;" +
    "border:3px solid rgba(255,45,149,.9);box-shadow:inset 0 0 14px rgba(255,45,149,.45);" +
    "border-radius:3px';" +
    // Badge pill, top-right.
    "var b=mk('" + CONTROL_BADGE_ID + "','div');" +
    "b.style.cssText='position:fixed;top:10px;right:12px;z-index:2147483646;pointer-events:none;" +
    "font:600 11px/1.5 -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;color:#fff;" +
    "background:#12001a;border:1px solid #ff2d95;border-radius:11px;padding:2px 9px;" +
    "letter-spacing:.07em;box-shadow:0 0 8px rgba(255,45,149,.8)';" +
    "b.textContent='AI CONTROL';" +
    // Cursor container with a pulsing halo behind a bold neon pointer.
    "var c=mk('" + AGENT_CURSOR_ID + "','div');" +
    "c.style.cssText='position:fixed;left:0;top:0;z-index:2147483647;width:32px;height:32px;" +
    "pointer-events:none;will-change:transform;" +
    "transition:transform .12s cubic-bezier(.22,1,.36,1);" +
    "filter:drop-shadow(0 0 3px #ff2d95) drop-shadow(0 0 7px rgba(255,45,149,.95)) " +
    "drop-shadow(0 0 14px rgba(255,45,149,.65))';" +
    "c.innerHTML=\"<div style='position:absolute;left:-9px;top:-9px;width:34px;height:34px;" +
    "border-radius:50%;background:radial-gradient(circle,rgba(255,45,149,.55),rgba(255,45,149,0) 70%);" +
    "animation:sakPulse 1.6s ease-out infinite'></div>" +
    "<svg width='32' height='32' viewBox='0 0 24 24' xmlns='http://www.w3.org/2000/svg' " +
    "style='position:relative'>" +
    "<path d='M2 1 L2 19 L7 14 L10.5 21 L13.6 19.6 L10.1 13 L17 13 Z' fill='#ffffff' " +
    "stroke='#12001a' stroke-width='1.3' stroke-linejoin='round'/></svg>\";" +
    "c.style.transform='translate(" + px + "px," + py + "px)';})();"
  );
}

// Toggle the presence overlay's visibility without removing it (used to hide it for the duration
// of a screenshot capture so it never appears in the image / over page media).
function presenceVisibilityScript(visible) {
  const disp = visible ? "" : "none";
  return (
    "(function(){['" + CONTROL_FRAME_ID + "','" + CONTROL_BADGE_ID + "','" + AGENT_CURSOR_ID +
    "'].forEach(function(id){var e=document.getElementById(id);if(e){e.style.display='" + disp +
    "';}});})();"
  );
}

async function moveAgentCursor(tabId, x, y) {
  lastCursorPoint = { x, y };
  await sendCdp(tabId, "Runtime.evaluate", { expression: controlPresenceScript(x, y) }).catch(() => {});
}

// Ensure the control presence (frame + badge + parked cursor) is on the page and mark the tab in
// the toolbar with an "AI" badge. Called on attach and after navigations; idempotent and cheap.
async function refreshControlPresence(tabId) {
  await sendCdp(tabId, "Runtime.evaluate", {
    expression: controlPresenceScript(lastCursorPoint.x, lastCursorPoint.y),
  }).catch(() => {});
  setTabControlBadge(tabId, true);
}

// Remove the on-page overlay and clear the toolbar badge when control of the tab ends.
async function removeControlPresence(tabId) {
  await sendCdp(tabId, "Runtime.evaluate", {
    expression: "(function(){['" + PRESENCE_IDS.join("','") +
      "'].forEach(function(id){var e=document.getElementById(id);if(e){e.remove();}});})();",
  }).catch(() => {});
  setTabControlBadge(tabId, false);
}

async function setPresenceVisible(tabId, visible) {
  await sendCdp(tabId, "Runtime.evaluate", { expression: presenceVisibilityScript(visible) })
    .catch(() => {});
}

// Wrap a page-content read so the presence overlay is not part of the answer. innerText is
// RENDERED text and outerHTML is the live tree, so an overlay left in place makes the badge's
// literal "AI CONTROL" and the frame/cursor markup part of what the model is told the PAGE
// contains -- and browser_wait_for {text:"AI CONTROL"} is then satisfied by our own furniture on
// any attached tab. The nodes are detached and restored INSIDE one evaluate: the whole sequence is
// a single task, so no frame is ever painted without the presence and control is never visually
// unmarked -- and the restore rides in a finally, so a read that throws still puts it back.
// `readExpr` is a constant from this file; nothing page- or model-controlled is interpolated here.
// The restore runs BACKWARDS: the overlay nodes are siblings, so the node one of them was recorded
// in front of is usually the next overlay node, which is still detached while the list is walked
// forwards -- and insertBefore throws when its reference is not a child of the parent. Reverse
// order puts each node back only after the sibling it names is already home, and a reference that
// moved anyway is dropped so the node lands under its own parent instead of failing the read (it
// is position:fixed furniture; where it sits among its siblings is not a fact about the page).
function presenceFreeReadScript(readExpr) {
  return (
    "(function(){var slots=[];['" + PRESENCE_IDS.join("','") + "'].forEach(function(id){" +
    "var e=document.getElementById(id);" +
    "if(e&&e.parentNode){slots.push([e,e.parentNode,e.nextSibling]);e.parentNode.removeChild(e);}" +
    "});try{return " + readExpr + ";}" +
    "finally{for(var i=slots.length-1;i>=0;i--){var s=slots[i];" +
    "s[1].insertBefore(s[0],s[2]&&s[2].parentNode===s[1]?s[2]:null);}}})()"
  );
}

// Per-tab toolbar badge: only the controlled tab shows the pink "AI" chip, so the user can tell
// which tab (and page) the assistant is driving from the tab strip / toolbar.
function setTabControlBadge(tabId, on) {
  // Called with no callback, the MV3 chrome.action methods return PROMISES. A synchronous catch
  // never sees their rejection -- and "No tab with id N" is the routine one, since control ends
  // precisely when a tab is closing -- so without settling each promise the intended silent no-op
  // becomes an unhandled rejection in the worker.
  const ignore = () => {};
  const settle = (p) => { Promise.resolve(p).catch(ignore); };
  try {
    if (on) {
      settle(chrome.action.setBadgeText({ tabId, text: "AI" }));
      settle(chrome.action.setBadgeBackgroundColor({ tabId, color: "#ff2d95" }));
      settle(chrome.action.setTitle({ tabId, title: "S.A.K. assistant is controlling this tab" }));
    } else {
      settle(chrome.action.setBadgeText({ tabId, text: "" }));
      settle(chrome.action.setTitle({ tabId, title: "" }));
    }
  } catch (_e) {
    // chrome.action is unavailable in some contexts; the on-page overlay still signals control.
  }
}

async function dispatchMouse(tabId, type, x, y, extra) {
  await sendCdp(tabId, "Input.dispatchMouseEvent", Object.assign({ type, x, y }, extra || {}));
}

const MOUSE_BUTTONS = { left: 1, right: 2, middle: 4 };

// Parse "Control+Shift" -> the CDP modifier bitmask (reuses MODIFIER_BITS).
// A modifier the map does not know is REFUSED, not dropped. Silently ignoring it would send a
// plain click while reporting the modified one the model asked for -- and "ctrl+click" versus
// "click" is the difference between opening a background tab and navigating the page away.
function parseModifiers(spec) {
  if (!spec) { return 0; }
  let mods = 0;
  for (const part of String(spec).split("+")) {
    const name = part.trim().toLowerCase();
    if (name === "") { continue; }
    const bit = MODIFIER_BITS[name];
    if (!bit) {
      throw new Error(
        "Unknown modifier '" + name + "'; use one or more of: " +
          Object.keys(MODIFIER_BITS).join(", ") + ".");
    }
    mods |= bit;
  }
  return mods;
}

// A pointer button the model did not name is REFUSED rather than coerced to left: a right-click
// silently downgraded to a left-click opens no context menu and reports success for an action
// that never happened.
function mouseButton(spec) {
  if (spec === undefined || spec === null || spec === "") { return "left"; }
  const name = String(spec).toLowerCase();
  if (name !== "left" && name !== "right" && name !== "middle") {
    throw new Error("Unknown button '" + spec + "'; use left, right, or middle.");
  }
  return name;
}

// Likewise for the click count: 4 is not a triple-click and must not be reported as one.
function clickCountOf(spec) {
  if (spec === undefined || spec === null || spec === "") { return 1; }
  const n = Number(spec);
  if (n !== 1 && n !== 2 && n !== 3) {
    throw new Error("click_count must be 1, 2, or 3.");
  }
  return n;
}

// A click at a CSS-pixel viewport point with button/count/modifiers. For clickCount > 1 the
// press/release pair repeats with an incrementing clickCount, which is how Chrome derives
// dblclick (2) and tripleclick (3). The agent cursor is moved there first.
async function clickAt(tabId, x, y, opts) {
  opts = opts || {};
  // Through the same refusing helpers its callers use, rather than a third private copy of the
  // rule that quietly answered an unnamed button with a left click.
  const button = mouseButton(opts.button);
  const mask = MOUSE_BUTTONS[button] || 1;
  const clickCount = clickCountOf(opts.clickCount);
  const modifiers = opts.modifiers || 0;
  await moveAgentCursor(tabId, x, y);
  await dispatchMouse(tabId, "mouseMoved", x, y, { modifiers });
  for (let c = 1; c <= clickCount; c++) {
    await dispatchMouse(tabId, "mousePressed", x, y, { button, buttons: mask, clickCount: c, modifiers });
    await dispatchMouse(tabId, "mouseReleased", x, y, { button, buttons: 0, clickCount: c, modifiers });
  }
}

async function leftClickAt(tabId, x, y) {
  await clickAt(tabId, x, y, {});
}

// Hit-test the point a ref click will land on: is the topmost element there the target (or a
// descendant of it), or is something else painted over it? Returns {occluded, by, unknown}.
// This is a GATE, not a warning: handleClick refuses on occluded, because DOM.getNodeForLocation
// honours pointer-events, so a node it names over the target is a node the user's own click
// would hit instead. A hit-test that could not be taken returns occluded with unknown:true --
// it proves nothing, and "not occluded" would assert exactly what was never established.
async function occlusionAt(tabId, x, y, targetBackendId) {
  try {
    const hit = await sendCdp(tabId, "DOM.getNodeForLocation",
      { x: Math.round(x), y: Math.round(y), includeUserAgentShadowDOM: false });
    const hitBackend = hit && hit.backendNodeId;
    if (typeof hitBackend !== "number" || hitBackend === targetBackendId) {
      return { occluded: false };
    }
    // The hit node may be a descendant of the target (e.g. clicking a button lands on its inner
    // <span>); that is not occlusion. Ask the target whether it contains the hit node.
    const targetObj = await resolveNodeObjectId(tabId, targetBackendId).catch(() => null);
    const hitResolved = await sendCdp(tabId, "DOM.resolveNode",
      { backendNodeId: hitBackend, objectGroup: CDP_OBJECT_GROUP }).catch(() => null);
    const hitObj = hitResolved && hitResolved.object && hitResolved.object.objectId;
    if (targetObj && hitObj) {
      const call = await sendCdp(tabId, "Runtime.callFunctionOn", {
        objectId: targetObj,
        functionDeclaration: "function(other){return this===other||this.contains(other);}",
        arguments: [{ objectId: hitObj }],
        returnByValue: true,
      });
      if (call && call.result && call.result.value === true) {
        return { occluded: false };
      }
    }
    const d = await sendCdp(tabId, "DOM.describeNode", { backendNodeId: hitBackend }).catch(() => null);
    const node = d && d.node;
    const tag = node ? String(node.nodeName || "").toLowerCase() : "";
    return { occluded: true, by: { tag, backendNodeId: hitBackend } };
  } catch (_e) {
    // The hit-test is the only evidence that the click will land on the intended element.
    // Reporting "not occluded" because the test itself failed asserts exactly what could not
    // be established, so an unusable hit-test blocks the click instead of waving it through.
    return { occluded: true, unknown: true, by: { tag: "", backendNodeId: null } };
  }
}

async function handleClick(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  const button = mouseButton(args.button);
  const clickCount = clickCountOf(args.click_count);
  const modifiers = parseModifiers(args.modifiers);
  const occ = await occlusionAt(tabId, point.x, point.y, args.backendNodeId);
  if (occ.occluded) {
    // DOM.getNodeForLocation honours pointer-events, so a node it returns over the target is a
    // node a real user click would hit instead. Dispatching anyway would drive a cookie banner
    // or modal the model cannot see and report ok:true for an action on the wrong element.
    throw new Error(
      occ.unknown
        ? "Could not verify what is at the element's click point; re-snapshot and try again."
        : "The element is covered by <" + (occ.by && occ.by.tag ? occ.by.tag : "another element") +
            ">; dismiss the overlay or use browser_js_click.");
  }
  await clickAt(tabId, point.x, point.y, { button, clickCount, modifiers });
  return { ok: true, x: Math.round(point.x), y: Math.round(point.y), button, click_count: clickCount };
}

// Move-only pointer over a target so :hover/mouseenter UI (menus, tooltips) reveals; the model
// then re-snapshots to read what appeared. Optional dwell lets hover-intent JS settle.
async function handleHover(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  await moveAgentCursor(tabId, point.x, point.y);
  await dispatchMouse(tabId, "mouseMoved", point.x, point.y, { modifiers: parseModifiers(args.modifiers) });
  const dwell = Math.min(5000, Math.max(0, Number(args.duration_ms) || 0));
  if (dwell > 0) { await new Promise((r) => setTimeout(r, dwell)); }
  return { ok: true, x: Math.round(point.x), y: Math.round(point.y) };
}

// Drag from a source (ref or x,y) to a target (ref or x,y) with interpolated moves so drag
// thresholds trigger. hold_ms pauses after press (long-press pickup for sortables/kanban).
async function handleDrag(tabId, args) {
  await ensureAttached(tabId);
  // Both endpoints take a ref, and this was the one ref-taking handler that never checked the
  // snapshot. Either ref alone is enough to land the drag on a tab or a document the model
  // never saw, so the gate is keyed on either being present -- matching the bridge, which
  // refuses a stale-snapshot command carrying ref OR to_ref.
  if (typeof args.backendNodeId === "number" || typeof args.to_backendNodeId === "number") {
    requireSnapshotTab(tabId);
  }
  const from = typeof args.backendNodeId === "number"
    ? await resolveActionPoint(tabId, args.backendNodeId)
    : { x: Number(args.from_x), y: Number(args.from_y) };
  const to = typeof args.to_backendNodeId === "number"
    ? await resolveActionPoint(tabId, args.to_backendNodeId)
    : { x: Number(args.to_x), y: Number(args.to_y) };
  if (![from.x, from.y, to.x, to.y].every(Number.isFinite)) {
    throw new Error("browser_drag needs a from (ref or from_x/from_y) and a to (to_ref or to_x/to_y).");
  }
  const steps = Math.min(60, Math.max(2, Number(args.steps) || 12));
  const hold = Math.min(5000, Math.max(0, Number(args.hold_ms) || 0));
  await moveAgentCursor(tabId, from.x, from.y);
  await dispatchMouse(tabId, "mouseMoved", from.x, from.y, {});
  await dispatchMouse(tabId, "mousePressed", from.x, from.y, { button: "left", buttons: 1, clickCount: 1 });
  // The button is DOWN from here. Anything that throws mid-drag would otherwise leave the page
  // holding a pressed button forever -- every later hover reads as a drag and the next click
  // completes a drag the model never asked for -- so the release always runs.
  // Release where the pointer actually got to, not at the intended destination: a drag that
  // failed halfway must not drop its payload on the target as though it had arrived.
  let atX = from.x;
  let atY = from.y;
  try {
    if (hold > 0) { await new Promise((r) => setTimeout(r, hold)); }
    for (let i = 1; i <= steps; i++) {
      const x = from.x + ((to.x - from.x) * i) / steps;
      const y = from.y + ((to.y - from.y) * i) / steps;
      await moveAgentCursor(tabId, x, y);
      await dispatchMouse(tabId, "mouseMoved", x, y, { button: "left", buttons: 1 });
      atX = x;
      atY = y;
    }
  } finally {
    await dispatchMouse(tabId, "mouseReleased", atX, atY, { button: "left", buttons: 0, clickCount: 1 })
      .catch(() => {});
  }
  return { ok: true, from: { x: Math.round(from.x), y: Math.round(from.y) }, to: { x: Math.round(to.x), y: Math.round(to.y) } };
}

async function handleClickAt(tabId, args) {
  await ensureAttached(tabId);
  const shot = lastShot;
  if (!shot || shot.tabId !== tabId) {
    throw new Error(
      "No current screenshot for the active tab; call browser_screenshot before " +
        "browser_click_at so the coordinates match what you see.");
  }
  if (shot.fullPage) {
    throw new Error(
      "The last screenshot was full-page (document coordinates); take a viewport " +
        "screenshot (full_page:false) before browser_click_at so x/y map to the visible page.");
  }
  const sx = Number(args.x);
  const sy = Number(args.y);
  if (!Number.isFinite(sx) || !Number.isFinite(sy) || sx < 0 || sy < 0) {
    throw new Error("browser_click_at needs non-negative x and y in screenshot pixels.");
  }
  // Fail CLOSED if the render moved since the screenshot: a dpr change would mis-scale the
  // conversion, a scroll would make viewport coordinates point elsewhere, a navigation (or a
  // same-URL reload, which only the DOM generation shows) would put them on a different
  // document, and a viewport resize would relay the page out under them. The coordinates are
  // only valid against the exact image the model measured.
  const now = await viewportState(tabId);
  if (!shotMatchesRender(shot, now)) {
    lastShot = null;
    throw new Error(
      "The page moved (scrolled, zoomed, resized, reloaded, or navigated) since the " +
        "screenshot; take a fresh browser_screenshot before browser_click_at.");
  }
  // Screenshot pixels are DEVICE pixels; convert to the CSS pixels CDP Input wants using the
  // dpr captured WITH the screenshot (not a re-read), so the two can never disagree.
  const button = mouseButton(args.button);
  const clickCount = clickCountOf(args.click_count);
  await clickAt(tabId, sx / shot.dpr, sy / shot.dpr, { button, clickCount, modifiers: parseModifiers(args.modifiers) });
  return { ok: true, x: Math.round(sx), y: Math.round(sy), button, click_count: clickCount };
}

// Is the ref'd node (or something inside it) what the page has focused RIGHT NOW? Asked on the
// ref-resolved node through a CONSTANT function body, so no page content is ever evaluated.
async function nodeHasFocus(tabId, backendNodeId) {
  const objectId = await resolveNodeObjectId(tabId, backendNodeId);
  const focusFn = function () {
    // Walk the focus chain from the outermost activeElement inward, testing at EVERY level.
    // Descending first and testing only the innermost gets delegatesFocus exactly backwards:
    // for <my-input> with attachShadow({delegatesFocus:true}), DOM.focus on the host succeeds
    // and document.activeElement IS the host, but the real focus sits on an input inside the
    // shadow root -- and Node.contains does NOT cross a shadow boundary, so host.contains(inner)
    // is false and the node we just focused would read as "focus moved away". Testing each
    // level also covers the mirror case, where the ref names a node inside the shadow root.
    var node = document.activeElement;
    while (node) {
      if (node === this || this.contains(node)) {
        return true;
      }
      if (node.shadowRoot && node.shadowRoot.activeElement) {
        node = node.shadowRoot.activeElement;
        continue;
      }
      return false;
    }
    return false;
  };
  return (await callOnNode(tabId, objectId, focusFn, [])) === true;
}

async function handleType(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const text = typeof args.text === "string" ? args.text : "";
  // A validated ref is required: typing into the page-focused element would let page
  // content (element.focus()/autofocus) redirect the model's text into a field the
  // bridge never chose. Focus the snapshot-resolved node, and abort (never silently type
  // into the wrong place) if that node cannot be focused.
  if (typeof args.backendNodeId !== "number") {
    throw new Error("This action needs a valid element ref from the latest snapshot.");
  }
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  await moveAgentCursor(tabId, point.x, point.y);
  try {
    await sendCdp(tabId, "DOM.focus", { backendNodeId: args.backendNodeId });
  } catch (_e) {
    throw new Error("Could not focus the target element (it may not be focusable); take a "
      + "fresh snapshot and target an input.");
  }
  // Input.insertText lands wherever focus IS, and the focus/focusin handler the DOM.focus above
  // just ran can move it (a page can call another field's focus() synchronously). The ref only
  // pins the target if the focus it asked for actually stuck, so it is re-read on the node
  // itself before a single character is inserted.
  if (!(await nodeHasFocus(tabId, args.backendNodeId))) {
    throw new Error("The page moved focus off the target element; take a fresh snapshot and "
      + "type into the element that is focused.");
  }
  await sendCdp(tabId, "Input.insertText", { text });
  if (args.submit === true) {
    await dispatchKey(tabId, KEY_DEFS.enter, 0);
  }
  return { ok: true, typed: text.length, submitted: args.submit === true };
}

// One CONSTANT function body covers single (value/label/index) and multi (values) selection;
// all match criteria arrive as CDP argument VALUES, never interpolated into code. It runs in the
// PAGE, serialized by source, so it closes over nothing here -- everything it needs is a
// parameter. Kept at module scope for that reason: nesting it would imply a closure it cannot have.
const selectOptionFn = function (mode, value, label, index, values) {
  var el = this;
  if (!el || el.tagName !== "SELECT") {
    return { ok: false, error: "the ref is not a <select> element" };
  }
  if (mode === "values") {
    if (!el.multiple) {
      return { ok: false, error: "the <select> is not a multiple-select; use value/label/index" };
    }
    // A keyed object literal inherits Object.prototype, so want["toString"] / want["constructor"]
    // / want["valueOf"] read back truthy for options that were never requested -- and option
    // values and labels are page-controlled data, so a page could have any of them selected by
    // any call. Match against the requested list itself: only what was asked for can hit.
    var want = [];
    var hit = [];
    for (var k = 0; k < values.length; k++) { want[k] = String(values[k]); hit[k] = false; }
    // Resolve every requested value BEFORE touching the control: a value naming an option this
    // <select> does not have was not honored, and half-applying the request would leave a
    // selection the caller never asked for while reporting a clean success.
    var take = [];
    for (var j = 0; j < el.options.length; j++) {
      var o = el.options[j];
      var at = -1;
      for (var w = 0; w < want.length; w++) {
        if (want[w] === o.value || want[w] === o.text) { at = w; break; }
      }
      if (at >= 0) { hit[at] = true; }
      take[j] = at >= 0;
    }
    var missing = "";
    for (k = 0; k < want.length; k++) {
      if (!hit[k]) { missing = missing ? missing + ", " + want[k] : want[k]; }
    }
    if (missing) { return { ok: false, error: "no option matched: " + missing }; }
    var picked = [];
    for (j = 0; j < el.options.length; j++) {
      el.options[j].selected = take[j];
      if (take[j]) { picked.push({ value: el.options[j].value, label: el.options[j].text }); }
    }
    el.dispatchEvent(new Event("input", { bubbles: true }));
    el.dispatchEvent(new Event("change", { bubbles: true }));
    return { ok: true, multiple: true, selected: picked };
  }
  var chosen = -1;
  for (var i = 0; i < el.options.length; i++) {
    var opt = el.options[i];
    if (mode === "value" && opt.value === value) { chosen = i; break; }
    if (mode === "label" && (opt.label === label || opt.text === label)) { chosen = i; break; }
    if (mode === "index" && i === index) { chosen = i; break; }
  }
  if (chosen < 0) { return { ok: false, error: "no option matched" }; }
  el.selectedIndex = chosen;
  el.dispatchEvent(new Event("input", { bubbles: true }));
  el.dispatchEvent(new Event("change", { bubbles: true }));
  return { ok: true, selectedIndex: chosen, value: el.value, label: el.options[chosen].text };
};

// The CDP argument values for selectOptionFn: the match mode plus one slot per criterion.
// Exactly one criterion, as the tool documents. Picking one by precedence would silently
// resolve a contradictory call (value:"a" with index:3) to whichever branch happens to win, and
// act on an option the caller did not unambiguously name.
// One row per criterion: present() tests whether the caller named it, row() is the CDP
// argument tuple when it is the chosen one. Table-driven so selectCallArgs stays a flat
// "exactly one must match" check instead of a chain of parallel ternaries (CCN gate).
const SELECT_CRITERIA = [
  { present: (a) => typeof a.value === "string" && a.value.length > 0,
    row: (a) => ["value", a.value, "", -1, []] },
  { present: (a) => typeof a.label === "string" && a.label.length > 0,
    row: (a) => ["label", "", a.label, -1, []] },
  { present: (a) => Number.isInteger(a.index),
    row: (a) => ["index", "", "", a.index, []] },
  { present: (a) => Array.isArray(a.values) && a.values.length > 0,
    row: (a) => ["values", "", "", -1, a.values.map(String)] },
];

function selectCallArgs(args) {
  const matched = SELECT_CRITERIA.filter((c) => c.present(args));
  if (matched.length === 0) {
    throw new Error("browser_select needs one of value, label, index, or values.");
  }
  if (matched.length > 1) {
    throw new Error("browser_select takes exactly one of value, label, index, or values.");
  }
  return matched[0].row(args);
}

// Select an <option> in a <select> by value, visible label, or index. The matching runs
// inside the page via Runtime.callFunctionOn on the ref-resolved node -- the function body
// is a CONSTANT string and value/label/index are passed as CDP argument VALUES (never
// interpolated into code), so there is no page-content injection surface.
async function handleSelect(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  if (typeof args.backendNodeId !== "number") {
    throw new Error("This action needs a valid <select> element ref from the latest snapshot.");
  }
  const callArgs = selectCallArgs(args);
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  await moveAgentCursor(tabId, point.x, point.y);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const result = await callOnNode(tabId, objectId, selectOptionFn, callArgs);
  if (!result || !result.ok) {
    throw new Error((result && result.error) || "browser_select failed");
  }
  if (result.multiple) {
    return { ok: true, multiple: true, selected: result.selected };
  }
  return { ok: true, selectedIndex: result.selectedIndex, value: result.value, label: result.label };
}

// Resolve a snapshot ref to a JS object handle for a constant callFunctionOn. Shared by the
// value/media setters, which act via a fixed function body (no page content is ever eval'd).
async function resolveNodeObjectId(tabId, backendNodeId) {
  if (typeof backendNodeId !== "number") {
    throw new Error("This action needs a valid element ref from the latest snapshot.");
  }
  const resolved = await sendCdp(tabId, "DOM.resolveNode",
    { backendNodeId, objectGroup: CDP_OBJECT_GROUP });
  const objectId = resolved && resolved.object && resolved.object.objectId;
  if (!objectId) {
    throw new Error("Could not resolve the element; take a fresh snapshot.");
  }
  return objectId;
}

// awaitPromise lets a caller whose function body returns a promise (media playback) have CDP
// resolve it before the result comes back, so the outcome is the real one and not "a promise was
// created". It stays opt-in: the other callers are synchronous and must not pay for it.
async function callOnNode(tabId, objectId, fn, args, awaitPromise) {
  const call = await sendCdp(tabId, "Runtime.callFunctionOn", {
    objectId,
    functionDeclaration: fn.toString(),
    arguments: (args || []).map((v) => ({ value: v })),
    returnByValue: true,
    awaitPromise: awaitPromise === true,
  });
  return call && call.result && call.result.value;
}

// Set a control's value or checked state and fire input/change. Works for range sliders,
// date/time/color/number inputs, checkboxes/radios, contenteditable, and hidden or custom
// controls (no visible box needed -- it acts on the ref'd node via a CONSTANT function; the
// value/checked come in as CDP argument values, never interpolated as code).
async function handleSetValue(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const hasChecked = typeof args.checked === "boolean";
  const hasValue = typeof args.value === "string";
  if (!hasChecked && !hasValue) {
    throw new Error("browser_set_value needs a value or checked.");
  }
  // Preferring one over the other would silently drop half of a contradictory request.
  if (hasChecked && hasValue) {
    throw new Error("browser_set_value takes either value or checked, not both.");
  }
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const point = await resolveActionPoint(tabId, args.backendNodeId).catch(() => null);
  if (point) { await moveAgentCursor(tabId, point.x, point.y); }
  const setFn = function (mode, value, checked) {
    var el = this;
    if (!el) { return { ok: false, error: "no element" }; }
    var type = (el.type || "").toLowerCase();
    if (mode === "checked") {
      // A checked request aimed at anything else is a mis-identified control (a text input, a
      // <select>, a custom element with a value property). Falling through to the value branch
      // would assign it the empty string this path carries -- wiping the control -- and report
      // success for a checkbox action that never happened.
      if (type !== "checkbox" && type !== "radio") {
        return { ok: false, error: "checked only applies to a checkbox or radio" };
      }
      el.checked = checked;
    } else if ("value" in el) {
      el.value = value;
    } else if (el.isContentEditable) {
      el.textContent = value;
    } else {
      return { ok: false, error: "element has no settable value" };
    }
    el.dispatchEvent(new Event("input", { bubbles: true }));
    el.dispatchEvent(new Event("change", { bubbles: true }));
    return { ok: true, value: "value" in el ? el.value : el.textContent || "", checked: !!el.checked };
  };
  const mode = hasChecked ? "checked" : "value";
  const r = await callOnNode(tabId, objectId, setFn, [mode, hasValue ? args.value : "", hasChecked ? args.checked : false]);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_set_value failed"); }
  return { ok: true, value: r.value, checked: r.checked };
}

// Control an <audio>/<video> element: play/pause/mute/unmute/seek/volume/rate. Returns the
// resulting media state. Constant function body; the numeric argument arrives as a value.
async function handleMedia(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const action = String(args.action || "").toLowerCase();
  const num = args.value !== undefined && args.value !== null && String(args.value) !== ""
    ? Number(args.value) : NaN;
  // A seek/volume/rate without a usable number is a request this cannot carry out. Doing nothing
  // and answering ok:true reports an action that never happened, and the caller is left believing
  // the media moved to a position it never went to. Checked before the page is touched at all.
  if ((action === "seek" || action === "volume" || action === "rate") && isNaN(num)) {
    throw new Error("browser_media " + action + " needs a numeric value.");
  }
  // Clamping is a silent substitution: volume 5 becomes 1 and is reported as done. The valid
  // range is the caller's to respect.
  if (action === "volume" && (num < 0 || num > 1)) {
    throw new Error("browser_media volume must be between 0 and 1.");
  }
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const mediaFn = async function (action, num) {
    var el = this;
    if (!el || (el.tagName !== "VIDEO" && el.tagName !== "AUDIO")) {
      return { ok: false, error: "the ref is not an <audio> or <video> element" };
    }
    try {
      // play() is asynchronous and REJECTS when the browser refuses it (autoplay policy, no user
      // gesture). Sampling el.paused right after the call reads the state before the refusal has
      // landed, so a blocked play reports paused:false -- playing -- for silence.
      if (action === "play") { await el.play(); }
      else if (action === "pause") { el.pause(); }
      else if (action === "mute") { el.muted = true; }
      else if (action === "unmute") { el.muted = false; }
      else if (action === "seek") { el.currentTime = num; }
      else if (action === "volume") { el.volume = num; }
      else if (action === "rate") { el.playbackRate = num; }
      else { return { ok: false, error: "unknown media action: " + action }; }
    } catch (e) { return { ok: false, error: String(e && e.message ? e.message : e) }; }
    return {
      ok: true, paused: el.paused, muted: el.muted, currentTime: el.currentTime,
      duration: isFinite(el.duration) ? el.duration : null, volume: el.volume, rate: el.playbackRate,
    };
  };
  const r = await callOnNode(tabId, objectId, mediaFn, [action, isNaN(num) ? 0 : num], true);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_media failed"); }
  return r;
}

// -- inspection + robustness (mostly read-only) ------------------------------

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// Does `selector` currently match an element in the tab? Injection-safe: the selector rides as
// a CDP DOM.querySelector PARAMETER, never interpolated into evaluated code. Throws on an
// invalid selector so a mistyped condition fails fast instead of polling until timeout.
async function selectorPresent(tabId, selector) {
  // Deep match that PIERCES open shadow DOM (plain DOM.querySelector stops at a shadow
  // boundary, so web-component content -- common on real sites -- would never be found). The
  // selector rides as a callFunctionOn ARGUMENT value, never interpolated into code. An invalid
  // selector throws inside querySelector; we surface that as an error so a mistyped wait fails
  // fast instead of silently polling until timeout.
  const docObj = await sendCdp(tabId, "Runtime.evaluate",
    { expression: "document", returnByValue: false, objectGroup: CDP_OBJECT_GROUP });
  const objectId = docObj && docObj.result && docObj.result.objectId;
  // No handle means the page could not be read at all (an execution context destroyed mid
  // navigation), which establishes nothing about the selector. Answering "not present" turns that
  // failed read into a fact -- and under `absent` that fact reads as the wait being satisfied.
  if (!objectId) {
    throw transientReadError("Could not read the page to test the selector; it may be navigating.");
  }
  const matchFn = function (sel) {
    function walk(root) {
      if (root.querySelector(sel)) { return true; }
      var all = root.querySelectorAll("*");
      for (var i = 0; i < all.length; i++) {
        var sr = all[i].shadowRoot;
        if (sr && walk(sr)) { return true; }
      }
      return false;
    }
    return walk(this);  // `this` is the document (the callFunctionOn objectId)
  };
  const call = await sendCdp(tabId, "Runtime.callFunctionOn", {
    objectId,
    functionDeclaration: matchFn.toString(),
    arguments: [{ value: selector }],
    returnByValue: true,
  });
  if (call && call.exceptionDetails) {
    throw new Error("Invalid selector for browser_wait_for: " + selector);
  }
  return !!(call && call.result && call.result.value === true);
}

// The page's visible text. A read that FAILED is not empty text: with `absent` set, "" satisfies
// the wait, so swallowing an "execution context was destroyed" would report a spinner as gone
// having never seen the page. The failure is the answer, and the caller can re-issue the wait.
async function bodyText(tabId) {
  const res = await sendCdp(tabId, "Runtime.evaluate", {
    expression: presenceFreeReadScript("document.body ? document.body.innerText : ''"),
    returnByValue: true,
  });
  if (!res || res.exceptionDetails || !res.result || typeof res.result.value !== "string") {
    throw transientReadError("Could not read the page text; it may be navigating.");
  }
  return res.result.value;
}

// A read that could not be taken because the page was mid-navigation. It is NOT an answer --
// under `absent` an unreadable page must never count as "the thing is gone" -- but it is also
// not a reason to abandon the wait: an execution context torn down by an ordinary navigation is
// exactly what a wait is usually sitting through. Marked so the poll loop can tell it apart
// from a caller error (a mistyped selector) that should fail immediately.
function transientReadError(message) {
  const error = new Error(message);
  error.transient = true;
  return error;
}

// A wait duration the caller stated, bounded by `cap`. `Number(x) || fallback` rewrites an
// explicit 0 -- "check once, do not wait" -- into the default, answering a different question
// than the one asked; and a negative or non-numeric duration is not a wait this can honor at all.
function waitDurationMs(raw, name, fallback, cap) {
  if (raw === undefined || raw === null) {
    return Math.min(cap, fallback);
  }
  const ms = Number(raw);
  if (!Number.isFinite(ms) || ms < 0) {
    throw new Error(
      "browser_wait_for " + name + " must be a non-negative number of milliseconds.");
  }
  return Math.min(cap, ms);
}

// Poll until a page condition holds or the timeout elapses. Read-only: it reads text/URL or
// checks element presence, never mutating the page. Exactly one of text / url_contains /
// selector; `absent` inverts the wait (gone instead of present). Text/selector conditions run
// injection-safely (text compared here, selector passed as a CDP param).
async function handleWaitFor(tabId, args) {
  await ensureAttached(tabId);
  // The generation this wait belongs to. Re-checked every poll so a superseded wait stops
  // driving the page instead of finishing into a connection that is already gone.
  const gen = commandGeneration;
  // The whole exchange lives under the bridge's single I/O deadline (30s); a wait allowed to
  // outlast it leaves the app reporting a transport reset while this loop keeps polling and
  // finally posts its reply into a dead relay. Stay inside the deadline so the timeout the
  // caller sees is this handler's honest "not satisfied", not a torn-down connection.
  const timeout = waitDurationMs(args.timeout_ms, "timeout_ms", 8000, 25000);
  const absent = args.absent === true;
  const text = typeof args.text === "string" && args.text.length > 0 ? args.text : null;
  const urlSub = typeof args.url_contains === "string" && args.url_contains.length > 0 ? args.url_contains : null;
  const selector = typeof args.selector === "string" && args.selector.length > 0 ? args.selector : null;
  const netIdle = args.network_idle === true;
  if ((text ? 1 : 0) + (urlSub ? 1 : 0) + (selector ? 1 : 0) + (netIdle ? 1 : 0) !== 1) {
    throw new Error(
      "browser_wait_for needs exactly one of text, url_contains, selector, or network_idle.");
  }
  // network_idle: no in-flight requests for a quiet window (idle_ms). Not subject to `absent`.
  const idleMs = waitDurationMs(args.idle_ms, "idle_ms", 500, 30000);
  const holds = async () => {
    if (netIdle) {
      // The predicate is read entirely off the Network domain's events. Without the domain
      // enabled no event ever fires, so the set stays empty and the timestamp stays at attach
      // time: the first poll would report a quiet page on the strength of no observation at all.
      if (!networkInstrumented) {
        throw new Error(
          "network_idle is unavailable: the Network domain is not instrumenting this tab.");
      }
      return inflightRequests.size === 0 && Date.now() - lastNetworkActivityMs >= idleMs;
    }
    let hit;
    try {
      if (text) { hit = (await bodyText(tabId)).includes(text); }
      else if (urlSub) { hit = (await tabInfo(tabId)).url.includes(urlSub); }
      else { hit = await selectorPresent(tabId, selector); }
    } catch (e) {
      // A page that could not be read this poll establishes NOTHING, so the condition does not
      // hold -- in either polarity. Returning false rather than !hit is the point: under
      // `absent`, treating an unreadable page as "the element is gone" would satisfy the wait on
      // the strength of a failed observation. The loop simply tries again, and if the page never
      // becomes readable the wait ends as an honest timed-out/not-satisfied. A caller error --
      // an invalid selector -- is not transient and still fails immediately.
      if (e && e.transient === true) { return false; }
      throw e;
    }
    return absent ? !hit : hit;
  };
  const started = Date.now();
  for (;;) {
    if (gen !== commandGeneration) {
      throw new Error("The wait was superseded by a newer command (or the tab session ended).");
    }
    if (await holds()) {
      return { ok: true, satisfied: true, waited_ms: Date.now() - started };
    }
    if (Date.now() - started >= timeout) {
      return { ok: true, satisfied: false, timed_out: true, waited_ms: Date.now() - started };
    }
    await delay(250);
  }
}

// Read the live state of a control by ref (value, checked, selected options, contenteditable
// text, disabled). Read-only; constant function body. Everything read here is page-controlled
// data of unbounded size, so each string and the option list are capped -- and a cap that BIT is
// reported, because a silently truncated value reads as the whole value to the model.
async function handleGetValue(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const getFn = function () {
    var el = this;
    if (!el) { return { ok: false, error: "no element" }; }
    var cap = function (s, n) { return s.length > n ? s.slice(0, n) : s; };
    var out = { ok: true, tag: String(el.tagName || "").toLowerCase() };
    if ("value" in el) {
      var v = String(el.value);
      out.value = cap(v, 5000);
      if (v.length > 5000) { out.value_truncated = true; }
    }
    if (typeof el.checked === "boolean") { out.checked = el.checked; }
    if (el.tagName === "SELECT") {
      out.multiple = !!el.multiple;
      var picked = el.selectedOptions || [];
      out.selected = Array.prototype.slice.call(picked, 0, 200).map(function (o) {
        return { value: cap(String(o.value), 2048), label: cap(String(o.text), 2048) };
      });
      out.selected_capped = picked.length > 200;
    }
    if (el.isContentEditable) {
      var t = String(el.textContent || "");
      out.text = cap(t, 5000);
      if (t.length > 5000) { out.text_truncated = true; }
    }
    out.disabled = !!el.disabled;
    return out;
  };
  const r = await callOnNode(tabId, objectId, getFn, []);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_get_value failed"); }
  return r;
}

// Read one attribute (name given) or all attributes of a ref. Read-only; values and the attribute
// LIST are page-derived and unbounded (hundreds of data-* attributes, a 2 MB data: URI in src), so
// both are capped -- and a cap that bit is reported, because a href cut at exactly 2048 chars
// reads back as a complete URL the model may then navigate to.
async function handleGetAttribute(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const name = typeof args.name === "string" ? args.name : "";
  const attrFn = function (name) {
    var el = this;
    if (!el || !el.getAttribute) { return { ok: false, error: "element has no attributes" }; }
    var cap = function (v) { return String(v).slice(0, 2048); };
    var bit = function (v) { return String(v).length > 2048; };
    if (name) {
      var one = el.getAttribute(name);
      if (one === null) { return { ok: true, name: name, value: null }; }
      var out = { ok: true, name: name, value: cap(one) };
      if (bit(one)) { out.value_truncated = true; }
      return out;
    }
    var all = {};
    var cut = [];
    var total = el.attributes.length;
    var shown = total > 200 ? 200 : total;
    for (var i = 0; i < shown; i++) {
      var a = el.attributes[i];
      // defineProperty, not all[a.name] = v: an attribute named __proto__ is a legal attribute,
      // and plain assignment of a string to it sets nothing at all. The attribute would then be
      // missing from a listing whose count still claims it -- the page choosing which of its own
      // attributes the model gets to see.
      Object.defineProperty(all, a.name,
        { value: cap(a.value), writable: true, enumerable: true, configurable: true });
      if (bit(a.value)) { cut.push(a.name); }
    }
    return { ok: true, attributes: all, count: total, capped: total > shown,
             truncated_attributes: cut };
  };
  const r = await callOnNode(tabId, objectId, attrFn, [name]);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_get_attribute failed"); }
  return r;
}

// The element's content quad, or null when the page genuinely has no box for it (display:none,
// an unrendered subtree). DOM.getBoxModel also fails for reasons that are NOT facts about the
// layout -- a ref that no longer names a node, a dead session, a transport error -- and those
// must surface as the errors they are: reporting laid_out:false for them tells the model the
// element exists but is hidden, when what actually happened is that nothing could be read.
async function boxModelQuad(tabId, backendNodeId) {
  let model;
  try {
    model = await sendCdp(tabId, "DOM.getBoxModel", { backendNodeId });
  } catch (e) {
    const m = String(e && e.message ? e.message : e);
    if (/could not compute box model/i.test(m)) {
      return null;
    }
    throw new Error("The element's box could not be read (" + m + "); take a fresh snapshot.");
  }
  const quad = model && model.model && model.model.content;
  return quad && quad.length >= 8 ? quad : null;
}

// The axis-aligned CSS-pixel rectangle a CDP content quad covers.
function quadRect(quad) {
  const xs = [quad[0], quad[2], quad[4], quad[6]];
  const ys = [quad[1], quad[3], quad[5], quad[7]];
  const left = Math.min.apply(null, xs), top = Math.min.apply(null, ys);
  const right = Math.max.apply(null, xs), bottom = Math.max.apply(null, ys);
  return { x: Math.round(left), y: Math.round(top),
           width: Math.round(right - left), height: Math.round(bottom - top) };
}

// Report a ref's geometry, whether it is inside the viewport, and whether an overlay covers its
// center (occlusion). Read-only; scrolls-into-view are not performed here so the box reflects
// where the element actually sits right now.
async function handleBox(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  if (typeof args.backendNodeId !== "number") {
    throw new Error("This action needs a valid element ref from the latest snapshot.");
  }
  const quad = await boxModelQuad(tabId, args.backendNodeId);
  if (!quad) { return { ok: true, laid_out: false }; }
  const xs = [quad[0], quad[2], quad[4], quad[6]];
  const ys = [quad[1], quad[3], quad[5], quad[7]];
  const left = Math.min.apply(null, xs), top = Math.min.apply(null, ys);
  const right = Math.max.apply(null, xs), bottom = Math.max.apply(null, ys);
  const center = quadCenter(quad);
  const out = {
    ok: true, laid_out: true,
    x: Math.round(left), y: Math.round(top),
    width: Math.round(right - left), height: Math.round(bottom - top),
    center: { x: Math.round(center.x), y: Math.round(center.y) },
  };
  const vp = await viewportState(tabId);
  const metrics = await sendCdp(tabId, "Page.getLayoutMetrics", {}).catch(() => null);
  const view = metrics && (metrics.cssVisualViewport || metrics.visualViewport);
  if (view) {
    const vw = view.clientWidth || 0, vh = view.clientHeight || 0;
    out.in_viewport = left >= 0 && top >= 0 && right <= vw && bottom <= vh;
  } else {
    out.viewport_unknown = true;
  }
  // viewportState's contract: a failed read returns placeholders behind ok:false, so the dpr is
  // only a real measurement when ok. Emitting the placeholder 1 would state a scale factor the
  // page never reported, which a caller converting device pixels would act on.
  if (vp.ok) { out.dpr = vp.dpr; } else { out.dpr_unknown = true; }
  const occ = await occlusionAt(tabId, center.x, center.y, args.backendNodeId);
  out.occluded = occ.occluded;
  if (occ.unknown) { out.occlusion_unknown = true; }
  if (occ.occluded) { out.occluded_by = occ.by; }
  return out;
}

// Focus a ref (fires the page's focus events, like a real tab-into). Pointer-adjacent and
// non-destructive; used to prepare an element for browser_press_key without a click.
async function handleFocus(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  if (typeof args.backendNodeId !== "number") {
    throw new Error("This action needs a valid element ref from the latest snapshot.");
  }
  try {
    await sendCdp(tabId, "DOM.focus", { backendNodeId: args.backendNodeId });
  } catch (_e) {
    throw new Error("Could not focus the element (it may not be focusable); take a fresh snapshot.");
  }
  // DOM.focus resolving is not the element HAVING focus: the focus/focusin handler it just ran
  // can move focus elsewhere synchronously. Reporting ok on the call alone is what would send the
  // next browser_press_key into a control the model never chose, so the post-condition is read
  // back on the node itself.
  if (!(await nodeHasFocus(tabId, args.backendNodeId))) {
    throw new Error("The page moved focus off the element (or it cannot hold focus); take a "
      + "fresh snapshot and target the element that is focused.");
  }
  return { ok: true, focused: true };
}

// Scroll a ref into view (bring an off-screen element on-screen before a screenshot or
// coordinate click). Viewport-only change, like browser_scroll.
async function handleReveal(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  if (typeof args.backendNodeId !== "number") {
    throw new Error("This action needs a valid element ref from the latest snapshot.");
  }
  try {
    await sendCdp(tabId, "DOM.scrollIntoViewIfNeeded", { backendNodeId: args.backendNodeId });
  } catch (_e) {
    throw new Error("The element is not laid out (hidden or gone); take a fresh snapshot.");
  }
  const point = await resolveActionPoint(tabId, args.backendNodeId).catch(() => null);
  if (point) { await moveAgentCursor(tabId, point.x, point.y); }
  // This tool exists to GUARANTEE the element is on screen before a screenshot or a coordinate
  // click, so the reply carries where it actually landed. A bare ok would be the caller's own
  // request echoed back, with nothing to check the next coordinate against.
  const quad = await boxModelQuad(tabId, args.backendNodeId);
  if (!quad) { return { ok: true, laid_out: false }; }
  return { ok: true, laid_out: true, box: quadRect(quad) };
}

// NOTE: a dedicated file-upload tool is intentionally absent. CDP DOM.setFileInputFiles is
// blocked ("Not allowed") for chrome.debugger extension sessions -- Chrome forbids programmatic
// local-file selection through an extension, which is the exact exfiltration vector such a tool
// would create. File uploads are instead done by composition: browser_click the file input to
// open the native Windows chooser, then drive that dialog with the win32 UIA tools.

// Programmatic el.click() in-page, as a fallback when a real pointer click cannot land (target
// occluded by an overlay, zero-box but present, or a control that ignores synthetic pointer
// events). GATED like browser_click. Constant function body.
async function handleJsClick(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const point = await resolveActionPoint(tabId, args.backendNodeId).catch(() => null);
  if (point) { await moveAgentCursor(tabId, point.x, point.y); }
  const clickFn = function () {
    var el = this;
    if (!el || typeof el.click !== "function") { return { ok: false, error: "element is not clickable" }; }
    // click() on a DISABLED control dispatches nothing at all, so returning ok would report a
    // click no handler ever saw -- and a submit button gated until a form validates is exactly
    // where the model would believe it had acted.
    if (el.disabled === true) {
      return { ok: false, error: "the element is disabled; nothing was clicked" };
    }
    el.click();
    return { ok: true, tag: String(el.tagName || "").toLowerCase().slice(0, 40) };
  };
  const r = await callOnNode(tabId, objectId, clickFn, []);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_js_click failed"); }
  return { ok: true, tag: r.tag || "" };
}

// The definition for one key token, under the modifiers it is pressed with. Shift is part of the
// definition rather than of dispatch alone: a printable key held with Shift produces its SHIFTED
// character, and emitting the raw token as `text` types "1" for "Shift+1" or a lower-case letter
// for "Shift+a" -- the wrong character, reported as the chord that was asked for.
function keyDefinition(token, modifiers) {
  const named = KEY_DEFS[token.toLowerCase()];
  if (named) {
    return named;
  }
  const shifted = ((modifiers || 0) & MODIFIER_BITS.shift) !== 0;
  if (token.length === 1) {
    const upper = token.toUpperCase();
    if (/[A-Z]/.test(upper)) {
      // key casing follows the (separate) Shift modifier; shortcut matching uses
      // code/keyCode + modifiers, so emit lower-case key to keep DOM state consistent.
      return { key: token.toLowerCase(), code: "Key" + upper, keyCode: upper.charCodeAt(0),
        text: shifted ? upper : token };
    }
    // A token that is ALREADY the shifted glyph ("?" rather than "/") has no further shifted
    // form, so it stands for itself.
    const text = shifted ? SHIFTED_KEYS[token] || token : token;
    if (/[0-9]/.test(upper)) {
      return { key: token, code: "Digit" + upper, keyCode: upper.charCodeAt(0), text };
    }
    const oem = OEM_KEYS[token];
    if (oem) {
      return { key: token, code: oem[0], keyCode: oem[1], text };
    }
  }
  throw new Error("Unsupported key: " + token);
}

function parseChord(keys) {
  // '+' is a key in its own right ("+" alone, "Control++"), and splitting on '+' leaves it as an
  // empty trailing token. Dropping empty tokens would lose the key entirely -- or, worse, promote
  // the last MODIFIER into the key slot and press something the caller never named.
  const parts = String(keys || "").split("+");
  let keyToken = String(parts.pop()).trim();
  if (keyToken === "" && parts.length > 0 && parts[parts.length - 1] === "") {
    parts.pop();
    keyToken = "+";
  }
  if (keyToken === "") {
    throw new Error('pressKey needs a key or chord, e.g. "Enter" or "Control+A".');
  }
  let modifiers = 0;
  for (const mod of parts) {
    const bit = MODIFIER_BITS[String(mod).trim().toLowerCase()];
    if (!bit) {
      throw new Error("Unknown modifier: " + mod);
    }
    modifiers |= bit;
  }
  return { modifiers, def: keyDefinition(keyToken, modifiers) };
}

async function dispatchKey(tabId, def, modifiers) {
  const base = {
    modifiers,
    key: def.key,
    code: def.code,
    windowsVirtualKeyCode: def.keyCode,
    nativeVirtualKeyCode: def.keyCode,
  };
  const down = Object.assign({ type: "keyDown" }, base);
  // A keyDown with `text` fires keypress/char: this is what makes Enter's implicit form
  // submit work and makes a printable key actually insert its character. Suppress it under
  // Ctrl/Alt/Meta so chords (Control+A) stay edit commands rather than typing a char.
  const nonShift = MODIFIER_BITS.alt | MODIFIER_BITS.control | MODIFIER_BITS.meta;
  if (def.text && (modifiers & nonShift) === 0) {
    down.text = def.text;
  }
  await sendCdp(tabId, "Input.dispatchKeyEvent", down);
  await sendCdp(tabId, "Input.dispatchKeyEvent", Object.assign({ type: "keyUp" }, base));
}

// Where a global key press is about to land: the page's view of document.activeElement, reduced
// to a short descriptor. This is REPORTING, not a boundary (the page owns its focus and can move
// it), and it exists so a key that went somewhere the model did not expect is visible in the
// reply instead of invisible. A failed read is not reported as "nothing focused" -- it fails the
// command, because an unattributable key press is exactly what this is meant to prevent.
async function focusedElementInfo(tabId) {
  const res = await sendCdp(tabId, "Runtime.evaluate", {
    expression:
      "(function(){var e=document.activeElement;" +
      "while(e&&e.shadowRoot&&e.shadowRoot.activeElement){e=e.shadowRoot.activeElement;}" +
      "if(!e){return null;}" +
      "return {tag:String(e.tagName||'').toLowerCase().slice(0,40)," +
      "id:String(e.id||'').slice(0,120),name:String(e.name||'').slice(0,120)," +
      "type:String(e.type||'').slice(0,40)};})()",
    returnByValue: true,
  });
  const value = res && res.result ? res.result.value : undefined;
  if (value === undefined) {
    throw new Error("Could not read which element has focus; take a fresh snapshot.");
  }
  return value;
}

async function handlePressKey(tabId, args) {
  await ensureAttached(tabId);
  const { modifiers, def } = parseChord(args.keys);
  // The key goes to whatever the page has focused -- which the page itself can move between a
  // browser_click/browser_focus and this call. Read the target first so the reply says WHERE the
  // key went, not only what was sent.
  const focus = await focusedElementInfo(tabId);
  await dispatchKey(tabId, def, modifiers);
  return { ok: true, keys: String(args.keys), focus };
}

// How far to scroll. Coercing a malformed distance to the default scrolls the page by an amount
// nobody asked for and then reports that as the scroll that was requested.
function scrollAmountPx(raw) {
  if (raw === undefined || raw === null) {
    return DEFAULT_SCROLL_PX;
  }
  const px = Number(raw);
  if (!Number.isFinite(px) || px <= 0) {
    throw new Error("browser_scroll amount must be a positive number of pixels.");
  }
  return px;
}

// Which way to scroll. An exact-match test against "up" makes every other spelling -- "Up", "UP",
// and any word that is not a direction at all -- scroll DOWN, which for "Up" is the literal
// opposite of what was asked for.
function scrollDirection(raw) {
  if (raw === undefined || raw === null) {
    return "down";
  }
  const dir = String(raw).trim().toLowerCase();
  if (dir !== "up" && dir !== "down") {
    throw new Error('browser_scroll direction must be "up" or "down".');
  }
  return dir;
}

async function handleScroll(tabId, args) {
  await ensureAttached(tabId);
  const amount = scrollAmountPx(args.amount);
  const deltaY = scrollDirection(args.direction) === "up" ? -amount : amount;
  let point;
  if (typeof args.backendNodeId === "number") {
    requireSnapshotTab(tabId);
    point = await resolveActionPoint(tabId, args.backendNodeId);
  } else {
    // The point decides WHICH scroller receives the wheel event, so a made-up one scrolls a
    // container the caller never named. A viewport that cannot be read is not a viewport of
    // 800x600 sitting at (200,200); it is an unknown, and the scroll cannot be placed.
    const metrics = await sendCdp(tabId, "Page.getLayoutMetrics", {});
    const vp = metrics && (metrics.cssVisualViewport || metrics.visualViewport);
    if (!vp || !(vp.clientWidth > 0) || !(vp.clientHeight > 0)) {
      throw new Error("Could not read the viewport to place the scroll; take a fresh snapshot.");
    }
    point = { x: vp.clientWidth / 2, y: vp.clientHeight / 2 };
  }
  await moveAgentCursor(tabId, point.x, point.y);
  await dispatchMouse(tabId, "mouseWheel", point.x, point.y, { deltaX: 0, deltaY });
  return { ok: true, deltaY };
}

// -- dialogs -----------------------------------------------------------------
//
// Dialogs are answered automatically by the onEvent handler above so the page never wedges.
// This tool ARMS the response for the NEXT dialog (one-shot) -- use action "accept" before
// the action that pops a confirm()/prompt() you want accepted (with optional text for a
// prompt) -- and reports the last dialog the extension handled. An arm is bound to the document
// it was armed against, so an embedded third-party frame cannot consume it.

// Is the frame that opened a dialog the document the response was armed against? A real origin
// compares as an origin (a same-origin subframe is still the same document's script); an opaque
// origin has none, so the exact document URL must match instead.
function dialogSourceMatches(frameUrl, armedOrigin) {
  if (!frameUrl || !armedOrigin) {
    return false;
  }
  const frameOrigin = originOf(frameUrl);
  return frameOrigin ? frameOrigin === armedOrigin : frameUrl === armedOrigin;
}

async function handleDialog(tabId, args) {
  await ensureAttached(tabId);
  // "dismiss" is the documented default, but an unrecognized action is a request we do not
  // understand -- answering it as a dismiss would silently substitute a different answer.
  const action = args && args.action !== undefined && args.action !== null
    ? String(args.action) : "dismiss";
  if (action !== "accept" && action !== "dismiss") {
    throw new Error("browser_dialog action must be \"accept\" or \"dismiss\".");
  }
  const text = args && typeof args.text === "string" ? args.text : undefined;
  // Bind the arm to the document being driven so a third-party iframe in the same tab cannot
  // consume it (see dialogSourceMatches).
  const info = await tabInfo(tabId);
  if (!info.url) {
    throw new Error("browser_dialog could not identify the tab's document to bind the response "
      + "to; take a fresh snapshot.");
  }
  pendingDialogPolicy = { accept: action === "accept", text, origin: originOf(info.url) || info.url };
  return {
    ok: true,
    armed: action,
    last_dialog: lastDialog,
  };
}

// -- navigation + tabs (via chrome.tabs, no debugger banner) ------------------

function normalizeUrl(raw) {
  const url = String(raw || "").trim();
  // Only http(s): never let the model drive the tab to javascript:/data:/file:.
  if (/^https?:\/\//i.test(url)) {
    return url;
  }
  if (/^[\w.-]+\.[a-z]{2,}(\/|$|:)/i.test(url)) {
    return "https://" + url;
  }
  return null;
}

// Whether a tab read AFTER the listener attached shows the requested navigation already finished.
// "complete" on its own is not enough: between asking for a navigation and reading the tab, the
// browser may not have started it yet, and the old document is "complete" too -- settling on that
// reports the page the caller navigated AWAY from as the loaded new one. So a pending navigation
// disqualifies the read, and the url must no longer be the pre-navigation one. priorUrl null means
// a freshly created tab, which has no previous document to be confused with; there any non-empty
// url is the requested navigation having landed.
function tabSettledAt(tab, priorUrl) {
  if (!tab || tab.status !== "complete" || tab.pendingUrl) {
    return false;
  }
  if (priorUrl === null) {
    return typeof tab.url === "string" && tab.url.length > 0;
  }
  // The previous document could not be read, so "this is no longer it" cannot be established.
  // The listener still covers the load; only this shortcut is withheld.
  if (typeof priorUrl !== "string") {
    return false;
  }
  return tab.url !== priorUrl;
}

// Wait for a navigation just requested on tabId to finish. priorUrl is the url the tab showed
// before the request (null for a tab created for this navigation); see tabSettledAt.
//
// Resolves TRUE when the tab reported a completed load, FALSE when the navigation timeout ran out
// first. The two outcomes are not the same fact, and a caller that cannot tell them apart reports
// a load that never finished exactly like one that did. Rejects when the tab itself cannot be
// read, which is neither outcome.
function waitForComplete(tabId, priorUrl) {
  return new Promise((resolve, reject) => {
    let done = false;
    // The deadline timer is held so a completed load can cancel it. Left running, every navigate,
    // back/forward and reload pins its listener and this closure alive for the rest of the
    // 15 s window -- in a service worker that is also a keepalive nothing asked for.
    let timer = null;
    const settle = (fn, value) => {
      if (done) {
        return;
      }
      done = true;
      if (timer !== null) {
        clearTimeout(timer);
        timer = null;
      }
      chrome.tabs.onUpdated.removeListener(listener);
      fn(value);
    };
    const finish = (completed) => settle(resolve, completed);
    const listener = (id, info) => {
      if (id === tabId && info.status === "complete") {
        finish(true);
      }
    };
    chrome.tabs.onUpdated.addListener(listener);
    timer = setTimeout(() => finish(false), NAV_TIMEOUT_MS);
    // A load that finished between the caller requesting it and this listener attaching fires no
    // further event, so the listener alone would sit out the whole 15 s window and then report
    // load_complete:false for a page that IS loaded -- a false statement about the page, not a
    // slow one. Reading the tab once the listener is attached closes that window from the other
    // side: the listener covers everything completing from here on, this read covers anything
    // that completed before it, and neither can miss it.
    chrome.tabs.get(tabId).then(
      (tab) => {
        if (tabSettledAt(tab, priorUrl)) {
          finish(true);
        }
      },
      // A tab that cannot be read (closed mid-navigation) will never complete either. Waiting out
      // the deadline to then call it "did not finish loading" describes the page; the tab being
      // gone is what actually happened, so it is what the caller is told.
      (e) => settle(reject, e instanceof Error ? e : new Error(String(e))));
  });
}

async function handleNavigate(tabId, args) {
  const url = normalizeUrl(args && args.url);
  if (!url) {
    throw new Error("navigate requires an http(s) URL.");
  }
  // Read the outgoing document before asking for the new one, so the wait can tell "the new page
  // is already up" from "the old page is still up because the navigation has not started".
  const before = await chrome.tabs.get(tabId);
  await chrome.tabs.update(tabId, { url });
  // load_complete carries whether the page actually finished loading. Without it a hung or very
  // slow navigation is indistinguishable from a completed one, and the model reads a partial
  // page as the final one.
  const loaded = await waitForComplete(tabId, before.url);
  const info = await tabInfo(tabId);
  return { ok: true, url: info.url, title: info.title, load_complete: loaded };
}

async function handleHistory(direction) {
  const tab = await activeTab();
  if (direction === "back") {
    await chrome.tabs.goBack(tab.id);
  } else {
    await chrome.tabs.goForward(tab.id);
  }
  const loaded = await waitForComplete(tab.id, tab.url);
  const info = await tabInfo(tab.id);
  return { ok: true, url: info.url, title: info.title, load_complete: loaded };
}

async function handleReload() {
  const tab = await activeTab();
  await chrome.tabs.reload(tab.id);
  // A reload lands on the same url it started from, so the pre-navigation url never stops
  // matching and the wait rests entirely on the completion event -- which a reload always fires.
  const loaded = await waitForComplete(tab.id, tab.url);
  const info = await tabInfo(tab.id);
  return { ok: true, url: info.url, title: info.title, load_complete: loaded };
}

// Caps for the tab listing. Titles and URLs are page-controlled (history.pushState rewrites both
// at will) and this tool is read-only, so its reply reaches the model with no confirmation: an
// uncapped listing puts unbounded page-chosen strings into that context and can outgrow the
// relay's frame. The per-string caps mirror kMaxUrlChars/kMaxTitleChars on the C++ side.
const MAX_LIST_TABS = 100;
const MAX_TAB_TITLE_CHARS = 300;
const MAX_TAB_URL_CHARS = 2048;

// Write one capped, page-controlled string onto an entry, marking the cut when it bites -- a
// silently sliced title or URL reads to the model as the whole thing.
function capField(entry, key, raw, cap) {
  const s = String(raw || "");
  entry[key] = s.length > cap ? s.slice(0, cap) : s;
  if (s.length > cap) {
    entry[key + "_truncated"] = true;
  }
}

async function handleListTabs() {
  const tabs = (await chrome.tabs.query({ lastFocusedWindow: true })).sort((a, b) => a.index - b.index);
  const groupTitles = new Map();
  const list = [];
  for (const t of tabs.slice(0, MAX_LIST_TABS)) {
    const entry = { index: t.index, id: t.id, active: !!t.active };
    capField(entry, "title", t.title, MAX_TAB_TITLE_CHARS);
    capField(entry, "url", t.url, MAX_TAB_URL_CHARS);
    if (typeof t.groupId === "number" && t.groupId >= 0) {
      entry.group_id = t.groupId;
      if (!groupTitles.has(t.groupId)) {
        try { const g = await chrome.tabGroups.get(t.groupId); groupTitles.set(t.groupId, g.title || ""); } catch (_e) { groupTitles.set(t.groupId, ""); }
      }
      capField(entry, "group", groupTitles.get(t.groupId), MAX_TAB_TITLE_CHARS);
    }
    list.push(entry);
  }
  // Remember which tab each index named, so an index-addressed action can prove it still names
  // that same tab, in that same window, before it acts. Only the LISTED tabs are remembered: an
  // index past the cap was never shown to the model, so it names nothing it could have confirmed.
  lastTabListing = {
    windowId: tabs.length ? tabs[0].windowId : null,
    entries: list.map((e) => ({ index: e.index, id: e.id })),
  };
  return { tabs: list, count: tabs.length, capped: tabs.length > MAX_LIST_TABS };
}

// An index-addressed tab action is meaningful only against the listing the model read. A tab id
// is the tab's stable identity for its whole lifetime, so compare THAT: if the tab now sitting
// at the index is a different tab -- or the query resolved a different window than the one that
// was listed -- the index no longer names what the operator confirmed.
function requireListedTab(tab) {
  if (!lastTabListing || lastTabListing.windowId !== tab.windowId) {
    throw new Error(
      "There is no current tab listing for this window; call browser_tabs before acting on a " +
        "tab index.");
  }
  const listed = lastTabListing.entries.find((e) => e.index === tab.index);
  if (!listed || typeof tab.id !== "number" || listed.id !== tab.id) {
    throw new Error(
      "The tabs moved since the last listing (index " + tab.index + " is a different tab now); " +
        "call browser_tabs again before acting on a tab index.");
  }
}

const GROUP_COLORS = new Set(["grey", "blue", "red", "yellow", "green", "pink", "purple", "cyan", "orange"]);

// Resolve a comma-separated zero-based tab-index spec to tab ids; default to the active tab.
async function tabIdsFromIndices(spec) {
  const tabs = await chrome.tabs.query({ lastFocusedWindow: true });
  if (spec === undefined || spec === null || String(spec).trim() === "") {
    const active = tabs.find((t) => t.active);
    if (!active) { throw new Error("No active tab to group."); }
    return [active.id];
  }
  // Every token has to name a tab. Dropping the ones that do not parse would act on a SUBSET of
  // what was confirmed, and an empty token is worse than that: Number("") is 0, so "0,,1" or a
  // trailing comma would fabricate tab index 0 -- a tab the caller never named.
  const indices = String(spec).split(",").map((token) => {
    const trimmed = token.trim();
    const n = Number(trimmed);
    if (trimmed === "" || !Number.isInteger(n) || n < 0) {
      throw new Error(
        "tab_indices must be comma-separated non-negative integers, e.g. \"0,1\" (got \"" +
          trimmed + "\").");
    }
    return n;
  });
  const ids = [];
  for (const idx of indices) {
    const t = tabs.find((x) => x.index === idx);
    if (!t) { throw new Error("No tab at index " + idx + "."); }
    requireListedTab(t);
    ids.push(t.id);
  }
  return ids;
}

async function handleGroupTabs(args) {
  const tabIds = await tabIdsFromIndices(args && args.tab_indices);
  const groupId = await chrome.tabs.group({ tabIds });
  lastTabListing = null;  // grouping moves tabs together: every index after this is unproven
  const update = {};
  if (args && typeof args.title === "string" && args.title.length > 0) { update.title = args.title; }
  // A color Chrome does not know is not a color it silently keeps: ignoring it would leave the
  // group whatever shade Chrome picked while the caller believes it named one.
  if (args && typeof args.color === "string") {
    if (!GROUP_COLORS.has(args.color)) {
      throw new Error("browser_group_tabs color must be one of: " +
        Array.from(GROUP_COLORS).join(", ") + ".");
    }
    update.color = args.color;
  }
  if (Object.keys(update).length > 0) { await chrome.tabGroups.update(groupId, update); }
  // The group's title/color are reported as BROWSER state, so they have to come from the browser.
  // Falling back to the requested values on a failed read would echo the request back as if it
  // had been observed, which is the one thing a read-back exists to rule out.
  const group = await chrome.tabGroups.get(groupId);
  return { ok: true, group_id: groupId, title: group.title || "", color: group.color || "", tab_count: tabIds.length };
}

async function handleUngroupTabs(args) {
  const tabIds = await tabIdsFromIndices(args && args.tab_indices);
  await chrome.tabs.ungroup(tabIds);
  lastTabListing = null;  // ungrouping moves tabs out of the group: the indices are unproven
  return { ok: true, ungrouped: tabIds.length };
}

async function tabByIndex(index) {
  const tabs = await chrome.tabs.query({ lastFocusedWindow: true });
  const tab = tabs.find((t) => t.index === index);
  if (!tab) {
    throw new Error("No tab at index " + index + ".");
  }
  requireListedTab(tab);
  return tab;
}

async function handleSelectTab(args) {
  const index = Number(args && args.index);
  const tab = await tabByIndex(index);
  await chrome.tabs.update(tab.id, { active: true });
  if (typeof tab.windowId === "number") {
    await chrome.windows.update(tab.windowId, { focused: true });
  }
  const info = await tabInfo(tab.id);
  return { ok: true, index, url: info.url, title: info.title };
}

async function handleNewTab(args) {
  // A supplied url must be usable. Letting a blank one through opens a blank tab under the guise
  // of having honored the request, which is not the tab the caller asked for.
  if (args && typeof args.url === "string" && args.url.trim().length === 0) {
    throw new Error("newTab url must be http(s).");
  }
  const url = args && args.url ? normalizeUrl(args.url) : null;
  if (args && args.url && !url) {
    throw new Error("newTab url must be http(s).");
  }
  const tab = await chrome.tabs.create(url ? { url } : {});
  lastTabListing = null;  // a new tab shifts what an index names
  // chrome.tabs.create resolves the moment the tab EXISTS -- the target is still in pendingUrl
  // and url is empty. Reporting the requested url here would state that the tab is at a page no
  // load has been attempted for, so wait for the load and report what the tab actually shows
  // (a redirect, an interstitial, or a failed navigation all say so on their own).
  let loaded = true;
  if (url) {
    // null: a tab created for this navigation has no previous document, so any settled url is
    // this load having landed. waitForComplete re-reads the tab after attaching its listener,
    // which is what makes a load that finished before the wait began report as complete instead
    // of sitting out the navigation timeout and then denying it loaded.
    loaded = await waitForComplete(tab.id, null);
  }
  const now = await chrome.tabs.get(tab.id);
  return {
    ok: true, index: now.index,
    url: now.url || now.pendingUrl || "", title: now.title || "",
    load_complete: loaded,
  };
}

async function handleCloseTab(args) {
  const tab =
    args && args.index !== undefined && args.index !== null
      ? await tabByIndex(Number(args.index))
      : await activeTab();
  await chrome.tabs.remove(tab.id);
  lastTabListing = null;  // every index after the closed one has shifted
  return { ok: true, index: tab.index };
}

// -- browser windows (chrome.windows) ----------------------------------------
//
// A model-opened link can spawn a popup or a new window; browser_windows lists them (and
// browser_tabs lists the tabs inside) so nothing opened off-screen is invisible. window CRUD
// mutates the browser (opening/closing windows), so it is gated at the normal-confirm tier.

async function handleListWindows() {
  const wins = await chrome.windows.getAll({ populate: true });
  // Only what the browser actually supplied. A missing type reported as "normal" is a claim about
  // a window nobody read, and an absent tabs array reported as tab_count 0 describes a populated
  // window as empty -- the operator then closes what looks like an empty window.
  const list = wins.map((w) => {
    const entry = { window_id: w.id, focused: !!w.focused, incognito: !!w.incognito };
    if (typeof w.type === "string") { entry.type = w.type; }
    if (typeof w.state === "string") { entry.state = w.state; }
    if (Array.isArray(w.tabs)) { entry.tab_count = w.tabs.length; }
    return entry;
  });
  // Remember which ids were actually shown, so a focus/close can prove the id it was given names
  // a window the caller was told about rather than one it produced on its own.
  lastWindowListing = new Set(list.map((w) => w.window_id));
  return { windows: list };
}

// A window action is meaningful only against a window the caller has been shown. Chrome window
// ids are small sequential integers, so an invented or stale one readily names somebody's live
// window -- and "close" takes it and its tabs down.
function requireListedWindow(windowId) {
  if (!lastWindowListing || !lastWindowListing.has(windowId)) {
    throw new Error("Window " + windowId + " is not in the current window listing; call " +
      "browser_windows before acting on a window id.");
  }
}

// The window as it exists right now, so the reply attributes the action to something observed
// rather than to the bare integer that was confirmed.
async function windowFacts(windowId) {
  const win = await chrome.windows.get(windowId, { populate: true });
  const facts = {};
  if (typeof win.type === "string") { facts.type = win.type; }
  if (Array.isArray(win.tabs)) { facts.tab_count = win.tabs.length; }
  return facts;
}

async function handleWindow(args) {
  const action = String((args && args.action) || "").toLowerCase();
  if (action !== "new" && action !== "focus" && action !== "close") {
    throw new Error("browser_window action must be new, focus, or close.");
  }
  if (action === "new") {
    // A standard browser window (Chrome's extension API does not reliably honor a popup type
    // for a navigated window, so we do not expose one). An optional URL opens it there.
    const url = args && args.url ? normalizeUrl(args.url) : null;
    if (args && args.url && !url) {
      throw new Error("browser_window new url must be http(s).");
    }
    const win = await chrome.windows.create(url ? { url } : {});
    const firstTab = (win.tabs && win.tabs[0]) || null;
    // A window this session just opened is one the caller has been told about, so it can be
    // named next without a re-listing.
    if (!lastWindowListing) { lastWindowListing = new Set(); }
    lastWindowListing.add(win.id);
    const created = { ok: true, window_id: win.id, tab_index: firstTab ? firstTab.index : null };
    if (typeof win.type === "string") { created.type = win.type; }  // never a fabricated "normal"
    return created;
  }
  const windowId = Number(args && args.window_id);
  if (!Number.isInteger(windowId)) {
    throw new Error("browser_window needs window_id for focus/close (see browser_windows).");
  }
  requireListedWindow(windowId);
  const facts = await windowFacts(windowId);
  if (action === "focus") {
    // Windows can refuse a foreground activation (the OS foreground lock), and the promise still
    // resolves. Every ambient-tab command in this file resolves its target through
    // lastFocusedWindow, so reporting a focus that did not happen silently points the rest of the
    // session at the OLD window: take the answer from the Window the API hands back.
    const win = await chrome.windows.update(windowId, { focused: true });
    if (!win || win.focused !== true) {
      throw new Error("The browser did not bring window " + windowId +
        " to the foreground (the OS may have blocked the activation).");
    }
    return Object.assign({ ok: true, window_id: win.id, focused: true }, facts);
  }
  await chrome.windows.remove(windowId);  // action === "close"
  lastWindowListing.delete(windowId);  // that window is gone; it can never be named again
  return Object.assign({ ok: true, window_id: windowId, closed: true }, facts);
}

// -- device emulation (Emulation domain) -------------------------------------
//
// Was this argument SUPPLIED? Distinguishing "absent" from "present but unusable" is what lets a
// malformed value be refused instead of quietly treated as though it had never been sent.
function argProvided(v) {
  return v !== undefined && v !== null;
}

// Emulate a device viewport / user-agent / touch for the active tab (responsive testing, UA
// gating, mobile layouts). Overrides are per-CDP-session, so they clear automatically when we
// detach (tab switch / disconnect); reset clears them explicitly and restores the real UA.
// Undo every override this tool can install. A clear that failed leaves the tab emulating -- a
// fake viewport, touch input, or a spoofed identity -- for the rest of the session. Swallowing
// that and answering ok:true reports the one state the caller must never be told without it being
// true, so let the failure surface.
async function resetEmulation(tabId) {
  await sendCdp(tabId, "Emulation.clearDeviceMetricsOverride");
  await sendCdp(tabId, "Emulation.setTouchEmulationEnabled", { enabled: false });
  if (originalUserAgent) {
    await sendCdp(tabId, "Emulation.setUserAgentOverride", { userAgent: originalUserAgent });
  }
  lastShot = null;  // clearing the metrics relays the page out from under any screenshot
  // False only when no real UA was ever captured -- in which case no override could have been
  // installed either (see applyUserAgentOverride), so there is nothing left spoofed.
  return { ok: true, reset: true, user_agent_restored: !!originalUserAgent };
}

// A viewport is width AND height together. Skipping the override because one of them was missing
// or non-positive leaves the tab laid out at a size nobody asked for, while the reply lists
// whatever else did apply -- so a metrics request that cannot be honored is refused rather than
// dropped, and a device_scale_factor is never quietly rewritten to 1.
async function applyDeviceMetrics(tabId, args, applied) {
  if (!argProvided(args.width) && !argProvided(args.height)
      && !argProvided(args.device_scale_factor)) {
    return;
  }
  const w = Number(args.width);
  const h = Number(args.height);
  const dsf = Number(args.device_scale_factor);
  if (!(Number.isFinite(w) && Number.isFinite(h) && w > 0 && h > 0)) {
    throw new Error("browser_emulate needs both width and height as positive integers.");
  }
  if (argProvided(args.device_scale_factor) && !(Number.isFinite(dsf) && dsf > 0)) {
    throw new Error("browser_emulate device_scale_factor must be a positive number.");
  }
  await sendCdp(tabId, "Emulation.setDeviceMetricsOverride", {
    width: Math.round(w), height: Math.round(h),
    deviceScaleFactor: Number.isFinite(dsf) && dsf > 0 ? dsf : 1,
    mobile: args.mobile === true,
  });
  applied.width = Math.round(w);
  applied.height = Math.round(h);
  applied.mobile = args.mobile === true;
  if (argProvided(args.device_scale_factor)) { applied.device_scale_factor = dsf; }
}

async function applyUserAgentOverride(tabId, args, applied) {
  if (typeof args.user_agent !== "string" || args.user_agent.length === 0) {
    return;
  }
  // An override with no captured original cannot be undone; refuse it rather than pin an
  // identity on the tab that reset has no real value to restore.
  if (!originalUserAgent) {
    throw new Error("Cannot override the user agent: the browser's real user agent could not "
      + "be read, so the override could not be undone.");
  }
  await sendCdp(tabId, "Emulation.setUserAgentOverride", { userAgent: args.user_agent });
  // The full string is what was applied; the echo is capped. A silent slice reads back as the
  // whole override, so mark it the way every other capped field in this file does.
  applied.user_agent = args.user_agent.slice(0, 120);
  if (args.user_agent.length > 120) {
    applied.user_agent_truncated = true;
    applied.user_agent_length = args.user_agent.length;
  }
}

async function handleEmulate(tabId, args) {
  await ensureAttached(tabId);
  if (originalUserAgent === null) {
    // The real UA comes from THIS worker's own context, never from the page: navigator.userAgent
    // read in the page world is a value a hostile page can redefine, and reset would then install
    // that attacker-chosen string as a genuine override for the rest of the session.
    const ua = navigator && typeof navigator.userAgent === "string" ? navigator.userAgent : "";
    if (ua.length > 0) { originalUserAgent = ua; }
  }
  if (args.reset === true) {
    return await resetEmulation(tabId);
  }
  const applied = {};
  await applyDeviceMetrics(tabId, args, applied);
  await applyUserAgentOverride(tabId, args, applied);
  if (typeof args.touch === "boolean") {
    // maxTouchPoints makes navigator.maxTouchPoints reflect touch live (the 'ontouchstart' in
    // window feature flag is fixed at page load, so it only flips after a reload).
    await sendCdp(tabId, "Emulation.setTouchEmulationEnabled",
      { enabled: args.touch, maxTouchPoints: args.touch ? 5 : 0 });
    applied.touch = args.touch;
  }
  if (Object.keys(applied).length === 0) {
    throw new Error("browser_emulate needs width+height, user_agent, touch, or reset.");
  }
  // Emulation relays the page out (a new viewport, a UA-gated variant): any outstanding
  // screenshot describes a render that no longer exists, so a coordinate click must not be
  // able to match its fingerprint.
  lastShot = null;
  return { ok: true, applied };
}

// -- print to PDF (Page.printToPDF) ------------------------------------------
//
// Render the active tab to a PDF at the browser level (no OS print dialog, no printer).
// The base64 PDF rides back in the reply; the bridge decodes it, size-caps it, and writes
// the file to disk -- the extension never touches the filesystem. Backgrounds print by
// default (technicians usually want the page as it looks). Only well-formed numeric/enum
// options are forwarded, so a page can neither steer the output path nor inject options.
// A print option that is out of range is a request this layer cannot honor -- and it is the only
// layer that range-checks, since the C++ contract only type-checks. Dropping one renders the PDF
// at Chrome's default instead, which is a different document than the one that was asked for.
function printPageOptions(args) {
  const opts = { transferMode: "ReturnAsBase64" };
  if (typeof args.landscape === "boolean") { opts.landscape = args.landscape; }
  opts.printBackground = args.print_background === false ? false : true;
  if (args.scale !== undefined && args.scale !== null) {
    const scale = Number(args.scale);
    if (!Number.isFinite(scale) || scale < 0.1 || scale > 2) {
      throw new Error("browser_print scale must be between 0.1 and 2.");
    }
    opts.scale = scale;
  }
  const paper = [["paper_width", "paperWidth"], ["paper_height", "paperHeight"]];
  for (const [argName, optName] of paper) {
    if (args[argName] === undefined || args[argName] === null) { continue; }
    const inches = Number(args[argName]);
    if (!Number.isFinite(inches) || inches <= 0) {
      throw new Error("browser_print " + argName + " must be a positive number of inches.");
    }
    opts[optName] = inches;
  }
  return opts;
}

async function handlePrint(tabId, args) {
  await ensureAttached(tabId);
  const opts = printPageOptions(args);
  if (typeof args.page_ranges === "string" && args.page_ranges.trim().length > 0) {
    const ranges = args.page_ranges.trim();
    // Truncating a range spec prints a valid-but-DIFFERENT set of pages (a cut mid-token still
    // parses), and the reply would call that a success.
    if (ranges.length > 100) {
      throw new Error("browser_print page_ranges is too long (max 100 characters).");
    }
    opts.pageRanges = ranges;
  }
  // The document the operator approved printing is the one that was in front of them. A
  // meta-refresh, a JS redirect, or an ad frame navigating the top frame between that
  // confirmation and this round trip renders a DIFFERENT page into the file, and the reply would
  // present it as the print that was asked for. Bind the capture to the generation it started
  // against, the same way a snapshot is.
  const startEpoch = domEpoch;
  const res = await sendCdp(tabId, "Page.printToPDF", opts);
  if (domEpoch !== startEpoch) {
    throw new Error("The page navigated while it was being printed; print it again.");
  }
  const data = res && typeof res.data === "string" ? res.data : "";
  if (!data) { throw new Error("The browser returned an empty PDF."); }
  // Bound the payload where it is produced. A reply past the relay's frame cap does not come
  // back as an error at all: it fails the frame parse, tears the relay down, and the caller sees
  // a transport reset instead of the real reason. Mirror the bridge's own PDF cap so an
  // oversized print is reported as what it is, with the way out.
  if (data.length > MAX_PDF_BASE64) {
    throw new Error("The printed PDF is too large to return; narrow it with page_ranges.");
  }
  const info = await tabInfo(tabId);
  return { data, url: info.url, title: info.title };
}

// -- site permissions (chrome.contentSettings) -------------------------------
//
// Grant/block/reset a site permission (geolocation, notifications, camera, mic, ...) for an
// origin, so automation can pre-answer the permission prompts that would otherwise block a
// flow. Uses the native, origin-scoped contentSettings API rather than a tab-attached CDP
// Browser.* call (which is unreliable through chrome.debugger). Only a fixed allowlist of
// content types and settings is accepted and the origin is reduced to a validated http(s)
// pattern, so a page can neither widen its own grants nor steer the pattern.
const PERMISSION_TYPES = {
  geolocation: "location",
  notifications: "notifications",
  camera: "camera",
  microphone: "microphone",
  images: "images",
  javascript: "javascript",
  popups: "popups",
  automatic_downloads: "automaticDownloads",
};
const PERMISSION_SETTINGS = ["allow", "block", "ask"];

// A single concrete host: a dotted name, or a bracketed IPv6 literal. "*" is NOT a forbidden
// host code point in the URL standard, so `new URL("https://*")` parses with host "*" -- and
// that host would reach chrome.contentSettings as the pattern "https://*/*", which grants the
// permission on EVERY https origin in the profile. A permission is scoped to one site by
// definition, so anything that is not one host is refused rather than widened.
const CONCRETE_HOST =
  /^(\[[0-9a-f:.]+\]|[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*)$/i;

// Reduce an origin or full URL to an origin match pattern ("https://host:port/*"); returns
// null for anything that is not http(s) on a single concrete host.
function originPattern(origin) {
  let parsed;
  try {
    parsed = new URL(origin);
  } catch (e) {
    return null;
  }
  if (parsed.protocol !== "http:" && parsed.protocol !== "https:") {
    return null;
  }
  if (!CONCRETE_HOST.test(parsed.hostname)) {
    return null;
  }
  return parsed.protocol + "//" + parsed.host + "/*";
}

async function handlePermission(args) {
  const typeKey = PERMISSION_TYPES[String((args && args.name) || "").toLowerCase()];
  if (!typeKey) {
    throw new Error(
      "browser_permission name must be one of: " + Object.keys(PERMISSION_TYPES).join(", ") + ".");
  }
  const setting = String((args && args.setting) || "").toLowerCase();
  if (!PERMISSION_SETTINGS.includes(setting)) {
    throw new Error("browser_permission setting must be allow, block, or ask.");
  }
  let origin = args && args.origin ? String(args.origin) : null;
  // A grant is persistent, origin-scoped, and outlives the session, and the human approves the
  // ARGUMENTS -- with the origin omitted nobody in the chain can see which site is being handed
  // the camera. Only the restrictive settings may name the site by "whatever is in front of us".
  if (!origin && setting === "allow") {
    throw new Error(
      "browser_permission allow needs an explicit http(s) origin, so the site being granted the " +
        "permission is named in the request.");
  }
  let ambientTab = null;
  if (!origin) {
    ambientTab = await activeTab();
    origin = ambientTab && ambientTab.url ? ambientTab.url : null;
  }
  const pattern = origin ? originPattern(origin) : null;
  if (!pattern) {
    throw new Error("browser_permission needs a valid http(s) origin (or an active http(s) tab).");
  }
  const store = chrome.contentSettings[typeKey];
  if (!store || typeof store.set !== "function") {
    throw new Error("This browser cannot set the '" + args.name + "' permission.");
  }
  // The ambient origin was read before the store lookup; a tab switch or a navigation in between
  // would redirect the setting to a site nobody named. Prove the same tab still shows the same
  // origin immediately before the write.
  if (ambientTab !== null) {
    const now = await activeTab();
    if (now.id !== ambientTab.id || originPattern(now.url || "") !== pattern) {
      throw new Error(
        "The active tab changed while applying the permission; name the origin explicitly.");
    }
  }
  // set() rejects when a setting does not apply to a type (e.g. 'ask' on images); that error
  // surfaces honestly to the caller rather than being swallowed.
  await store.set({ primaryPattern: pattern, setting });
  return { ok: true, name: String(args.name).toLowerCase(), setting, pattern };
}

// -- web storage (localStorage / sessionStorage) -----------------------------
//
// Read or write the active tab's local/session storage: get/set/remove/clear/keys. The op runs
// through the CDP DOMStorage domain, which reaches the storage area from the BROWSER process.
// Running it in the page world instead would put every step on surfaces the page owns --
// window.localStorage can be shadowed, and Storage.prototype.getItem/setItem/clear can be
// replaced -- so a hostile page could forge a read, or no-op a write or a token-wiping clear,
// and the tool would report success for an operation that never happened. Returned keys and
// values are capped so a huge store cannot flood the transport.
const MAX_STORAGE_VALUE_CHARS = 20000;
const MAX_STORAGE_KEYS = 500;
// A key NAME is page-controlled too, and the count cap alone bounds nothing: 500 names of a
// megabyte each is a half-gigabyte reply that never parses as a frame.
const MAX_STORAGE_KEY_CHARS = 512;

// Name the storage area by the attached page's own security origin. An opaque origin
// (sandboxed, file:) has no area to address, so it fails closed rather than resolving to
// something else's store.
function storageIdFor(url, area) {
  const origin = originOf(url);
  if (!origin) {
    throw new Error(
      "This page has no addressable storage origin (it is sandboxed or has an opaque origin).");
  }
  return { securityOrigin: origin, isLocalStorage: area !== "session" };
}

// The area's entries as [key, value] pairs, straight from the browser process.
async function storageEntries(tabId, storageId) {
  const res = await sendCdp(tabId, "DOMStorage.getDOMStorageItems", { storageId });
  return (res && res.entries) || [];
}

async function storageRead(tabId, storageId, action, key) {
  const entries = await storageEntries(tabId, storageId);
  if (action === "keys") {
    const names = entries.slice(0, MAX_STORAGE_KEYS).map((e) => String(e[0]));
    // A sliced name reads back as the whole key -- and a key the caller then passes to
    // get/remove would name nothing -- so the cut is marked, like every other capped field here.
    return {
      ok: true,
      keys: names.map((k) => (k.length > MAX_STORAGE_KEY_CHARS ? k.slice(0, MAX_STORAGE_KEY_CHARS) : k)),
      count: entries.length,
      capped: entries.length > MAX_STORAGE_KEYS,
      keys_truncated: names.some((k) => k.length > MAX_STORAGE_KEY_CHARS),
    };
  }
  const hit = entries.find((e) => e && String(e[0]) === key);
  if (!hit) {
    return { ok: true, present: false, value: null };
  }
  const v = String(hit[1]);
  if (v.length > MAX_STORAGE_VALUE_CHARS) {
    return { ok: true, present: true, value: v.slice(0, MAX_STORAGE_VALUE_CHARS), truncated: true };
  }
  return { ok: true, present: true, value: v };
}

// Every write reports what the AREA says afterwards, not what was asked for. A storage command
// that resolves is not a storage command that took effect (a quota refusal, an area that dropped
// the key), and "ok" with no read-back is the tool asserting a stored value the caller would then
// build on. The reads are cheap: one DOMStorage.getDOMStorageItems per write.
async function storageWrite(tabId, storageId, action, key, value) {
  if (action === "set") {
    await sendCdp(tabId, "DOMStorage.setDOMStorageItem", { storageId, key, value });
    const stored = await storageEntries(tabId, storageId);
    const hit = stored.find((e) => e && String(e[0]) === key);
    if (!hit || String(hit[1]) !== value) {
      throw new Error("The value was not stored (the area rejected or dropped the write).");
    }
    return { ok: true, stored: true };
  }
  if (action === "remove") {
    // Whether the key was ever there is the whole answer to a remove, and it is only knowable
    // BEFORE the removal -- removing a key that never existed must not read like clearing a
    // real one.
    const existing = await storageEntries(tabId, storageId);
    const existed = existing.some((e) => e && String(e[0]) === key);
    await sendCdp(tabId, "DOMStorage.removeDOMStorageItem", { storageId, key });
    const left = await storageEntries(tabId, storageId);
    if (left.some((e) => e && String(e[0]) === key)) {
      throw new Error("The key is still present after the remove.");
    }
    return { ok: true, removed: existed };
  }
  // How many entries a clear removed is only knowable before it runs.
  const before = await storageEntries(tabId, storageId);
  await sendCdp(tabId, "DOMStorage.clear", { storageId });
  const after = await storageEntries(tabId, storageId);
  if (after.length > 0) {
    throw new Error("The storage area still holds " + after.length + " entries after the clear.");
  }
  return { ok: true, cleared: before.length };
}

async function handleStorage(tabId, args) {
  await ensureAttached(tabId);
  const action = String((args && args.action) || "").toLowerCase();
  if (["get", "set", "remove", "clear", "keys"].indexOf(action) < 0) {
    throw new Error("browser_storage action must be get, set, remove, clear, or keys.");
  }
  const area = String((args && args.area) || "local").toLowerCase();
  if (area !== "local" && area !== "session") {
    throw new Error("browser_storage area must be local or session.");
  }
  if ((action === "get" || action === "set" || action === "remove") &&
      (typeof args.key !== "string" || args.key.length === 0)) {
    throw new Error("browser_storage " + action + " needs a key.");
  }
  if (action === "set" && typeof args.value !== "string") {
    throw new Error("browser_storage set needs a string value.");
  }
  // The command carries no site of its own: it acts on whichever tab is active when the frame
  // executes. Resolve that origin BEFORE touching anything, so a caller that states which site
  // it meant is refused on a mismatch rather than reading (or clearing) another site's tokens,
  // and so the reply names the origin the operation actually touched.
  const info = await tabInfo(tabId);
  if (args && typeof args.expect_origin === "string" && args.expect_origin.length > 0 &&
      !originsMatch(info.url, args.expect_origin)) {
    throw new Error("The active tab is " + (info.url || "an unknown page") + ", not " +
      args.expect_origin + "; browser_storage will not touch another site's storage.");
  }
  const storageId = storageIdFor(info.url, area);
  await sendCdp(tabId, "DOMStorage.enable");
  // A DOMStorage failure (a blocked origin, a dead session) rejects with its own message, which
  // is the honest answer: there is no reading of it that makes a missed write a success.
  const result = action === "get" || action === "keys"
    ? await storageRead(tabId, storageId, action, args.key || "")
    : await storageWrite(tabId, storageId, action, args.key || "",
                         action === "set" ? args.value : "");
  return Object.assign({ url: info.url }, result);
}

// -- cookies (chrome.cookies) ------------------------------------------------
//
// Read/set/remove cookies for an origin via the native chrome.cookies API (reliable + covers
// httpOnly cookies a page's document.cookie cannot). SENSITIVE: get returns cookie VALUES,
// which can include session tokens, and set can install a session cookie -- the tool is gated
// (confirmed each call) and its scope is bound to a validated http(s) url; a set is confined by
// the API to that url's domain, so it cannot forge a cookie for an unrelated site. Values and
// the returned list are capped so a large jar cannot flood the transport.
const COOKIE_SAME_SITE = { no_restriction: "no_restriction", lax: "lax", strict: "strict" };

function cookieView(c) {
  // A value long enough to be sliced is not the value the caller reads back. Chrome's own ~4096
  // byte cookie limit makes the cut rare, but a cap that is silent is a cap that misreports the
  // day it does bite -- every other capped field in this file says so.
  const overCap = !!(c.value && c.value.length > 4096);
  return {
    name: c.name,
    value: overCap ? c.value.slice(0, 4096) : c.value,
    value_truncated: overCap,
    domain: c.domain,
    path: c.path,
    secure: !!c.secure,
    http_only: !!c.httpOnly,
    same_site: c.sameSite || null,
    session: !!c.session,
    expires: c.expirationDate || null,
  };
}

// A cookie attribute that cannot be honored is not an attribute that may be dropped: scope
// (path), cross-site exposure (same_site), and lifetime (expires_days) are the whole point of
// asking for them, and Chrome's default for each is a DIFFERENT cookie than the one requested.
function cookieDetails(url, args) {
  const details = { url, name: String(args.name), value: args.value };
  if (args.path !== undefined && args.path !== null) {
    if (typeof args.path !== "string" || args.path.length === 0) {
      throw new Error("browser_cookies path must be a non-empty string.");
    }
    details.path = args.path;
  }
  if (typeof args.secure === "boolean") { details.secure = args.secure; }
  if (typeof args.http_only === "boolean") { details.httpOnly = args.http_only; }
  if (args.same_site !== undefined && args.same_site !== null) {
    const ss = typeof args.same_site === "string" ? COOKIE_SAME_SITE[args.same_site.toLowerCase()] : null;
    if (!ss) {
      throw new Error("browser_cookies same_site must be one of: " +
        Object.keys(COOKIE_SAME_SITE).join(", ") + ".");
    }
    details.sameSite = ss;
  }
  if (args.expires_days !== undefined && args.expires_days !== null) {
    const days = Number(args.expires_days);
    if (!Number.isFinite(days) || days <= 0) {
      throw new Error("browser_cookies expires_days must be a positive number.");
    }
    details.expirationDate = Math.floor(Date.now() / 1000) + Math.round(days * 86400);
  }
  return details;
}

async function cookiesSet(url, args) {
  if (typeof args.value !== "string") {
    throw new Error("browser_cookies set needs a string value.");
  }
  const details = cookieDetails(url, args);
  const cookie = await chrome.cookies.set(details);
  if (!cookie) {
    throw new Error("browser_cookies set failed (the browser rejected the cookie).");
  }
  // Report the cookie the browser actually installed, not the one that was asked for: Chrome
  // adjusts path, domain, sameSite and lifetime on its own rules, and a caller that cannot see
  // the difference cannot tell a session cookie from the persistent one it requested. The value
  // is the caller's own and is left out -- a set is not a reason to put a token in the reply.
  const view = cookieView(cookie);
  delete view.value;
  delete view.value_truncated;
  return Object.assign({ ok: true, url, set: true }, view);
}

async function handleCookies(args) {
  const action = String((args && args.action) || "").toLowerCase();
  if (["get", "set", "remove"].indexOf(action) < 0) {
    throw new Error("browser_cookies action must be get, set, or remove.");
  }
  // A url that was SUPPLIED must be usable. Letting an empty or blank one fall through to the
  // active tab answers a different question than the one that was asked -- and the answer is a
  // site's cookies, which can be its session tokens.
  if (args && args.url !== undefined && args.url !== null &&
      (typeof args.url !== "string" || args.url.trim().length === 0)) {
    throw new Error("browser_cookies url must be a non-empty http(s) url.");
  }
  let url = args && args.url ? String(args.url).trim() : null;
  if (!url) {
    const tab = await activeTab();
    url = tab && tab.url ? tab.url : null;
  }
  if (!url || !/^https?:/i.test(url)) {
    throw new Error("browser_cookies needs a valid http(s) url (or an active http(s) tab).");
  }
  if (action === "get") {
    const all = await chrome.cookies.getAll({ url });
    return { ok: true, url, count: all.length, capped: all.length > 100,
             cookies: all.slice(0, 100).map(cookieView) };
  }
  if (!args || typeof args.name !== "string" || args.name.length === 0) {
    throw new Error("browser_cookies " + action + " needs a name.");
  }
  if (action === "remove") {
    const removed = await chrome.cookies.remove({ url, name: String(args.name) });
    return { ok: true, url, name: String(args.name), removed: !!removed };
  }
  return await cookiesSet(url, args);
}

// -- downloads (chrome.downloads) --------------------------------------------
//
// Download a url to disk via the native chrome.downloads API and wait for it to finish,
// returning the real saved path + byte size. The op writes a file, so it is gated. filename
// is optional and must be RELATIVE with no ".." (chrome.downloads rejects absolute/parent
// paths anyway; we reject early with a clear message), so a page cannot steer the write
// outside the browser's download tree. Poll to completion under a bounded timeout.
async function pollDownload(id, timeoutMs, gen) {
  const deadline = Date.now() + timeoutMs;
  let item = null;
  while (Date.now() < deadline) {
    // The command this poll belongs to is retired the moment a newer frame arrives, the host
    // cancels, or the session is torn down. Polling on past that keeps working for a reply
    // nothing is waiting for, and finally posts it into a relay that has already been reset.
    if (gen !== commandGeneration) {
      throw new Error("The download wait was superseded by a newer command.");
    }
    const found = await chrome.downloads.search({ id });
    item = found && found[0];
    if (item && item.state !== "in_progress") {
      return item;
    }
    await new Promise((r) => setTimeout(r, 250));
  }
  return item;
}

async function handleDownload(args) {
  // The generation this download wait belongs to, captured before anything can supersede it.
  const gen = commandGeneration;
  const url = args && args.url ? String(args.url) : "";
  if (!/^https?:/i.test(url)) {
    throw new Error("browser_download needs a valid http(s) url.");
  }
  const opts = { url, conflictAction: "uniquify", saveAs: false };
  if (typeof args.filename === "string" && args.filename.trim().length > 0) {
    const fn = args.filename.trim();
    if (/^([a-zA-Z]:|\\|\/)/.test(fn) || fn.indexOf("..") >= 0) {
      throw new Error("browser_download filename must be a relative name without '..'.");
    }
    opts.filename = fn;
  }
  let timeoutMs = Number(args && args.timeout_ms);
  if (!Number.isFinite(timeoutMs)) { timeoutMs = 30000; }
  timeoutMs = Math.min(120000, Math.max(1000, timeoutMs));
  const id = await chrome.downloads.download(opts);
  if (typeof id !== "number") {
    throw new Error("browser_download failed to start.");
  }
  const item = await pollDownload(id, timeoutMs, gen);
  if (!item) {
    throw new Error("browser_download could not track the download.");
  }
  if (item.state !== "complete") {
    return { ok: false, id, state: item.state, error: item.error || null,
             path: item.filename || null };
  }
  return { ok: true, id, state: item.state, path: item.filename || null,
           bytes: item.fileSize || item.totalBytes || 0, url };
}

// -- HTTP auth (Fetch domain) ------------------------------------------------
//
// Arm credentials to auto-answer HTTP Basic/Digest 401 challenges on the active tab, so
// automation can reach password-protected pages without a native auth dialog wedging the
// browser. Enables the Fetch domain (handleAuthRequests); the onEvent handler answers each
// challenge and continues every other paused request. clear:true (or a tab switch/detach)
// disarms + disables Fetch. The password is used only to answer challenges and is never
// echoed back in the result.
// Carry any interception failure back on the reply. While Fetch is on, every request in the tab
// is paused, so a continue that failed is a resource the page is still waiting on -- the operator
// sees only a slow page unless the arm/disarm reply says so.
function fetchStateReply(reply) {
  if (lastFetchError !== null) {
    reply.last_fetch_error = lastFetchError;
  }
  return reply;
}

async function handleHttpAuth(tabId, args) {
  await ensureAttached(tabId);
  if (args && args.clear === true) {
    httpAuthCreds = null;
    await sendCdp(tabId, "Fetch.disable").catch(() => {});
    return fetchStateReply({ ok: true, armed: false });
  }
  const username = args && typeof args.username === "string" ? args.username : "";
  const password = args && typeof args.password === "string" ? args.password : "";
  if (!username) {
    throw new Error("browser_http_auth needs a username (or clear:true to disarm).");
  }
  // Bind the credentials to the current tab's origin so the onEvent handler only ever hands
  // them to a same-origin challenge. Fail closed if the tab has no resolvable origin.
  let origin = null;
  try {
    const tab = await chrome.tabs.get(tabId);
    if (tab && tab.url) {
      origin = new URL(tab.url).origin;
    }
  } catch (_e) {
    origin = null;
  }
  if (!origin || origin === "null") {
    throw new Error(
      "browser_http_auth could not resolve the tab's origin to bind the credentials to.");
  }
  // The origin comes from whichever tab is active when the frame executes, which need not be the
  // site the password was confirmed for. When the caller states the origin it means, the arm
  // happens only if the tab is still that origin.
  if (args && typeof args.origin === "string" && args.origin.length > 0 &&
      !originsMatch(args.origin, origin)) {
    throw new Error("The active tab is " + origin + ", not " + args.origin +
      "; browser_http_auth will not arm credentials for a different origin.");
  }
  httpAuthCreds = { username, password, origin };
  await sendCdp(tabId, "Fetch.enable",
    { handleAuthRequests: true, patterns: [{ urlPattern: "*" }] });
  return fetchStateReply({ ok: true, armed: true, username, origin });
}

// -- Bring the bridge up -----------------------------------------------------

// Connect when the extension loads, when the browser starts, and on demand from the
// toolbar button (which also re-arms after a disconnect).
chrome.runtime.onInstalled.addListener(connect);
chrome.runtime.onStartup.addListener(connect);
chrome.action.onClicked.addListener(connect);

// Expose the last health snapshot to a popup / options page later.
chrome.runtime.onMessage.addListener((request, _sender, sendResponse) => {
  if (request && request.type === "sak.getHealth") {
    connect();
    sendResponse(health);
  }
  return false;
});

connect();
