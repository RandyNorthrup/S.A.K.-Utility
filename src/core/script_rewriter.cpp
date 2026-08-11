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
#include <QSaveFile>
#include <QTextStream>

namespace sak {

namespace {

// A matched wrapping quote pair is two characters (the opening and closing quote).
constexpr int kWrappingQuotePairChars = 2;

/// @brief Build the fail-closed error for an incomplete rewrite, or "" if the
///        rewrite internalized every expected URL. A rewrite that leaves a
///        download URL live would silently fall back to the network (defeating
///        offline internalization) yet formerly reported success, so a mapped
///        URL that was NOT found in the script -- or a script with declared
///        resources but ZERO replacements -- is a hard failure, not a success.
QString rewriteFailure(const QStringList& unreplaced,
                       const QStringList& unmapped,
                       bool has_resources,
                       bool replaced_any) {
    if (!unreplaced.isEmpty()) {
        return QStringLiteral("Expected download URL(s) not found in script: %1")
            .arg(unreplaced.join(QStringLiteral(", ")));
    }
    if (!unmapped.isEmpty()) {
        // A declared download URL with no internalized local file: shipping it live would fall
        // back to the network at install time. Fail closed rather than call a partial map done.
        return QStringLiteral("Declared download URL(s) with no internalized local file: %1")
            .arg(unmapped.join(QStringLiteral(", ")));
    }
    if (has_resources && !replaced_any) {
        return QStringLiteral(
            "No declared download URL matched the script; "
            "nothing was internalized");
    }
    return QString();
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

RewrittenScript ScriptRewriter::rewrite(const ParsedInstallScript& parsed,
                                        const QHash<QString, QString>& local_filenames) {
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
    QStringList unreplaced;
    QStringList unmapped;

    for (const auto& resource : parsed.resources) {
        for (const QString& url : {resource.url, resource.url_64bit}) {
            if (url.isEmpty()) {
                continue;  // a resource may declare only a 32- or only a 64-bit URL
            }
            if (!local_filenames.contains(url)) {
                unmapped.append(url);  // a declared download with no local file to point at
                continue;
            }
            const qsizetype before = result.replacements.size();
            rewritten = replaceUrl(rewritten, url, local_filenames.value(url), result.replacements);
            if (result.replacements.size() == before) {
                unreplaced.append(url);  // mapped but absent from the script
            }
        }
    }

    const QString failure = rewriteFailure(
        unreplaced, unmapped, !parsed.resources.isEmpty(), !result.replacements.isEmpty());
    if (!failure.isEmpty()) {
        result.error_message = failure;
        sak::logError("[ScriptRewriter] {}", failure.toStdString());
        return result;  // fail closed: do NOT report a half-rewritten script as done
    }

    result.script_content = rewritten;
    result.success = true;

    sak::logInfo("[ScriptRewriter] Rewrote script: {} replacements applied",
                 static_cast<int>(result.replacements.size()));

    return result;
}

RewrittenScript ScriptRewriter::rewriteToFile(const ParsedInstallScript& parsed,
                                              const QHash<QString, QString>& local_filenames,
                                              const QString& output_path) {
    auto result = rewrite(parsed, local_filenames);
    if (!result.success) {
        return result;
    }

    // Ensure the output directory exists
    const QFileInfo info(output_path);
    const QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // QSaveFile: write to a temp file and atomically commit, so a crash or a
    // stream error never leaves a truncated/half-rewritten install script in
    // place of the original.
    QSaveFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        result.success = false;
        result.error_message = QString("Cannot write to: %1").arg(output_path);
        sak::logError("[ScriptRewriter] {}", result.error_message.toStdString());
        return result;
    }

    QTextStream stream(&file);
    stream << result.script_content;
    stream.flush();
    if (stream.status() != QTextStream::Ok || !file.commit()) {
        result.success = false;
        result.error_message = QString("Failed to write: %1").arg(output_path);
        sak::logError("[ScriptRewriter] {}", result.error_message.toStdString());
        return result;
    }

    sak::logInfo("[ScriptRewriter] Wrote rewritten script to: {}", output_path.toStdString());

    return result;
}

// ============================================================================
// Private Helpers
// ============================================================================

ScriptRewriter::ReplacementSpan ScriptRewriter::urlReplacementSpan(const QString& script,
                                                                   int found_pos,
                                                                   int url_len) {
    ReplacementSpan span{.start = found_pos, .length = url_len};
    // An out-of-range index is not a wrapping pair to swallow; return the bare span. Compute the
    // end position in a wide type so found_pos + url_len cannot overflow int on a multi-GB script
    // (signed overflow is UB), and reject a negative index/length outright.
    if (found_pos <= 0 || url_len < 0) {
        return span;
    }
    const qsizetype after_pos = static_cast<qsizetype>(found_pos) + url_len;
    if (after_pos < script.length()) {
        const QChar before = script.at(found_pos - 1);
        const QChar after = script.at(after_pos);
        const bool wrapped = (before == QLatin1Char('\'') && after == QLatin1Char('\'')) ||
                             (before == QLatin1Char('"') && after == QLatin1Char('"'));
        if (wrapped) {
            span.start = found_pos - 1;                       // swallow the opening quote
            span.length = url_len + kWrappingQuotePairChars;  // ...and the closing quote
        }
    }
    return span;
}

QString ScriptRewriter::replaceUrl(const QString& script,
                                   const QString& url,
                                   const QString& local_filename,
                                   QVector<ScriptReplacement>& replacements) {
    QString result = script;
    const QString tools_path = buildToolsPath(local_filename);

    // Replace URL in both quoted and unquoted contexts. tools_path is a PS
    // EXPRESSION, so a matched wrapping quote pair must be consumed too --
    // otherwise '<url>' becomes '(Join-Path ...)' , a literal string, and the
    // rewritten download silently does nothing.
    int search_pos = 0;
    while (true) {
        const int found_pos =
            static_cast<int>(result.indexOf(url, search_pos, Qt::CaseInsensitive));
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

        search_pos = static_cast<int>(span.start + tools_path.length());
    }

    return result;
}

QString ScriptRewriter::buildToolsPath(const QString& filename) {
    // Produce: (Join-Path $toolsDir 'filename.ext'). A single quote inside a PowerShell
    // single-quoted string is escaped by doubling it; do so here so a quote in the filename
    // (e.g. a %27-decoded URL name) cannot terminate the literal or inject tokens.
    QString escaped = filename;
    escaped.replace(QLatin1Char('\''), QLatin1String("''"));
    return QString("(Join-Path $toolsDir '%1')").arg(escaped);
}

}  // namespace sak
