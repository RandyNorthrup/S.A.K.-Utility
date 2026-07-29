// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>

#include <optional>

/// @file win32_mcp_dispatch.h
/// @brief JSON-RPC 2.0 request routing for the native win32 MCP server, kept
/// separate from the stdio loop so it can be unit-tested by feeding request
/// objects and inspecting response objects with no process I/O.
namespace sak::win32mcp {

class BrowserControl;

/// Handle one parsed JSON-RPC request. Returns the response object to write back,
/// or std::nullopt for notifications (no `id`, e.g. notifications/initialized)
/// which the protocol says must not be answered.
///
/// When @p browser is non-null, its `browser_*` tools are merged into `tools/list`
/// and a matching `tools/call` is routed to it (live browser control). When null
/// (the default, and every pure unit test), only the built-in win32 tools are
/// advertised and a browser_* call falls through to the unknown-tool error.
[[nodiscard]] std::optional<QJsonObject> handleRequest(const QJsonObject& request,
                                                       BrowserControl* browser = nullptr);

}  // namespace sak::win32mcp
