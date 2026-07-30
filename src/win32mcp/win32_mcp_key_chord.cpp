// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_key_chord.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace sak::win32mcp {

namespace {

WORD modifierVk(const QString& name) {
    const QString low = name.toLower();
    if (low == QLatin1String("ctrl") || low == QLatin1String("control")) {
        return VK_CONTROL;
    }
    if (low == QLatin1String("alt")) {
        return VK_MENU;
    }
    if (low == QLatin1String("shift")) {
        return VK_SHIFT;
    }
    if (low == QLatin1String("win") || low == QLatin1String("meta")) {
        return VK_LWIN;
    }
    return 0;
}

WORD namedKeyVk(const QString& low) {
    static const struct {
        const char* name;
        WORD vk;
    } kKeys[] = {
        {"enter", VK_RETURN},  {"return", VK_RETURN}, {"tab", VK_TAB},        {"escape", VK_ESCAPE},
        {"esc", VK_ESCAPE},    {"space", VK_SPACE},   {"backspace", VK_BACK}, {"delete", VK_DELETE},
        {"del", VK_DELETE},    {"up", VK_UP},         {"down", VK_DOWN},      {"left", VK_LEFT},
        {"right", VK_RIGHT},   {"home", VK_HOME},     {"end", VK_END},        {"pageup", VK_PRIOR},
        {"pagedown", VK_NEXT}, {"insert", VK_INSERT}, {"f1", VK_F1},          {"f2", VK_F2},
        {"f3", VK_F3},         {"f4", VK_F4},         {"f5", VK_F5},          {"f6", VK_F6},
        {"f7", VK_F7},         {"f8", VK_F8},         {"f9", VK_F9},          {"f10", VK_F10},
        {"f11", VK_F11},       {"f12", VK_F12},
    };
    for (const auto& key : kKeys) {
        if (low == QLatin1String(key.name)) {
            return key.vk;
        }
    }
    return 0;
}

WORD keyNameVk(const QString& name) {
    if (name.size() == 1) {
        const QChar upper = name.at(0).toUpper();
        const char16_t code = upper.unicode();
        if ((code >= u'A' && code <= u'Z') || (code >= u'0' && code <= u'9')) {
            return static_cast<WORD>(code);
        }
    }
    return namedKeyVk(name.toLower());
}

}  // namespace

bool parseKeyChord(const QString& chord,
                   QVector<unsigned short>& modifiers,
                   unsigned short& main_key) {
    const QStringList parts = chord.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return false;
    }
    for (int i = 0; i < parts.size() - 1; ++i) {
        const WORD mod = modifierVk(parts[i].trimmed());
        if (mod == 0) {
            return false;
        }
        modifiers.append(mod);
    }
    main_key = keyNameVk(parts.last().trimmed());
    return main_key != 0;
}

}  // namespace sak::win32mcp
