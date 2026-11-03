// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/migration_report.h"

#include <QTest>

using namespace sak;

/**
 * @brief Unit tests for MigrationReport: its entries, bounds-checked access and counts.
 */
class TestMigrationReport : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    // -- Entry CRUD ------------------------------------------
    void testAddEntry();
    void testGetEntryBoundsChecked();  // B7-22
    void testClear();

    // -- Selection -------------------------------------------
    void testSelectAll();

    // -- Counts & statistics ---------------------------------
    void testGetEntryCount();
    void testGetSelectedCount();
    void testGetMatchedCount();
    void testGetUnmatchedCount();

    // -- Metadata --------------------------------------------
    void testInitialMetadata();

private:
    MigrationReport m_report;

    static MigrationReport::MigrationEntry makeEntry(const QString& name,
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

// B7-22: getEntry() must be bounds-checked -- std::vector::operator[] on a bad
// index is UB. An out-of-range/negative index returns a default entry, not a
// read/write past the vector.
void TestMigrationReport::testGetEntryBoundsChecked() {
    m_report.addEntry(makeEntry("Real", "real", 0.9, "exact"));

    // Valid index returns the real entry.
    QCOMPARE(m_report.getEntry(0).app_name, QStringLiteral("Real"));

    // Out-of-range and negative indices return an empty default -- no crash, no OOB.
    QVERIFY(m_report.getEntry(1).app_name.isEmpty());
    QVERIFY(m_report.getEntry(999).app_name.isEmpty());
    QVERIFY(m_report.getEntry(-1).app_name.isEmpty());

    // A stray write through the non-const OOB overload must not corrupt real entries.
    m_report.getEntry(999).app_name = QStringLiteral("SHOULD-NOT-STICK");
    QCOMPARE(m_report.getEntry(0).app_name, QStringLiteral("Real"));
    QVERIFY(m_report.getEntry(999).app_name.isEmpty());  // scratch reset on each OOB access
}

void TestMigrationReport::testClear() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact", true));
    m_report.addEntry(makeEntry("B", "b", 0.5, "fuzzy"));
    m_report.selectAll();
    // Dirty every cached header statistic so clear() has something to reset; otherwise the
    // post-conditions below are satisfied by their pre-existing zero values.
    m_report.getMetadata().matched_apps = 2;
    m_report.getMetadata().match_rate = 1.0;
    QCOMPARE(m_report.getMetadata().total_apps, 2);
    QCOMPARE(m_report.getMetadata().selected_apps, 2);

    m_report.clear();
    QCOMPARE(m_report.getEntryCount(), 0);
    // clear() must zero the cached header statistics too, not just the vector -- a stale
    // total_apps/selected_apps is what the exported report header would carry.
    QCOMPARE(m_report.getMetadata().total_apps, 0);
    QCOMPARE(m_report.getMetadata().selected_apps, 0);
    QCOMPARE(m_report.getMetadata().matched_apps, 0);
    QCOMPARE(m_report.getSelectedCount(), 0);
}

// ============================================================================
// Selection logic
// ============================================================================

void TestMigrationReport::testSelectAll() {
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact", false));
    m_report.addEntry(makeEntry("B", "b", 0.5, "fuzzy", false));

    m_report.selectAll();
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
    // De-confound the fixture: makeEntry ties both `available` and `match_type` to whether a
    // package was given, so counting either of those also yields 1 above. This entry HAS a
    // package while being unavailable and typed "none" -- matched is keyed only on a
    // non-empty choco_package.
    auto stale = makeEntry("MatchedUnavailable", "pkg2", 0.0, "none");
    stale.available = false;
    m_report.addEntry(stale);
    QCOMPARE(m_report.getMatchedCount(), 2);
    QCOMPARE(m_report.getUnmatchedCount(), 1);
}

void TestMigrationReport::testGetUnmatchedCount() {
    m_report.addEntry(makeEntry("M", "pkg", 1.0, "exact"));
    m_report.addEntry(makeEntry("U1", "", 0.0, "none"));
    m_report.addEntry(makeEntry("U2", "", 0.0, "none"));
    QCOMPARE(m_report.getUnmatchedCount(), 2);
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
