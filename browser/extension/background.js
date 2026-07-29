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
// Input actions (click/type/pressKey/scroll) and screenshot are deferred to later units
// and answered with an explicit "not supported yet" error.
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

let port = null;
let health = { connected: false, bridge: null, error: null };
let attachedTabId = null;
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
    case "type":
    case "pressKey":
    case "scroll":
      throw new Error("Input actions are not enabled in this build yet.");
    case "screenshot":
      throw new Error("Screenshots are not enabled in this build yet.");
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
  }
});

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
