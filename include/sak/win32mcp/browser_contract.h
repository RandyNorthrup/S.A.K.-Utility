// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/// @file browser_contract.h
/// @brief Pure, OS-agnostic contract for the assistant's browser-control surface.
///
/// This is the design core of S.A.K.'s browser control, modeled on how OpenAI
/// Codex drives Chrome: the model works off a filtered DOM (a tree of interactable
/// elements with stable ids) and acts on elements by id, not screen pixels. A
/// Chrome MV3 extension attaches the DevTools Protocol to the user's active tab,
/// walks the live DOM, and streams a normalized node list here; this module turns
/// that into the compact outline the model reads and translates the model's tool
/// calls into commands the extension executes over CDP.
///
/// Everything here is deliberately transport-free and free of any Windows / CDP /
/// Chrome dependency so the contract can be locked and unit-tested with JSON
/// fixtures before the extension and native-messaging bridge exist. The extension
/// side and the live win32 MCP wiring build against this contract.
namespace sak::win32mcp::browser {

/// The rendered view of one DOM capture, ready for the model.
///
/// `outline` is the human/model-facing text (a Playwright-aria-style indented
/// tree). `ref_index` maps each assigned element ref (e.g. "e5") to a small object
/// carrying the CDP `backendNodeId` plus role/name, so a later click/type by ref
/// resolves back to the exact node. `element_count` is how many interactable refs
/// were assigned.
struct SnapshotView {
    QString url;
    QString title;
    QString outline;
    QJsonObject ref_index;
    int element_count{0};
};

/// One translated extension command. `ok` is false with a human-readable `error`
/// when the tool call cannot be honored (unknown tool, missing required argument,
/// or a `ref` that is not in the current snapshot's index). `command` is the JSON
/// envelope the extension consumes over the native-messaging bridge.
struct ExtensionCommand {
    QJsonObject command;
    bool ok{false};
    QString error;
};

/// Filter + render a raw DOM capture from the extension into the model-facing
/// SnapshotView. The capture shape is the contract this function defines:
///
/// ```json
/// { "url": "...", "title": "...", "nodes": [
///     { "backendNodeId": 42, "role": "button", "name": "Sign in",
///       "tag": "button", "value": "", "depth": 2, "interactable": true,
///       "visible": true, "disabled": false, "editable": false,
///       "bounds": { "x": 10, "y": 20, "width": 80, "height": 30 } }, ... ] }
/// ```
///
/// Invisible and zero-area nodes are dropped; interactable nodes get a stable
/// `eN` ref in document order. Rendering is total: a missing or malformed capture
/// yields an empty outline with element_count 0 rather than throwing.
[[nodiscard]] SnapshotView renderSnapshot(const QJsonObject& capture);

/// The browser tool catalog advertised to the model: one entry per tool with a
/// strict JSON-Schema `inputSchema` (`additionalProperties:false`). DOM-first, so
/// the primary act-on-element tools take a `ref` from the latest snapshot.
[[nodiscard]] QJsonArray browserToolCatalog();

/// Translate a model tool call into the extension command envelope. `ref_index` is
/// the index from the most recent renderSnapshot for the target tab, used to
/// resolve any `ref` argument to a `backendNodeId`. Never throws: bad input comes
/// back as `ok == false` with a reason the model can read and recover from.
[[nodiscard]] ExtensionCommand buildExtensionCommand(const QString& tool,
                                                     const QJsonObject& arguments,
                                                     const QJsonObject& ref_index);

}  // namespace sak::win32mcp::browser
