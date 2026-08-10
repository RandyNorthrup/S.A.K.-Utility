// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_INPUT_PLAN_H
#define SAK_WIN32MCP_WIN32_MCP_INPUT_PLAN_H

#include <QString>
#include <QVector>

// Pure decision logic behind the SendInput tools (mouse_click / type_text), split out of
// win32_mcp_input.cpp so the coordinate mapping, bounds check, button mapping, and keystroke plan
// can be pinned by deterministic unit tests. Portable integer widths at the boundary (long == LONG,
// unsigned long == DWORD, unsigned short == WORD/VK) keep the header free of <windows.h>; the .cpp
// maps them to the Win32 MOUSEEVENTF_*/VK_* constants.

namespace sak::win32mcp {

// A point in the 0..65535 normalized absolute space SendInput uses with
// MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK.
struct AbsPoint {
    long nx{0};
    long ny{0};
};

// The virtual-screen rect (origin + size) in physical pixels (SM_X/Y/CX/CYVIRTUALSCREEN).
struct ScreenBox {
    int x{0};
    int y{0};
    int w{0};
    int h{0};
};

// True when (px, py) lies inside [screen.x, screen.x+screen.w) x [screen.y, screen.y+screen.h). A
// width or height <= 0 (no such metric) makes every point out of range. Callers reject an
// out-of-range click rather than let the OS clamp it to a desktop-edge control.
[[nodiscard]] bool pointInVirtualScreen(int px, int py, const ScreenBox& screen);

// Map a virtual-screen point to the 0..65535 absolute space. The divisor is floored at 1 so a
// degenerate 1px virtual screen cannot divide by zero. Assumes the point is already validated.
AbsPoint toAbsCoord(int px, int py, const ScreenBox& screen);

// Map a button name to its MOUSEEVENTF down/up flags (unsigned long == DWORD). Case-insensitive and
// trimmed; an empty name defaults to "left". Returns false (leaving down/up untouched) on an
// unknown name so the tool refuses rather than firing an unintended button.
[[nodiscard]] bool mouseButtonFlags(const QString& button, unsigned long& down, unsigned long& up);

// One planned keystroke: either a UTF-16 code unit typed via KEYEVENTF_UNICODE (is_vk == false) or
// a virtual-key press (is_vk == true, e.g. VK_RETURN for a newline). key_up distinguishes the
// press from the release.
struct KeyStroke {
    unsigned short code{0};
    bool is_vk{false};
    bool key_up{false};

    bool operator==(const KeyStroke& other) const {
        return code == other.code && is_vk == other.is_vk && key_up == other.key_up;
    }
};

// Plan type_text into an ordered keystroke list: the CR of a CRLF is dropped so the pair collapses
// to one newline, while a LF ("\n") -- and a lone CR that is NOT part of a CRLF -- becomes a
// VK_RETURN down+up pair (a bare CR is itself a line break and must not be silently swallowed).
// Every other UTF-16 unit becomes a unicode down+up pair (an astral character's two surrogates each
// emit their own pair). The count of source units actually turned into keystrokes (i.e. excluding
// only the dropped CR of a CRLF) can be recovered by the caller for an honest "typed" report.
QVector<KeyStroke> planTypeText(const QString& text);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_INPUT_PLAN_H
