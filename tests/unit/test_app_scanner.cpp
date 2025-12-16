// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for AppScanner
 * Tests application detection from Registry and AppX packages
 */

#include "sak/app_scanner.h"
#include <QTest>
#include <QTemporaryDir>

class TestAppScanner : public QObject {
    Q_OBJECT

private slots:
    void testScanAll() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        // Should find at least some apps on any Windows system
        QVERIFY(!apps.isEmpty());
        
        // Verify app info structure
        for (const auto& app : apps) {
            QVERIFY(!app.name.isEmpty());
            // Version may be empty for some apps
            // Publisher may be empty for some apps
        }
    }

    void testRegistryScanning() {
        sak::AppScanner scanner;
        auto apps = scanner.scanRegistry();
        
        QVERIFY(!apps.isEmpty());
        
        // All registry apps should have registry_key
        for (const auto& app : apps) {
            QVERIFY(!app.registry_key.isEmpty());
            QCOMPARE(app.source, sak::AppScanner::AppInfo::Source::Registry);
        }
    }

    void testAppXScanning() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAppX();
        
        // May be empty on older Windows or without Store apps
        // Just verify the source is correct if any found
        for (const auto& app : apps) {
            QCOMPARE(app.source, sak::AppScanner::AppInfo::Source::AppX);
        }
    }

    void testAppInfoFields() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        QVERIFY(!apps.isEmpty());
        
        // Test that at least one app has detailed info
        bool foundDetailedApp = false;
        for (const auto& app : apps) {
            if (!app.name.isEmpty() && 
                !app.version.isEmpty() && 
                !app.publisher.isEmpty()) {
                foundDetailedApp = true;
                
                // Verify field types
                QVERIFY(app.name.length() > 0);
                QVERIFY(app.version.length() > 0);
                QVERIFY(app.publisher.length() > 0);
                break;
            }
        }
        
        QVERIFY(foundDetailedApp);
    }

    void testDuplicateRemoval() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        // Check for duplicates by name
        QSet<QString> appNames;
        for (const auto& app : apps) {
            QString key = app.name.toLower();
            QVERIFY2(!appNames.contains(key), 
                     qPrintable("Duplicate app found: " + app.name));
            appNames.insert(key);
        }
    }

    void testVersionParsing() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        // Find apps with versions and verify format
        int appsWithVersions = 0;
        for (const auto& app : apps) {
            if (!app.version.isEmpty()) {
                appsWithVersions++;
                
                // Version should contain at least one number
                QVERIFY2(app.version.contains(QRegularExpression("\\d")),
                         qPrintable("Invalid version format: " + app.version));
            }
        }
        
        QVERIFY(appsWithVersions > 0);
    }

    void testInstallLocationParsing() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        // Some apps should have install locations
        int appsWithLocation = 0;
        for (const auto& app : apps) {
            if (!app.install_location.isEmpty()) {
                appsWithLocation++;
                
                // Should be a valid path format
                QVERIFY(app.install_location.contains(":") || 
                        app.install_location.startsWith("\\"));
            }
        }
        
        // At least 10% of apps should have location info
        QVERIFY(appsWithLocation > apps.size() / 10);
    }

    void testUninstallStringParsing() {
        sak::AppScanner scanner;
        auto apps = scanner.scanAll();
        
        // Many apps should have uninstall strings
        int appsWithUninstall = 0;
        for (const auto& app : apps) {
            if (!app.uninstall_string.isEmpty()) {
                appsWithUninstall++;
            }
        }
        
        QVERIFY(appsWithUninstall > apps.size() / 4);
    }

    void testPerformance() {
        sak::AppScanner scanner;
        
        QElapsedTimer timer;
        timer.start();
        
        auto apps = scanner.scanAll();
        
        qint64 elapsed = timer.elapsed();
        
        // Scanning should complete within 30 seconds
        QVERIFY2(elapsed < 30000, 
                 qPrintable(QString("Scan took %1ms").arg(elapsed)));
        
        qDebug() << "Scanned" << apps.size() << "apps in" << elapsed << "ms";
    }

    void testProgressSignals() {
        sak::AppScanner scanner;
        
        int progressCount = 0;
        int lastProgress = -1;
        
        connect(&scanner, &sak::AppScanner::progress, 
                [&](int current, int total) {
            progressCount++;
            QVERIFY(current >= 0);
            QVERIFY(total > 0);
            QVERIFY(current <= total);
            QVERIFY(current >= lastProgress); // Should be monotonic
            lastProgress = current;
        });
        
        scanner.scanAll();
        
        QVERIFY(progressCount > 0);
    }

    void testErrorHandling() {
        // Test that scanner doesn't crash on registry errors
        sak::AppScanner scanner;
        
        bool completed = false;
        try {
            auto apps = scanner.scanAll();
            completed = true;
        } catch (...) {
            QFAIL("Scanner should not throw exceptions");
        }
        
        QVERIFY(completed);
    }
};

QTEST_MAIN(TestAppScanner)
#include "test_app_scanner.moc"
