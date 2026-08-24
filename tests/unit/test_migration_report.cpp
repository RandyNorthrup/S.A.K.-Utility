// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/migration_report.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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

    // -- Entry CRUD ------------------------------------------
    void testAddEntry();
    void testUpdateEntry();
    void testRemoveEntry();
    void testRemoveEntryInvalidIndex();
    void testGetEntryBoundsChecked();  // B7-22
    void testClear();

    // -- Selection -------------------------------------------
    void testSelectEntry();
    void testSelectAll();
    void testDeselectAll();
    void testSelectByConfidence();
    void testSelectByMatchType();

    // -- Counts & statistics ---------------------------------
    void testGetEntryCount();
    void testGetSelectedCount();
    void testGetMatchedCount();
    void testGetUnmatchedCount();
    void testGetMatchRateEmpty();
    void testGetMatchRatePartial();

    // -- Filtered views --------------------------------------
    void testGetSelectedEntries();
    void testGetUnmatchedEntries();
    void testGetMatchTypeDistribution();

    // -- Metadata --------------------------------------------
    void testInitialMetadata();

    // -- Export/import safety (P06-37/38/39/40) --------------
    void testHtmlEscapesMarkup();
    void testCsvNeutralizesFormula();
    void testImportRejectsInvalidPreservesEntries();
    void testImportRejectPreservesMetadata();  // B7-34
    void testExportSuccessReportsTrue();

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

    // On an EMPTY report "still 0 entries" is trivially true whatever the guard does. Repeat
    // against a POPULATED report, including the one-past-the-end index, so a dropped negative
    // check or a <= off-by-one visibly loses an entry.
    m_report.addEntry(makeEntry("A", "a", 1.0, "exact"));
    m_report.addEntry(makeEntry("B", "b", 0.5, "fuzzy"));
    m_report.removeEntry(-1);
    m_report.removeEntry(2);
    m_report.removeEntry(999);
    QCOMPARE(m_report.getEntryCount(), 2);
    QCOMPARE(m_report.getEntry(0).app_name, QStringLiteral("A"));
    QCOMPARE(m_report.getEntry(1).app_name, QStringLiteral("B"));
    QCOMPARE(m_report.getMetadata().total_apps, 2);
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
    m_report.selectEntry(1, true);
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
    // "Low" starts SELECTED so the else-arm (clear below threshold) is actually proven rather
    // than satisfied by MigrationEntry::selected defaulting to false.
    m_report.addEntry(makeEntry("Low", "l", 0.3, "fuzzy", true));
    m_report.addEntry(makeEntry("Mid", "m", 0.5, "fuzzy"));
    m_report.addEntry(makeEntry("High", "h", 0.9, "exact"));
    QCOMPARE(m_report.getSelectedCount(), 1);

    m_report.selectByConfidence(0.5);

    // Mid (0.5 -- the threshold is inclusive) and High (0.9) selected; Low (0.3) CLEARED:
    // selectByConfidence is exclusive, unlike the additive selectByMatchType.
    QVERIFY(!m_report.getEntry(0).selected);
    QVERIFY(m_report.getEntry(1).selected);
    QVERIFY(m_report.getEntry(2).selected);
    QCOMPARE(m_report.getSelectedCount(), 2);
}

void TestMigrationReport::testSelectByMatchType() {
    m_report.addEntry(makeEntry("E1", "e1", 1.0, "exact"));
    m_report.addEntry(makeEntry("F1", "f1", 0.5, "fuzzy"));
    m_report.addEntry(makeEntry("E2", "e2", 0.9, "exact"));
    // A pre-selected NON-matching entry: selectByMatchType is ADDITIVE -- it only ever sets
    // selected=true for the requested type and must never clear an existing selection.
    m_report.addEntry(makeEntry("F2", "f2", 0.4, "fuzzy", true));

    m_report.selectByMatchType("exact");

    QVERIFY(m_report.getEntry(0).selected);
    QVERIFY(!m_report.getEntry(1).selected);
    QVERIFY(m_report.getEntry(2).selected);
    QVERIFY(m_report.getEntry(3).selected);
    QCOMPARE(m_report.getSelectedCount(), 3);
    QCOMPARE(m_report.getMetadata().selected_apps, 3);
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

void TestMigrationReport::testGetMatchRateEmpty() {
    QCOMPARE(m_report.getMatchRate(), 0.0);
}

void TestMigrationReport::testGetMatchRatePartial() {
    m_report.addEntry(makeEntry("M1", "pkg", 1.0, "exact"));
    m_report.addEntry(makeEntry("U1", "", 0.0, "none"));

    const double rate = m_report.getMatchRate();
    // One matched (non-empty choco_package) + one unmatched = 1/2 = exactly 0.5.
    QCOMPARE(rate, 0.5);
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

// ============================================================================
// Export/import safety (P06-37/38/39/40)
// ============================================================================

// P06-38: HTML export must escape attacker-controlled markup so an app name
// like "<script>...</script>" cannot execute when the report is opened.
void TestMigrationReport::testHtmlEscapesMarkup() {
    auto e = makeEntry("<script>alert(1)</script>", "", 0.5, "exact");
    e.app_publisher = "<img src=x onerror=alert(2)>";
    m_report.addEntry(e);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("out.html");
    QVERIFY(m_report.exportToHtml(path));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString html = QString::fromUtf8(f.readAll());
    QVERIFY(!html.contains("<script>alert(1)</script>"));
    QVERIFY(html.contains("<td>&lt;script&gt;alert(1)&lt;/script&gt;</td>"));
    QVERIFY(!html.contains("<img src=x onerror"));
    QVERIFY(html.contains("<td>&lt;img src=x onerror=alert(2)&gt;</td>"));
}

// P06-39: CSV export must neutralize spreadsheet formula injection by prefixing
// a leading single quote to fields that start with a formula trigger.
void TestMigrationReport::testCsvNeutralizesFormula() {
    auto e = makeEntry("=HYPERLINK(\"http://x\",\"y\")", "", 0.5, "@cmd");
    // The trigger set is "=+-@\t\r"; only '=' and '@' were covered, so an implementation that
    // neutralized just those two stayed green. Drive one field per remaining trigger.
    e.app_version = QStringLiteral("+1+1");
    e.app_publisher = QStringLiteral("-2-2");
    e.notes = QStringLiteral("\tTAB");
    m_report.addEntry(e);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("out.csv");
    QVERIFY(m_report.exportToCsv(path));

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString csv = QString::fromUtf8(f.readAll());
    QVERIFY(csv.contains("\"'=HYPERLINK(\"\"http://x\"\",\"\"y\"\")\""));
    QVERIFY(csv.contains("\"'@cmd\""));
    QVERIFY(!csv.contains(",=HYPERLINK"));
    // The remaining triggers: '+', '-' and a leading TAB must be neutralized the same way.
    QVERIFY(csv.contains("\"'+1+1\""));
    QVERIFY(csv.contains("\"'-2-2\""));
    QVERIFY(csv.contains("\"'\tTAB\""));
}

// P06-37: a malformed import must be rejected without wiping the current report.
void TestMigrationReport::testImportRejectsInvalidPreservesEntries() {
    m_report.addEntry(makeEntry("Keep1", "k1", 0.9, "exact"));
    m_report.addEntry(makeEntry("Keep2", "k2", 0.8, "fuzzy"));
    QCOMPARE(m_report.getEntryCount(), 2);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString empty = QDir(dir.path()).filePath("empty.json");
    {
        QFile f(empty);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write("{}"), 2);
    }
    QVERIFY(!m_report.importFromJson(empty));
    QCOMPARE(m_report.getEntryCount(), 2);

    const QString bad = QDir(dir.path()).filePath("bad.json");
    {
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray payload = "{\"entries\": 5}";
        QCOMPARE(f.write(payload), payload.size());
    }
    QVERIFY(!m_report.importFromJson(bad));
    QCOMPARE(m_report.getEntryCount(), 2);

    // A well-formed array holding a NON-object element hits a DIFFERENT guard, and must be
    // refused without half-committing the entries parsed before it.
    const QString nonobj = QDir(dir.path()).filePath("nonobject.json");
    {
        QFile f(nonobj);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray payload = "{\"entries\": [{\"app_name\": \"Ghost\"}, 5]}";
        QCOMPARE(f.write(payload), payload.size());
    }
    QVERIFY(!m_report.importFromJson(nonobj));
    QCOMPARE(m_report.getEntryCount(), 2);
    QCOMPARE(m_report.getEntry(0).app_name, QStringLiteral("Keep1"));
    QCOMPARE(m_report.getEntry(1).app_name, QStringLiteral("Keep2"));

    // Positive control: without it every refusal above is satisfied by an importFromJson()
    // that unconditionally returns false. Hand-written JSON (NOT exportToJson output) so the
    // key names are pinned independently of the writer.
    const QString good = QDir(dir.path()).filePath("good.json");
    {
        QFile f(good);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray payload =
            "{\"entries\": [{\"app_name\": \"Imported\", \"choco_package\": \"imp\", "
            "\"confidence\": 0.75, \"match_type\": \"exact\", \"selected\": true}]}";
        QCOMPARE(f.write(payload), payload.size());
    }
    QVERIFY(m_report.importFromJson(good));
    QCOMPARE(m_report.getEntryCount(), 1);
    QCOMPARE(m_report.getEntry(0).app_name, QStringLiteral("Imported"));
    QCOMPARE(m_report.getEntry(0).choco_package, QStringLiteral("imp"));
    QCOMPARE(m_report.getEntry(0).confidence, 0.75);
    QCOMPARE(m_report.getEntry(0).match_type, QStringLiteral("exact"));
    QVERIFY(m_report.getEntry(0).selected);
    // Counts are recomputed from the parsed entries, never taken from the file header.
    QCOMPARE(m_report.getMetadata().total_apps, 1);
    QCOMPARE(m_report.getMetadata().matched_apps, 1);
    QCOMPARE(m_report.getMetadata().selected_apps, 1);
    QCOMPARE(m_report.getMetadata().match_rate, 1.0);
}

void TestMigrationReport::testImportRejectPreservesMetadata() {
    // A rejected import must not half-apply: the metadata carried in an otherwise
    // invalid file (no entries array) must NOT overwrite the current metadata (B7-34).
    m_report.getMetadata().source_machine = QStringLiteral("ORIGINAL-PC");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("meta_no_entries.json");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // Valid metadata but a missing "entries" array -> the import must be rejected.
        const QByteArray payload = "{\"metadata\": {\"source_machine\": \"EVIL-PC\"}}";
        QCOMPARE(f.write(payload), payload.size());
    }

    QVERIFY(!m_report.importFromJson(path));
    QCOMPARE(m_report.getMetadata().source_machine, QStringLiteral("ORIGINAL-PC"));
}

// P06-40: a successful export returns true and writes a non-empty file.
void TestMigrationReport::testExportSuccessReportsTrue() {
    m_report.addEntry(makeEntry("App1", "app1", 0.9, "exact"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString js = QDir(dir.path()).filePath("r.json");
    const QString cs = QDir(dir.path()).filePath("r.csv");
    const QString ht = QDir(dir.path()).filePath("r.html");
    QVERIFY(m_report.exportToJson(js));
    QVERIFY(m_report.exportToCsv(cs));
    QVERIFY(m_report.exportToHtml(ht));
    QVERIFY(QFileInfo(js).size() > 0);
    QVERIFY(QFileInfo(ht).size() > 0);
    // A size floor passes for a header-only or column-scrambled writer. The CSV for this fixture
    // is fully determined, so pin it byte for byte (EOLs normalized because the writer emits
    // platform line endings).
    QFile csf(cs);
    QVERIFY(csf.open(QIODevice::ReadOnly));
    QString csv_text = QString::fromUtf8(csf.readAll());
    csv_text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QCOMPARE(csv_text,
             QStringLiteral("App Name,Version,Publisher,Install Location,Install Date,"
                            "Chocolatey Package,Confidence,Match Type,Available,"
                            "Available Version,Selected,Version Lock,Locked Version,Notes,"
                            "Status,Error Message\n"
                            "App1,1.0,TestPublisher,,,app1,0.90,exact,Yes,,No,No,,,pending,\n"));
}

QTEST_MAIN(TestMigrationReport)
#include "test_migration_report.moc"
