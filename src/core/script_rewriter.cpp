// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file script_rewriter.cpp
/// @brief Rewrites Chocolatey install scripts for offline/internalized use

#include "sak/script_rewriter.h"

#include "sak/logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace sak {

// ============================================================================
// Public API
// ============================================================================

RewrittenScript ScriptRewriter::rewrite(const ParsedInstallScript& parsed,
                                        const QHash<QString, QString>& local_filenames) const {
    RewrittenScript result;

    if (parsed.original_script.isEmpty()) {
        result.error_message = "Empty script content";
        return result;
    }

    if (local_filenames.isEmpty()) {
        result.error_message = "No local filename mappings provided";
        return result;
    }

    QString rewritten = parsed.original_script;

    for (const auto& resource : parsed.resources) {
        // Replace primary URL
        if (!resource.url.isEmpty() && local_filenames.contains(resource.url)) {
            rewritten = replaceUrl(
                rewritten, resource.url, local_filenames.value(resource.url), result.replacements);
        }

        // Replace 64-bit URL
        if (!resource.url_64bit.isEmpty() && local_filenames.contains(resource.url_64bit)) {
            rewritten = replaceUrl(rewritten,
                                   resource.url_64bit,
                                   local_filenames.value(resource.url_64bit),
                                   result.replacements);
        }
    }

    result.script_content = rewritten;
    result.success = true;

    sak::logInfo("[ScriptRewriter] Rewrote script: {} replacements applied",
                 static_cast<int>(result.replacements.size()));

    return result;
}

RewrittenScript ScriptRewriter::rewriteToFile(const ParsedInstallScript& parsed,
                                              const QHash<QString, QString>& local_filenames,
                                              const QString& output_path) const {
    auto result = rewrite(parsed, local_filenames);
    if (!result.success) {
        return result;
    }

    // Ensure the output directory exists
    QFileInfo info(output_path);
    QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        result.success = false;
        result.error_message = QString("Cannot write to: %1").arg(output_path);
        sak::logError("[ScriptRewriter] {}", result.error_message.toStdString());
        return result;
    }

    QTextStream stream(&file);
    stream << result.script_content;
    file.close();

    sak::logInfo("[ScriptRewriter] Wrote rewritten script to: {}", output_path.toStdString());

    return result;
}

// ============================================================================
// Private Helpers
// ============================================================================

ScriptRewriter::ReplacementSpan ScriptRewriter::urlReplacementSpan(const QString& script,
                                                                   int found_pos,
                                                                   int url_len) {
    ReplacementSpan span{found_pos, url_len};
    const int after_pos = found_pos + url_len;
    if (found_pos > 0 && after_pos < script.length()) {
        const QChar before = script.at(found_pos - 1);
        const QChar after = script.at(after_pos);
        const bool wrapped = (before == QLatin1Char('\'') && after == QLatin1Char('\'')) ||
                             (before == QLatin1Char('"') && after == QLatin1Char('"'));
        if (wrapped) {
            span.start = found_pos - 1;  // swallow the opening quote
            span.length = url_len + 2;   // ...and the closing quote
        }
    }
    return span;
}

QString ScriptRewriter::replaceUrl(const QString& script,
                                   const QString& url,
                                   const QString& local_filename,
                                   QVector<ScriptReplacement>& replacements) const {
    QString result = script;
    QString tools_path = buildToolsPath(local_filename);

    // Replace URL in both quoted and unquoted contexts. tools_path is a PS
    // EXPRESSION, so a matched wrapping quote pair must be consumed too --
    // otherwise '<url>' becomes '(Join-Path ...)' , a literal string, and the
    // rewritten download silently does nothing.
    int search_pos = 0;
    while (true) {
        int found_pos = result.indexOf(url, search_pos, Qt::CaseInsensitive);
        if (found_pos < 0) {
            break;
        }

        const ReplacementSpan span =
            urlReplacementSpan(result, found_pos, static_cast<int>(url.length()));

        ScriptReplacement replacement;
        replacement.original_url = url;
        replacement.local_path = tools_path;

        // Count newlines up to this position for line number
        replacement.line_number = static_cast<int>(result.left(span.start).count('\n')) + 1;

        result.replace(span.start, span.length, tools_path);
        replacements.append(replacement);

        search_pos = span.start + tools_path.length();
    }

    return result;
}

QString ScriptRewriter::buildToolsPath(const QString& filename) const {
    // Produce: (Join-Path $toolsDir 'filename.ext'). A single quote inside a PowerShell
    // single-quoted string is escaped by doubling it; do so here so a quote in the filename
    // (e.g. a %27-decoded URL name) cannot terminate the literal or inject tokens.
    QString escaped = filename;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return QString("(Join-Path $toolsDir '%1')").arg(escaped);
}

}  // namespace sak
