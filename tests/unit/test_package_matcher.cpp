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

    // Fuzzy matching internal â€” via findMatch
    void findMatch_exactMapping();
    void findMatch_noResult();

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
    // Should have some built-in mappings
    QVERIFY(matcher.getMappingCount() > 0);
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

    original.exportMappings(exportPath);
    QVERIFY(QFile::exists(exportPath));

    sak::PackageMatcher imported;
    imported.importMappings(exportPath);

    QVERIFY(imported.hasMapping("UniqueTestApp"));
    QCOMPARE(imported.getMapping("UniqueTestApp"), QString("unique-test-pkg"));
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
    QVERIFY(result->confidence > 0.9);
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
// Match Config
// ============================================================================

void PackageMatcherTests::matchConfig_defaults() {
    sak::PackageMatcher::MatchConfig cfg;
    QVERIFY(cfg.use_exact_mappings);
    QVERIFY(cfg.use_fuzzy_matching);
    QVERIFY(cfg.use_choco_search);
    QVERIFY(cfg.min_confidence >= 0.0 && cfg.min_confidence <= 1.0);
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
