// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_html_sanitizer.h
/// @brief Neutralizes active content in untrusted email HTML before it is saved/rendered.
///
/// Email bodies are attacker-controlled. When we persist them as a standalone .html page or feed
/// them to a QTextDocument for PDF rendering, raw <script>, event handlers, and javascript: URIs
/// would execute or phone home when the technician opens the artifact. This strips those.
///
/// This is ONE layer. Callers add the hard guarantees on top: the HTML writer emits a strict CSP
/// meta (blocks remote loads + script execution), and the PDF writer renders through a
/// QTextDocument that denies every external resource load (no local-file disclosure).

#pragma once

#include <QRegularExpression>
#include <QString>

namespace sak {

/// @brief Strip script blocks, framing/loading tags, inline event handlers, and javascript:/
/// vbscript: URIs from an untrusted email body. Header-only + pure so both writers and their unit
/// tests share exactly one implementation.
inline QString sanitizeEmailBodyHtml(const QString& html) {
    QString out = html;

    // Remove entire <script>...</script> blocks, including their contents, across newlines.
    static const QRegularExpression kScriptBlock(
        QStringLiteral("<script\\b[^>]*>.*?</script\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    out.remove(kScriptBlock);

    // Remove framing / resource-loading / script-hosting tags (open, close, or self-closing).
    static const QRegularExpression kDangerTags(
        QStringLiteral("</?\\s*(script|iframe|object|embed|frame|frameset|applet|base|meta|link|"
                       "form)\\b[^>]*>"),
        QRegularExpression::CaseInsensitiveOption);
    out.remove(kDangerTags);

    // Remove inline event-handler attributes: on...="..." / on...='...' / on...=bareword.
    static const QRegularExpression kEventHandlers(
        QStringLiteral("\\son[a-zA-Z]+\\s*=\\s*(\"[^\"]*\"|'[^']*'|[^\\s>]+)"),
        QRegularExpression::CaseInsensitiveOption);
    out.remove(kEventHandlers);

    // Neutralize javascript:/vbscript: URIs wherever they appear (href, src, style, ...).
    static const QRegularExpression kScriptUris(QStringLiteral("(javascript|vbscript)\\s*:"),
                                                QRegularExpression::CaseInsensitiveOption);
    out.replace(kScriptUris, QStringLiteral("blocked:"));

    return out;
}

}  // namespace sak
