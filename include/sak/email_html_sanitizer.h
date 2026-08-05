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

#include <QLatin1String>
#include <QRegularExpression>
#include <QString>
#include <QUrl>

namespace sak {

/// @brief True only for a resource an untrusted-email renderer may load: a self-contained data:
/// URI.
///
/// Qt's rich text has no script engine, so the live exposure in a message viewer is not code
/// execution -- it is resource loading. QTextDocument/QTextBrowser resolve <img src=...> (and a
/// link target on activation) through loadResource(), which will read file:/// and UNC paths off
/// the technician's machine, so <img src="file:///C:/Users/Username/.ssh/id_rsa"> discloses local
/// content and a remote src is a tracking pixel. data: is the one scheme that cannot reach disk or
/// network, and is what the inspector rewrites cid:/downloaded images into.
///
/// Fail closed: anything else -- file, http(s), ftp, qrc, cid, UNC, and every relative reference
/// that would resolve against the document base -- is refused, never passed through. Pure and
/// QtCore-only so the policy is unit-testable without a display; sak/email_safe_text_browser.h
/// applies it, and pdf_email_writer.cpp enforces the same rule on its render document.
[[nodiscard]] inline bool emailResourceIsAllowed(const QUrl& url) {
    return url.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) == 0;
}

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

    // A <style> block (and every inline style=) is kept, because it carries the formatting a
    // technician needs to read the message -- but the two CSS constructs that are not formatting
    // are neutralized. expression() is IE's CSS-to-script escape hatch.
    static const QRegularExpression kCssExpression(QStringLiteral("expression\\s*\\("),
                                                   QRegularExpression::CaseInsensitiveOption);
    out.replace(kCssExpression, QStringLiteral("blocked("));

    // url(...) is a resource fetch: remote (tracking pixel / @import beacon) or file:// and
    // relative refs (local disclosure). Only a self-contained data: URI is kept; a cid: url()
    // was never resolvable here either, so nothing that worked is lost.
    static const QRegularExpression kCssUrl(QStringLiteral("url\\(\\s*['\"]?(?!data:)[^)]*\\)"),
                                            QRegularExpression::CaseInsensitiveOption);
    out.replace(kCssUrl, QStringLiteral("none"));

    return out;
}

}  // namespace sak
