// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/rich_text_constants.h"

#include <QLatin1Char>
#include <QLatin1String>
#include <QString>

/// @file rich_text_safety.h
/// @brief Neutralize untrusted text before it reaches a Qt rich-text sink.
///
/// Qt renders a string as rich text whenever the sink allows it and the string looks like markup
/// (Qt::mightBeRichText). QLabel, QMessageBox and QTextEdit::append() all default to
/// Qt::AutoText, and a tooltip has NO plain-text mode at all -- so a program name read out of the
/// HKCU uninstall keys, a file name parsed out of a raw filesystem image, an SSID broadcast by a
/// nearby AP, or a line of AI/tool output can smuggle markup into a dialog: spoofed emphasis on a
/// destructive-action confirmation, or an <img src=...> that makes the app fetch a local or remote
/// resource. (No script runs -- Qt's rich text is a small HTML subset with no JS.)
///
/// There are exactly three correct sinks, and every fix in the GUI uses one of them:
///
///   1. The template WANTS markup (<b>, <br>) and the value does not
///      -> interpolate `value.toHtmlEscaped()` (the convention the report generators already use).
///   2. A QLabel wants no markup at all
///      -> sak::plainTextLabel() / setTextFormat(Qt::PlainText); nothing needs escaping then.
///   3. The sink is AutoText and cannot be switched to plain text (tooltips, QTextEdit::append)
///      -> sak::ui::asLiteralRichText(), below.
///
/// Case 3 cannot be solved by escaping alone: Qt::mightBeRichText only reports true for an escaped
/// string when it happens to contain "&lt;", so "AT&amp;T" would be shown literally as
/// "AT&amp;amp;T" on a sink that stayed in plain-text mode. Wrapping forces the rich-text branch,
/// so the escaped entities always decode back to the original characters.
namespace sak::ui {

/// Render @p untrusted verbatim on a sink that is stuck in Qt::AutoText.
///
/// Escapes every markup-significant character, turns real newlines into explicit line breaks, and
/// wraps the result so the wrapper -- never the value -- decides that the string is rich text.
/// The wrapper keeps white-space:pre-wrap so runs of spaces and tabs in a path or a log line
/// survive the trip through the rich-text engine instead of collapsing.
[[nodiscard]] inline QString asLiteralRichText(const QString& untrusted) {
    if (untrusted.isEmpty()) {
        // An empty tooltip means "no tooltip"; a wrapper alone would pop up an empty box.
        return {};
    }
    QString escaped = untrusted.toHtmlEscaped();
    escaped.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
    return literalRichTextOpenTag() + escaped + literalRichTextCloseTag();
}

}  // namespace sak::ui
