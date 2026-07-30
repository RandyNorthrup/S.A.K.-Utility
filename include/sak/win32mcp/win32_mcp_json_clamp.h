// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_JSON_CLAMP_H
#define SAK_WIN32MCP_WIN32_MCP_JSON_CLAMP_H

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace sak::win32mcp {

// Read args[key] as a millisecond count and clamp it to [lo, hi], falling back to def when the key
// is absent or not a number. This is the enforcement point for the wait-loop safety caps (poll
// floor, 2-hour timeout ceiling), so it must be robust to a hostile value: the clamp is applied in
// the double domain BEFORE narrowing to qint64, so a JSON number larger than INT64_MAX (a valid
// double) is pinned to hi instead of overflowing the narrowing cast to INT64_MIN and collapsing to
// the floor.
qint64 clampMs(const QJsonObject& args, const QString& key, qint64 def, qint64 lo, qint64 hi);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_JSON_CLAMP_H
