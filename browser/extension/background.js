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
// {type:"bridge_ready"|"bridge_unavailable"} frames are informational.
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
// Cap how many omitted cross-origin frame URLs a snapshot reports (the C++ renderer caps
// the listing too); a hostile page could otherwise spawn thousands of iframes.
const MAX_OMITTED_FRAMES = 20;
// Skia caps a single raster surface at 16384 DEVICE px per edge; a full-page clip past
// that fails the capture. The clip is expressed in CSS px and rasterized at the display's
// devicePixelRatio, so the CSS clip is clamped to this cap divided by dpr (below).
const MAX_SHOT_EDGE_PX = 16384;

// CDP Input.dispatchKeyEvent modifier bitmask (Alt=1, Control=2, Meta=4, Shift=8).
const MODIFIER_BITS = { alt: 1, control: 2, ctrl: 2, meta: 4, command: 4, cmd: 4, shift: 8 };
// Named non-printable keys the model may press, mapped to their DOM code + legacy
// keyCode (needed so browser shortcuts like Control+A actually register).
const KEY_DEFS = {
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
};
// OEM punctuation -> [DOM code, Windows virtual key code]. ASCII charCodeAt is NOT the
// virtual key (e.g. '.' is 46 = VK_DELETE), so a chord like Control+/ needs the real VK.
const OEM_KEYS = {
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
};
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
// Live network-activity tracking for browser_wait_for network_idle: the count of in-flight
// requests on the attached tab and the timestamp of the last request start/finish. Fed by the
// CDP Network domain events; reset on navigation/detach so a stalled request cannot wedge idle.
let inflightRequests = 0;
let lastNetworkActivityMs = 0;
// The tab's real User-Agent, captured before any browser_emulate override so reset can restore
// it. Null until first captured; cleared on detach (a new session recaptures its own).
let originalUserAgent = null;
// Credentials armed for HTTP auth challenges via browser_http_auth. Null = disarmed (no Fetch
// interception). When set, the Fetch domain is enabled and the onEvent handler answers auth
// challenges with these; cleared on detach (Fetch is per-session, so it auto-disables too).
let httpAuthCreds = null;

let port = null;
let health = { connected: false, bridge: null, error: null };
let attachedTabId = null;
// The tab whose nodes populated the current ref_index. An element ref (backendNodeId) is
// only valid against THIS tab: if the active tab changed since the snapshot, applying the
// ref elsewhere could click/type a node the user never saw, so ref actions refuse.
let lastSnapshotTabId = null;
// Fingerprint of the render the most recent screenshot captured: {tabId, fullPage, dpr,
// scrollX, scrollY, href}. A coordinate click (browser_click_at) is meaningful only against
// that exact image, so it converts device->CSS with THIS dpr (not a re-read) and refuses if
// the tab, dpr, scroll, or document changed since -- a full-page shot is document-space, not
// click-space. null when no valid screenshot is outstanding.
let lastShot = null;
// A one-shot response armed for the NEXT JavaScript dialog by browser_dialog, e.g.
// {accept:true, text:"..."}. It is scoped to the single command that follows the arm (see
// runCommand): consumed if that command's action opens a dialog, otherwise dropped when the
// command finishes -- so an armed "accept" can never linger and auto-confirm an unrelated
// later dialog. Also cleared on navigation and teardown.
let pendingDialogPolicy = null;
// The most recent dialog the extension handled: {type, message, accepted}. Reported by
// browser_dialog so the model can see what an auto-dismissed alert/confirm said.
let lastDialog = null;
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
    console.error("[SAK] postMessage threw:", e);
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
    if (msg.protocol !== BRIDGE_PROTOCOL) {
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
    console.warn("[SAK] bridge unavailable:", health.error);
    return;
  }
  if (msg.type === "command") {
    handleCommand(msg);
    return;
  }
  console.warn("[SAK] unexpected frame type:", msg.type);
}

async function handleCommand(msg) {
  const id = msg.id;
  const cmd = msg.cmd;
  try {
    const payload = await runCommand(cmd, msg);
    send({ type: "result", id, cmd, payload });
  } catch (e) {
    send({ type: "error", id, cmd, error: e && e.message ? e.message : String(e) });
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
  try {
    return await dispatchCommand(cmd, args);
  } finally {
    if (scopedPolicy !== null && pendingDialogPolicy === scopedPolicy) {
      pendingDialogPolicy = null;
    }
  }
}

async function dispatchCommand(cmd, args) {
  switch (cmd) {
    case "snapshot":
      return await captureSnapshot(await activeTabId());
    case "read":
      return await handleRead(await activeTabId(), args);
    case "navigate":
      return await handleNavigate(await activeTabId(), args);
    case "back":
      return await handleHistory("back");
    case "forward":
      return await handleHistory("forward");
    case "reload":
      return await handleReload();
    case "listTabs":
      return await handleListTabs();
    case "selectTab":
      return await handleSelectTab(args);
    case "newTab":
      return await handleNewTab(args);
    case "closeTab":
      return await handleCloseTab(args);
    case "click":
      return await handleClick(await activeTabId(), args);
    case "clickAt":
      return await handleClickAt(await activeTabId(), args);
    case "hover":
      return await handleHover(await activeTabId(), args);
    case "drag":
      return await handleDrag(await activeTabId(), args);
    case "dialog":
      return await handleDialog(await activeTabId(), args);
    case "type":
      return await handleType(await activeTabId(), args);
    case "select":
      return await handleSelect(await activeTabId(), args);
    case "setValue":
      return await handleSetValue(await activeTabId(), args);
    case "media":
      return await handleMedia(await activeTabId(), args);
    case "pressKey":
      return await handlePressKey(await activeTabId(), args);
    case "scroll":
      return await handleScroll(await activeTabId(), args);
    case "screenshot":
      return await handleScreenshot(await activeTabId(), args);
    case "groupTabs":
      return await handleGroupTabs(args);
    case "ungroupTabs":
      return await handleUngroupTabs(args);
    case "waitFor":
      return await handleWaitFor(await activeTabId(), args);
    case "getValue":
      return await handleGetValue(await activeTabId(), args);
    case "getAttribute":
      return await handleGetAttribute(await activeTabId(), args);
    case "box":
      return await handleBox(await activeTabId(), args);
    case "focus":
      return await handleFocus(await activeTabId(), args);
    case "reveal":
      return await handleReveal(await activeTabId(), args);
    case "jsClick":
      return await handleJsClick(await activeTabId(), args);
    case "listWindows":
      return await handleListWindows();
    case "window":
      return await handleWindow(args);
    case "emulate":
      return await handleEmulate(await activeTabId(), args);
    case "print":
      return await handlePrint(await activeTabId(), args);
    case "permission":
      return await handlePermission(args);
    case "storage":
      return await handleStorage(await activeTabId(), args);
    case "cookies":
      return await handleCookies(args);
    case "download":
      return await handleDownload(args);
    case "httpAuth":
      return await handleHttpAuth(await activeTabId(), args);
    default:
      throw new Error("Unknown command: " + cmd);
  }
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

async function tabInfo(tabId) {
  const tab = await chrome.tabs.get(tabId);
  return { url: tab.url || "", title: tab.title || "" };
}

// -- CDP attach lifecycle ----------------------------------------------------

function sendCdp(tabId, method, params) {
  return chrome.debugger.sendCommand({ tabId }, method, params || {});
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
    attachedTabId = tabId;
    // getFullAXTree needs the DOM + Accessibility domains; Page backs getFrameTree (used
    // to detect omitted cross-origin frames). enable is idempotent.
    await sendCdp(tabId, "DOM.enable");
    await sendCdp(tabId, "Accessibility.enable");
    await sendCdp(tabId, "Page.enable");
    // Network domain backs browser_wait_for network_idle (in-flight request counting).
    await sendCdp(tabId, "Network.enable").catch(() => {});
    inflightRequests = 0;
    lastNetworkActivityMs = Date.now();
  }
  // Keep the control presence visible on every action (and re-created after a navigation wiped
  // it); cheap + idempotent. This is what makes the assistant's control persistently apparent,
  // not only during a pointer action.
  await refreshControlPresence(tabId);
}

async function detachAll(_reason) {
  lastSnapshotTabId = null;  // any ref_index is now unverifiable against a live tab
  lastShot = null;           // and any screenshot coordinates are against a dead render
  pendingDialogPolicy = null;  // an armed dialog response does not carry across sessions
  lastDialog = null;         // nor does a prior page's dialog report
  inflightRequests = 0;      // network-idle counting does not carry across sessions
  originalUserAgent = null;  // emulation overrides are per-session; recapture on the next tab
  httpAuthCreds = null;      // armed HTTP-auth credentials do not carry across sessions
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
    setTabControlBadge(source.tabId, false);  // control ended; clear the toolbar badge
    attachedTabId = null;
    lastSnapshotTabId = null;
    lastShot = null;
    pendingDialogPolicy = null;
    lastDialog = null;
    inflightRequests = 0;
    httpAuthCreds = null;
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

chrome.debugger.onEvent.addListener((source, method, params) => {
  if (!source || source.tabId !== attachedTabId) {
    return;
  }
  // Network-activity tracking for browser_wait_for network_idle. A request starting increments
  // the in-flight count; finishing/failing decrements it (floored at 0). Every event stamps the
  // last-activity time so idle can require a quiet window.
  if (method === "Network.requestWillBeSent") {
    inflightRequests++;
    lastNetworkActivityMs = Date.now();
    return;
  }
  if (method === "Network.loadingFinished" || method === "Network.loadingFailed") {
    if (inflightRequests > 0) { inflightRequests--; }
    lastNetworkActivityMs = Date.now();
    return;
  }
  // HTTP-auth interception (browser_http_auth). While Fetch is enabled we MUST answer every
  // paused request or the page wedges: an auth challenge gets the armed credentials (or the
  // browser default when disarmed), and every other paused request is continued untouched.
  if (method === "Fetch.authRequired") {
    const response = httpAuthCreds
      ? { response: "ProvideCredentials", username: httpAuthCreds.username,
          password: httpAuthCreds.password }
      : { response: "Default" };
    sendCdp(source.tabId, "Fetch.continueWithAuth",
      { requestId: params.requestId, authChallengeResponse: response }).catch(() => {});
    return;
  }
  if (method === "Fetch.requestPaused") {
    sendCdp(source.tabId, "Fetch.continueRequest", { requestId: params.requestId }).catch(() => {});
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
    pendingDialogPolicy = null;
    if (topFrameNav) {
      // A new document: any in-flight count from the old page is moot -- reset so a request
      // that never reported completion cannot keep network_idle from ever resolving.
      inflightRequests = 0;
      lastNetworkActivityMs = Date.now();
    }
    // A navigation wiped the injected overlay; re-assert the control presence on the new
    // document so the assistant's control stays visible without waiting for the next action.
    refreshControlPresence(source.tabId);
    return;
  }
  if (method !== "Page.javascriptDialogOpening") {
    return;
  }
  const type = params && typeof params.type === "string" ? params.type : "alert";
  const message = params && typeof params.message === "string" ? params.message : "";
  let accept = defaultDialogAccept(type);
  let promptText;
  if (pendingDialogPolicy) {
    accept = pendingDialogPolicy.accept;
    promptText = pendingDialogPolicy.text;
    pendingDialogPolicy = null;  // one-shot: never carry an armed response to a later dialog
  }
  lastDialog = { type, message, accepted: accept };
  const answer = { accept };
  if (accept && typeof promptText === "string") {
    answer.promptText = promptText;
  }
  sendCdp(source.tabId, "Page.handleJavaScriptDialog", answer).catch(() => {});
});

// Refs are valid only on the tab the snapshot was taken from; refuse a ref action when
// the active tab has changed since (a tab switch, or a model-opened foreground tab).
function requireSnapshotTab(tabId) {
  if (lastSnapshotTabId === null || tabId !== lastSnapshotTabId) {
    throw new Error(
      "The active tab changed since the last snapshot; call browser_snapshot on the " +
        "current tab before acting on an element.");
  }
}

// -- snapshot: accessibility roles/names joined with DOM-snapshot geometry ----

function axValue(v) {
  return v && v.value !== undefined && v.value !== null ? String(v.value) : "";
}

function indexProps(properties) {
  const map = {};
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
  if (typeof node.backendDOMNodeId === "number") {
    rec.backendNodeId = node.backendDOMNodeId;
    const b = boundsByBackend.get(node.backendDOMNodeId);
    if (b) {
      rec.bounds = b;
    }
  }
  if (props.disabled === true) {
    rec.disabled = true;
  }
  if (isEditableRole(role) || (props.editable !== undefined && props.editable !== false)) {
    rec.editable = true;
  }
  if (props.checked !== undefined) {
    rec.checked = props.checked === true || props.checked === "true" || props.checked === "mixed";
  }
  // ARIA state + current value, so the model can read expanded/selected/pressed/validity and
  // the live value of inputs, sliders, and spinbuttons without a separate round-trip.
  const truthy = (v) => v === true || v === "true" || v === "mixed";
  if (props.expanded !== undefined && props.expanded !== "undefined") {
    rec.expanded = truthy(props.expanded);
  }
  if (truthy(props.selected)) { rec.selected = true; }
  if (truthy(props.pressed)) { rec.pressed = true; }
  if (props.readonly === true) { rec.readonly = true; }
  if (props.required === true) { rec.required = true; }
  if (props.invalid !== undefined && props.invalid !== false && props.invalid !== "false") {
    rec.invalid = true;
  }
  if (props.busy === true) { rec.busy = true; }
  const currentValue = node.value && node.value.value;
  if (currentValue !== undefined && currentValue !== null && currentValue !== "") {
    rec.value = String(currentValue).slice(0, 80);
  }
  if (typeof props.valuemin === "number" && typeof props.valuemax === "number") {
    rec.valuemin = props.valuemin;
    rec.valuemax = props.valuemax;
  }
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
  const stack = roots.map((n) => ({ node: n, depth: 0 }));
  while (stack.length && out.length < MAX_CAPTURE_NODES) {
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

// The http(s) frames present in the tab but NOT covered by the same-process capture, i.e.
// the cross-origin (out-of-process) iframes whose content is missing. Deduplicated and
// capped; only http(s) so the model gets frames it can actually navigate to.
function collectOmittedFrames(snapshot, frameTree) {
  const covered = sameProcessUrls(snapshot);
  const all = [];
  collectFrameUrls(frameTree, all);
  const omitted = [];
  const seen = new Set();
  for (const url of all) {
    const key = stripFragment(url);
    if (!/^https?:\/\//i.test(url) || covered.has(key) || seen.has(key)) {
      continue;
    }
    seen.add(key);
    omitted.push(url);  // the original (already fragmentless) URL, navigable as-is
    if (omitted.length >= MAX_OMITTED_FRAMES) {
      break;
    }
  }
  return omitted;
}

// <audio>/<video> elements are frequently absent or unnamed in the AX tree (no native
// controls), so the model gets no ref to drive browser_media. Surface them directly from
// the DOM (by tag) as ref'd capture nodes, deduped against what the AX pass already emitted.
async function collectMediaNodes(tabId, existing) {
  const have = new Set(
    existing.filter((n) => typeof n.backendNodeId === "number").map((n) => n.backendNodeId));
  const out = [];
  try {
    const doc = await sendCdp(tabId, "DOM.getDocument", { depth: 0 });
    const root = doc && doc.root && doc.root.nodeId;
    if (!root) { return out; }
    const found = await sendCdp(tabId, "DOM.querySelectorAll", { nodeId: root, selector: "audio,video" });
    for (const nodeId of (found && found.nodeIds) || []) {
      const d = await sendCdp(tabId, "DOM.describeNode", { nodeId });
      const node = d && d.node;
      if (!node || typeof node.backendNodeId !== "number" || have.has(node.backendNodeId)) { continue; }
      const attrs = {};
      const a = node.attributes || [];
      for (let i = 0; i + 1 < a.length; i += 2) { attrs[a[i]] = a[i + 1]; }
      const role = (node.nodeName || "").toLowerCase() === "video" ? "video" : "audio";
      const name = attrs["aria-label"] || attrs["title"] || role;
      out.push({ role, name, depth: 0, interactable: true, visible: true, backendNodeId: node.backendNodeId });
      have.add(node.backendNodeId);
    }
  } catch (_e) { /* media surfacing is best-effort */ }
  return out;
}

async function captureSnapshot(tabId) {
  await ensureAttached(tabId);
  lastSnapshotTabId = tabId;  // refs from this snapshot are valid only against this tab
  const snapshot = await sendCdp(tabId, "DOMSnapshot.captureSnapshot", { computedStyles: [] });
  const boundsByBackend = buildBoundsMap(snapshot);
  const axTree = await sendCdp(tabId, "Accessibility.getFullAXTree", {});
  const built = buildNodes(axTree, boundsByBackend);
  const nodes = built.nodes.concat(await collectMediaNodes(tabId, built.nodes));
  const truncated = built.truncated;
  const info = await tabInfo(tabId);
  // getFullAXTree + DOMSnapshot cover the main frame and its SAME-process subframes.
  // Cross-origin (out-of-process) iframes live in separate targets this single pass does
  // not reach, so their content is absent. Detect that (more frames exist than
  // same-process documents) and flag it, so the model is told the capture is partial
  // instead of concluding those elements do not exist.
  let iframesOmitted = false;
  let omittedFrames = [];
  try {
    const tree = await sendCdp(tabId, "Page.getFrameTree", {});
    omittedFrames = collectOmittedFrames(snapshot, tree && tree.frameTree);
    const totalFrames = countFrames(tree && tree.frameTree);
    const sameProcessDocs = ((snapshot && snapshot.documents) || []).length;
    // Flag omission from either signal: the named http(s) frames, or a frame-count excess
    // (covers non-http frames the URL list intentionally skips).
    iframesOmitted = omittedFrames.length > 0 || totalFrames > sameProcessDocs;
  } catch (_e) {
    // If the frame tree is unavailable, do not claim completeness we cannot verify.
    iframesOmitted = true;
  }
  return { url: info.url, title: info.title, nodes, truncated, iframesOmitted, omittedFrames };
}

// -- read --------------------------------------------------------------------

async function handleRead(tabId, args) {
  const format = args && args.format === "html" ? "html" : "text";
  await ensureAttached(tabId);
  const expr =
    format === "html"
      ? "document.documentElement ? document.documentElement.outerHTML : ''"
      : "document.body ? document.body.innerText : " +
        "(document.documentElement ? document.documentElement.innerText : '')";
  const res = await sendCdp(tabId, "Runtime.evaluate", { expression: expr, returnByValue: true });
  let content = res && res.result && typeof res.result.value === "string" ? res.result.value : "";
  let truncated = false;
  if (content.length > MAX_READ_CHARS) {
    content = content.slice(0, MAX_READ_CHARS);
    truncated = true;
  }
  const info = await tabInfo(tabId);
  return { format, content, truncated, url: info.url, title: info.title };
}

// -- screenshot (vision): a PNG of the active tab over CDP --------------------
//
// Page.captureScreenshot rasterizes at the browser level (no getUserMedia / display
// prompt, no OS screen capture), so it sees only the tab -- never other windows or the
// desktop. full_page uses captureBeyondViewport with a clip sized to the document, capped
// at the Skia edge limit. The base64 PNG rides back in the reply payload; the bridge caps
// its size and returns it as an MCP image content block.
// The live tab's render fingerprint in one evaluate: devicePixelRatio (>= 1), scroll offset,
// and document URL. dpr bounds the screenshot clip in the DEVICE pixels Skia rasters, and
// the whole tuple binds a screenshot to the exact render a later coordinate click must match.
// `ok` is false when the page could not be read: callers must fail closed on it rather than
// trust the placeholder values, so that two failed reads (shot + click) can never compare
// equal and wave a blind coordinate click through.
async function viewportState(tabId) {
  const res = await sendCdp(tabId, "Runtime.evaluate", {
    expression:
      "({dpr: window.devicePixelRatio, sx: window.scrollX, sy: window.scrollY, href: location.href})",
    returnByValue: true,
  }).catch(() => null);
  const v = res && res.result && res.result.value ? res.result.value : null;
  if (!v || typeof v.href !== "string" || !Number.isFinite(v.dpr) || v.dpr <= 0) {
    return { ok: false, dpr: 1, scrollX: 0, scrollY: 0, href: "" };
  }
  return {
    ok: true,
    dpr: v.dpr,
    scrollX: Number.isFinite(v.sx) ? Math.round(v.sx) : 0,
    scrollY: Number.isFinite(v.sy) ? Math.round(v.sy) : 0,
    href: v.href,
  };
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
  if (fullPage) {
    const content = metrics && (metrics.cssContentSize || metrics.contentSize);
    if (content) {
      params.clip = {
        x: 0,
        y: 0,
        width: Math.min(Math.round(content.width), maxCssEdge),
        height: Math.min(Math.round(content.height), maxCssEdge),
        scale: 1,
      };
    }
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
  // Bind this image to the exact render so a later browser_click_at can convert with the
  // same dpr and refuse if the tab, dpr, scroll, or document moved. Only bind when the
  // fingerprint was read successfully; otherwise leave no binding so a coordinate click
  // fails closed ("no current screenshot") rather than trusting placeholder values.
  lastShot = state.ok
    ? { tabId, fullPage, dpr: state.dpr, scrollX: state.scrollX, scrollY: state.scrollY,
        href: state.href }
    : null;
  const info = await tabInfo(tabId);
  // Dimensions are read authoritatively from the PNG header on the bridge side, so they
  // reflect the real device pixels regardless of dpr/clip -- nothing is reported here.
  return { data, mimeType: "image/png", url: info.url, title: info.title };
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
function controlPresenceScript(x, y) {
  // The arrow's tip is at (2,1) in the 24-unit viewBox rendered at 32px (scale ~1.33); offset
  // the translate so the tip lands on the action point.
  const px = (Number.isFinite(x) ? Math.round(x) : 0) - 3;
  const py = (Number.isFinite(y) ? Math.round(y) : 0) - 1;
  return (
    "(function(){" +
    "function mk(id,tag){var e=document.getElementById(id);if(!e){e=document.createElement(tag);" +
    "e.id=id;(document.body||document.documentElement).appendChild(e);}return e;}" +
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

// Per-tab toolbar badge: only the controlled tab shows the pink "AI" chip, so the user can tell
// which tab (and page) the assistant is driving from the tab strip / toolbar.
function setTabControlBadge(tabId, on) {
  try {
    if (on) {
      chrome.action.setBadgeText({ tabId, text: "AI" });
      chrome.action.setBadgeBackgroundColor({ tabId, color: "#ff2d95" });
      chrome.action.setTitle({ tabId, title: "S.A.K. assistant is controlling this tab" });
    } else {
      chrome.action.setBadgeText({ tabId, text: "" });
      chrome.action.setTitle({ tabId, title: "" });
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
function parseModifiers(spec) {
  if (!spec) { return 0; }
  let mods = 0;
  for (const part of String(spec).split("+")) {
    const bit = MODIFIER_BITS[part.trim().toLowerCase()];
    if (bit) { mods |= bit; }
  }
  return mods;
}

// A click at a CSS-pixel viewport point with button/count/modifiers. For clickCount > 1 the
// press/release pair repeats with an incrementing clickCount, which is how Chrome derives
// dblclick (2) and tripleclick (3). The agent cursor is moved there first.
async function clickAt(tabId, x, y, opts) {
  opts = opts || {};
  const button = opts.button === "right" || opts.button === "middle" ? opts.button : "left";
  const mask = MOUSE_BUTTONS[button] || 1;
  const clickCount = opts.clickCount === 2 || opts.clickCount === 3 ? opts.clickCount : 1;
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
// descendant of it), or is something else painted over it? Returns {occluded, by} best-effort;
// a failure to hit-test is reported as not-occluded (never blocks the click). This lets a click
// warn the model when an overlay/modal intercepted it, so it can dismiss it or use js_click.
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
    const hitResolved = await sendCdp(tabId, "DOM.resolveNode", { backendNodeId: hitBackend }).catch(() => null);
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
    return { occluded: false };
  }
}

async function handleClick(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  const button = args.button === "right" || args.button === "middle" ? args.button : "left";
  const clickCount = Number(args.click_count) === 2 || Number(args.click_count) === 3 ? Number(args.click_count) : 1;
  const occ = await occlusionAt(tabId, point.x, point.y, args.backendNodeId);
  await clickAt(tabId, point.x, point.y, { button, clickCount, modifiers: parseModifiers(args.modifiers) });
  const out = { ok: true, x: Math.round(point.x), y: Math.round(point.y), button, click_count: clickCount };
  if (occ.occluded) { out.occluded_by = occ.by; }
  return out;
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
  if (hold > 0) { await new Promise((r) => setTimeout(r, hold)); }
  for (let i = 1; i <= steps; i++) {
    const x = from.x + ((to.x - from.x) * i) / steps;
    const y = from.y + ((to.y - from.y) * i) / steps;
    await moveAgentCursor(tabId, x, y);
    await dispatchMouse(tabId, "mouseMoved", x, y, { button: "left", buttons: 1 });
  }
  await dispatchMouse(tabId, "mouseReleased", to.x, to.y, { button: "left", buttons: 0, clickCount: 1 });
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
  // conversion, a scroll would make viewport coordinates point elsewhere, and a navigation
  // would put them on a different document. The coordinates are only valid against the exact
  // image the model measured.
  const now = await viewportState(tabId);
  if (!now.ok || now.dpr !== shot.dpr || now.href !== shot.href ||
      now.scrollX !== shot.scrollX || now.scrollY !== shot.scrollY) {
    lastShot = null;
    throw new Error(
      "The page moved (scrolled, zoomed, or navigated) since the screenshot; take a fresh " +
        "browser_screenshot before browser_click_at.");
  }
  // Screenshot pixels are DEVICE pixels; convert to the CSS pixels CDP Input wants using the
  // dpr captured WITH the screenshot (not a re-read), so the two can never disagree.
  const button = args.button === "right" || args.button === "middle" ? args.button : "left";
  const clickCount = Number(args.click_count) === 2 || Number(args.click_count) === 3 ? Number(args.click_count) : 1;
  await clickAt(tabId, sx / shot.dpr, sy / shot.dpr, { button, clickCount, modifiers: parseModifiers(args.modifiers) });
  return { ok: true, x: Math.round(sx), y: Math.round(sy), button, click_count: clickCount };
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
  await sendCdp(tabId, "Input.insertText", { text });
  if (args.submit === true) {
    await dispatchKey(tabId, KEY_DEFS.enter, 0);
  }
  return { ok: true, typed: text.length, submitted: args.submit === true };
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
  const hasValue = typeof args.value === "string" && args.value.length > 0;
  const hasLabel = typeof args.label === "string" && args.label.length > 0;
  const hasIndex = Number.isInteger(args.index);
  const hasValues = Array.isArray(args.values) && args.values.length > 0;
  if (!hasValue && !hasLabel && !hasIndex && !hasValues) {
    throw new Error("browser_select needs one of value, label, index, or values.");
  }
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  await moveAgentCursor(tabId, point.x, point.y);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  // One CONSTANT function body covers single (value/label/index) and multi (values) selection;
  // all match criteria arrive as CDP argument VALUES, never interpolated into code.
  const selectFn = function (mode, value, label, index, values) {
    var el = this;
    if (!el || el.tagName !== "SELECT") {
      return { ok: false, error: "the ref is not a <select> element" };
    }
    if (mode === "values") {
      if (!el.multiple) {
        return { ok: false, error: "the <select> is not a multiple-select; use value/label/index" };
      }
      var want = {};
      for (var k = 0; k < values.length; k++) { want[String(values[k])] = true; }
      var picked = [];
      for (var j = 0; j < el.options.length; j++) {
        var o = el.options[j];
        o.selected = !!(want[o.value] || want[o.text]);
        if (o.selected) { picked.push({ value: o.value, label: o.text }); }
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
  const mode = hasValues ? "values" : hasValue ? "value" : hasLabel ? "label" : "index";
  const result = await callOnNode(tabId, objectId, selectFn, [
    mode,
    hasValue ? args.value : "",
    hasLabel ? args.label : "",
    hasIndex ? args.index : -1,
    hasValues ? args.values.map(String) : [],
  ]);
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
  const resolved = await sendCdp(tabId, "DOM.resolveNode", { backendNodeId });
  const objectId = resolved && resolved.object && resolved.object.objectId;
  if (!objectId) {
    throw new Error("Could not resolve the element; take a fresh snapshot.");
  }
  return objectId;
}

async function callOnNode(tabId, objectId, fn, args) {
  const call = await sendCdp(tabId, "Runtime.callFunctionOn", {
    objectId,
    functionDeclaration: fn.toString(),
    arguments: (args || []).map((v) => ({ value: v })),
    returnByValue: true,
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
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const point = await resolveActionPoint(tabId, args.backendNodeId).catch(() => null);
  if (point) { await moveAgentCursor(tabId, point.x, point.y); }
  const setFn = function (mode, value, checked) {
    var el = this;
    if (!el) { return { ok: false, error: "no element" }; }
    var type = (el.type || "").toLowerCase();
    if (mode === "checked" && (type === "checkbox" || type === "radio")) {
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
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const mediaFn = function (action, num, hasNum) {
    var el = this;
    if (!el || (el.tagName !== "VIDEO" && el.tagName !== "AUDIO")) {
      return { ok: false, error: "the ref is not an <audio> or <video> element" };
    }
    try {
      if (action === "play") { var p = el.play(); if (p && p.catch) { p.catch(function () {}); } }
      else if (action === "pause") { el.pause(); }
      else if (action === "mute") { el.muted = true; }
      else if (action === "unmute") { el.muted = false; }
      else if (action === "seek") { if (hasNum) { el.currentTime = num; } }
      else if (action === "volume") { if (hasNum) { el.volume = Math.max(0, Math.min(1, num)); } }
      else if (action === "rate") { if (hasNum) { el.playbackRate = num; } }
      else { return { ok: false, error: "unknown media action: " + action }; }
    } catch (e) { return { ok: false, error: String(e && e.message ? e.message : e) }; }
    return {
      ok: true, paused: el.paused, muted: el.muted, currentTime: el.currentTime,
      duration: isFinite(el.duration) ? el.duration : null, volume: el.volume, rate: el.playbackRate,
    };
  };
  const r = await callOnNode(tabId, objectId, mediaFn, [action, isNaN(num) ? 0 : num, !isNaN(num)]);
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
  const docObj = await sendCdp(tabId, "Runtime.evaluate", { expression: "document", returnByValue: false });
  const objectId = docObj && docObj.result && docObj.result.objectId;
  if (!objectId) { return false; }
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

async function bodyText(tabId) {
  const res = await sendCdp(tabId, "Runtime.evaluate", {
    expression: "document.body ? document.body.innerText : ''",
    returnByValue: true,
  }).catch(() => null);
  return res && res.result && typeof res.result.value === "string" ? res.result.value : "";
}

// Poll until a page condition holds or the timeout elapses. Read-only: it reads text/URL or
// checks element presence, never mutating the page. Exactly one of text / url_contains /
// selector; `absent` inverts the wait (gone instead of present). Text/selector conditions run
// injection-safely (text compared here, selector passed as a CDP param).
async function handleWaitFor(tabId, args) {
  await ensureAttached(tabId);
  const timeout = Math.min(120000, Math.max(0, Number(args.timeout_ms) || 8000));
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
  const idleMs = Math.min(30000, Math.max(0, Number(args.idle_ms) || 500));
  const holds = async () => {
    if (netIdle) {
      return inflightRequests <= 0 && Date.now() - lastNetworkActivityMs >= idleMs;
    }
    let hit;
    if (text) { hit = (await bodyText(tabId)).includes(text); }
    else if (urlSub) { hit = (await tabInfo(tabId)).url.includes(urlSub); }
    else { hit = await selectorPresent(tabId, selector); }
    return absent ? !hit : hit;
  };
  const started = Date.now();
  for (;;) {
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
// text, disabled). Read-only; constant function body.
async function handleGetValue(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const getFn = function () {
    var el = this;
    if (!el) { return { ok: false, error: "no element" }; }
    var out = { ok: true, tag: String(el.tagName || "").toLowerCase() };
    if ("value" in el) { out.value = String(el.value); }
    if (typeof el.checked === "boolean") { out.checked = el.checked; }
    if (el.tagName === "SELECT") {
      out.multiple = !!el.multiple;
      out.selected = Array.prototype.map.call(el.selectedOptions || [],
        function (o) { return { value: o.value, label: o.text }; });
    }
    if (el.isContentEditable) { out.text = String(el.textContent || "").slice(0, 5000); }
    out.disabled = !!el.disabled;
    return out;
  };
  const r = await callOnNode(tabId, objectId, getFn, []);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_get_value failed"); }
  return r;
}

// Read one attribute (name given) or all attributes of a ref. Read-only; values are page-
// derived, so each is length-capped to keep one giant data: URI from flooding the reply.
async function handleGetAttribute(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const objectId = await resolveNodeObjectId(tabId, args.backendNodeId);
  const name = typeof args.name === "string" ? args.name : "";
  const attrFn = function (name) {
    var el = this;
    if (!el || !el.getAttribute) { return { ok: false, error: "element has no attributes" }; }
    var cap = function (v) { return v === null ? null : String(v).slice(0, 2048); };
    if (name) { return { ok: true, name: name, value: cap(el.getAttribute(name)) }; }
    var all = {};
    for (var i = 0; i < el.attributes.length; i++) { all[el.attributes[i].name] = cap(el.attributes[i].value); }
    return { ok: true, attributes: all };
  };
  const r = await callOnNode(tabId, objectId, attrFn, [name]);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_get_attribute failed"); }
  return r;
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
  let model;
  try {
    model = await sendCdp(tabId, "DOM.getBoxModel", { backendNodeId: args.backendNodeId });
  } catch (_e) {
    return { ok: true, laid_out: false };
  }
  const quad = model && model.model && model.model.content;
  if (!quad || quad.length < 8) { return { ok: true, laid_out: false }; }
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
  }
  out.dpr = vp.dpr;
  const occ = await occlusionAt(tabId, center.x, center.y, args.backendNodeId);
  out.occluded = occ.occluded;
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
  return { ok: true };
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
  return { ok: true };
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
    el.click();
    return { ok: true };
  };
  const r = await callOnNode(tabId, objectId, clickFn, []);
  if (!r || !r.ok) { throw new Error((r && r.error) || "browser_js_click failed"); }
  return { ok: true };
}

function keyDefinition(token) {
  const named = KEY_DEFS[token.toLowerCase()];
  if (named) {
    return named;
  }
  if (token.length === 1) {
    const upper = token.toUpperCase();
    if (/[A-Z]/.test(upper)) {
      // key casing follows the (separate) Shift modifier; shortcut matching uses
      // code/keyCode + modifiers, so emit lower-case key to keep DOM state consistent.
      return { key: token.toLowerCase(), code: "Key" + upper, keyCode: upper.charCodeAt(0),
        text: token };
    }
    if (/[0-9]/.test(upper)) {
      return { key: token, code: "Digit" + upper, keyCode: upper.charCodeAt(0), text: token };
    }
    const oem = OEM_KEYS[token];
    if (oem) {
      return { key: token, code: oem[0], keyCode: oem[1], text: token };
    }
  }
  throw new Error("Unsupported key: " + token);
}

function parseChord(keys) {
  const parts = String(keys || "").split("+").map((p) => p.trim()).filter(Boolean);
  if (!parts.length) {
    throw new Error('pressKey needs a key or chord, e.g. "Enter" or "Control+A".');
  }
  const keyToken = parts.pop();
  let modifiers = 0;
  for (const mod of parts) {
    const bit = MODIFIER_BITS[mod.toLowerCase()];
    if (!bit) {
      throw new Error("Unknown modifier: " + mod);
    }
    modifiers |= bit;
  }
  return { modifiers, def: keyDefinition(keyToken) };
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

async function handlePressKey(tabId, args) {
  await ensureAttached(tabId);
  const { modifiers, def } = parseChord(args.keys);
  await dispatchKey(tabId, def, modifiers);
  return { ok: true, keys: String(args.keys) };
}

async function handleScroll(tabId, args) {
  await ensureAttached(tabId);
  const requested = Number(args.amount);
  const amount = Number.isFinite(requested) && requested > 0 ? requested : DEFAULT_SCROLL_PX;
  const deltaY = args.direction === "up" ? -amount : amount;
  let point;
  if (typeof args.backendNodeId === "number") {
    requireSnapshotTab(tabId);
    point = await resolveActionPoint(tabId, args.backendNodeId);
  } else {
    const metrics = await sendCdp(tabId, "Page.getLayoutMetrics", {}).catch(() => null);
    const vp = metrics && (metrics.cssVisualViewport || metrics.visualViewport);
    point = vp
      ? { x: (vp.clientWidth || 800) / 2, y: (vp.clientHeight || 600) / 2 }
      : { x: 200, y: 200 };
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
// prompt) -- and reports the last dialog the extension handled.
async function handleDialog(tabId, args) {
  await ensureAttached(tabId);
  const accept = !!(args && args.action === "accept");
  const text = args && typeof args.text === "string" ? args.text : undefined;
  pendingDialogPolicy = { accept, text };
  return {
    ok: true,
    armed: accept ? "accept" : "dismiss",
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

function waitForComplete(tabId) {
  return new Promise((resolve) => {
    let done = false;
    const finish = () => {
      if (done) {
        return;
      }
      done = true;
      chrome.tabs.onUpdated.removeListener(listener);
      resolve();
    };
    const listener = (id, info) => {
      if (id === tabId && info.status === "complete") {
        finish();
      }
    };
    chrome.tabs.onUpdated.addListener(listener);
    setTimeout(finish, NAV_TIMEOUT_MS);
  });
}

async function handleNavigate(tabId, args) {
  const url = normalizeUrl(args && args.url);
  if (!url) {
    throw new Error("navigate requires an http(s) URL.");
  }
  await chrome.tabs.update(tabId, { url });
  await waitForComplete(tabId);
  const info = await tabInfo(tabId);
  return { ok: true, url: info.url, title: info.title };
}

async function handleHistory(direction) {
  const tab = await activeTab();
  if (direction === "back") {
    await chrome.tabs.goBack(tab.id);
  } else {
    await chrome.tabs.goForward(tab.id);
  }
  await waitForComplete(tab.id);
  const info = await tabInfo(tab.id);
  return { ok: true, url: info.url, title: info.title };
}

async function handleReload() {
  const tab = await activeTab();
  await chrome.tabs.reload(tab.id);
  await waitForComplete(tab.id);
  const info = await tabInfo(tab.id);
  return { ok: true, url: info.url, title: info.title };
}

async function handleListTabs() {
  const tabs = (await chrome.tabs.query({ lastFocusedWindow: true })).sort((a, b) => a.index - b.index);
  const groupTitles = new Map();
  const list = [];
  for (const t of tabs) {
    const entry = { index: t.index, title: t.title || "", url: t.url || "", active: !!t.active };
    if (typeof t.groupId === "number" && t.groupId >= 0) {
      entry.group_id = t.groupId;
      if (!groupTitles.has(t.groupId)) {
        try { const g = await chrome.tabGroups.get(t.groupId); groupTitles.set(t.groupId, g.title || ""); } catch (_e) { groupTitles.set(t.groupId, ""); }
      }
      entry.group = groupTitles.get(t.groupId);
    }
    list.push(entry);
  }
  return { tabs: list };
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
  const indices = String(spec).split(",").map((s) => Number(s.trim())).filter((n) => Number.isInteger(n));
  if (!indices.length) { throw new Error("tab_indices must be comma-separated integers, e.g. \"0,1\"."); }
  const ids = [];
  for (const idx of indices) {
    const t = tabs.find((x) => x.index === idx);
    if (!t) { throw new Error("No tab at index " + idx + "."); }
    ids.push(t.id);
  }
  return ids;
}

async function handleGroupTabs(args) {
  const tabIds = await tabIdsFromIndices(args && args.tab_indices);
  const groupId = await chrome.tabs.group({ tabIds });
  const update = {};
  if (args && typeof args.title === "string" && args.title.length > 0) { update.title = args.title; }
  if (args && typeof args.color === "string" && GROUP_COLORS.has(args.color)) { update.color = args.color; }
  if (Object.keys(update).length > 0) { await chrome.tabGroups.update(groupId, update); }
  let group = { title: update.title || "", color: update.color || "" };
  try { group = await chrome.tabGroups.get(groupId); } catch (_e) {}
  return { ok: true, group_id: groupId, title: group.title || "", color: group.color || "", tab_count: tabIds.length };
}

async function handleUngroupTabs(args) {
  const tabIds = await tabIdsFromIndices(args && args.tab_indices);
  await chrome.tabs.ungroup(tabIds);
  return { ok: true, ungrouped: tabIds.length };
}

async function tabByIndex(index) {
  const tabs = await chrome.tabs.query({ lastFocusedWindow: true });
  const tab = tabs.find((t) => t.index === index);
  if (!tab) {
    throw new Error("No tab at index " + index + ".");
  }
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
  const url = args && args.url ? normalizeUrl(args.url) : null;
  if (args && args.url && !url) {
    throw new Error("newTab url must be http(s).");
  }
  const tab = await chrome.tabs.create(url ? { url } : {});
  return { ok: true, index: tab.index, url: tab.url || url || "" };
}

async function handleCloseTab(args) {
  const tab =
    args && args.index !== undefined && args.index !== null
      ? await tabByIndex(Number(args.index))
      : await activeTab();
  await chrome.tabs.remove(tab.id);
  return { ok: true, index: tab.index };
}

// -- browser windows (chrome.windows) ----------------------------------------
//
// A model-opened link can spawn a popup or a new window; browser_windows lists them (and
// browser_tabs lists the tabs inside) so nothing opened off-screen is invisible. window CRUD
// mutates the browser (opening/closing windows), so it is gated at the normal-confirm tier.

async function handleListWindows() {
  const wins = await chrome.windows.getAll({ populate: true });
  const list = wins.map((w) => ({
    window_id: w.id,
    focused: !!w.focused,
    type: w.type || "normal",
    state: w.state || "",
    tab_count: (w.tabs || []).length,
    incognito: !!w.incognito,
  }));
  return { windows: list };
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
    return {
      ok: true, window_id: win.id, type: win.type || "normal",
      tab_index: firstTab ? firstTab.index : null,
    };
  }
  const windowId = Number(args && args.window_id);
  if (!Number.isInteger(windowId)) {
    throw new Error("browser_window needs window_id for focus/close (see browser_windows).");
  }
  if (action === "focus") {
    await chrome.windows.update(windowId, { focused: true });
    return { ok: true, window_id: windowId, focused: true };
  }
  await chrome.windows.remove(windowId);  // action === "close"
  return { ok: true, window_id: windowId, closed: true };
}

// -- device emulation (Emulation domain) -------------------------------------
//
// Emulate a device viewport / user-agent / touch for the active tab (responsive testing, UA
// gating, mobile layouts). Overrides are per-CDP-session, so they clear automatically when we
// detach (tab switch / disconnect); reset clears them explicitly and restores the real UA.
async function handleEmulate(tabId, args) {
  await ensureAttached(tabId);
  if (originalUserAgent === null) {
    const r = await sendCdp(tabId, "Runtime.evaluate", { expression: "navigator.userAgent", returnByValue: true }).catch(() => null);
    if (r && r.result && typeof r.result.value === "string") { originalUserAgent = r.result.value; }
  }
  if (args.reset === true) {
    await sendCdp(tabId, "Emulation.clearDeviceMetricsOverride").catch(() => {});
    await sendCdp(tabId, "Emulation.setTouchEmulationEnabled", { enabled: false }).catch(() => {});
    if (originalUserAgent) {
      await sendCdp(tabId, "Emulation.setUserAgentOverride", { userAgent: originalUserAgent }).catch(() => {});
    }
    return { ok: true, reset: true };
  }
  const applied = {};
  const w = Number(args.width);
  const h = Number(args.height);
  if (Number.isFinite(w) && Number.isFinite(h) && w > 0 && h > 0) {
    const dsf = Number(args.device_scale_factor);
    await sendCdp(tabId, "Emulation.setDeviceMetricsOverride", {
      width: Math.round(w), height: Math.round(h),
      deviceScaleFactor: Number.isFinite(dsf) && dsf > 0 ? dsf : 1,
      mobile: args.mobile === true,
    });
    applied.width = Math.round(w);
    applied.height = Math.round(h);
    applied.mobile = args.mobile === true;
  }
  if (typeof args.user_agent === "string" && args.user_agent.length > 0) {
    await sendCdp(tabId, "Emulation.setUserAgentOverride", { userAgent: args.user_agent });
    applied.user_agent = args.user_agent.slice(0, 120);
  }
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
  return { ok: true, applied };
}

// -- print to PDF (Page.printToPDF) ------------------------------------------
//
// Render the active tab to a PDF at the browser level (no OS print dialog, no printer).
// The base64 PDF rides back in the reply; the bridge decodes it, size-caps it, and writes
// the file to disk -- the extension never touches the filesystem. Backgrounds print by
// default (technicians usually want the page as it looks). Only well-formed numeric/enum
// options are forwarded, so a page can neither steer the output path nor inject options.
async function handlePrint(tabId, args) {
  await ensureAttached(tabId);
  const opts = { transferMode: "ReturnAsBase64" };
  if (typeof args.landscape === "boolean") { opts.landscape = args.landscape; }
  opts.printBackground = args.print_background === false ? false : true;
  const scale = Number(args.scale);
  if (Number.isFinite(scale) && scale >= 0.1 && scale <= 2) { opts.scale = scale; }
  const pw = Number(args.paper_width);
  const ph = Number(args.paper_height);
  if (Number.isFinite(pw) && pw > 0) { opts.paperWidth = pw; }
  if (Number.isFinite(ph) && ph > 0) { opts.paperHeight = ph; }
  if (typeof args.page_ranges === "string" && args.page_ranges.trim().length > 0) {
    opts.pageRanges = args.page_ranges.trim().slice(0, 100);
  }
  const res = await sendCdp(tabId, "Page.printToPDF", opts);
  const data = res && typeof res.data === "string" ? res.data : "";
  if (!data) { throw new Error("The browser returned an empty PDF."); }
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

// Reduce an origin or full URL to an origin match pattern ("https://host:port/*"); returns
// null for anything that is not http(s).
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
  if (!origin) {
    const tab = await activeTab();
    origin = tab && tab.url ? tab.url : null;
  }
  const pattern = origin ? originPattern(origin) : null;
  if (!pattern) {
    throw new Error("browser_permission needs a valid http(s) origin (or an active http(s) tab).");
  }
  const store = chrome.contentSettings[typeKey];
  if (!store || typeof store.set !== "function") {
    throw new Error("This browser cannot set the '" + args.name + "' permission.");
  }
  // set() rejects when a setting does not apply to a type (e.g. 'ask' on images); that error
  // surfaces honestly to the caller rather than being swallowed.
  await store.set({ primaryPattern: pattern, setting });
  return { ok: true, name: String(args.name).toLowerCase(), setting, pattern };
}

// -- web storage (localStorage / sessionStorage) -----------------------------
//
// Read or write the active tab's local/session storage: get/set/remove/clear/keys. The op
// runs as a CONSTANT function via Runtime.callFunctionOn on the page window, with area/action/
// key/value passed as CDP argument VALUES (never interpolated into code), so a page cannot be
// made to run injected script. The function returns a structured result by value and never
// throws across CDP (storage on an opaque/blocked origin comes back as {ok:false,error}).
// Returned keys and values are capped so a huge store cannot flood the transport.

// eslint-disable-next-line no-unused-vars -- args arrive as CDP callFunctionOn values.
function storageOp(area, action, key, value) {
  try {
    var store = area === "session" ? this.sessionStorage : this.localStorage;
    if (action === "get") {
      var v = store.getItem(key);
      if (v !== null && v.length > 20000) {
        return { ok: true, present: true, value: v.slice(0, 20000), truncated: true };
      }
      return { ok: true, present: v !== null, value: v };
    }
    if (action === "set") {
      store.setItem(key, value);
      return { ok: true };
    }
    if (action === "remove") {
      store.removeItem(key);
      return { ok: true };
    }
    if (action === "clear") {
      var n = store.length;
      store.clear();
      return { ok: true, cleared: n };
    }
    var out = [];
    for (var i = 0; i < store.length && out.length < 500; i++) {
      out.push(store.key(i));
    }
    return { ok: true, keys: out, count: store.length, capped: store.length > 500 };
  } catch (e) {
    return { ok: false, error: String(e && e.message ? e.message : e) };
  }
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
  const win = await sendCdp(tabId, "Runtime.evaluate", { expression: "window", returnByValue: false });
  const objectId = win && win.result && win.result.objectId;
  if (!objectId) {
    throw new Error("Could not access the page window.");
  }
  const call = await sendCdp(tabId, "Runtime.callFunctionOn", {
    objectId,
    functionDeclaration: storageOp.toString(),
    arguments: [area, action, args.key || "", action === "set" ? args.value : ""].map((v) => ({ value: v })),
    returnByValue: true,
  });
  const result = call && call.result && call.result.value;
  if (!result || result.ok !== true) {
    const why = result && result.error ? result.error : "storage unavailable (the page origin may block it)";
    throw new Error("browser_storage failed: " + why);
  }
  return result;
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
  return {
    name: c.name,
    value: c.value && c.value.length > 4096 ? c.value.slice(0, 4096) : c.value,
    domain: c.domain,
    path: c.path,
    secure: !!c.secure,
    http_only: !!c.httpOnly,
    same_site: c.sameSite || null,
    session: !!c.session,
    expires: c.expirationDate || null,
  };
}

async function cookiesSet(url, args) {
  if (typeof args.value !== "string") {
    throw new Error("browser_cookies set needs a string value.");
  }
  const details = { url, name: String(args.name), value: args.value };
  if (typeof args.path === "string" && args.path) { details.path = args.path; }
  if (typeof args.secure === "boolean") { details.secure = args.secure; }
  if (typeof args.http_only === "boolean") { details.httpOnly = args.http_only; }
  const ss = typeof args.same_site === "string" ? COOKIE_SAME_SITE[args.same_site.toLowerCase()] : null;
  if (ss) { details.sameSite = ss; }
  const days = Number(args.expires_days);
  if (Number.isFinite(days) && days > 0) {
    details.expirationDate = Math.floor(Date.now() / 1000) + Math.round(days * 86400);
  }
  const cookie = await chrome.cookies.set(details);
  if (!cookie) {
    throw new Error("browser_cookies set failed (the browser rejected the cookie).");
  }
  return { ok: true, url, name: details.name, set: true, domain: cookie.domain };
}

async function handleCookies(args) {
  const action = String((args && args.action) || "").toLowerCase();
  if (["get", "set", "remove"].indexOf(action) < 0) {
    throw new Error("browser_cookies action must be get, set, or remove.");
  }
  let url = args && args.url ? String(args.url) : null;
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
async function pollDownload(id, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let item = null;
  while (Date.now() < deadline) {
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
  const item = await pollDownload(id, timeoutMs);
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
async function handleHttpAuth(tabId, args) {
  await ensureAttached(tabId);
  if (args && args.clear === true) {
    httpAuthCreds = null;
    await sendCdp(tabId, "Fetch.disable").catch(() => {});
    return { ok: true, armed: false };
  }
  const username = args && typeof args.username === "string" ? args.username : "";
  const password = args && typeof args.password === "string" ? args.password : "";
  if (!username) {
    throw new Error("browser_http_auth needs a username (or clear:true to disarm).");
  }
  httpAuthCreds = { username, password };
  await sendCdp(tabId, "Fetch.enable",
    { handleAuthRequests: true, patterns: [{ urlPattern: "*" }] });
  return { ok: true, armed: true, username };
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
