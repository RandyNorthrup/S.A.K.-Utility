// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/migration_report.h"

#include <QTest>

using namespace sak;

/**
 * @brief Unit tests for MigrationReport.
 *
 * Covers entry management, selection logic, counting,
 * match-rate calculation, and distribution histograms.
 * Tests file export/import are skipped (require I/O);
 * the pure data logic is fully exercised here.
 */
class TestMigrationReport : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    // ── Entry CRUD ──────────────────────────────────────────
    void testAddEntry();
    void testUpdateEntry();
    void testRemoveEntry();
    void testRemoveEntryInvalidIndex();
    void testClear();

    // ── Selection ───────────────────────────────────────────
    void testSelectEntry();
    void testSelectAll();
    void testDeselectAll();
    void testSelectByConfidence();
    void testSelectByMatchType();

    // ── Counts & statistics ─────────────────────────────────
    void testGetEntryCount();
    void testGetSelectedCount();
    void testGetMatchedCount();
    void testGetUnmatchedCount();
    void testGetMatchRateEmpty();
    void testGetMatchRatePartial();

    // ── Filtered views ──────────────────────────────────────
    void testGetSelectedEntries();
    void testGetUnmatchedEntries();
    void testGetMatchTypeDistribution();

    // ── Metadata ────────────────────────────────────────────
    void testInitialMetadata();

private:
    MigrationReport m_report;

    MigrationReport::MigrationEntry makeEntry(const QString& name,
                                              const QString& choco,
                                              double confidence,
                                              const QString& matchType,
                                              bool selected = false);
};

void TestMigrationReport::init() {
    m_report.clear();
}

MigrationReport::MigrationEntry TestMigrationReport::makeEntry(const QString& name,
                                                               const QString& choco,
                                                               double confidence,
                                                               const QString& matchType,
                                                               bool selected) {
    MigrationReport::MigrationEntry e;
    e.app_name = name;
    e.app_version = "1.0";
    e.app_publisher = "TestPublisher";
    e.choco_package = choco;
    e.confidence = confidence;
    e.match_type = matchType;
    e.available = !choco.isEmpty();
    e.selected = selected;
    e.status = "pending";
    return e;
}

// ============================================================================
// Entry CRUD
// ============================================================================

void TestMigrationReport::testAddEntry() {
    QCOMPARE(m_report.getEntryCount(), 0);
    m_report.addEntry(makeEntry("App1", "app1", 0.9, "exact"));
    QCOMPARE(m_report.getEntryCount(), 1);
    QCOMPARE(m_report.getEntry(0).app_name, "App1");
}

void TestMigrationReport::testUpdateEntry() {
    m_report.addEntry(makeEntry("Old", "old-pkg", 0.5, "fuzzy"));

    auto updated = makeEntry("New", "new-pkg", 0.95, "exact");
    m_report.updateEntry(0, updated);

    QCOMPARE(m_report.getEntry(0).app_name, "New");
    QCOMPARE(m_report.getEntry(0).choco_package, "new-pkg");
}

void TestMigrationReport::testRemoveEntry() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact"));
    m_report.addEntry(makeEntry("B", "b", 1.0, "exact"));

    QCOMPARE(m_report.getEntryCount(), 2);
    m_report.removeEntry(0);
    QCOMPARE(m_report.getEntryCount(), 1);
    QCOMPARE(m_report.getEntry(0).app_name, "B");
}

void TestMigrationReport::testRemoveEntryInvalidIndex() {
    // Should not crash on out-of-bounds
    m_report.removeEntry(-1);
    m_report.removeEntry(999);
    QCOMPARE(m_report.getEntryCount(), 0);
}

void TestMigrationReport::testClear() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact"));
    m_report.addEntry(makeEntry("B", "b", 0.5, "fuzzy"));
    m_report.clear();
    QCOMPARE(m_report.getEntryCount(), 0);
}

// ============================================================================
// Selection logic
// ============================================================================

void TestMigrationReport::testSelectEntry() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact", false));
    QCOMPARE(m_report.getSelectedCount(), 0);

    m_report.selectEntry(0, true);
    QCOMPARE(m_report.getSelectedCount(), 1);

    m_report.selectEntry(0, false);
    QCOMPARE(m_report.getSelectedCount(), 0);
}

void TestMigrationReport::testSelectAll() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact", false));
    m_report.addEntry(makeEntry("B", "b", 0.5, "fuzzy", false));

    m_report.selectAll();
    QCOMPARE(m_report.getSelectedCount(), 2);
}

void TestMigrationReport::testDeselectAll() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact", true));
    m_report.addEntry(makeEntry("B", "b", 0.5, "fuzzy", true));

    m_report.deselectAll();
    QCOMPARE(m_report.getSelectedCount(), 0);
}

void TestMigrationReport::testSelectByConfidence() {
    m_report.addEntry(makeEntry("Low", "l", 0.3, "fuzzy"));
    m_report.addEntry(makeEntry("Mid", "m", 0.5, "fuzzy"));
    m_report.addEntry(makeEntry("High", "h", 0.9, "exact"));

    m_report.selectByConfidence(0.5);

    // Mid (0.5) and High (0.9) selected, Low (0.3) not
    QVERIFY(!m_report.getEntry(0).selected);
    QVERIFY(m_report.getEntry(1).selected);
    QVERIFY(m_report.getEntry(2).selected);
    QCOMPARE(m_report.getSelectedCount(), 2);
}

void TestMigrationReport::testSelectByMatchType() {
    m_report.addEntry(makeEntry("E1", "e1", 1.0, "exact"));
    m_report.addEntry(makeEntry("F1", "f1", 0.5, "fuzzy"));
    m_report.addEntry(makeEntry("E2", "e2", 0.9, "exact"));

    m_report.selectByMatchType("exact");

    QVERIFY(m_report.getEntry(0).selected);
    QVERIFY(!m_report.getEntry(1).selected);
    QVERIFY(m_report.getEntry(2).selected);
    QCOMPARE(m_report.getSelectedCount(), 2);
}

// ============================================================================
// Counts & statistics
// ============================================================================

void TestMigrationReport::testGetEntryCount() {
    QCOMPARE(m_report.getEntryCount(), 0);
    m_report.addEntry(makeEntry("X", "x", 1.0, "exact"));
    QCOMPARE(m_report.getEntryCount(), 1);
}

void TestMigrationReport::testGetSelectedCount() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact", true));
    m_report.addEntry(makeEntry("B", "", 0.0, "none", false));
    QCOMPARE(m_report.getSelectedCount(), 1);
}

void TestMigrationReport::testGetMatchedCount() {
    m_report.addEntry(makeEntry("Matched", "pkg", 1.0, "exact"));
    m_report.addEntry(makeEntry("Unmatched", "", 0.0, "none"));
    QCOMPARE(m_report.getMatchedCount(), 1);
}

void TestMigrationReport::testGetUnmatchedCount() {
    m_report.addEntry(makeEntry("M", "pkg", 1.0, "exact"));
    m_report.addEntry(makeEntry("U1", "", 0.0, "none"));
    m_report.addEntry(makeEntry("U2", "", 0.0, "none"));
    QCOMPARE(m_report.getUnmatchedCount(), 2);
}

void TestMigrationReport::testGetMatchRateEmpty() {
    QCOMPARE(m_report.getMatchRate(), 0.0);
}

void TestMigrationReport::testGetMatchRatePartial() {
    m_report.addEntry(makeEntry("M1", "pkg", 1.0, "exact"));
    m_report.addEntry(makeEntry("U1", "", 0.0, "none"));

    const double rate = m_report.getMatchRate();
    QVERIFY(rate > 0.49 && rate < 0.51);  // ~50%
}

// ============================================================================
// Filtered views
// ============================================================================

void TestMigrationReport::testGetSelectedEntries() {
    m_report.addEntry(makeEntry("S1", "s1", 1.0, "exact", true));
    m_report.addEntry(makeEntry("N1", "", 0.0, "none", false));
    m_report.addEntry(makeEntry("S2", "s2", 0.8, "exact", true));

    auto selected = m_report.getSelectedEntries();
    QCOMPARE(static_cast<int>(selected.size()), 2);
    QCOMPARE(selected[0].app_name, "S1");
    QCOMPARE(selected[1].app_name, "S2");
}

void TestMigrationReport::testGetUnmatchedEntries() {
    m_report.addEntry(makeEntry("M", "pkg", 1.0, "exact"));
    m_report.addEntry(makeEntry("U", "", 0.0, "none"));

    auto unmatched = m_report.getUnmatchedEntries();
    QCOMPARE(static_cast<int>(unmatched.size()), 1);
    QCOMPARE(unmatched[0].app_name, "U");
}

void TestMigrationReport::testGetMatchTypeDistribution() {
    m_report.addEntry(makeEntry("E1", "e1", 1.0, "exact"));
    m_report.addEntry(makeEntry("E2", "e2", 0.9, "exact"));
    m_report.addEntry(makeEntry("F1", "f1", 0.5, "fuzzy"));
    m_report.addEntry(makeEntry("N1", "", 0.0, "none"));

    auto dist = m_report.getMatchTypeDistribution();
    QCOMPARE(dist.value("exact"), 2);
    QCOMPARE(dist.value("fuzzy"), 1);
    QCOMPARE(dist.value("none"), 1);
}

// ============================================================================
// Metadata
// ============================================================================

void TestMigrationReport::testInitialMetadata() {
    const auto& meta = m_report.getMetadata();
    QCOMPARE(meta.report_version, "1.0");
    QVERIFY(!meta.source_os.isEmpty());
}

QTEST_MAIN(TestMigrationReport)
#include "test_migration_report.moc"
