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

bool pointInVirtualScreen(int px, int py, const ScreenBox& screen) {
    if (screen.w <= 0 || screen.h <= 0) {
        return false;
    }
    return px >= screen.x && px < screen.x + screen.w && py >= screen.y && py < screen.y + screen.h;
}

AbsPoint toAbsCoord(int px, int py, const ScreenBox& screen) {
    AbsPoint p;
    p.nx = static_cast<long>(std::llround((px - screen.x) * 65535.0 / std::max(1, screen.w - 1)));
    p.ny = static_cast<long>(std::llround((py - screen.y) * 65535.0 / std::max(1, screen.h - 1)));
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
    strokes.reserve(text.size() * 2);
    for (const QChar ch : text) {
        if (ch == QLatin1Char('\r')) {
            continue;  // CRLF collapses to a single newline
        }
        if (ch == QLatin1Char('\n')) {
            strokes.append(KeyStroke{static_cast<unsigned short>(VK_RETURN), true, false});
            strokes.append(KeyStroke{static_cast<unsigned short>(VK_RETURN), true, true});
            continue;
        }
        const unsigned short unit = ch.unicode();
        strokes.append(KeyStroke{unit, false, false});
        strokes.append(KeyStroke{unit, false, true});
    }
    return strokes;
}

}  // namespace sak::win32mcp
