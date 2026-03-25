// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file script_rewriter.h
/// @brief Rewrites Chocolatey install scripts for offline/internalized use
///
/// Takes a parsed install script and rewrites download URLs to reference
/// local file paths within the package's tools directory. This enables
/// Chocolatey packages to install without internet access.

#pragma once

#include "sak/install_script_parser.h"

#include <QString>
#include <QVector>

namespace sak {

/// @brief A single URL-to-local-path replacement applied to a script
struct ScriptReplacement {
    /// @brief Original URL that was replaced
    QString original_url;
    /// @brief Local path that replaced the URL
    QString local_path;
    /// @brief Line number where the replacement occurred
    int line_number{0};
};

/// @brief Result of rewriting an install script for offline use
struct RewrittenScript {
    /// @brief The rewritten script content
    QString script_content;
    /// @brief All replacements that were applied
    QVector<ScriptReplacement> replacements;
    /// @brief Whether the rewrite was successful
    bool success{false};
    /// @brief Error message if rewrite failed
    QString error_message;
};

/// @brief Rewrites chocolateyInstall.ps1 scripts for offline deployment
///
/// Replaces remote download URLs with local $toolsDir references so that
/// Chocolatey packages can install without internet connectivity.
class ScriptRewriter {
public:
    ScriptRewriter() = default;

    /// @brief Rewrite a parsed script for offline use
    /// @param parsed Previously parsed install script
    /// @param local_filenames Map of original URLs to local filenames
    /// @return Rewritten script with local references
    [[nodiscard]] RewrittenScript rewrite(const ParsedInstallScript& parsed,
                                          const QHash<QString, QString>& local_filenames) const;

    /// @brief Rewrite and save to a file
    /// @param parsed Previously parsed install script
    /// @param local_filenames Map of original URLs to local filenames
    /// @param output_path File path to write the rewritten script
    /// @return Rewritten script result
    [[nodiscard]] RewrittenScript rewriteToFile(const ParsedInstallScript& parsed,
                                                const QHash<QString, QString>& local_filenames,
                                                const QString& output_path) const;

private:
    /// @brief Replace a single URL in script content with a local path
    [[nodiscard]] QString replaceUrl(const QString& script,
                                     const QString& url,
                                     const QString& local_filename,
                                     QVector<ScriptReplacement>& replacements) const;

    /// @brief Build a $toolsDir-relative path expression
    [[nodiscard]] QString buildToolsPath(const QString& filename) const;
};

}  // namespace sak
