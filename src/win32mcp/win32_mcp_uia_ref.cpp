// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_uia_ref.h"

namespace sak::win32mcp {

bool uiaRefDrifted(const QVector<UiaRefNode>& snapshot, const QVector<UiaRefNode>& live, int ref) {
    if (ref < 0 || ref >= live.size() || ref >= snapshot.size()) {
        return true;  // out of range against either walk -> fail closed
    }
    const UiaRefNode& a = snapshot.at(ref);
    const UiaRefNode& b = live.at(ref);
    return a.role != b.role || a.name != b.name || a.left != b.left || a.top != b.top;
}

}  // namespace sak::win32mcp
