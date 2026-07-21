// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

namespace sak {

/**
 * @brief Intelligent file filtering to exclude dangerous and unnecessary files
 *
 * Prevents corruption by excluding:
 * - Registry hives (NTUSER.DAT, UsrClass.dat)
 * - Lock files and temp files
 * - Cache directories
 * - Machine-specific encrypted data
 */
class SmartFileFilter {
public:
    explicit SmartFileFilter(const SmartFilter& rules = SmartFilter());

    /**
     * @brief Check if a file should be excluded from backup
     * @param fileInfo File to check
     * @param profilePath User profile root path
     * @return true if file should be EXCLUDED
     */
    bool shouldExcludeFile(const QFileInfo& fileInfo, const QString& profilePath) const;

    /**
     * @brief Check if a folder should be excluded
     * @param folderInfo Folder to check
     * @param profilePath User profile root path
     * @return true if folder should be EXCLUDED
     */
    bool shouldExcludeFolder(const QFileInfo& folderInfo, const QString& profilePath) const;

    /**
     * @brief Check if file exceeds size limit
     * @param size File size in bytes
     * @return true if file is too large
     */
    bool exceedsSizeLimit(qint64 size) const;

    /**
     * @brief Get human-readable reason for exclusion
     * @param fileInfo File that was excluded
     * @return Reason string
     */
    QString getExclusionReason(const QFileInfo& fileInfo) const;

    /**
     * @brief Get the filter rules
     */
    const SmartFilter& getRules() const { return m_rules; }

    /**
     * @brief Update filter rules
     */
    void setRules(const SmartFilter& rules);

    /**
     * @brief Check if path is in AppData cache directory
     */
    static bool isInCacheDirectory(const QString& path);

    /**
     * @brief Check if file is a dangerous system file
     */
    bool isDangerousFile(const QString& fileName) const;

    /**
     * @brief Exclusion patterns that failed to compile as regular expressions.
     *
     * Each entry is "<pattern>: <error>". An invalid rule is dropped from the
     * active pattern set, so callers should surface these to the user rather
     * than silently letting the intended-to-exclude files through.
     */
    const QStringList& invalidPatterns() const { return m_invalidPatterns; }

private:
    void compileRegexPatterns();
    bool matchesPattern(const QString& fileName) const;
    bool isInExcludedFolder(const QString& relativePath) const;

    SmartFilter m_rules;
    QVector<QRegularExpression> m_compiledPatterns;
    QStringList m_invalidPatterns;
    QSet<QString> m_dangerousFilesSet;
    QSet<QString> m_excludeFoldersSet;
};

}  // namespace sak
