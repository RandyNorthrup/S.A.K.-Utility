// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_smart_file_filter.cpp
/// @brief Unit tests for SmartFileFilter exclusion logic

#include "sak/smart_file_filter.h"
#include "sak/user_profile_types.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

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
    void excludePattern_invalidRecorded();

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

void SmartFileFilterTests::initTestCase() {
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

void SmartFileFilterTests::defaultConstruction() {
    sak::SmartFileFilter filter;
    const auto& rules = filter.getRules();
    // Default rules = the fixed 7-entry mandatory-dangerous list, in order. A truncation or
    // a dropped hive file (a security regression) must fail, not just pass !isEmpty().
    QCOMPARE(rules.dangerous_files,
             (QStringList{"NTUSER.DAT",
                          "NTUSER.DAT.LOG1",
                          "NTUSER.DAT.LOG2",
                          "ntuser.ini",
                          "UsrClass.dat",
                          "UsrClass.dat.LOG1",
                          "UsrClass.dat.LOG2"}));
    // The other two default catalogs ARE the rest of the exclusion policy and were pinned by
    // nothing at all: a dropped ".*\\.lock$" copies live lock files, a dropped "$RECYCLE.BIN"
    // or "Cache" copies junk/volatile state. Ordered compare, not membership.
    QCOMPARE(rules.exclude_patterns,
             (QStringList{".*\\.tmp$",
                          ".*\\.temp$",
                          ".*\\.cache$",
                          ".*\\.lock$",
                          ".*\\.lck$",
                          ".*~$",
                          ".*\\.crdownload$",
                          ".*\\.part$",
                          "desktop\\.ini$",
                          "thumbs\\.db$",
                          "\\.DS_Store$"}));
    QCOMPARE(rules.exclude_folders,
             (QStringList{"Temp",
                          "temp",
                          "$RECYCLE.BIN",
                          "Cache",
                          "GPUCache",
                          "Code Cache",
                          "Service Worker",
                          "Session Storage",
                          "WebCache",
                          "node_modules",
                          ".git",
                          ".svn",
                          "__pycache__",
                          "Packages"}));
    // Size filtering is OFF by default; a default that flipped it on would silently drop every
    // large file from every backup.
    QVERIFY(!rules.enable_file_size_limit);
    QVERIFY(!rules.enable_folder_size_limit);
    // Every default pattern compiles -- none is silently discarded as invalid.
    QVERIFY(!filter.hasInvalidPatterns());
}

void SmartFileFilterTests::constructionWithRules() {
    sak::SmartFilter rules;
    rules.dangerous_files = {"custom_dangerous.dat"};
    rules.exclude_patterns = {"*.tmp"};

    sak::SmartFileFilter filter(rules);
    QVERIFY(filter.isDangerousFile("custom_dangerous.dat"));
    // The mandatory hive set is re-unioned on top of the caller's list, so a caller list that
    // omits it can never SHRINK the dangerous set...
    QVERIFY(filter.isDangerousFile("NTUSER.DAT"));
    QVERIFY(filter.isDangerousFile("UsrClass.dat.LOG2"));
    // ...and nothing outside (caller list + mandatory list) becomes dangerous.
    QVERIFY(!filter.isDangerousFile("other_dangerous.dat"));
    // exclude_patterns was supplied by the fixture and asserted on by nothing. "*.tmp" is a
    // GLOB, and PCRE rejects a leading quantifier, so the ctor must RECORD it as invalid
    // instead of compiling it or dropping it silently (callers fail closed on this list).
    QVERIFY(filter.hasInvalidPatterns());
    QCOMPARE(filter.invalidPatterns().size(), 1);
    QVERIFY(filter.invalidPatterns().first().startsWith(QStringLiteral("*.tmp: ")));
}

// ============================================================================
// Dangerous File Tests
// ============================================================================

void SmartFileFilterTests::dangerousFile_detected() {
    sak::SmartFileFilter filter;
    QVERIFY(filter.isDangerousFile("NTUSER.DAT"));
}

void SmartFileFilterTests::dangerousFile_caseInsensitive() {
    sak::SmartFileFilter filter;
    QVERIFY(filter.isDangerousFile("ntuser.dat"));
    QVERIFY(filter.isDangerousFile("Ntuser.DAT"));
}

void SmartFileFilterTests::normalFile_notDangerous() {
    sak::SmartFileFilter filter;
    QVERIFY(!filter.isDangerousFile("document.docx"));
    QVERIFY(!filter.isDangerousFile("photo.jpg"));
}

// ============================================================================
// Size Limit Tests
// ============================================================================

void SmartFileFilterTests::sizeLimit_enabled_exceedsLimit() {
    sak::SmartFilter rules;
    rules.enable_file_size_limit = true;
    rules.max_single_file_size_bytes = 100;
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    QVERIFY(filter.exceedsSizeLimit(200));
}

void SmartFileFilterTests::sizeLimit_enabled_withinLimit() {
    sak::SmartFilter rules;
    rules.enable_file_size_limit = true;
    rules.max_single_file_size_bytes = 1000;
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    QVERIFY(!filter.exceedsSizeLimit(500));
    // The comparison is strict (`size > max`), so a file EXACTLY at the limit is kept and one
    // byte over is dropped. 200-vs-100 and 500-vs-1000 never touch the boundary, so a `>=`
    // mutant -- which would drop every file sitting exactly on the configured limit -- stayed
    // green in both size tests.
    QVERIFY(!filter.exceedsSizeLimit(999));
    QVERIFY(!filter.exceedsSizeLimit(1000));
    QVERIFY(filter.exceedsSizeLimit(1001));
    QVERIFY(!filter.exceedsSizeLimit(0));
}

void SmartFileFilterTests::sizeLimit_disabled_noFiltering() {
    sak::SmartFilter rules;
    rules.enable_file_size_limit = false;
    rules.max_single_file_size_bytes = 10;
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    // With size limit disabled, even huge sizes should pass
    QVERIFY(!filter.exceedsSizeLimit(999'999'999));
}

// ============================================================================
// Pattern Exclusion Tests
// ============================================================================

void SmartFileFilterTests::excludePattern_matchesRegex() {
    sak::SmartFilter rules;
    // No initializeDefaults() call here: SmartFilter's own constructor already seeded the
    // defaults, and calling it AFTER assigning REPLACES exclude_patterns -- the caller's list was
    // being discarded, so this test only ever exercised the built-in default patterns.
    rules.exclude_patterns = {".*\\.tmp$", ".*\\.log$"};

    sak::SmartFileFilter filter(rules);
    QVERIFY(!filter.hasInvalidPatterns());

    QFileInfo tmpFile(m_tempDir.filePath("temp.tmp"));
    // Create the file so QFileInfo can report on it
    QFile f(tmpFile.filePath());
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("temp");
    f.close();

    QVERIFY(filter.shouldExcludeFile(tmpFile, m_profilePath));
}

void SmartFileFilterTests::excludePattern_noMatch() {
    sak::SmartFilter rules;
    rules.exclude_patterns = {".*\\.tmp$"};
    rules.dangerous_files.clear();  // Clear defaults for clean test
    rules.exclude_folders.clear();

    sak::SmartFileFilter filter(rules);

    QFileInfo docFile(m_tempDir.filePath("normal.txt"));
    QVERIFY(!filter.shouldExcludeFile(docFile, m_profilePath));
}

void SmartFileFilterTests::excludePattern_invalidRecorded() {
    sak::SmartFilter rules;
    // One invalid regex (unterminated class) alongside a valid one.
    rules.exclude_patterns = {"[unterminated", ".*\\.tmp$"};
    rules.dangerous_files.clear();
    rules.exclude_folders.clear();

    sak::SmartFileFilter filter(rules);

    // The invalid pattern is surfaced rather than silently dropped.
    QCOMPARE(filter.invalidPatterns().size(), 1);
    QVERIFY(filter.hasInvalidPatterns());
    // The recorded entry is exactly "<pattern>: <error>" -- offending pattern first, then a
    // NON-empty diagnostic. contains() alone stayed green for an entry that was only the
    // pattern with the error dropped, or for the two halves swapped. The PCRE message text
    // itself is Qt-version dependent, so only the invariant shape is pinned.
    const QString detail = filter.invalidPatterns().first();
    QVERIFY2(detail.startsWith(QStringLiteral("[unterminated: ")), qPrintable(detail));
    QVERIFY2(detail.size() > QStringLiteral("[unterminated: ").size(), qPrintable(detail));
    QVERIFY(filter.invalidPatterns().first().contains(QStringLiteral("[unterminated")));

    // The valid pattern still filters.
    QFile f(m_tempDir.filePath("temp.tmp"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();
    const QFileInfo tmpInfo(m_tempDir.filePath("temp.tmp"));
    QVERIFY(filter.shouldExcludeFile(tmpInfo, m_profilePath));
    // ...and it is the surviving PATTERN that excluded it, not one of shouldExcludeFile's four
    // sibling arms: one bad rule must not disable the good ones, and must not be papered over by
    // the dangerous-file / size / folder / cache guards.
    QCOMPARE(filter.getExclusionReason(tmpInfo), QString("Matches exclusion pattern: temp.tmp"));
    // A file the surviving pattern does not match is still kept.
    QVERIFY(!filter.shouldExcludeFile(QFileInfo(m_tempDir.filePath("normal.txt")), m_profilePath));
}

// ============================================================================
// Folder Exclusion Tests
// ============================================================================

void SmartFileFilterTests::excludeFolder_detected() {
    sak::SmartFilter rules;
    // No initializeDefaults() call: the SmartFilter constructor already seeded the defaults, and
    // calling it AFTER assigning REPLACES exclude_folders with the built-in list (which happens
    // to contain both names below), so the caller's list was never the thing under test.
    rules.exclude_folders = {"node_modules", ".git"};

    sak::SmartFileFilter filter(rules);

    QDir(m_tempDir.path()).mkpath("node_modules");
    QFileInfo folder(m_tempDir.filePath("node_modules"));
    QVERIFY(filter.shouldExcludeFolder(folder, m_profilePath));
    // Every entry of the caller's list is honored, not just the first.
    QDir(m_tempDir.path()).mkpath(".git");
    QVERIFY(filter.shouldExcludeFolder(QFileInfo(m_tempDir.filePath(".git")), m_profilePath));
}

void SmartFileFilterTests::excludeFolder_nestedPath() {
    sak::SmartFilter rules;
    // Use a name with no "cache" substring: shouldExcludeFolder's third arm (isInCacheDirectory)
    // fires on ANY path containing "/cache/", so a nested "Cache" folder proves nothing about the
    // relative-path walk. No initializeDefaults() either -- it REPLACES exclude_folders with the
    // built-in list, so the caller's list was never under test.
    rules.exclude_folders = {"node_modules"};

    sak::SmartFileFilter filter(rules);

    // The excluded component is an ANCESTOR, not the leaf: shouldExcludeFolder tests the leaf
    // name against the set FIRST and returns before it ever reaches the relative-path walk, so a
    // leaf-named fixture leaves that walk unexercised.
    QDir(m_tempDir.path()).mkpath("node_modules/pkg/dist");
    QFileInfo folder(m_tempDir.filePath("node_modules/pkg/dist"));
    QVERIFY(filter.shouldExcludeFolder(folder, m_profilePath));
}

void SmartFileFilterTests::normalFolder_notExcluded() {
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

void SmartFileFilterTests::cacheDirectory_detected() {
    sak::SmartFileFilter filter;
    // isInCacheDirectory checks for \cache\ (with separators) in the path
    // Use a path where Cache is an intermediate directory, not the final segment
    QString cachePath = m_tempDir.filePath(
        "AppData/Local/Google/Chrome/User "
        "Data/Default/Cache/cached_data.bin");
    QVERIFY(filter.isInCacheDirectory(cachePath));
    // The detector is a 14-entry catalog, not one "/cache/" probe: a build that dropped any
    // family (or either separator flavour) would still pass the single Chrome-Cache probe
    // above while backing up GPU/shader/service-worker state.
    QVERIFY(filter.isInCacheDirectory("C:/p/GPUCache/x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:/p/Code Cache/x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:/p/ShaderCache/x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:/p/WebCache/x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:/p/Service Worker/x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:/p/Session Storage/x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\Cache\\x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\GPUCache\\x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\Code Cache\\x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\ShaderCache\\x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\WebCache\\x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\Service Worker\\x.bin"));
    QVERIFY(filter.isInCacheDirectory("C:\\p\\Session Storage\\x.bin"));
    // Matching is case-insensitive (the path is lowercased first)...
    QVERIFY(filter.isInCacheDirectory("C:/p/CACHE/x.bin"));
    // ...and separator-bounded: a name that merely embeds "cache", or a trailing directory
    // with no closing separator, is NOT a cache path.
    QVERIFY(!filter.isInCacheDirectory("C:/p/precached/x.bin"));
    QVERIFY(!filter.isInCacheDirectory("C:/p/Cache"));
}

void SmartFileFilterTests::nonCacheDirectory_notDetected() {
    sak::SmartFileFilter filter;
    QVERIFY(!filter.isInCacheDirectory(m_tempDir.filePath("Documents/MyFile.txt")));
}

// ============================================================================
// Full File Exclusion Tests
// ============================================================================

void SmartFileFilterTests::shouldExcludeFile_dangerous() {
    sak::SmartFileFilter filter;
    QFileInfo fileInfo(m_tempDir.filePath("NTUSER.DAT"));
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_tooLarge() {
    sak::SmartFilter rules;
    rules.enable_file_size_limit = true;
    rules.max_single_file_size_bytes = 1;  // 1 byte limit
    rules.initializeDefaults();

    sak::SmartFileFilter filter(rules);
    QFileInfo fileInfo(m_tempDir.filePath("normal.txt"));  // "normal content" = 14 bytes
    QCOMPARE(fileInfo.size(), qint64(14));
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
    // ...and the SIZE guard is what excluded it: the same file under the same rules with the
    // limit switched off must be KEPT, so this cannot be riding on a sibling arm (dangerous
    // list / pattern / folder / cache).
    sak::SmartFilter unlimited = rules;
    unlimited.enable_file_size_limit = false;
    QVERIFY(!sak::SmartFileFilter(unlimited).shouldExcludeFile(fileInfo, m_profilePath));

    // The guard is strict: a limit EQUAL to the file size keeps the file, one byte under
    // drops it.
    sak::SmartFilter atLimit = rules;
    atLimit.max_single_file_size_bytes = 14;
    QVERIFY(!sak::SmartFileFilter(atLimit).shouldExcludeFile(fileInfo, m_profilePath));

    sak::SmartFilter justUnder = rules;
    justUnder.max_single_file_size_bytes = 13;
    QVERIFY(sak::SmartFileFilter(justUnder).shouldExcludeFile(fileInfo, m_profilePath));
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_patternMatch() {
    sak::SmartFilter rules;
    rules.exclude_patterns = {".*\\.dat$"};
    rules.dangerous_files.clear();  // Don't catch .dat via dangerous list
    // Do NOT call initializeDefaults() -- it would overwrite our custom rules

    sak::SmartFileFilter filter(rules);
    // The pattern should catch .dat files
    QFile f(m_tempDir.filePath("test.dat"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("data");
    f.close();

    QFileInfo fileInfo(m_tempDir.filePath("test.dat"));
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
    // Prove the PATTERN arm is what caught it. dangerous_files.clear() does NOT empty the
    // dangerous set (the mandatory hive list is re-unioned by the ctor), so "the pattern
    // caught it" needs saying out loud rather than inferring it from a bare bool.
    QVERIFY(!filter.isDangerousFile("test.dat"));
    QCOMPARE(filter.getExclusionReason(fileInfo), QString("Matches exclusion pattern: test.dat"));
    // A sibling file the pattern does not match is kept.
    QVERIFY(!filter.shouldExcludeFile(QFileInfo(m_tempDir.filePath("normal.txt")), m_profilePath));
}

void SmartFileFilterTests::shouldExcludeFile_inCacheDir() {
    sak::SmartFileFilter filter;
    QString cachePath =
        m_tempDir.filePath("AppData/Local/Google/Chrome/User Data/Default/Cache/cached_file.bin");
    QDir().mkpath(QFileInfo(cachePath).absolutePath());
    QFile f(cachePath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("cached");
    f.close();

    QFileInfo fileInfo(cachePath);
    QVERIFY(filter.shouldExcludeFile(fileInfo, m_profilePath));
    // The default rules' exclude_folders list already contains "Cache", and shouldExcludeFile
    // consults that list (arm 4) BEFORE the cache-directory heuristic (arm 5) -- so the
    // assertion above holds even if isInCacheDirectory() were dead. Re-run with an empty
    // dangerous/pattern/folder rule set and the limit off, so the cache arm is the ONLY guard
    // that can fire.
    sak::SmartFilter cacheOnly;
    cacheOnly.dangerous_files.clear();
    cacheOnly.exclude_patterns.clear();
    cacheOnly.exclude_folders.clear();
    cacheOnly.enable_file_size_limit = false;
    sak::SmartFileFilter cacheFilter(cacheOnly);
    QVERIFY(cacheFilter.shouldExcludeFile(fileInfo, m_profilePath));
    QCOMPARE(cacheFilter.getExclusionReason(fileInfo), QString("Located in cache directory"));
}

void SmartFileFilterTests::shouldExcludeFile_normal() {
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

void SmartFileFilterTests::exclusionReason_dangerous() {
    sak::SmartFileFilter filter;
    QFileInfo fileInfo(m_tempDir.filePath("NTUSER.DAT"));
    QString reason = filter.getExclusionReason(fileInfo);
    QCOMPARE(reason, QString("Dangerous system file: NTUSER.DAT (would corrupt profile)"));
}

void SmartFileFilterTests::exclusionReason_normal() {
    sak::SmartFilter rules;
    rules.dangerous_files.clear();
    rules.exclude_patterns.clear();
    rules.exclude_folders.clear();
    rules.enable_file_size_limit = false;

    sak::SmartFileFilter filter(rules);
    QFileInfo fileInfo(m_tempDir.filePath("normal.txt"));
    QString reason = filter.getExclusionReason(fileInfo);
    // Nothing matches (no dangerous/size/pattern/cache branch), so it falls through to the
    // fixed fallback reason -- pin it so a mistakenly-taken branch is caught.
    QCOMPARE(reason, QString("Excluded by filter rules"));
}

// ============================================================================
// Rule Update Tests
// ============================================================================

void SmartFileFilterTests::setRules_updatesFiltering() {
    sak::SmartFileFilter filter;

    // A caller-supplied dangerous file is rule-controlled: present under default rules...
    sak::SmartFilter withCustom;
    withCustom.dangerous_files.append("my_custom_secret.dat");
    filter.setRules(withCustom);
    QVERIFY(filter.isDangerousFile("my_custom_secret.dat"));

    // ...and gone once the rules no longer list it. This proves setRules updates filtering.
    sak::SmartFilter cleared;
    cleared.dangerous_files.clear();
    filter.setRules(cleared);
    QVERIFY(!filter.isDangerousFile("my_custom_secret.dat"));

    // But the registry-hive protections are mandatory and survive ANY supplied rules,
    // including an empty set: a live NTUSER.DAT copied during a profile restore corrupts the
    // profile, so no caller may filter it back in as copyable. This is a fail-closed
    // invariant, not a rule the caller controls.
    QVERIFY(filter.isDangerousFile("NTUSER.DAT"));
    QVERIFY(filter.isDangerousFile("UsrClass.dat"));

    // setRules must rebuild the compiled-pattern set and the folder set too, not just the
    // dangerous list -- only the dangerous list was ever checked here, so a setRules that
    // refreshed m_dangerousFilesSet and left m_compiledPatterns / m_excludeFoldersSet stale
    // stayed green.
    sak::SmartFilter badPattern;
    badPattern.exclude_patterns = {"[unterminated"};
    filter.setRules(badPattern);
    QVERIFY(filter.hasInvalidPatterns());
    QCOMPARE(filter.invalidPatterns().size(), 1);

    sak::SmartFilter goodRules;
    goodRules.exclude_patterns = {".*\\.tmp$"};
    goodRules.exclude_folders = {"node_modules"};
    filter.setRules(goodRules);
    // The stale diagnostic is cleared on recompile, not accumulated.
    QVERIFY(!filter.hasInvalidPatterns());
    QVERIFY(filter.invalidPatterns().isEmpty());

    QDir(m_tempDir.path()).mkpath("node_modules");
    const QFileInfo nodeModules(m_tempDir.filePath("node_modules"));
    QVERIFY(filter.shouldExcludeFolder(nodeModules, m_profilePath));

    // ...and the folder set is CLEARED, not just unioned, when the new rules drop an entry.
    sak::SmartFilter noFolders;
    noFolders.exclude_folders.clear();
    filter.setRules(noFolders);
    QVERIFY(!filter.shouldExcludeFolder(nodeModules, m_profilePath));
}

QTEST_GUILESS_MAIN(SmartFileFilterTests)
#include "test_smart_file_filter.moc"
