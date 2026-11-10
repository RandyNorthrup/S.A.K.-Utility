// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_UIA_REF_H
#define SAK_WIN32MCP_WIN32_MCP_UIA_REF_H

#include <QString>
#include <QVector>

namespace sak::win32mcp {

// The stable-identity fields of one UI Automation node, captured at inspection time and re-read
// before a ref is acted on. A ref is the depth-first index into the inspected tree; comparing these
// fields at that index detects that the tree shifted between calls so a stored ref no longer points
// at the same control.
struct UiaRefNode {
    QString role;
    QString name;
    long left{0};  // bounding-rectangle origin, screen space
    long top{0};
};

// True when ref no longer identifies the same control it did at inspection time -- i.e. the tree
// drifted, so acting on the ref would target the wrong element. Out of range against either the
// snapshot or the live walk counts as drift (fail closed). Identity is role + accessible name +
// bounding-rectangle origin, so two same-role controls with empty/identical names that swap
// position are still caught (name alone would not distinguish them).
bool uiaRefDrifted(const QVector<UiaRefNode>& snapshot, const QVector<UiaRefNode>& live, int ref);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_UIA_REF_H
