// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_smart_file_filter.cpp
/// @brief Unit tests for SmartFileFilter exclusion logic

#include <QtTest/QtTest>

#include "sak/smart_file_filter.h"
#include "sak/user_profile_types.h"

#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>

class SmartFileFilterTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Constructor
    void defaultConstruction();
    void constructionWithRules();

    // Dangerous files
    void dangerousFile_detected();
    void dangerousFile_caseInsensitive();
    void normalFile_notDangerous();

    // Size limits
    void sizeLimit_enabled_exceedsLimit();
    void sizeLimit_enabled_withinLimit();
    void sizeLimit_disabled_noFiltering();

    // Pattern exclusion
    void excludePattern_matchesRegex();
    void excludePattern_noMatch();

    // Folder exclusion
    void excludeFolder_detected();
    void excludeFolder_nestedPath();
    void normalFolder_notExcluded();

    // Cache directory
    void cacheDirectory_detected();
    void nonCacheDirectory_notDetected();

    // Full file exclusion
    void shouldExcludeFile_dangerous();
    void shouldExcludeFile_tooLarge();
    void shouldExcludeFile_patternMatch();
    void shouldExcludeFile_inCacheDir();
    void shouldExcludeFile_normal();

    // Exclusion reasons
    void exclusionReason_dangerous();
    void exclusionReason_normal();

    // Rule updates
    void setRules_updatesFiltering();

private:
    QTemporaryDir m_tempDir;
    QString m_profilePath;
};

void SmartFileFilterTests::initTestCase()
{
    QVERIFY(m_tempDir.isValid());
    m_profilePath = m_tempDir.path();

    // Create test files
    QFile f(m_tempDir.filePath("normal.txt"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("normal content");
    f.close();

    QFile d(m_tempDir.filePath("NTUSER.DAT"));
    QVERIFY(d.open(QIODevice::WriteOnly));
    d.write("registry data");
    d.close();

    QDir(m_tempDir.path()).mkpath("AppData/Local/Google/Chrome/User Data/Default/Cache");
}

// ============================================================================
// Constructor Tests
// ============================================================================

void SmartFileFilterTests::defaultConstruction()
{
    sak::SmartFileFilter filter;
    const auto& rules = filter.getRules();
    // Default rules should be initialized
    QVERIFY(!rules.dangerous_files.isEmpty());
}

void SmartFileFilterTests::constructionWithRules()
{
    sak::SmartFilter rules;
    rules.dangerous_files = {"custom_dangerous.dat"};
    rules.exclude_patterns = {"*.tmp"};

    sak::SmartFileFilter filter(rules);
    QVERIFY(filter.isDangerousFile("custom_dangerous.dat"));
}

// ============================================================================
// Dangerous File Tests
// ============================================================================

void SmartFileFilterTests::dangerousFile_detected()
{
    sak::SmartFileFilter filter;
    QVERIFY(filter.isDangerousFile("NTUSER.DAT"));
}

void SmartFileFilterTests::dangerousFile_caseInsensitive()
{
    sak::SmartFileFilter filter;
    QVERIFY(filter.isDangerousFile("ntuser.dat"));
    QVERIFY(filter.isDangerousFile("Ntuser.DAT"));
}

void SmartFileFilterTests::normalFile_notDangerous()
{
    sak::SmartFileFilter filter;
    QVERIFY(!filter.isDangerousFile("document.docx"));
    QVERIFY(!filter.isDangerousFile("photo.jpg"));
}

// ============================================================================
// Size Limit Tests
// ============================================================================

void SmartFileFilterTests::sizeLimit_enabled_exceedsLimit()
{
    sak::SmartFilter rules;
    rules.enable_file_size_limit = true;
    rules.max_single_file_size_bytes = 100;
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    QVERIFY(filter.exceedsSizeLimit(200));
}

void SmartFileFilterTests::sizeLimit_enabled_withinLimit()
{
    sak::SmartFilter rules;
    rules.enable_file_size_limit = true;
    rules.max_single_file_size_bytes = 1000;
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    QVERIFY(!filter.exceedsSizeLimit(500));
}

void SmartFileFilterTests::sizeLimit_disabled_noFiltering()
{
    sak::SmartFilter rules;
    rules.enable_file_size_limit = false;
    rules.max_single_file_size_bytes = 10;
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    // With size limit disabled, even huge sizes should pass
    QVERIFY(!filter.exceedsSizeLimit(999999999));
}

// ============================================================================
// Pattern Exclusion Tests
// ============================================================================

void SmartFileFilterTests::excludePattern_matchesRegex()
{
    sak::SmartFilter rules;
    rules.exclude_patterns = {".*\\.tmp$", ".*\\.log$"};
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);

    QFileInfo tmpFile(m_tempDir.filePath("temp.tmp"));
    // Create the file so QFileInfo can report on it
    QFile f(tmpFile.filePath());
    f.open(QIODevice::WriteOnly);
    f.write("temp");
    f.close();

    QVERIFY(filter.shouldExcludeFile(tmpFile, m_profilePath));
}

void SmartFileFilterTests::excludePattern_noMatch()
{
    sak::SmartFilter rules;
    rules.exclude_patterns = {".*\\.tmp$"};
    rules.dangerous_files.clear(); // Clear defaults for clean test
    rules.exclude_folders.clear();

    sak::SmartFileFilter filter(rules);

    QFileInfo docFile(m_tempDir.filePath("normal.txt"));
    QVERIFY(!filter.shouldExcludeFile(docFile, m_profilePath));
}

// ============================================================================
// Folder Exclusion Tests
// ============================================================================

void SmartFileFilterTests::excludeFolder_detected()
{
    sak::SmartFilter rules;
    rules.exclude_folders = {"node_modules", ".git"};
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);

    QDir(m_tempDir.path()).mkpath("node_modules");
    QFileInfo folder(m_tempDir.filePath("node_modules"));
    QVERIFY(filter.shouldExcludeFolder(folder, m_profilePath));
}

void SmartFileFilterTests::excludeFolder_nestedPath()
{
    sak::SmartFilter rules;
    rules.exclude_folders = {"Cache"};
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);

    QDir(m_tempDir.path()).mkpath("sub/Cache");
    QFileInfo folder(m_tempDir.filePath("sub/Cache"));
    QVERIFY(filter.shouldExcludeFolder(folder, m_profilePath));
}

void SmartFileFilterTests::normalFolder_notExcluded()
{
    sak::SmartFilter rules;
    rules.exclude_folders = {"node_modules"};
    rules.dangerous_files.clear();

    sak::SmartFileFilter filter(rules);

    QDir(m_tempDir.path()).mkpath("Documents");
    QFileInfo folder(m_tempDir.filePath("Documents"));
    QVERIFY(!filter.shouldExcludeFolder(folder, m_profilePath));
}

// ============================================================================
// Cache Directory Tests
// ============================================================================

void SmartFileFilterTests::cacheDirectory_detected()
{
    sak::SmartFileFilter filter;
    // isInCacheDirectory checks for \cache\ (with separators) in the path
    // Use a path where Cache is an intermediate directory, not the final segment
    QString cachePath = m_tempDir.filePath("AppData/Local/Google/Chrome/User Data/Default/Cache/cached_data.bin");
    QVERIFY(filter.isInCacheDirectory(cachePath));
}

void SmartFileFilterTests::nonCacheDirectory_notDetected()
{
    sak::SmartFileFilter filter;
    QVERIFY(!filter.isInCacheDirectory(m_tempDir.filePath("Documents/MyFile.txt")));
}

// ============================================================================
// Full File Exclusion Tests
// ============================================================================

void SmartFileFilterTests::shouldExcludeFile_dangerous()
{
    sak::SmartFileFilter filter;
    QFileInfo fileInfo(m_tempDir.filePath("NTUSER.DAT"));
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_tooLarge()
{
    sak::SmartFilter rules;
    rules.enable_file_size_limit = true;
    rules.max_single_file_size_bytes = 1; // 1 byte limit
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    QFileInfo fileInfo(m_tempDir.filePath("normal.txt")); // "normal content" = 14 bytes
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_patternMatch()
{
    sak::SmartFilter rules;
    rules.exclude_patterns = {".*\\.dat$"};
    rules.dangerous_files.clear(); // Don't catch .dat via dangerous list
    // Do NOT call initializeDefaults() â€” it would overwrite our custom rules

    sak::SmartFileFilter filter(rules);
    // The pattern should catch .dat files
    QFile f(m_tempDir.filePath("test.dat"));
    f.open(QIODevice::WriteOnly);
    f.write("data");
    f.close();

    QFileInfo fileInfo(m_tempDir.filePath("test.dat"));
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_inCacheDir()
{
    sak::SmartFileFilter filter;
    QString cachePath = m_tempDir.filePath(
        "AppData/Local/Google/Chrome/User Data/Default/Cache/cached_file.bin");
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QFile f(cachePath);
    f.open(QIODevice::WriteOnly);
    f.write("cached");
    f.close();

    QFileInfo fileInfo(cachePath);
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_normal()
{
    sak::SmartFilter rules;
    rules.dangerous_files.clear();
    rules.exclude_patterns.clear();
    rules.exclude_folders.clear();
    rules.enable_file_size_limit = false;

    sak::SmartFileFilter filter(rules);
    QFileInfo fileInfo(m_tempDir.filePath("normal.txt"));
    QVERIFY(!filter.shouldExcludeFile(fileInfo, m_profilePath));
}

// ============================================================================
// Exclusion Reason Tests
// ============================================================================

void SmartFileFilterTests::exclusionReason_dangerous()
{
    sak::SmartFileFilter filter;
    QFileInfo fileInfo(m_tempDir.filePath("NTUSER.DAT"));
    QString reason = filter.getExclusionReason(fileInfo);
    QVERIFY(!reason.isEmpty());
}

void SmartFileFilterTests::exclusionReason_normal()
{
    sak::SmartFilter rules;
    rules.dangerous_files.clear();
    rules.exclude_patterns.clear();
    rules.exclude_folders.clear();
    rules.enable_file_size_limit = false;

    sak::SmartFileFilter filter(rules);
    QFileInfo fileInfo(m_tempDir.filePath("normal.txt"));
    QString reason = filter.getExclusionReason(fileInfo);
    // getExclusionReason always returns a non-empty string (fallback reason)
    // It's designed to explain exclusions, not to determine if a file is excluded
    QVERIFY(!reason.isEmpty());
}

// ============================================================================
// Rule Update Tests
// ============================================================================

void SmartFileFilterTests::setRules_updatesFiltering()
{
    sak::SmartFileFilter filter;

    // Initially should detect NTUSER.DAT as dangerous
    QVERIFY(filter.isDangerousFile("NTUSER.DAT"));

    // Update rules with empty dangerous files
    sak::SmartFilter newRules;
    newRules.dangerous_files.clear();
    filter.setRules(newRules);

    QVERIFY(!filter.isDangerousFile("NTUSER.DAT"));
}

QTEST_GUILESS_MAIN(SmartFileFilterTests)
#include "test_smart_file_filter.moc"
