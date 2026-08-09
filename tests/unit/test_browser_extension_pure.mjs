// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Unit tests for the pure decision functions inside the browser-control service worker.
//
// WHY THIS EXISTS
// browser/extension/background.js drives a real user's browser over the DevTools protocol,
// parses page-controlled data, and holds every fail-closed guard in the browser surface --
// and until this file existed, NO gate had ever seen it. clang-format, clang-tidy, cppcheck
// and lizard are all wired to C/C++ by construction, and there was no test harness of any
// kind, so ~3300 lines of privileged JavaScript rested entirely on review (R5-G21-9).
//
// The worker is loaded and evaluated AS SHIPPED rather than split into a testable module.
// Extracting the pure functions into their own file would have meant changing the artifact
// that is packed, signed and installed, and then testing the copy instead of the thing that
// runs. background.js only touches chrome at top level to register listeners and to call
// connect(), so a recording stub satisfies it and every top-level function becomes callable.
// The consequence worth stating: these tests exercise the exact bytes that ship.
//
// Scope is deliberately the PURE functions -- the ones that decide something without asking
// Chrome. Those are where the refusals live (an unnamed pointer button, a non-http URL, a
// navigation that has not actually landed), and a refusal that silently becomes a default is
// the failure mode this repo cares most about.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import vm from "node:vm";

const here = dirname(fileURLToPath(import.meta.url));
const workerPath = join(here, "..", "..", "browser", "extension", "background.js");

// Every chrome.* access resolves to this same callable proxy, so arbitrarily deep chains
// (chrome.debugger.onDetach.addListener(fn)) work without enumerating the API surface.
// It records nothing and returns itself; the tests below never assert on chrome behaviour,
// they assert on decisions the worker makes before it would ever call chrome.
function makeChromeStub() {
  const target = function () {};
  return new Proxy(target, {
    get(_t, prop) {
      if (prop === "then") { return undefined; }  // must not look like a thenable
      return makeChromeStub();
    },
    apply() { return makeChromeStub(); },
    construct() { return makeChromeStub(); },
  });
}

// The names pulled out of the worker's scope. Anything not listed here stays private to the
// worker, which keeps this file from quietly becoming a second copy of its API.
const EXPORTED = [
  "normalizeUrl", "tabSettledAt", "stripFragment", "sameProcessUrls", "capFrameUrls",
  "collectFrameUrls", "countFrames", "parseModifiers", "mouseButton", "clickCountOf",
  "scrollAmountPx", "selectCallArgs", "originsMatch", "defaultDialogAccept",
  "transientReadError", "isEditableRole", "axValue", "MAX_OMITTED_FRAMES",
  "COMMAND_TABLE", "dispatchCommand", "axNodeToCapture", "indexProps",
  "AX_VALUE_MAX_CHARS",
];

function loadWorker() {
  const source = readFileSync(workerPath, "utf8");
  const context = vm.createContext({
    chrome: makeChromeStub(),
    console: { log() {}, warn() {}, error() {}, debug() {} },
    setTimeout, clearTimeout, setInterval, clearInterval,
    URL, TextEncoder, TextDecoder, structuredClone,
  });
  const epilogue = `\n;globalThis.__sak_exports = { ${EXPORTED.join(", ")} };\n`;
  vm.runInContext(source + epilogue, context, { filename: workerPath });
  const exported = context.__sak_exports;
  for (const name of EXPORTED) {
    if (exported[name] === undefined) {
      throw new Error(`background.js no longer defines ${name}; the harness is stale`);
    }
  }
  return exported;
}

const w = loadWorker();

// Values the worker returns are built with the vm realm's intrinsics, so an array it
// created has a different Array.prototype than one written here. deepStrictEqual compares
// prototypes and rejects them as unequal even when every element matches -- which reads
// like a real failure and invites "fixing" it by weakening the assertion to a loose
// compare. Round-tripping through JSON re-creates the value with this realm's intrinsics,
// so the comparison stays strict and only the realm boundary is removed. Safe here because
// every asserted shape is plain strings, numbers and arrays.
function crossRealm(value) {
  return JSON.parse(JSON.stringify(value));
}

test("normalizeUrl refuses every scheme that is not http(s)", () => {
  // The model must not be able to drive the tab to a script or a local file. These are
  // refusals, not sanitizations: there is no "safe" rewrite of javascript: to fall back to.
  for (const hostile of [
    "javascript:alert(1)",
    "data:text/html,<script>alert(1)</script>",
    "file:///C:/Windows/System32/config/SAM",
    "chrome://settings",
    "vbscript:msgbox(1)",
    "  JAVASCRIPT:alert(1)  ",
  ]) {
    assert.equal(w.normalizeUrl(hostile), null, `must refuse ${hostile}`);
  }
});

test("normalizeUrl accepts http(s) and upgrades a bare host", () => {
  assert.equal(w.normalizeUrl("https://example.com/a"), "https://example.com/a");
  assert.equal(w.normalizeUrl("HTTP://example.com"), "HTTP://example.com");
  assert.equal(w.normalizeUrl("example.com"), "https://example.com");
  assert.equal(w.normalizeUrl("example.com/path"), "https://example.com/path");
  assert.equal(w.normalizeUrl(""), null);
  assert.equal(w.normalizeUrl(null), null);
  assert.equal(w.normalizeUrl("not a url"), null);
});

test("tabSettledAt does not mistake the PREVIOUS document for the new one", () => {
  // The bug this guards: between requesting a navigation and reading the tab, the browser
  // may not have started it, and the old document is "complete" too. Settling on that
  // reports the page the caller navigated AWAY from as the freshly loaded one.
  const prior = "https://old.example/page";
  assert.equal(w.tabSettledAt({ status: "complete", url: prior }, prior), false);
  assert.equal(w.tabSettledAt({ status: "complete", url: "https://new.example/" }, prior), true);
  // A pending navigation disqualifies the read regardless of status.
  assert.equal(
    w.tabSettledAt({ status: "complete", url: "https://new.example/", pendingUrl: "https://x/" }, prior),
    false);
  assert.equal(w.tabSettledAt({ status: "loading", url: "https://new.example/" }, prior), false);
  assert.equal(w.tabSettledAt(null, prior), false);
});

test("tabSettledAt treats an unreadable prior url as unresolved, not as settled", () => {
  // priorUrl null means a tab created for this navigation: no previous document exists to
  // be confused with, so any non-empty url is the navigation having landed.
  assert.equal(w.tabSettledAt({ status: "complete", url: "https://new.example/" }, null), true);
  assert.equal(w.tabSettledAt({ status: "complete", url: "" }, null), false);
  // A prior url that is neither a string nor null cannot establish "this is no longer it",
  // so the shortcut is withheld rather than guessed at.
  assert.equal(w.tabSettledAt({ status: "complete", url: "https://new.example/" }, undefined), false);
  assert.equal(w.tabSettledAt({ status: "complete", url: "https://new.example/" }, 42), false);
});

test("mouseButton refuses an unrecognized button instead of defaulting to left", () => {
  // A right-click silently downgraded to a left-click opens no context menu and reports
  // success for an action that never happened.
  assert.equal(w.mouseButton(undefined), "left");
  assert.equal(w.mouseButton(""), "left");
  assert.equal(w.mouseButton("RIGHT"), "right");
  assert.equal(w.mouseButton("middle"), "middle");
  assert.throws(() => w.mouseButton("side"), /Unknown button/);
  assert.throws(() => w.mouseButton("leftt"), /Unknown button/);
});

test("clickCountOf refuses a count that is not a real click gesture", () => {
  assert.equal(w.clickCountOf(undefined), 1);
  assert.equal(w.clickCountOf(2), 2);
  assert.equal(w.clickCountOf("3"), 3);
  assert.throws(() => w.clickCountOf(4), /click_count/);
  assert.throws(() => w.clickCountOf(0), /click_count/);
  assert.throws(() => w.clickCountOf("many"), /click_count/);
});

test("parseModifiers refuses an unknown modifier rather than dropping it", () => {
  // A dropped modifier turns ctrl+click into a plain click: a different action, reported
  // as the one that was asked for.
  assert.equal(w.parseModifiers(undefined), 0);
  assert.equal(w.parseModifiers(""), 0);
  const ctrl = w.parseModifiers("ctrl");
  const shift = w.parseModifiers("shift");
  assert.ok(ctrl > 0 && shift > 0);
  assert.equal(w.parseModifiers("ctrl+shift"), ctrl | shift);
  assert.equal(w.parseModifiers("CTRL + SHIFT"), ctrl | shift);
  assert.throws(() => w.parseModifiers("hyper"), /Unknown modifier/);
  assert.throws(() => w.parseModifiers("ctrl+hyper"), /Unknown modifier/);
});

test("scrollAmountPx refuses a non-positive or non-finite distance", () => {
  assert.ok(w.scrollAmountPx(undefined) > 0);
  assert.equal(w.scrollAmountPx(120), 120);
  assert.throws(() => w.scrollAmountPx(0), /positive/);
  assert.throws(() => w.scrollAmountPx(-40), /positive/);
  assert.throws(() => w.scrollAmountPx("lots"), /positive/);
  assert.throws(() => w.scrollAmountPx(Infinity), /positive/);
});

test("selectCallArgs requires exactly one selection criterion", () => {
  assert.throws(() => w.selectCallArgs({}), /needs one of/);
  assert.throws(() => w.selectCallArgs({ value: "", label: "" }), /needs one of/);
  assert.throws(() => w.selectCallArgs({ value: "a", index: 2 }), /exactly one/);
  assert.throws(() => w.selectCallArgs({ label: "a", values: ["b"] }), /exactly one/);

  assert.deepEqual(crossRealm(w.selectCallArgs({ value: "a" })), ["value", "a", "", -1, []]);
  assert.deepEqual(crossRealm(w.selectCallArgs({ label: "L" })), ["label", "", "L", -1, []]);
  // index 0 is a real choice and must not be read as "absent".
  assert.deepEqual(crossRealm(w.selectCallArgs({ index: 0 })), ["index", "", "", 0, []]);
  assert.deepEqual(crossRealm(w.selectCallArgs({ values: ["x", 2] })),
                   ["values", "", "", -1, ["x", "2"]]);
});

test("capFrameUrls reports truncation explicitly instead of just returning fewer frames", () => {
  // A caller that cannot tell a complete list from a capped one treats "these are the
  // frames" and "these are some of the frames" as the same answer.
  const cap = w.MAX_OMITTED_FRAMES;
  const many = [];
  for (let i = 0; i < cap + 5; i++) { many.push(`https://f${i}.example/`); }

  const capped = w.capFrameUrls(many, null);
  assert.equal(capped.frames.length, cap);
  assert.equal(capped.truncated, true);

  const few = w.capFrameUrls(["https://a.example/", "https://b.example/"], null);
  assert.equal(few.frames.length, 2);
  assert.equal(few.truncated, false);
});

test("capFrameUrls drops non-http, already-covered and duplicate frames", () => {
  const covered = new Set(["https://covered.example/"]);
  const out = w.capFrameUrls([
    "https://covered.example/",          // already captured
    "https://covered.example/#anchor",   // same document, fragment only
    "about:blank",                       // not http(s)
    "data:text/html,x",                  // not http(s)
    "https://kept.example/",
    "https://kept.example/",             // duplicate
  ], covered);
  assert.deepEqual(crossRealm(out.frames), ["https://kept.example/"]);
  assert.equal(out.truncated, false);
});

test("frame tree walking counts and collects every nested frame", () => {
  const tree = {
    frame: { url: "https://root.example/" },
    childFrames: [
      { frame: { url: "https://a.example/" }, childFrames: [
        { frame: { url: "https://a1.example/" } },
      ] },
      { frame: { url: "https://b.example/" } },
    ],
  };
  assert.equal(w.countFrames(tree), 4);
  assert.equal(w.countFrames(null), 0);

  const urls = [];  // built here, so this one is already same-realm
  w.collectFrameUrls(tree, urls);
  assert.deepEqual(urls, [
    "https://root.example/", "https://a.example/", "https://a1.example/", "https://b.example/",
  ]);
});

test("sameProcessUrls resolves documentURL through the shared string table", () => {
  // DOMSnapshot stores documentURL as an INDEX into snapshot.strings, not as a string.
  const snapshot = {
    strings: ["https://one.example/page#frag", "https://two.example/"],
    documents: [{ documentURL: 0 }, { documentURL: 1 }, { documentURL: 99 }, {}],
  };
  const urls = w.sameProcessUrls(snapshot);
  assert.ok(urls.has("https://one.example/page"));  // fragment stripped
  assert.ok(urls.has("https://two.example/"));
  assert.equal(urls.size, 2);  // out-of-range index and missing key contribute nothing
  assert.equal(w.sameProcessUrls(null).size, 0);
});

test("originsMatch compares origins, and falls back to equality only when unparseable", () => {
  assert.equal(w.originsMatch("https://x.example/a", "https://x.example/b"), true);
  assert.equal(w.originsMatch("https://x.example/", "http://x.example/"), false);
  assert.equal(w.originsMatch("https://x.example/", "https://y.example/"), false);
  assert.equal(w.originsMatch("", "https://x.example/"), false);
  assert.equal(w.originsMatch(null, null), false);
  assert.equal(w.originsMatch("not-a-url", "not-a-url"), true);
});

test("defaultDialogAccept only auto-accepts dialogs with nothing to decide", () => {
  // alert and beforeunload have no meaningful "cancel"; confirm and prompt do, and
  // answering those on the user's behalf is a decision the extension must not make.
  assert.equal(w.defaultDialogAccept("alert"), true);
  assert.equal(w.defaultDialogAccept("beforeunload"), true);
  assert.equal(w.defaultDialogAccept("confirm"), false);
  assert.equal(w.defaultDialogAccept("prompt"), false);
});

// A CDP accessibility node, in the shape Chrome actually sends: role/name are wrapped
// values, and properties is a list of {name, value:{value}} rather than a plain object.
function axNode(role, name, properties, extra) {
  return {
    role: { value: role },
    name: { value: name },
    properties: properties.map(([n, v]) => ({ name: n, value: { value: v } })),
    ...(extra || {}),
  };
}

test("indexProps cannot have its prototype set by a property name", () => {
  // The keys are property names from the accessibility tree and the values are read back by
  // name. On a plain object literal, "__proto__" would set the map's prototype instead of
  // becoming a key, and a later lookup for a state the page never set would resolve through
  // an object the page chose.
  const evil = [
    { name: "__proto__", value: { value: { disabled: true } } },
    { name: "focusable", value: { value: true } },
  ];
  const props = indexPropsOf(evil);
  assert.equal(Object.getPrototypeOf(props), null, "props must have no prototype");
  assert.equal(props.disabled, undefined, "a state the page never set must not resolve");
  assert.equal(props.focusable, true);
});

function indexPropsOf(properties) {
  return w.indexProps(properties);
}

test("axNodeToCapture reports ARIA mixed as its own state, never as a boolean", () => {
  // "mixed" is a partially-checked control. Collapsing it to true or false states something
  // about the control that is not true.
  const mixed = w.axNodeToCapture(axNode("checkbox", "Select all", [["checked", "mixed"]]),
                                  0, new Map());
  assert.equal(mixed.mixed, true);
  assert.equal("checked" in mixed, false, "mixed must not also claim a checked boolean");

  const checked = w.axNodeToCapture(axNode("checkbox", "A", [["checked", true]]), 0, new Map());
  assert.equal(checked.checked, true);
  assert.equal("mixed" in checked, false);

  const unchecked = w.axNodeToCapture(axNode("checkbox", "B", [["checked", false]]), 0, new Map());
  assert.equal(unchecked.checked, false);

  // A control with no checked property must claim neither.
  const plain = w.axNodeToCapture(axNode("button", "Go", []), 0, new Map());
  assert.equal("checked" in plain, false);
  assert.equal("mixed" in plain, false);
});

test("axNodeToCapture flags a truncated value instead of silently shortening it", () => {
  const long = "x".repeat(w.AX_VALUE_MAX_CHARS + 25);
  const rec = w.axNodeToCapture(
    axNode("textbox", "Notes", [], { value: { value: long } }), 0, new Map());
  assert.equal(rec.value.length, w.AX_VALUE_MAX_CHARS);
  assert.equal(rec.value_truncated, true);

  const short = w.axNodeToCapture(
    axNode("textbox", "Notes", [], { value: { value: "abc" } }), 0, new Map());
  assert.equal(short.value, "abc");
  assert.equal("value_truncated" in short, false);
});

test("axNodeToCapture omits a state the page never reported", () => {
  // Absent and false are different facts. A fabricated false would tell the model the page
  // said something it did not.
  const rec = w.axNodeToCapture(axNode("button", "Go", [["focusable", true]]), 3, new Map());
  for (const never of ["disabled", "readonly", "required", "busy", "selected", "pressed",
                       "invalid", "expanded", "editable"]) {
    assert.equal(never in rec, false, `${never} must be absent when unreported`);
  }
  assert.equal(rec.depth, 3);
  assert.equal(rec.interactable, true);
  assert.equal(rec.visible, true);
});

test("axNodeToCapture applies the state table and drops ignored nodes", () => {
  const rec = w.axNodeToCapture(axNode("textbox", "Email", [
    ["disabled", true], ["required", true], ["invalid", "spelling"],
    ["selected", "true"], ["expanded", false], ["busy", false],
  ]), 0, new Map());
  assert.equal(rec.disabled, true);
  assert.equal(rec.required, true);
  assert.equal(rec.invalid, true, "invalid carries a reason string, not just true");
  assert.equal(rec.selected, true);
  assert.equal(rec.expanded, false, "a collapsed control is not the same as no state");
  assert.equal("busy" in rec, false, "an explicit false must not set the flag");
  assert.equal(rec.editable, true, "textbox is editable by role");

  assert.equal(w.axNodeToCapture({ ignored: true }, 0, new Map()), null);
});

test("axNodeToCapture attaches geometry only for a real backend node id", () => {
  const bounds = { x: 1, y: 2, w: 3, h: 4 };
  const withId = w.axNodeToCapture(
    axNode("button", "Go", [], { backendDOMNodeId: 7 }), 0, new Map([[7, bounds]]));
  assert.equal(withId.backendNodeId, 7);
  assert.deepEqual(crossRealm(withId.bounds), bounds);

  // Known id, no bounds recorded: the id is still reported, bounds are simply absent.
  const noBounds = w.axNodeToCapture(
    axNode("button", "Go", [], { backendDOMNodeId: 9 }), 0, new Map());
  assert.equal(noBounds.backendNodeId, 9);
  assert.equal("bounds" in noBounds, false);

  const noId = w.axNodeToCapture(axNode("button", "Go", []), 0, new Map());
  assert.equal("backendNodeId" in noId, false);
});

test("the command table cannot be reached through the prototype chain", async () => {
  // cmd arrives from the native-messaging relay, so the lookup must only ever find keys
  // that were deliberately put in the table. Written as an object literal instead of a Map,
  // every one of these names would resolve to an inherited value, pass the "known command?"
  // test, and then be invoked.
  for (const inherited of [
    "constructor", "toString", "valueOf", "hasOwnProperty", "__proto__",
    "isPrototypeOf", "propertyIsEnumerable", "toLocaleString",
  ]) {
    assert.equal(w.COMMAND_TABLE.get(inherited), undefined,
                 `${inherited} must not resolve to a command`);
    await assert.rejects(() => w.dispatchCommand(inherited, {}), /Unknown command/,
                         `${inherited} must be refused`);
  }
  await assert.rejects(() => w.dispatchCommand("nosuchcommand", {}), /Unknown command/);
});

test("every command table entry declares a callable handler", () => {
  assert.ok(w.COMMAND_TABLE.size >= 40, "the table lost entries");
  for (const [name, entry] of w.COMMAND_TABLE) {
    assert.equal(typeof entry.fn, "function", `${name} has no handler`);
    assert.equal(typeof entry.tab, "boolean", `${name} does not declare tab`);
    assert.equal(typeof entry.args, "boolean", `${name} does not declare args`);
  }
});

test("small pure helpers behave at their edges", () => {
  assert.equal(w.stripFragment("https://x.example/a#b#c"), "https://x.example/a");
  assert.equal(w.stripFragment("https://x.example/a"), "https://x.example/a");

  assert.equal(w.isEditableRole("textbox"), true);
  assert.equal(w.isEditableRole("searchbox"), true);
  assert.equal(w.isEditableRole("button"), false);

  assert.equal(w.axValue({ value: "v" }), "v");
  assert.equal(w.axValue({ value: 0 }), "0");
  assert.equal(w.axValue({ value: null }), "");
  assert.equal(w.axValue(undefined), "");

  const err = w.transientReadError("try again");
  assert.equal(err.transient, true);
  assert.equal(err.message, "try again");
});
