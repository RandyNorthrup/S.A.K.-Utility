// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_key_chord.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>

#include <algorithm>

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
        const char* m_name;
        WORD m_vk;
    } kKeys[] = {
        {.m_name = "enter", .m_vk = VK_RETURN},   {.m_name = "return", .m_vk = VK_RETURN},
        {.m_name = "tab", .m_vk = VK_TAB},        {.m_name = "escape", .m_vk = VK_ESCAPE},
        {.m_name = "esc", .m_vk = VK_ESCAPE},     {.m_name = "space", .m_vk = VK_SPACE},
        {.m_name = "backspace", .m_vk = VK_BACK}, {.m_name = "delete", .m_vk = VK_DELETE},
        {.m_name = "del", .m_vk = VK_DELETE},     {.m_name = "up", .m_vk = VK_UP},
        {.m_name = "down", .m_vk = VK_DOWN},      {.m_name = "left", .m_vk = VK_LEFT},
        {.m_name = "right", .m_vk = VK_RIGHT},    {.m_name = "home", .m_vk = VK_HOME},
        {.m_name = "end", .m_vk = VK_END},        {.m_name = "pageup", .m_vk = VK_PRIOR},
        {.m_name = "pagedown", .m_vk = VK_NEXT},  {.m_name = "insert", .m_vk = VK_INSERT},
        {.m_name = "f1", .m_vk = VK_F1},          {.m_name = "f2", .m_vk = VK_F2},
        {.m_name = "f3", .m_vk = VK_F3},          {.m_name = "f4", .m_vk = VK_F4},
        {.m_name = "f5", .m_vk = VK_F5},          {.m_name = "f6", .m_vk = VK_F6},
        {.m_name = "f7", .m_vk = VK_F7},          {.m_name = "f8", .m_vk = VK_F8},
        {.m_name = "f9", .m_vk = VK_F9},          {.m_name = "f10", .m_vk = VK_F10},
        {.m_name = "f11", .m_vk = VK_F11},        {.m_name = "f12", .m_vk = VK_F12},
    };
    for (const auto& key : kKeys) {
        if (low == QLatin1String(key.m_name)) {
            return key.m_vk;
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
    // empty segment: malformed separator usage (e.g. "Ctrl++S", "+S", "Ctrl+")
    if (std::ranges::any_of(parts, [](const QString& part) { return part.trimmed().isEmpty(); })) {
        return false;
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
