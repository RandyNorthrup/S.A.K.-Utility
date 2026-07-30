// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::win32mcp {

// Desktop-control tool surface (screen capture, UIA read/click, OCR, waits, input, and
// process/window control). Split out of win32_mcp_tools so each module stays focused and the
// native window/clipboard tools are not entangled with the heavier GDI/COM/WinRT code.
//
// The dispatcher in win32_mcp_tools consults desktopHandles() before its own table and routes
// matching calls to invokeDesktopTool(); desktopToolCatalog() is merged into the advertised
// tool list. Read-only observation tools are ungated; input and process/window tools are
// gated at the provider gateway (isWin32InputTool / isWin32HighRiskTool) before a call ever
// reaches here, so this module executes an already-authorized request.

/// The advertised schema entries for every desktop-control tool.
[[nodiscard]] QJsonArray desktopToolCatalog();

/// True iff `name` is a desktop-control tool this module implements.
[[nodiscard]] bool desktopHandles(const QString& name);

/// Run a desktop-control tool. Precondition: desktopHandles(name) is true.
[[nodiscard]] ToolResult invokeDesktopTool(const QString& name, const QJsonObject& args);

}  // namespace sak::win32mcp
