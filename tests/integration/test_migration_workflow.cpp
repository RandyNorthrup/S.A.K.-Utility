// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Integration test for Migration Workflow
 * Tests app scanning, matching, and migration report generation
 */

#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/package_matcher.h"
#include "sak/migration_report.h"
#include <QTest>
#include <QTemporaryDir>

class TestMigrationWorkflow : public QObject {
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

    void testAppScanningAndMatching() {
        // Scan installed apps
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        QVERIFY(!apps.isEmpty());
        
        // Initialize Chocolatey
        sak::ChocolateyManager chocoMgr;
        QString chocoPath = QCoreApplication::applicationDirPath() + "/../../tools/chocolatey";
        
        if (!chocoMgr.initialize(chocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        // Match apps to packages
        sak::PackageMatcher matcher;
        matcher.setChocolateyManager(&chocoMgr);
        
        sak::PackageMatcher::MatchConfig config;
        config.use_exact_mappings = true;
        config.use_fuzzy_matching = true;
        config.min_confidence = 0.7;
        
        auto matches = matcher.matchApps(apps, config);
        
        QVERIFY(!matches.isEmpty());
        
        // Verify match quality
        for (const auto& match : matches) {
            QVERIFY(match.confidence >= 0.7);
            QVERIFY(!match.package_id.isEmpty());
        }
    }

    void testMigrationReportCreation() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        sak::ChocolateyManager chocoMgr;
        QString chocoPath = QCoreApplication::applicationDirPath() + "/../../tools/chocolatey";
        
        if (!chocoMgr.initialize(chocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        sak::PackageMatcher matcher;
        matcher.setChocolateyManager(&chocoMgr);
        auto matches = matcher.matchApps(apps);
        
        // Create migration report
        sak::MigrationReport report;
        report.setSourceComputer("SourcePC");
        report.setTargetComputer("TargetPC");
        report.setMatches(matches);
        
        QString reportPath = tempDir->path() + "/migration_report.json";
        QVERIFY(report.exportToJson(reportPath));
        
        // Verify report file
        QVERIFY(QFile::exists(reportPath));
        
        // Read and verify content
        sak::MigrationReport loadedReport;
        QVERIFY(loadedReport.importFromJson(reportPath));
        
        QCOMPARE(loadedReport.getSourceComputer(), QString("SourcePC"));
        QCOMPARE(loadedReport.getTargetComputer(), QString("TargetPC"));
        QVERIFY(!loadedReport.getMatches().isEmpty());
    }

    void testMigrationExecution() {
        // Load migration report
        QString reportPath = tempDir->path() + "/test_report.json";
        createTestReport(reportPath);
        
        sak::MigrationReport report;
        QVERIFY(report.importFromJson(reportPath));
        
        sak::ChocolateyManager chocoMgr;
        QString chocoPath = QCoreApplication::applicationDirPath() + "/../../tools/chocolatey";
        
        if (!chocoMgr.initialize(chocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        // Execute migration (dry run)
        auto matches = report.getMatches();
        int successCount = 0;
        
        for (const auto& match : matches) {
            if (match.confidence >= 0.8) {
                // Would install package here
                successCount++;
            }
        }
        
        QVERIFY(successCount > 0);
    }

    void testProgressTracking() {
        sak::AppScanner scanner;
        
        int progressCount = 0;
        connect(&scanner, &sak::AppScanner::progress, 
                [&](int current, int total) {
            progressCount++;
            QVERIFY(current <= total);
        });
        
        scanner.scanAll();
        
        QVERIFY(progressCount > 0);
    }

    void testErrorHandling() {
        sak::MigrationReport report;
        
        // Try to load non-existent report
        bool errorOccurred = false;
        connect(&report, &sak::MigrationReport::error, 
                [&](const QString&) { errorOccurred = true; });
        
        QVERIFY(!report.importFromJson("/nonexistent/report.json"));
    }

private:
    void createTestReport(const QString& path) {
        QJsonObject json;
        json["source_computer"] = "TestSource";
        json["target_computer"] = "TestTarget";
        json["created_date"] = QDateTime::currentDateTime().toString(Qt::ISODate);
        
        QJsonArray matchesArray;
        QJsonObject match;
        match["app_name"] = "7-Zip";
        match["package_id"] = "7zip";
        match["confidence"] = 0.95;
        matchesArray.append(match);
        
        json["matches"] = matchesArray;
        
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(QJsonDocument(json).toJson());
        file.close();
    }
};

QTEST_MAIN(TestMigrationWorkflow)
#include "test_migration_workflow.moc"
