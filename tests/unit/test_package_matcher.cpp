// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_package_matcher.cpp
/// @brief Unit tests for package name matching and fuzzy search

#include "sak/package_matcher.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class PackageMatcherTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Constructor
    void constructor_initializesCommonMappings();

    // Exact matching
    void addMapping_canRetrieve();
    void removeMapping_noLongerFound();
    void hasMapping_existingKey();
    void hasMapping_nonExistentKey();
    void getMappingCount_afterAdd();

    // Mapping export/import
    void exportImport_roundTrip();
    void importMappings_missingFileReturnsFalse();

    // Fuzzy matching internal -- via findMatch
    void findMatch_exactMapping();
    void findMatch_noResult();

    // Fuzzy internals via the cache-seed seam (uninitialized ChocolateyManager)
    void fuzzyMatch_similarityAtThresholdMatches();
    void fuzzyMatch_bestSelectTieKeepsFirst();

    // Match config
    void matchConfig_defaults();
    void matchConfig_exactOnly();

    // Stats
    void stats_initiallyZero();

private:
    sak::AppScanner::AppInfo makeAppInfo(const QString& name) const;
};

sak::AppScanner::AppInfo PackageMatcherTests::makeAppInfo(const QString& name) const {
    sak::AppScanner::AppInfo info;
    info.name = name;
    return info;
}

// ============================================================================
// Constructor
// ============================================================================

void PackageMatcherTests::constructor_initializesCommonMappings() {
    sak::PackageMatcher matcher;
    // initializeCommonMappings seeds an exact table of 42 distinct app->package keys.
    QCOMPARE(matcher.getMappingCount(), 42);
}

// ============================================================================
// Mapping Management
// ============================================================================

void PackageMatcherTests::addMapping_canRetrieve() {
    sak::PackageMatcher matcher;
    matcher.addMapping("Test App", "test-app-choco");
    QVERIFY(matcher.hasMapping("Test App"));
    QCOMPARE(matcher.getMapping("Test App"), QString("test-app-choco"));
}

void PackageMatcherTests::removeMapping_noLongerFound() {
    sak::PackageMatcher matcher;
    matcher.addMapping("ToRemove", "remove-pkg");
    QVERIFY(matcher.hasMapping("ToRemove"));

    matcher.removeMapping("ToRemove");
    QVERIFY(!matcher.hasMapping("ToRemove"));
}

void PackageMatcherTests::hasMapping_existingKey() {
    sak::PackageMatcher matcher;
    matcher.addMapping("ExistingApp", "existing-pkg");
    QVERIFY(matcher.hasMapping("ExistingApp"));
}

void PackageMatcherTests::hasMapping_nonExistentKey() {
    sak::PackageMatcher matcher;
    QVERIFY(!matcher.hasMapping("NonExistentApp_xyz_12345"));
}

void PackageMatcherTests::getMappingCount_afterAdd() {
    sak::PackageMatcher matcher;
    int initial = matcher.getMappingCount();

    matcher.addMapping("NewApp1", "new-app-1");
    matcher.addMapping("NewApp2", "new-app-2");

    QCOMPARE(matcher.getMappingCount(), initial + 2);
}

// ============================================================================
// Export / Import
// ============================================================================

void PackageMatcherTests::exportImport_roundTrip() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString exportPath = tempDir.filePath("mappings.json");

    sak::PackageMatcher original;
    original.addMapping("UniqueTestApp", "unique-test-pkg");
    [[maybe_unused]] int originalCount = original.getMappingCount();

    // export/import now report success/failure so callers can detect I/O errors.
    QVERIFY(original.exportMappings(exportPath));
    QVERIFY(QFile::exists(exportPath));

    sak::PackageMatcher imported;
    QVERIFY(imported.importMappings(exportPath));

    QVERIFY(imported.hasMapping("UniqueTestApp"));
    QCOMPARE(imported.getMapping("UniqueTestApp"), QString("unique-test-pkg"));
}

void PackageMatcherTests::importMappings_missingFileReturnsFalse() {
    // A failed open must surface as false (not a silent void), so the caller knows the import did
    // not happen rather than assuming success.
    sak::PackageMatcher matcher;
    QVERIFY(
        !matcher.importMappings(QStringLiteral("Z:/no-such-dir/absent-mappings-xyz-98765.json")));
}

// ============================================================================
// findMatch Tests
// ============================================================================

void PackageMatcherTests::findMatch_exactMapping() {
    sak::PackageMatcher matcher;
    matcher.addMapping("My Test Application", "my-test-app");

    sak::PackageMatcher::MatchConfig cfg;
    cfg.use_exact_mappings = true;
    cfg.use_fuzzy_matching = false;
    cfg.use_choco_search = false;

    auto info = makeAppInfo("My Test Application");
    auto result = matcher.findMatch(info, nullptr, cfg);

    QVERIFY(result.has_value());
    QCOMPARE(result->choco_package, QString("my-test-app"));
    QCOMPARE(result->match_type, QString("exact"));
    QCOMPARE(result->matched_name, QString("My Test"));
    QVERIFY(result->available);
    // Direct exact-mapping hit returns confidence 1.0 (not the 0.95 case-insensitive path).
    QCOMPARE(result->confidence, 1.0);
}

void PackageMatcherTests::findMatch_noResult() {
    sak::PackageMatcher matcher;

    sak::PackageMatcher::MatchConfig cfg;
    cfg.use_exact_mappings = true;
    cfg.use_fuzzy_matching = false;
    cfg.use_choco_search = false;

    auto info = makeAppInfo("CompletelyUnknownApp_xyz_99999");
    auto result = matcher.findMatch(info, nullptr, cfg);

    QVERIFY(!result.has_value());
}

// ============================================================================
// Fuzzy internals (cache-seed seam)
// ============================================================================
//
// fuzzyMatch() is only reachable with a non-null ChocolateyManager, and its
// scoring path was previously never exercised because every other test passes
// choco_mgr=nullptr and disables fuzzy matching. fetchSearchOutput() consults
// the in-object search cache BEFORE it ever calls searchPackage() (whose only
// m_initialized gate lives), so seeding the cache lets these tests drive the
// real parse+score path with an UNINITIALIZED manager -- searchPackage() is
// never reached. The friend seam grants access to cacheSearch()/fuzzyMatch().

void PackageMatcherTests::fuzzyMatch_similarityAtThresholdMatches() {
    // A candidate whose similarity to the query lands EXACTLY on the fuzzy
    // threshold (0.6) must be accepted. calculateSimilarity("abcd","axyd") is
    // exactly 0.6: jaro-winkler("abcd","axyd") == 0.7 (jaro 2/3, one-char common
    // prefix) and levenshtein-sim == 0.5 (2 subs over length 4), averaged to
    // (0.7 + 0.5)/2 == 0.6. The real ">=" admits it; a ">" boundary-tighten
    // would reject it and return nullopt.
    sak::PackageMatcher matcher;
    sak::ChocolateyManager choco;  // uninitialized on purpose

    matcher.cacheSearch("abcd", QStringLiteral("axyd|1.0.0\n"));

    auto result = matcher.fuzzyMatch("abcd", &choco);

    QVERIFY(result.has_value());  // kills the ">=" -> ">" boundary tighten
    QCOMPARE(result->choco_package, QString("axyd"));
    QCOMPARE(result->matched_name, QString("axyd"));
    QCOMPARE(result->match_type, QString("fuzzy"));
    QVERIFY(qFuzzyCompare(result->confidence, 0.6));
}

void PackageMatcherTests::fuzzyMatch_bestSelectTieKeepsFirst() {
    // Two candidates that TIE on similarity: both contain the query "abcd", so
    // each scores exactly kContainedNameSimilarity (0.85). The real "<=" keeps
    // the FIRST candidate on a tie (a later equal score is skipped); the "<"
    // tie-flip mutant lets the second overwrite it. Order is the crafted line
    // order, so the winner distinguishes the two comparators.
    sak::PackageMatcher matcher;
    sak::ChocolateyManager choco;  // uninitialized on purpose

    matcher.cacheSearch("abcd", QStringLiteral("abcde|1.0.0\nabcdz|2.0.0\n"));

    auto result = matcher.fuzzyMatch("abcd", &choco);

    QVERIFY(result.has_value());
    // Real comparator keeps "abcde" (first); the "<" mutant would pick "abcdz".
    QCOMPARE(result->choco_package, QString("abcde"));
    QCOMPARE(result->match_type, QString("fuzzy"));
    QCOMPARE(result->matched_name, QString("abcde"));
    QVERIFY(qFuzzyCompare(result->confidence, 0.85));
}

// ============================================================================
// Match Config
// ============================================================================

void PackageMatcherTests::matchConfig_defaults() {
    sak::PackageMatcher::MatchConfig cfg;
    QVERIFY(cfg.use_exact_mappings);
    QVERIFY(cfg.use_fuzzy_matching);
    QVERIFY(cfg.use_choco_search);
    QCOMPARE(cfg.min_confidence, sak::kPackageMatcherDefaultMinConfidence);
}

void PackageMatcherTests::matchConfig_exactOnly() {
    sak::PackageMatcher::MatchConfig cfg;
    cfg.use_exact_mappings = true;
    cfg.use_fuzzy_matching = false;
    cfg.use_choco_search = false;

    QVERIFY(cfg.use_exact_mappings);
    QVERIFY(!cfg.use_fuzzy_matching);
    QVERIFY(!cfg.use_choco_search);
}

// ============================================================================
// Stats
// ============================================================================

void PackageMatcherTests::stats_initiallyZero() {
    sak::PackageMatcher matcher;
    QCOMPARE(matcher.getExactMatchCount(), 0);
    QCOMPARE(matcher.getFuzzyMatchCount(), 0);
    QCOMPARE(matcher.getSearchMatchCount(), 0);
}

QTEST_GUILESS_MAIN(PackageMatcherTests)
#include "test_package_matcher.moc"
