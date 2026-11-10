// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::win32mcp {

// Physical input injection (SendInput): mouse_click, click_text, type_text, send_keys. These
// actually move the pointer / press keys on the live desktop, so EVERY tool here is an input
// tool the provider gateway hard-gates (isWin32InputTool) -- confirmation is required in every
// mode, including Unattended. Kept in its own translation unit, separate from the read-only
// observation modules. Prefer uia_click_control (no cursor movement) when the target has a ref.
//
// The dispatcher in win32_mcp_tools consults inputHandles() and routes matching calls to
// invokeInputTool(); inputToolCatalog() is merged into the advertised tool list.

[[nodiscard]] QJsonArray inputToolCatalog();
[[nodiscard]] bool inputHandles(const QString& name);
[[nodiscard]] ToolResult invokeInputTool(const QString& name, const QJsonObject& args);

}  // namespace sak::win32mcp
