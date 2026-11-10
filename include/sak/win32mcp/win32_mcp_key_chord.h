// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_KEY_CHORD_H
#define SAK_WIN32MCP_WIN32_MCP_KEY_CHORD_H

#include <QString>
#include <QVector>

namespace sak::win32mcp {

// Parse a key chord such as "Ctrl+Shift+S" into the modifier virtual-key codes (in written order)
// and the single main key's VK. `unsigned short` is the Win32 WORD/VK width; the caller feeds the
// codes straight to SendInput.
//
// Recognized modifiers: ctrl/control, alt, shift, win/meta. Main key: a single A-Z or 0-9
// character, or a named key (enter/return, tab, esc/escape, space, backspace, delete/del, the
// arrows, home/end, pageup/pagedown, insert, f1..f12). All matching is case-insensitive.
//
// Returns false on an empty chord, an unknown modifier, or an unknown main key -- so send_keys
// refuses rather than injecting an unintended keystroke. On false the out-params are left as the
// caller initialized them (modifiers may hold the prefix parsed before the failure); callers must
// gate on the return value, not inspect the outputs.
bool parseKeyChord(const QString& chord,
                   QVector<unsigned short>& modifiers,
                   unsigned short& main_key);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_KEY_CHORD_H
