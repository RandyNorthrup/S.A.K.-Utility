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
// The agent's own on-page cursor: a page-injected overlay so the user SEES where the
// agent acts, distinct from their untouched OS cursor. Cosmetic only (never a security
// boundary): a hostile page can hide it, but cannot use it to drive input.
const AGENT_CURSOR_ID = "__sak_agent_cursor__";

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
    case "type":
      return await handleType(await activeTabId(), args);
    case "pressKey":
      return await handlePressKey(await activeTabId(), args);
    case "scroll":
      return await handleScroll(await activeTabId(), args);
    case "screenshot":
      return await handleScreenshot(await activeTabId(), args);
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
  if (attachedTabId === tabId) {
    return;
  }
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
}

async function detachAll(_reason) {
  lastSnapshotTabId = null;  // any ref_index is now unverifiable against a live tab
  lastShot = null;           // and any screenshot coordinates are against a dead render
  if (attachedTabId === null) {
    return;
  }
  const tabId = attachedTabId;
  attachedTabId = null;
  try {
    await chrome.debugger.detach({ tabId });
  } catch (_e) {
    // Already gone (tab closed / user detached); nothing to do.
  }
}

// The user opening DevTools, or the tab closing, force-detaches our session.
chrome.debugger.onDetach.addListener((source) => {
  if (source && source.tabId === attachedTabId) {
    attachedTabId = null;
    lastSnapshotTabId = null;
    lastShot = null;
  }
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

async function captureSnapshot(tabId) {
  await ensureAttached(tabId);
  lastSnapshotTabId = tabId;  // refs from this snapshot are valid only against this tab
  const snapshot = await sendCdp(tabId, "DOMSnapshot.captureSnapshot", { computedStyles: [] });
  const boundsByBackend = buildBoundsMap(snapshot);
  const axTree = await sendCdp(tabId, "Accessibility.getFullAXTree", {});
  const { nodes, truncated } = buildNodes(axTree, boundsByBackend);
  const info = await tabInfo(tabId);
  // getFullAXTree + DOMSnapshot cover the main frame and its SAME-process subframes.
  // Cross-origin (out-of-process) iframes live in separate targets this single pass does
  // not reach, so their content is absent. Detect that (more frames exist than
  // same-process documents) and flag it, so the model is told the capture is partial
  // instead of concluding those elements do not exist.
  let iframesOmitted = false;
  try {
    const tree = await sendCdp(tabId, "Page.getFrameTree", {});
    const totalFrames = countFrames(tree && tree.frameTree);
    const sameProcessDocs = ((snapshot && snapshot.documents) || []).length;
    iframesOmitted = totalFrames > sameProcessDocs;
  } catch (_e) {
    // If the frame tree is unavailable, do not claim completeness we cannot verify.
    iframesOmitted = true;
  }
  return { url: info.url, title: info.title, nodes, truncated, iframesOmitted };
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
  const res = await sendCdp(tabId, "Page.captureScreenshot", params);
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

// Move the agent's own on-page cursor to (x,y). Numbers are coerced to finite integers,
// so nothing page- or model-controlled is interpolated as code.
function agentCursorScript(x, y) {
  const px = Number.isFinite(x) ? Math.round(x) : 0;
  const py = Number.isFinite(y) ? Math.round(y) : 0;
  return (
    "(function(){var c=document.getElementById('" + AGENT_CURSOR_ID + "');" +
    "if(!c){c=document.createElement('div');c.id='" + AGENT_CURSOR_ID + "';" +
    "c.style.cssText='position:fixed;z-index:2147483647;width:18px;height:18px;" +
    "margin:-9px 0 0 -9px;border-radius:50%;border:2px solid #1e88e5;" +
    "background:rgba(30,136,229,.25);pointer-events:none;transition:left .12s,top .12s;" +
    "box-shadow:0 0 6px rgba(30,136,229,.9)';" +
    "(document.body||document.documentElement).appendChild(c);}" +
    "c.style.left='" + px + "px';c.style.top='" + py + "px';})();"
  );
}

async function moveAgentCursor(tabId, x, y) {
  await sendCdp(tabId, "Runtime.evaluate", { expression: agentCursorScript(x, y) }).catch(() => {});
}

async function dispatchMouse(tabId, type, x, y, extra) {
  await sendCdp(tabId, "Input.dispatchMouseEvent", Object.assign({ type, x, y }, extra || {}));
}

// A left click at a CSS-pixel viewport point: move the agent cursor there, then the
// move/press/release triad CDP needs for a real click.
async function leftClickAt(tabId, x, y) {
  await moveAgentCursor(tabId, x, y);
  await dispatchMouse(tabId, "mouseMoved", x, y, {});
  await dispatchMouse(tabId, "mousePressed", x, y, { button: "left", buttons: 1, clickCount: 1 });
  await dispatchMouse(tabId, "mouseReleased", x, y, { button: "left", buttons: 0, clickCount: 1 });
}

async function handleClick(tabId, args) {
  await ensureAttached(tabId);
  requireSnapshotTab(tabId);
  const point = await resolveActionPoint(tabId, args.backendNodeId);
  await leftClickAt(tabId, point.x, point.y);
  return { ok: true, x: Math.round(point.x), y: Math.round(point.y) };
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
  await leftClickAt(tabId, sx / shot.dpr, sy / shot.dpr);
  return { ok: true, x: Math.round(sx), y: Math.round(sy) };
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
  const tabs = await chrome.tabs.query({ lastFocusedWindow: true });
  const list = tabs
    .sort((a, b) => a.index - b.index)
    .map((t) => ({ index: t.index, title: t.title || "", url: t.url || "", active: !!t.active }));
  return { tabs: list };
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
