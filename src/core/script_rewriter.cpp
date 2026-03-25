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

QString ScriptRewriter::replaceUrl(const QString& script,
                                   const QString& url,
                                   const QString& local_filename,
                                   QVector<ScriptReplacement>& replacements) const {
    QString result = script;
    QString tools_path = buildToolsPath(local_filename);

    // Replace URL in both quoted and unquoted contexts
    // Pattern: 'http://...' or "http://..." or bare http://...
    int search_pos = 0;
    while (true) {
        int found_pos = result.indexOf(url, search_pos, Qt::CaseInsensitive);
        if (found_pos < 0) {
            break;
        }

        ScriptReplacement replacement;
        replacement.original_url = url;
        replacement.local_path = tools_path;

        // Count newlines up to this position for line number
        replacement.line_number = static_cast<int>(result.left(found_pos).count('\n')) + 1;

        result.replace(found_pos, url.length(), tools_path);
        replacements.append(replacement);

        search_pos = found_pos + tools_path.length();
    }

    return result;
}

QString ScriptRewriter::buildToolsPath(const QString& filename) const {
    // Produce: (Join-Path $toolsDir 'filename.ext')
    return QString("(Join-Path $toolsDir '%1')").arg(filename);
}

}  // namespace sak
