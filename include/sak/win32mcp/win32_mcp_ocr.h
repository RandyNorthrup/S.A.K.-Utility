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

// Locate the best on-screen occurrence of args["text"] and report the CENTER of its bounding box
// in virtual-screen coordinates. args selects the target -- foreground=true (the active window,
// including title-less pop-ups), window_title (a titled window), or neither (full screen) -- and
// contains=true opts into substring matches (default whole-word, so "OK" never lands on
// "cookies"). Used by the input module's click_text. Returns an error string when OCR is
// unavailable, the window is missing, or the text is not found; empty on success.
[[nodiscard]] QString ocrLocateText(const QJsonObject& args, int& center_x, int& center_y);

}  // namespace sak::win32mcp
