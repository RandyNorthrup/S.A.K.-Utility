// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/smart_file_filter.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDir>

#include <algorithm>

namespace sak {

namespace {
constexpr int kFileSizeDisplayPrecision = 1;
constexpr int kFileSizeLimitDisplayPrecision = 0;

// Upper bound on an exclude-pattern's source length. Filename-matching regexes
// are short in practice; anything longer is treated as invalid rather than
// compiled, bounding the work a pathological rule can force on every filename.
constexpr int kMaxExcludePatternLength = 1024;
}  // namespace

SmartFileFilter::SmartFileFilter(const SmartFilter& rules) : m_rules(rules) {
    compileRegexPatterns();

    // Convert lists to sets for faster lookup
    for (const QString& file : m_rules.dangerous_files) {
        m_dangerousFilesSet.insert(file.toLower());
    }

    // Defense in depth: the registry-hive protections are non-negotiable. A caller can
    // hand us an empty or hand-built rules struct whose dangerous_files list dropped
    // them, so re-union the mandatory set -- NTUSER.DAT and friends can never be
    // filtered back in as copyable and corrupt a live profile on restore.
    for (const QString& file : SmartFilter::mandatoryDangerousFiles()) {
        m_dangerousFilesSet.insert(file.toLower());
    }

    for (const QString& folder : m_rules.exclude_folders) {
        m_excludeFoldersSet.insert(folder.toLower());
    }
}

void SmartFileFilter::setRules(const SmartFilter& rules) {
    m_rules = rules;
    compileRegexPatterns();

    m_dangerousFilesSet.clear();
    for (const QString& file : m_rules.dangerous_files) {
        m_dangerousFilesSet.insert(file.toLower());
    }
    // Mandatory registry-hive protections survive any supplied rules (see ctor).
    for (const QString& file : SmartFilter::mandatoryDangerousFiles()) {
        m_dangerousFilesSet.insert(file.toLower());
    }

    m_excludeFoldersSet.clear();
    for (const QString& folder : m_rules.exclude_folders) {
        m_excludeFoldersSet.insert(folder.toLower());
    }
}

void SmartFileFilter::compileRegexPatterns() {
    m_compiledPatterns.clear();
    m_invalidPatterns.clear();

    for (const QString& pattern : m_rules.exclude_patterns) {
        // Record and log every rejected rule instead of dropping it silently:
        // otherwise files the user meant to exclude are processed with no
        // indication the pattern was invalid. Callers can fail closed via
        // hasInvalidPatterns().
        QString error;
        if (pattern.length() > kMaxExcludePatternLength) {
            error =
                QStringLiteral("pattern exceeds %1-character limit").arg(kMaxExcludePatternLength);
        } else {
            const QRegularExpression regex(pattern, QRegularExpression::CaseInsensitiveOption);
            if (regex.isValid()) {
                m_compiledPatterns.append(regex);
                continue;
            }
            error = regex.errorString();
        }
        const QString detail = QStringLiteral("%1: %2").arg(pattern, error);
        m_invalidPatterns.append(detail);
        logWarning("SmartFileFilter: ignoring invalid exclude pattern {}", detail.toStdString());
    }
}

bool SmartFileFilter::shouldExcludeFile(const QFileInfo& fileInfo,
                                        const QString& profilePath) const {
    const QString fileName = fileInfo.fileName();
    const QString absolutePath = fileInfo.absoluteFilePath();

    // 1. Check if it's a dangerous file (NTUSER.DAT, etc.)
    if (isDangerousFile(fileName)) {
        return true;
    }

    // 2. Check file size
    if (exceedsSizeLimit(fileInfo.size())) {
        return true;
    }

    // 3. Check filename patterns (*.tmp, *.lock, etc.)
    if (matchesPattern(fileName)) {
        return true;
    }

    // 4. Check if in excluded folder (Cache, Temp, etc.)
    const QString relativePath = QDir(profilePath).relativeFilePath(absolutePath);
    if (isInExcludedFolder(relativePath)) {
        return true;
    }

    // 5. Check if in cache directory
    if (isInCacheDirectory(absolutePath)) {
        return true;
    }

    return false;
}

bool SmartFileFilter::shouldExcludeFolder(const QFileInfo& folderInfo,
                                          const QString& profilePath) const {
    const QString folderName = folderInfo.fileName();
    const QString absolutePath = folderInfo.absoluteFilePath();

    // Check if folder name is in exclusion list
    if (m_excludeFoldersSet.contains(folderName.toLower())) {
        return true;
    }

    // Check relative path for excluded folders
    const QString relativePath = QDir(profilePath).relativeFilePath(absolutePath);
    if (isInExcludedFolder(relativePath)) {
        return true;
    }

    // Exclude cache directories
    if (isInCacheDirectory(absolutePath)) {
        return true;
    }

    return false;
}

bool SmartFileFilter::exceedsSizeLimit(qint64 size) const {
    // Only enforce limit if enabled
    if (!m_rules.enable_file_size_limit) {
        return false;
    }
    return size > m_rules.max_single_file_size_bytes;
}

bool SmartFileFilter::isDangerousFile(const QString& fileName) const {
    return m_dangerousFilesSet.contains(fileName.toLower());
}

bool SmartFileFilter::matchesPattern(const QString& fileName) const {
    return std::ranges::any_of(m_compiledPatterns,

                               [&fileName](const QRegularExpression& regex) {
                                   return regex.match(fileName).hasMatch();
                               });
}

bool SmartFileFilter::isInExcludedFolder(const QString& relativePath) const {
    // relativePath is whatever QDir::relativeFilePath() returned for a caller's
    // QFileInfo, so it can legitimately be empty; an empty path simply has no
    // components and is not in an excluded folder.
    // Split path into components
    QStringList components = relativePath.split('/', Qt::SkipEmptyParts);
    components.append(relativePath.split('\\', Qt::SkipEmptyParts));

    return std::ranges::any_of(components, [this](const QString& component) {
        return m_excludeFoldersSet.contains(component.toLower());
    });
}

bool SmartFileFilter::isInCacheDirectory(const QString& path) {
    // Public entry point: path comes from the caller and an empty one contains no
    // cache pattern, which the search below already reports correctly.
    QString lowerPath = path.toLower();

    // Common cache directory patterns
    QStringList cachePatterns = {"\\cache\\",
                                 "\\gpucache\\",
                                 "\\code cache\\",
                                 "\\shadercache\\",
                                 "\\webcache\\",
                                 "\\service worker\\",
                                 "\\session storage\\",
                                 "/cache/",
                                 "/gpucache/",
                                 "/code cache/",
                                 "/shadercache/",
                                 "/webcache/",
                                 "/service worker/",
                                 "/session storage/"};

    return std::ranges::any_of(cachePatterns,

                               [&lowerPath](const QString& pattern) {
                                   return lowerPath.contains(pattern);
                               });
}

QString SmartFileFilter::getExclusionReason(const QFileInfo& fileInfo) const {
    const QString fileName = fileInfo.fileName();

    if (isDangerousFile(fileName)) {
        return QString("Dangerous system file: %1 (would corrupt profile)").arg(fileName);
    }

    if (exceedsSizeLimit(fileInfo.size())) {
        const double sizeMB = static_cast<double>(fileInfo.size()) / kBytesPerMBf;
        if (m_rules.enable_file_size_limit) {
            return QString("File too large: %1 MB (limit: %2 MB)")
                .arg(sizeMB, 0, 'f', kFileSizeDisplayPrecision)
                .arg(static_cast<double>(m_rules.max_single_file_size_bytes) / kBytesPerMBf,
                     0,
                     'f',
                     kFileSizeLimitDisplayPrecision);
        }
    }

    if (matchesPattern(fileName)) {
        return QString("Matches exclusion pattern: %1").arg(fileName);
    }

    if (isInCacheDirectory(fileInfo.absoluteFilePath())) {
        return "Located in cache directory";
    }

    return "Excluded by filter rules";
}

}  // namespace sak
