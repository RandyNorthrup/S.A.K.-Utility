// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file rich_text_constants.h
/// @brief The fixed markup wrapper used to force the rich-text branch safely.
///
/// asLiteralRichText() wraps escaped untrusted text so the WRAPPER -- never the
/// value -- decides the string is rich text. The wrapper carries a single fixed
/// structural style attribute (white-space:pre-wrap, so runs of spaces and tabs
/// survive the rich-text engine). It is not a theme token, but it is still a raw
/// style literal, so it lives here on the check_gui_stylesheet_literals allowlist
/// rather than inline in the widely-included rich_text_safety.h.

#pragma once

#include <QString>

namespace sak::ui {

inline QString literalRichTextOpenTag() {
    return QStringLiteral("<html><span style='white-space: pre-wrap;'>");
}

inline QString literalRichTextCloseTag() {
    return QStringLiteral("</span></html>");
}

}  // namespace sak::ui
