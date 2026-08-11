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
        {.name = "enter", .vk = VK_RETURN},   {.name = "return", .vk = VK_RETURN},
        {.name = "tab", .vk = VK_TAB},        {.name = "escape", .vk = VK_ESCAPE},
        {.name = "esc", .vk = VK_ESCAPE},     {.name = "space", .vk = VK_SPACE},
        {.name = "backspace", .vk = VK_BACK}, {.name = "delete", .vk = VK_DELETE},
        {.name = "del", .vk = VK_DELETE},     {.name = "up", .vk = VK_UP},
        {.name = "down", .vk = VK_DOWN},      {.name = "left", .vk = VK_LEFT},
        {.name = "right", .vk = VK_RIGHT},    {.name = "home", .vk = VK_HOME},
        {.name = "end", .vk = VK_END},        {.name = "pageup", .vk = VK_PRIOR},
        {.name = "pagedown", .vk = VK_NEXT},  {.name = "insert", .vk = VK_INSERT},
        {.name = "f1", .vk = VK_F1},          {.name = "f2", .vk = VK_F2},
        {.name = "f3", .vk = VK_F3},          {.name = "f4", .vk = VK_F4},
        {.name = "f5", .vk = VK_F5},          {.name = "f6", .vk = VK_F6},
        {.name = "f7", .vk = VK_F7},          {.name = "f8", .vk = VK_F8},
        {.name = "f9", .vk = VK_F9},          {.name = "f10", .vk = VK_F10},
        {.name = "f11", .vk = VK_F11},        {.name = "f12", .vk = VK_F12},
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
    // KeepEmptyParts so a leading/trailing/doubled '+' (e.g. "Ctrl++S", "+S", "Ctrl+") surfaces as
    // an empty segment and is rejected, rather than being silently dropped into a different chord.
    const QStringList parts = chord.split(QLatin1Char('+'), Qt::KeepEmptyParts);
    for (const QString& part : parts) {
        if (part.trimmed().isEmpty()) {
            return false;  // empty segment: malformed separator usage
        }
    }
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
