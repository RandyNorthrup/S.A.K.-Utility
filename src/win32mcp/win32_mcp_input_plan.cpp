// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_input_plan.h"

#include <QChar>
#include <QLatin1Char>
#include <QLatin1String>

#include <algorithm>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sak::win32mcp {

namespace {
// SendInput absolute mouse coordinates are normalized into the range [0, 65535] across the
// virtual screen (MOUSEEVENTF_ABSOLUTE), so the span maps to this maximum.
constexpr double kAbsCoordMax = 65535.0;
// Each typed character plans a key-down plus a key-up stroke.
constexpr int kKeyStrokesPerChar = 2;
}  // namespace

bool pointInVirtualScreen(int px, int py, const ScreenBox& screen) {
    if (screen.w <= 0 || screen.h <= 0) {
        return false;
    }
    return px >= screen.x && px < screen.x + screen.w && py >= screen.y && py < screen.y + screen.h;
}

AbsPoint toAbsCoord(int px, int py, const ScreenBox& screen) {
    AbsPoint p;
    p.nx =
        static_cast<long>(std::llround((px - screen.x) * kAbsCoordMax / std::max(1, screen.w - 1)));
    p.ny =
        static_cast<long>(std::llround((py - screen.y) * kAbsCoordMax / std::max(1, screen.h - 1)));
    return p;
}

bool mouseButtonFlags(const QString& button, unsigned long& down, unsigned long& up) {
    const QString value = button.trimmed().toLower();
    if (value.isEmpty() || value == QLatin1String("left")) {
        down = MOUSEEVENTF_LEFTDOWN;
        up = MOUSEEVENTF_LEFTUP;
        return true;
    }
    if (value == QLatin1String("right")) {
        down = MOUSEEVENTF_RIGHTDOWN;
        up = MOUSEEVENTF_RIGHTUP;
        return true;
    }
    if (value == QLatin1String("middle")) {
        down = MOUSEEVENTF_MIDDLEDOWN;
        up = MOUSEEVENTF_MIDDLEUP;
        return true;
    }
    return false;
}

QVector<KeyStroke> planTypeText(const QString& text) {
    QVector<KeyStroke> strokes;
    strokes.reserve(text.size() * kKeyStrokesPerChar);
    const int count = static_cast<int>(text.size());
    for (int i = 0; i < count; ++i) {
        const QChar ch = text.at(i);
        if (ch == QLatin1Char('\r')) {
            // Drop only the CR that begins a CRLF -- the following LF emits the newline. A lone CR
            // is itself a line break, so emit a VK_RETURN pair rather than silently swallowing it.
            if (i + 1 < count && text.at(i + 1) == QLatin1Char('\n')) {
                continue;
            }
            strokes.append(KeyStroke{
                .code = static_cast<unsigned short>(VK_RETURN), .is_vk = true, .key_up = false});
            strokes.append(KeyStroke{
                .code = static_cast<unsigned short>(VK_RETURN), .is_vk = true, .key_up = true});
            continue;
        }
        if (ch == QLatin1Char('\n')) {
            strokes.append(KeyStroke{
                .code = static_cast<unsigned short>(VK_RETURN), .is_vk = true, .key_up = false});
            strokes.append(KeyStroke{
                .code = static_cast<unsigned short>(VK_RETURN), .is_vk = true, .key_up = true});
            continue;
        }
        const unsigned short unit = ch.unicode();
        strokes.append(KeyStroke{.code = unit, .is_vk = false, .key_up = false});
        strokes.append(KeyStroke{.code = unit, .is_vk = false, .key_up = true});
    }
    return strokes;
}

}  // namespace sak::win32mcp
