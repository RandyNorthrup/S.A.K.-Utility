// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::win32mcp {

// On-screen OCR tools (Windows.Media.Ocr): read text the model cannot get from the UI
// Automation tree -- canvas-drawn UI, images, custom-rendered installers/scan results. Split
// into its own translation unit so the C++/WinRT projection stays isolated from the GDI/UIA
// code; capture is shared via win32_mcp_capture. All tools are read-only and ungated.
//
// The dispatcher in win32_mcp_tools consults ocrHandles() and routes matching calls to
// invokeOcrTool(); ocrToolCatalog() is merged into the advertised tool list.

[[nodiscard]] QJsonArray ocrToolCatalog();
[[nodiscard]] bool ocrHandles(const QString& name);
[[nodiscard]] ToolResult invokeOcrTool(const QString& name, const QJsonObject& args);

}  // namespace sak::win32mcp
