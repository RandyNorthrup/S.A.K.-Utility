// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/migration_report.h"

#include <QTest>

using namespace sak;

/**
 * @brief Unit tests for MigrationReport: its entries and bounds-checked access.
 */
class TestMigrationReport : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    // -- Entry CRUD ------------------------------------------
    void testAddEntry();
    void testGetEntryBoundsChecked();  // B7-22

private:
    MigrationReport m_report;

    static MigrationReport::MigrationEntry makeEntry(const QString& name,
                                                     const QString& choco,
                                                     double confidence,
                                                     const QString& matchType);
};

void TestMigrationReport::init() {
    m_report = sak::MigrationReport{};
}

MigrationReport::MigrationEntry TestMigrationReport::makeEntry(const QString& name,
                                                               const QString& choco,
                                                               double confidence,
                                                               const QString& matchType) {
    MigrationReport::MigrationEntry e;
    e.app_name = name;
    e.app_version = "1.0";
    e.app_publisher = "TestPublisher";
    e.choco_package = choco;
    e.confidence = confidence;
    e.match_type = matchType;
    e.available = !choco.isEmpty();
    e.status = "pending";
    return e;
}

// ============================================================================
// Entry CRUD
// ============================================================================

void TestMigrationReport::testAddEntry() {
    QVERIFY(m_report.getEntries().empty());
    m_report.addEntry(makeEntry("App1", "app1", 0.9, "exact"));
    QCOMPARE(m_report.getEntries().size(), size_t(1));
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

QTEST_MAIN(TestMigrationReport)
#include "test_migration_report.moc"
