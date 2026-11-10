// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::win32mcp {

// Pixel + window observation/wait tools (get_pixel_color, get_window_snapshot,
// compare_screenshots, wait_for_window, wait_for_idle): the timing/state half of a
// read-check-act loop, built on the shared capture primitive. Own translation unit so the
// polling/fingerprint logic stays out of the capture/UIA/OCR code. All tools are read-only and
// ungated.
//
// The dispatcher in win32_mcp_tools consults watchHandles() and routes matching calls to
// invokeWatchTool(); watchToolCatalog() is merged into the advertised tool list.

[[nodiscard]] QJsonArray watchToolCatalog();
[[nodiscard]] bool watchHandles(const QString& name);
[[nodiscard]] ToolResult invokeWatchTool(const QString& name, const QJsonObject& args);

}  // namespace sak::win32mcp
