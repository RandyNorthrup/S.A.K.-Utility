// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

/// @file win32_mcp_tools.h
/// @brief Native, in-tree implementation of the win32 MCP tool surface.
///
/// This replaces the external, frozen win32-inspector server (a ~40 MB PyInstaller
/// bundle of Python + Tesseract). By owning the tools natively in C++ we ship no
/// additional third-party runtime: every tool here is backed only by Windows system
/// APIs (Win32/GDI/UI Automation) and Qt, which S.A.K. Utility already bundles.
namespace sak::win32mcp {

/// One tool invocation result. `text` is the (usually JSON) payload surfaced to the
/// model as MCP text content; `is_error` marks a failed call so the transport sets
/// the MCP `isError` flag rather than pretending success. `image_base64` (with
/// `image_mime`) is an optional binary image -- a browser screenshot -- returned as an
/// MCP `image` content block instead of a base64 blob buried in text; empty means the
/// result is text only.
struct ToolResult {
    QString text;
    bool is_error{false};
    QString image_base64;
    QString image_mime;
};

/// The static tool catalog returned by `tools/list`: one object per tool with
/// `name`, `description`, and a JSON-Schema `inputSchema`. Shape mirrors the tools
/// the external server advertised so the model sees a stable interface.
[[nodiscard]] QJsonArray toolCatalog();

/// Dispatch a `tools/call` by name. An unknown name yields an error ToolResult
/// (never a crash) so a mistaken model call degrades cleanly.
[[nodiscard]] ToolResult invokeTool(const QString& name, const QJsonObject& arguments);

/// Server identity for `initialize.serverInfo`.
[[nodiscard]] QString serverName();
[[nodiscard]] QString serverVersion();

}  // namespace sak::win32mcp
