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
  "AX_VALUE_MAX_CHARS", "printPageOptions", "buildBoundsMap",
  "buildNodes", "MAX_CAPTURE_NODES",
  "classifyHostFrame", "classifyCommandFrame", "BRIDGE_PROTOCOL", "viewportReading",
  "storageRequest", "storageArgError", "storageOriginError",
  "windowRequest", "createdWindowReply", "suppliedWindowId",
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

test("printPageOptions maps only recognized fields and pins the safe defaults", () => {
  // An empty request prints with the fixed transfer mode and backgrounds on; nothing else leaks.
  assert.deepEqual(crossRealm(w.printPageOptions({})),
                   { transferMode: "ReturnAsBase64", printBackground: true });
  // print_background is on unless EXACTLY false (a missing/true/truthy value stays on).
  assert.equal(w.printPageOptions({ print_background: false }).printBackground, false);
  assert.equal(w.printPageOptions({ print_background: true }).printBackground, true);
  assert.equal(w.printPageOptions({ print_background: "no" }).printBackground, true);
  // landscape is copied only when it is a real boolean.
  assert.equal(w.printPageOptions({ landscape: true }).landscape, true);
  assert.equal("landscape" in w.printPageOptions({ landscape: "yes" }), false);
});

test("printPageOptions enforces the scale and paper-size bounds", () => {
  assert.equal(w.printPageOptions({ scale: 1.5 }).scale, 1.5);
  assert.equal(w.printPageOptions({ scale: 0.1 }).scale, 0.1);  // inclusive lower bound
  assert.equal(w.printPageOptions({ scale: 2 }).scale, 2);      // inclusive upper bound
  assert.equal("scale" in w.printPageOptions({ scale: null }), false);
  for (const bad of [0.05, 2.5, "abc", NaN, Infinity]) {
    assert.throws(() => w.printPageOptions({ scale: bad }), /scale must be between 0.1 and 2/);
  }
  // Paper dimensions must be a finite positive number of inches.
  const opts = w.printPageOptions({ paper_width: 8.5, paper_height: 11 });
  assert.equal(opts.paperWidth, 8.5);
  assert.equal(opts.paperHeight, 11);
  assert.equal("paperWidth" in w.printPageOptions({ paper_width: null }), false);
  for (const bad of [0, -1, "x", NaN]) {
    assert.throws(() => w.printPageOptions({ paper_width: bad }),
                  /paper_width must be a positive number of inches/);
  }
});

test("buildBoundsMap rounds each node's layout box and skips unusable entries", () => {
  const snapshot = {
    documents: [
      {
        nodes: { backendNodeId: [100, 200, 300] },
        layout: {
          nodeIndex: [0, 1, 2],
          bounds: [[1.4, 2.6, 3.5, 4.5], [10, 20, 30, 40], null],
        },
      },
      null,                              // a null document is skipped
      { nodes: { backendNodeId: [9] } },  // no layout -> skipped
    ],
  };
  const map = w.buildBoundsMap(snapshot);
  // 100 and 200 carve out; 300's bounds were null, so it is dropped, not defaulted to zeros.
  assert.equal(map.size, 2);
  assert.deepEqual(crossRealm(map.get(100)), { x: 1, y: 3, width: 4, height: 5 });
  assert.deepEqual(crossRealm(map.get(200)), { x: 10, y: 20, width: 30, height: 40 });
  assert.equal(map.get(300), undefined);
  // A missing/empty snapshot yields an empty map, never a throw.
  assert.equal(w.buildBoundsMap(null).size, 0);
  assert.equal(w.buildBoundsMap({}).size, 0);
});

test("buildBoundsMap drops a node whose backend id is undefined", () => {
  // nodeIndex points past the backendNodeId array: the entry has no stable id, so it must be
  // dropped rather than keyed on undefined.
  const snapshot = {
    documents: [
      {
        nodes: { backendNodeId: [42] },
        layout: { nodeIndex: [0, 7], bounds: [[0, 0, 1, 1], [5, 5, 5, 5]] },
      },
    ],
  };
  const map = w.buildBoundsMap(snapshot);
  assert.equal(map.size, 1);
  assert.deepEqual(crossRealm(map.get(42)), { x: 0, y: 0, width: 1, height: 1 });
});

// A named node always survives axNodeToCapture (only unnamed structural filler is dropped), so a
// button with a name and an id makes every fixture node emit and isolates buildNodes' walk order.
function walkNode(id, name, childIds) {
  return axNode("button", name, [], { nodeId: id, childIds });
}

test("buildNodes walks the AX tree in pre-order with correct depth", () => {
  const axTree = {
    nodes: [
      walkNode("root", "R", ["a", "b"]),
      walkNode("a", "A", ["a1"]),
      walkNode("b", "B", []),
      walkNode("a1", "A1", []),
    ],
  };
  const { nodes, truncated } = w.buildNodes(axTree, new Map());
  // Depth-first, document order: the whole of a's subtree comes before sibling b.
  assert.deepEqual(crossRealm(nodes.map((n) => [n.name, n.depth])),
                   [["R", 0], ["A", 1], ["A1", 2], ["B", 1]]);
  assert.equal(truncated, false);
});

test("buildNodes emits a shared descendant once and never loops", () => {
  // "shared" is a child of both x and y (a DAG); the seen-set must emit it once and the walk must
  // terminate. An empty tree and a missing tree yield an empty, non-truncated result.
  const axTree = {
    nodes: [
      walkNode("root", "R", ["x", "y"]),
      walkNode("x", "X", ["shared"]),
      walkNode("y", "Y", ["shared"]),
      walkNode("shared", "S", []),
    ],
  };
  const { nodes } = w.buildNodes(axTree, new Map());
  assert.equal(nodes.filter((n) => n.name === "S").length, 1);
  assert.deepEqual(crossRealm(nodes.map((n) => n.name)), ["R", "X", "S", "Y"]);
  assert.deepEqual(crossRealm(w.buildNodes({ nodes: [] }, new Map())),
                   { nodes: [], truncated: false });
  assert.deepEqual(crossRealm(w.buildNodes(null, new Map())), { nodes: [], truncated: false });
});

test("buildNodes caps the emitted set at MAX_CAPTURE_NODES and reports truncation", () => {
  // A wide tree past the cap: the output stops at MAX_CAPTURE_NODES and truncated flags the drop,
  // so the caller never presents a partial outline as the whole page.
  const childIds = [];
  const nodesArr = [walkNode("root", "R", childIds)];
  for (let i = 0; i < w.MAX_CAPTURE_NODES + 200; i++) {
    const id = "c" + i;
    childIds.push(id);
    nodesArr.push(walkNode(id, "N" + i, []));
  }
  const { nodes, truncated } = w.buildNodes({ nodes: nodesArr }, new Map());
  assert.equal(nodes.length, w.MAX_CAPTURE_NODES);
  assert.equal(truncated, true);
});

// --------------------------------------------------------------------------------------
// Host-frame classification (R5-G21-9)
//
// Every refusal in the bridge protocol is decided by classifyHostFrame / classifyCommandFrame:
// an unannounced host must not get a privileged command executed on its say-so, a protocol skew
// has to STOP something to be a check at all, and the relay's one-op pump only stays in step if
// exactly one reply leaves per command frame. onHostMessage is a mechanical 1:1 apply of these
// decisions, so pinning them here is what makes those guards testable without a live port.
// --------------------------------------------------------------------------------------

const READY_STATE = { bridgeReady: true, lastError: null };

test("classifyHostFrame ignores a frame that is not an object", () => {
  for (const junk of [null, undefined, 0, "", "command", 42, true]) {
    assert.equal(w.classifyHostFrame(junk, READY_STATE).action, "ignore");
  }
});

test("classifyHostFrame accepts bridge_ready ONLY on an exact protocol match", () => {
  const ok = w.classifyHostFrame({ type: "bridge_ready", protocol: w.BRIDGE_PROTOCOL },
                                 { bridgeReady: false, lastError: null });
  assert.equal(ok.action, "bridge_ready");
  assert.equal(ok.accepted, true);
  assert.equal(ok.error, null);

  // A skew must be REPORTED, not tolerated: the frame shape is what every privileged command
  // is parsed against. String "1" is deliberately included -- the compare is ===, so a host
  // that sends the number as text does not get to speak a protocol it may not implement.
  for (const skew of [0, 2, w.BRIDGE_PROTOCOL + 1, String(w.BRIDGE_PROTOCOL), null, undefined]) {
    const bad = w.classifyHostFrame({ type: "bridge_ready", protocol: skew },
                                    { bridgeReady: false, lastError: null });
    assert.equal(bad.action, "bridge_ready");
    assert.equal(bad.accepted, false, `protocol ${String(skew)} must not be accepted`);
    assert.equal(bad.error,
                 "Bridge protocol mismatch: host " + skew + ", extension " + w.BRIDGE_PROTOCOL);
  }
});

test("classifyHostFrame reports bridge_unavailable with a reason, never an empty one", () => {
  assert.deepEqual(
    crossRealm(w.classifyHostFrame({ type: "bridge_unavailable", error: "pipe closed" },
                                   READY_STATE)),
    { action: "bridge_unavailable", error: "pipe closed" });
  // A host that says nothing still produces a stated reason: health.error is what the operator
  // and the refusal path below both read, and an empty one explains nothing.
  for (const blank of [undefined, null, ""]) {
    assert.equal(
      w.classifyHostFrame({ type: "bridge_unavailable", error: blank }, READY_STATE).error,
      "bridge unavailable");
  }
});

test("classifyHostFrame treats cancel as a non-command frame that is never replied to", () => {
  const d = w.classifyHostFrame({ type: "cancel", id: "c1", cmd: "browser_wait_for" },
                                READY_STATE);
  // Deliberately carries no id/cmd: a cancel retires the generation, and answering it would put
  // a second reply into a relay that expects exactly one per COMMAND frame.
  assert.deepEqual(crossRealm(d), { action: "cancel" });
});

test("classifyHostFrame refuses an unknown frame type instead of guessing", () => {
  const d = w.classifyHostFrame({ type: "nonsense" }, READY_STATE);
  assert.equal(d.action, "unexpected");
  assert.equal(d.type, "nonsense");
  // An object with no type at all is unexpected, NOT a command.
  assert.equal(w.classifyHostFrame({}, READY_STATE).action, "unexpected");
});

test("a command frame with no usable id is DROPPED, not answered", () => {
  // There is nothing to correlate a reply to, so this is the one frame that gets none.
  for (const id of [undefined, null, "", 7, {}]) {
    const d = w.classifyHostFrame({ type: "command", id, cmd: "browser_click" }, READY_STATE);
    assert.equal(d.action, "drop", `id ${JSON.stringify(id)} must drop`);
    assert.equal(d.reason, "no-id");
  }
});

test("a command frame with no command name is refused BY NAME, addressed to its id", () => {
  for (const cmd of [undefined, null, "", 7]) {
    const d = w.classifyHostFrame({ type: "command", id: "c1", cmd }, READY_STATE);
    assert.equal(d.action, "error");
    assert.equal(d.id, "c1");
    assert.equal(d.cmd, "");  // no cmd to echo: the reply must not invent one
    assert.equal(d.error, "The command frame carries no command name.");
  }
});

test("a command from an unannounced host is refused, and says why", () => {
  // The privileged surface (input injection, cookies, web storage, permissions) runs only
  // against a bridge that handshook. Refusing still emits exactly one reply, so the relay's
  // one-op pump stays in step.
  const d = w.classifyHostFrame({ type: "command", id: "c9", cmd: "browser_set_cookie" },
                                { bridgeReady: false, lastError: null });
  assert.equal(d.action, "error");
  assert.equal(d.id, "c9");
  assert.equal(d.cmd, "browser_set_cookie");  // the refused command IS echoed here
  assert.equal(d.error, "The bridge has not completed its readiness handshake.");

  // When a specific failure is already known (a protocol skew, a dead pipe), THAT is reported
  // rather than the generic handshake line -- otherwise a skew is indistinguishable from a
  // host that simply had not finished connecting.
  const skewed = w.classifyHostFrame(
    { type: "command", id: "c9", cmd: "browser_set_cookie" },
    { bridgeReady: false, lastError: "Bridge protocol mismatch: host 2, extension 1" });
  assert.equal(skewed.error, "Bridge protocol mismatch: host 2, extension 1");
});

test("a well-formed command from a ready bridge dispatches", () => {
  const d = w.classifyHostFrame({ type: "command", id: "c2", cmd: "browser_navigate" },
                                READY_STATE);
  assert.deepEqual(crossRealm(d), { action: "dispatch", id: "c2", cmd: "browser_navigate" });
});

test("classifyCommandFrame orders its guards id, name, readiness", () => {
  // Order is observable and load-bearing: a frame that is bad in two ways must be reported by
  // the FIRST guard, because a reply addressed to a missing id cannot be delivered at all and
  // a readiness refusal that echoed an absent cmd would put an invented name on the wire.
  const noIdNoCmd = w.classifyCommandFrame({ id: "", cmd: "" },
                                           { bridgeReady: false, lastError: "x" });
  assert.equal(noIdNoCmd.action, "drop");
  const noCmdNotReady = w.classifyCommandFrame({ id: "c3", cmd: "" },
                                               { bridgeReady: false, lastError: "x" });
  assert.equal(noCmdNotReady.error, "The command frame carries no command name.");
});

// --------------------------------------------------------------------------------------
// Viewport reading (R5-G21-9)
//
// Every field scales or offsets a coordinate the model measured off a screenshot, so an
// untrustworthy self-report has to produce ok:false rather than a plausible default: a dpr of 0
// or a negative width would silently place a click somewhere other than where the model aimed.
// --------------------------------------------------------------------------------------

const UNUSABLE = { ok: false, dpr: 1, scrollX: 0, scrollY: 0, href: "", width: 0, height: 0 };

test("viewportReading accepts a well-formed page self-report and rounds the pixel fields", () => {
  assert.deepEqual(
    crossRealm(w.viewportReading({ dpr: 1.5, sx: 10.4, sy: 20.6, href: "https://e/", iw: 800.7,
                                   ih: 600.2 })),
    { ok: true, dpr: 1.5, scrollX: 10, scrollY: 21, href: "https://e/", width: 801, height: 600 });
  // dpr is NOT rounded: it is a scale factor, and 1.5 rounded to 2 mis-places every coordinate.
  assert.equal(w.viewportReading({ dpr: 1.25, sx: 0, sy: 0, href: "x", iw: 10, ih: 10 }).dpr, 1.25);
});

test("viewportReading refuses a report it cannot trust, rather than defaulting", () => {
  const base = { dpr: 1, sx: 0, sy: 0, href: "https://e/", iw: 800, ih: 600 };
  for (const [label, bad] of [
    ["null", null],
    ["undefined", undefined],
    ["no href", { ...base, href: undefined }],
    ["non-string href", { ...base, href: 42 }],
    ["zero dpr", { ...base, dpr: 0 }],
    ["negative dpr", { ...base, dpr: -1 }],
    ["NaN dpr", { ...base, dpr: NaN }],
    ["Infinite dpr", { ...base, dpr: Infinity }],
    ["zero width", { ...base, iw: 0 }],
    ["zero height", { ...base, ih: 0 }],
    ["negative width", { ...base, iw: -800 }],
    ["negative height", { ...base, ih: -600 }],
    ["NaN width", { ...base, iw: NaN }],
    ["Infinite height", { ...base, ih: Infinity }],
  ]) {
    // The refusal shape is pinned in full: a caller that reads width/height without checking ok
    // must get 0, never a stale or invented extent.
    assert.deepEqual(crossRealm(w.viewportReading(bad)), UNUSABLE, `must refuse: ${label}`);
  }
});

test("viewportReading treats an unreadable SCROLL offset as 0, not as a reason to refuse", () => {
  // Scroll offsets are additive, so an absent one is the document's own origin. Discarding an
  // otherwise-good reading over it would refuse every page that reports no scroll at all.
  const r = w.viewportReading({ dpr: 2, sx: NaN, sy: undefined, href: "https://e/", iw: 400,
                                ih: 300 });
  assert.equal(r.ok, true);
  assert.equal(r.scrollX, 0);
  assert.equal(r.scrollY, 0);
  // A NEGATIVE scroll (rubber-band overscroll on some platforms) is a real offset and is kept.
  assert.equal(w.viewportReading({ dpr: 1, sx: -5.4, sy: 0, href: "x", iw: 1, ih: 1 }).scrollX, -5);
});

// --------------------------------------------------------------------------------------
// browser_storage request contract (R5-G21-9)
//
// This tool reads and clears web storage, which is where sites keep session tokens. Its whole
// refusal set lives in storageRequest / storageOriginError, and every one of those refusals is
// the difference between acting on the site the caller named and acting on whatever happens to
// be in the active tab.
// --------------------------------------------------------------------------------------

test("storageRequest refuses an unknown action instead of picking one", () => {
  for (const action of [undefined, null, "", "delete", "getAll", "set ", 7, {}]) {
    assert.equal(w.storageRequest({ action }).error,
                 "browser_storage action must be get, set, remove, clear, or keys.",
                 `must refuse action ${JSON.stringify(action)}`);
  }
  // A missing args object is refused the same way, not treated as a default action.
  assert.ok(w.storageRequest(undefined).error);
  assert.ok(w.storageRequest(null).error);
});

test("storageRequest accepts the five actions case-insensitively", () => {
  for (const action of ["get", "GET", "Set", "remove", "CLEAR", "keys"]) {
    const req = w.storageRequest({ action, key: "k", value: "v" });
    assert.equal(req.error, undefined, `${action} must be accepted`);
    assert.equal(req.action, action.toLowerCase());
  }
});

test("storageRequest defaults the area to local but refuses an unknown one", () => {
  assert.equal(w.storageRequest({ action: "clear" }).area, "local");
  assert.equal(w.storageRequest({ action: "clear", area: "SESSION" }).area, "session");
  // "sessionStorage", "cookie" and friends are NOT quietly mapped onto an area that exists.
  for (const area of ["sessionStorage", "cookie", "sync", "x"]) {
    assert.equal(w.storageRequest({ action: "clear", area }).error,
                 "browser_storage area must be local or session.");
  }
});

test("storageRequest requires a key for the keyed actions only", () => {
  for (const action of ["get", "set", "remove"]) {
    const args = action === "set" ? { action, value: "v" } : { action };
    assert.equal(w.storageRequest(args).error, "browser_storage " + action + " needs a key.");
    // An empty string is not a key, and a non-string is refused rather than coerced --
    // "undefined" and "null" are perfectly valid storage keys, so a coerced one would read or
    // overwrite a real entry under a name the caller never asked for.
    assert.equal(w.storageRequest({ ...args, key: "" }).error,
                 "browser_storage " + action + " needs a key.");
    assert.equal(w.storageRequest({ ...args, key: 42 }).error,
                 "browser_storage " + action + " needs a key.");
    assert.equal(w.storageRequest({ ...args, key: null }).error,
                 "browser_storage " + action + " needs a key.");
  }
  // clear and keys act on the whole area, so they need no key and must not demand one.
  assert.equal(w.storageRequest({ action: "clear" }).error, undefined);
  assert.equal(w.storageRequest({ action: "keys" }).error, undefined);
});

test("storageRequest requires a STRING value for set, and carries it only for set", () => {
  assert.equal(w.storageRequest({ action: "set", key: "k" }).error,
               "browser_storage set needs a string value.");
  for (const value of [42, null, true, {}, undefined]) {
    assert.equal(w.storageRequest({ action: "set", key: "k", value }).error,
                 "browser_storage set needs a string value.",
                 `must refuse value ${JSON.stringify(value)}`);
  }
  // An empty string IS a legal value: clearing an entry's contents is not the same as removing
  // it, and refusing it would make that distinction unreachable.
  assert.equal(w.storageRequest({ action: "set", key: "k", value: "" }).value, "");
  // A value supplied to a non-set action is dropped, so it can never reach a write path.
  assert.equal(w.storageRequest({ action: "remove", key: "k", value: "sneaky" }).value, "");
  assert.equal(w.storageRequest({ action: "get", key: "k", value: "sneaky" }).value, "");
});

test("storageRequest normalizes the key for the keyless actions", () => {
  // clear/keys are driven through the same CDP call shape, so the key must be a definite
  // empty string rather than undefined leaking into the protocol frame.
  assert.equal(w.storageRequest({ action: "clear" }).key, "");
  assert.equal(w.storageRequest({ action: "keys" }).key, "");
});

test("storageArgError guards are ordered key-then-value", () => {
  // A set with neither must report the KEY problem: the value check cannot be acted on until
  // there is an entry to act on, and reporting the second problem first sends the caller to
  // fix the wrong field.
  assert.equal(w.storageArgError("set", {}), "browser_storage set needs a key.");
});

test("storageOriginError refuses a site the caller did not name, and only then", () => {
  // No claim is not a mismatch.
  assert.equal(w.storageOriginError("https://bank.example/x", undefined), "");
  assert.equal(w.storageOriginError("https://bank.example/x", ""), "");
  assert.equal(w.storageOriginError("https://bank.example/x", null), "");
  assert.equal(w.storageOriginError("https://bank.example/x", 42), "");
  // A matching claim passes.
  assert.equal(w.storageOriginError("https://bank.example/x", "https://bank.example"), "");
  // A mismatch is refused and NAMES both sides, so the operator can see which site was live.
  assert.equal(
    w.storageOriginError("https://evil.example/x", "https://bank.example"),
    "The active tab is https://evil.example/x, not https://bank.example; " +
    "browser_storage will not touch another site's storage.");
  // An unreadable tab url is still a mismatch against a stated expectation -- it cannot be
  // proven to be the named site, so it is refused rather than waved through.
  assert.equal(
    w.storageOriginError("", "https://bank.example"),
    "The active tab is an unknown page, not https://bank.example; " +
    "browser_storage will not touch another site's storage.");
});

// --------------------------------------------------------------------------------------
// browser_window request contract (R5-G21-9)
// --------------------------------------------------------------------------------------

test("windowRequest refuses an unknown action", () => {
  for (const action of [undefined, null, "", "open", "minimize", 7]) {
    assert.equal(w.windowRequest({ action }).error,
                 "browser_window action must be new, focus, or close.");
  }
  assert.ok(w.windowRequest(undefined).error);
});

test("windowRequest normalizes a new-window url and refuses a non-http(s) one", () => {
  assert.deepEqual(crossRealm(w.windowRequest({ action: "new" })), { action: "new", url: null });
  assert.equal(w.windowRequest({ action: "NEW", url: "example.com" }).url, "https://example.com");
  // The same refusals normalizeUrl enforces everywhere else: a new window is still a navigation.
  for (const hostile of ["javascript:alert(1)", "file:///C:/Windows", "chrome://settings",
                         "data:text/html,x", "not a url"]) {
    assert.equal(w.windowRequest({ action: "new", url: hostile }).error,
                 "browser_window new url must be http(s).",
                 `must refuse ${hostile}`);
  }
});

test("windowRequest requires an INTEGER window id for focus and close", () => {
  const msg = "browser_window needs window_id for focus/close (see browser_windows).";
  for (const action of ["focus", "close"]) {
    assert.equal(w.windowRequest({ action, window_id: 12 }).windowId, 12);
    // Number("12") is 12, so a numeric string is a usable id and is accepted.
    assert.equal(w.windowRequest({ action, window_id: "12" }).windowId, 12);
    // Everything that Number() turns into a non-integer must be refused rather than addressed:
    // "" becomes 0 (a real window id on some platforms), "3abc" becomes NaN, and a fractional
    // or infinite id names no window at all.
    for (const bad of [undefined, null, "", "  ", "3abc", 1.5, NaN, Infinity, {}, []]) {
      assert.equal(w.windowRequest({ action, window_id: bad }).error, msg,
                   `${action} must refuse window_id ${JSON.stringify(bad)}`);
    }
  }
});

test("createdWindowReply reports only what the browser actually said", () => {
  assert.deepEqual(
    crossRealm(w.createdWindowReply({ id: 7, type: "normal", tabs: [{ index: 0 }] })),
    { ok: true, window_id: 7, tab_index: 0, type: "normal" });
  // No type reported -> no type echoed, rather than a fabricated "normal".
  assert.deepEqual(crossRealm(w.createdWindowReply({ id: 7, tabs: [{ index: 3 }] })),
                   { ok: true, window_id: 7, tab_index: 3 });
  assert.equal(w.createdWindowReply({ id: 7, type: 42, tabs: [{ index: 0 }] }).type, undefined);
  // No tab -> tab_index null, never 0, which would name a tab that does not exist.
  for (const tabs of [undefined, null, []]) {
    assert.equal(w.createdWindowReply({ id: 7, tabs }).tab_index, null);
  }
});

test("suppliedWindowId refuses a value the caller never supplied (a REAL find)", () => {
  // Number(null), Number(""), Number("   ") and Number([]) are ALL 0, so before this guard a
  // focus/close carrying no window_id addressed window 0 and was refused downstream by
  // requireListedWindow -- telling the operator the window "was not in the listing" when the
  // command in fact named no window at all. Found by writing this test, not by reading.
  for (const absent of [undefined, null, "", "   ", [], {}, true, false]) {
    assert.ok(Number.isNaN(w.suppliedWindowId(absent)),
              `${JSON.stringify(absent)} must not become a window id`);
  }
  // A real id, including 0 when it is genuinely supplied as a number, still passes through.
  assert.equal(w.suppliedWindowId(0), 0);
  assert.equal(w.suppliedWindowId(12), 12);
  assert.equal(w.suppliedWindowId("12"), 12);
  assert.equal(w.suppliedWindowId(" 12 "), 12);
  // Non-integers survive this step and are refused by the Number.isInteger check above it.
  assert.equal(w.suppliedWindowId(1.5), 1.5);
  assert.ok(Number.isNaN(w.suppliedWindowId("3abc")));
});
