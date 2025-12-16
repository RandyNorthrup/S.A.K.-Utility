// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for MigrationReport
 * Tests report export/import and data persistence
 */

#include "sak/migration_report.h"
#include "sak/app_scanner.h"
#include "sak/package_matcher.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

class TestMigrationReport : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void testCreateReport() {
        sak::MigrationReport report;
        
        report.setSourceComputer("SourcePC");
        report.setTargetComputer("TargetPC");
        
        QCOMPARE(report.getSourceComputer(), QString("SourcePC"));
        QCOMPARE(report.getTargetComputer(), QString("TargetPC"));
    }

    void testAddEntry() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "7-Zip";
        entry.app_version = "22.01";
        entry.app_publisher = "Igor Pavlov";
        entry.package_id = "7zip";
        entry.package_version = "22.01";
        entry.confidence = 0.95;
        entry.selected = true;
        
        report.addEntry(entry);
        
        auto entries = report.getEntries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].app_name, QString("7-Zip"));
        QCOMPARE(entries[0].package_id, QString("7zip"));
    }

    void testExportJson() {
        sak::MigrationReport report;
        report.setSourceComputer("PC1");
        report.setTargetComputer("PC2");
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Test App";
        entry.package_id = "testapp";
        entry.confidence = 0.8;
        entry.selected = true;
        report.addEntry(entry);
        
        QString jsonPath = tempDir->path() + "/test.json";
        QVERIFY(report.exportToJson(jsonPath));
        QVERIFY(QFile::exists(jsonPath));
    }

    void testImportJson() {
        // First export
        sak::MigrationReport report1;
        report1.setSourceComputer("PC1");
        report1.setTargetComputer("PC2");
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Test App";
        entry.package_id = "testapp";
        entry.confidence = 0.85;
        entry.selected = true;
        report1.addEntry(entry);
        
        QString jsonPath = tempDir->path() + "/import_test.json";
        QVERIFY(report1.exportToJson(jsonPath));
        
        // Then import
        sak::MigrationReport report2;
        QVERIFY(report2.importFromJson(jsonPath));
        
        QCOMPARE(report2.getSourceComputer(), QString("PC1"));
        QCOMPARE(report2.getTargetComputer(), QString("PC2"));
        
        auto entries = report2.getEntries();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries[0].app_name, QString("Test App"));
        QCOMPARE(entries[0].package_id, QString("testapp"));
        QCOMPARE(entries[0].confidence, 0.85);
    }

    void testExportCsv() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.app_name = "App 1";
        entry1.package_id = "app1";
        entry1.confidence = 0.9;
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.app_name = "App 2";
        entry2.package_id = "app2";
        entry2.confidence = 0.75;
        report.addEntry(entry2);
        
        QString csvPath = tempDir->path() + "/test.csv";
        QVERIFY(report.exportToCsv(csvPath));
        QVERIFY(QFile::exists(csvPath));
        
        // Verify CSV content
        QFile file(csvPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QVERIFY(content.contains("App 1"));
        QVERIFY(content.contains("App 2"));
        QVERIFY(content.contains("app1"));
        QVERIFY(content.contains("app2"));
    }

    void testExportHtml() {
        sak::MigrationReport report;
        report.setSourceComputer("OldPC");
        report.setTargetComputer("NewPC");
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Sample App";
        entry.package_id = "sampleapp";
        entry.confidence = 0.88;
        report.addEntry(entry);
        
        QString htmlPath = tempDir->path() + "/test.html";
        QVERIFY(report.exportToHtml(htmlPath));
        QVERIFY(QFile::exists(htmlPath));
        
        // Verify HTML content
        QFile file(htmlPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        
        QVERIFY(content.contains("<html"));
        QVERIFY(content.contains("Sample App"));
        QVERIFY(content.contains("sampleapp"));
    }

    void testGetSelectedCount() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.app_name = "App 1";
        entry1.selected = true;
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.app_name = "App 2";
        entry2.selected = false;
        report.addEntry(entry2);
        
        sak::MigrationReport::MigrationEntry entry3;
        entry3.app_name = "App 3";
        entry3.selected = true;
        report.addEntry(entry3);
        
        QCOMPARE(report.getSelectedCount(), 2);
    }

    void testGetMatchedCount() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.app_name = "App 1";
        entry1.package_id = "app1";
        entry1.confidence = 0.8;
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.app_name = "App 2";
        entry2.package_id = "";  // No match
        entry2.confidence = 0.0;
        report.addEntry(entry2);
        
        QCOMPARE(report.getMatchedCount(), 1);
    }

    void testGetUnmatchedCount() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.app_name = "App 1";
        entry1.package_id = "app1";
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.app_name = "App 2";
        entry2.package_id = "";
        report.addEntry(entry2);
        
        sak::MigrationReport::MigrationEntry entry3;
        entry3.app_name = "App 3";
        entry3.package_id = "";
        report.addEntry(entry3);
        
        QCOMPARE(report.getUnmatchedCount(), 2);
    }

    void testGetMatchRate() {
        sak::MigrationReport report;
        
        for (int i = 0; i < 8; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.package_id = QString("app%1").arg(i);
            report.addEntry(entry);
        }
        
        for (int i = 8; i < 10; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.package_id = "";
            report.addEntry(entry);
        }
        
        double matchRate = report.getMatchRate();
        QCOMPARE(matchRate, 0.8);  // 8 out of 10
    }

    void testGetMatchTypeDistribution() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.match_type = "Exact";
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.match_type = "Exact";
        report.addEntry(entry2);
        
        sak::MigrationReport::MigrationEntry entry3;
        entry3.match_type = "Fuzzy";
        report.addEntry(entry3);
        
        auto distribution = report.getMatchTypeDistribution();
        QCOMPARE(distribution["Exact"], 2);
        QCOMPARE(distribution["Fuzzy"], 1);
    }

    void testGetSelectedEntries() {
        sak::MigrationReport report;
        
        for (int i = 0; i < 5; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.selected = (i % 2 == 0);  // Select even-numbered
            report.addEntry(entry);
        }
        
        auto selected = report.getSelectedEntries();
        QCOMPARE(selected.size(), 3);  // 0, 2, 4
    }

    void testGetUnmatchedEntries() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry1;
        entry1.app_name = "Matched App";
        entry1.package_id = "matched";
        report.addEntry(entry1);
        
        sak::MigrationReport::MigrationEntry entry2;
        entry2.app_name = "Unmatched App 1";
        entry2.package_id = "";
        report.addEntry(entry2);
        
        sak::MigrationReport::MigrationEntry entry3;
        entry3.app_name = "Unmatched App 2";
        entry3.package_id = "";
        report.addEntry(entry3);
        
        auto unmatched = report.getUnmatchedEntries();
        QCOMPARE(unmatched.size(), 2);
    }

    void testClear() {
        sak::MigrationReport report;
        
        sak::MigrationReport::MigrationEntry entry;
        entry.app_name = "Test";
        report.addEntry(entry);
        
        QCOMPARE(report.getEntries().size(), 1);
        
        report.clear();
        
        QCOMPARE(report.getEntries().size(), 0);
    }

    void testLargeDataset() {
        sak::MigrationReport report;
        
        // Add 1000 entries
        for (int i = 0; i < 1000; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.package_id = QString("app%1").arg(i);
            entry.confidence = 0.8;
            report.addEntry(entry);
        }
        
        QCOMPARE(report.getEntries().size(), 1000);
        
        QString jsonPath = tempDir->path() + "/large.json";
        QVERIFY(report.exportToJson(jsonPath));
        
        sak::MigrationReport report2;
        QVERIFY(report2.importFromJson(jsonPath));
        QCOMPARE(report2.getEntries().size(), 1000);
    }

    void testRoundTripIntegrity() {
        sak::MigrationReport report1;
        report1.setSourceComputer("Source");
        report1.setTargetComputer("Target");
        
        for (int i = 0; i < 10; i++) {
            sak::MigrationReport::MigrationEntry entry;
            entry.app_name = QString("App %1").arg(i);
            entry.app_version = QString("1.%1").arg(i);
            entry.package_id = QString("app%1").arg(i);
            entry.confidence = 0.9 - (i * 0.05);
            entry.selected = (i % 2 == 0);
            report1.addEntry(entry);
        }
        
        QString jsonPath = tempDir->path() + "/roundtrip.json";
        QVERIFY(report1.exportToJson(jsonPath));
        
        sak::MigrationReport report2;
        QVERIFY(report2.importFromJson(jsonPath));
        
        // Verify all data preserved
        QCOMPARE(report2.getSourceComputer(), report1.getSourceComputer());
        QCOMPARE(report2.getTargetComputer(), report1.getTargetComputer());
        QCOMPARE(report2.getEntries().size(), report1.getEntries().size());
        
        auto entries1 = report1.getEntries();
        auto entries2 = report2.getEntries();
        
        for (int i = 0; i < entries1.size(); i++) {
            QCOMPARE(entries2[i].app_name, entries1[i].app_name);
            QCOMPARE(entries2[i].app_version, entries1[i].app_version);
            QCOMPARE(entries2[i].package_id, entries1[i].package_id);
            QCOMPARE(entries2[i].confidence, entries1[i].confidence);
            QCOMPARE(entries2[i].selected, entries1[i].selected);
        }
    }
};

QTEST_MAIN(TestMigrationReport)
#include "test_migration_report.moc"
