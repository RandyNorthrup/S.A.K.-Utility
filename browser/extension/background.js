// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// S.A.K. Utility Browser Control -- background service worker.
//
// Unit 3 scope: establish and verify the native-messaging bridge to the S.A.K.
// native host (the sak_win32_mcp executable launched by Chrome in host mode) with
// a ping/pong handshake. Later units add the DOM snapshot, CDP attach, and gated
// input on top of this same port.

const HOST_NAME = "com.sak.browsercontrol";

// Bridge protocol version the extension speaks; must match kBrowserBridgeProtocol
// on the native side. A mismatch is surfaced rather than silently tolerated.
const BRIDGE_PROTOCOL = 1;

let port = null;
let health = { connected: false, lastPong: null, error: null };

function disconnect(reason) {
  health.connected = false;
  health.error = reason || null;
  port = null;
}

function onHostMessage(msg) {
  if (!msg || typeof msg !== "object") {
    return;
  }
  if (msg.type === "pong") {
    health.connected = true;
    health.error = null;
    health.lastPong = { at: Date.now(), server: msg.server, version: msg.version };
    if (msg.protocol !== BRIDGE_PROTOCOL) {
      health.error =
        "Bridge protocol mismatch: host " + msg.protocol + ", extension " + BRIDGE_PROTOCOL;
    }
    console.info("[SAK] host alive:", msg.server, msg.version, "pid", msg.pid);
  } else if (msg.type === "error") {
    console.warn("[SAK] host error:", msg.error);
  }
}

function connect() {
  if (port) {
    return port;
  }
  try {
    port = chrome.runtime.connectNative(HOST_NAME);
  } catch (e) {
    disconnect(String(e));
    console.error("[SAK] connectNative threw:", e);
    return null;
  }
  port.onMessage.addListener(onHostMessage);
  port.onDisconnect.addListener(() => {
    const err = chrome.runtime.lastError;
    disconnect(err ? err.message : "port closed");
    console.warn("[SAK] host disconnected:", health.error);
  });
  return port;
}

function ping() {
  const p = connect();
  if (!p) {
    return;
  }
  try {
    p.postMessage({ type: "ping", id: Date.now() });
  } catch (e) {
    disconnect(String(e));
    console.error("[SAK] postMessage threw:", e);
  }
}

// Verify the bridge when the extension loads, when the browser starts, and on
// demand from the toolbar button.
chrome.runtime.onInstalled.addListener(ping);
chrome.runtime.onStartup.addListener(ping);
chrome.action.onClicked.addListener(ping);

// Expose the last health snapshot to anything that asks (popup / options later).
chrome.runtime.onMessage.addListener((request, _sender, sendResponse) => {
  if (request && request.type === "sak.getHealth") {
    ping();
    sendResponse(health);
  }
  return false;
});
