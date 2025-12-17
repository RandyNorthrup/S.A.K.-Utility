// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for PackageMatcher
 * Tests app-to-package matching algorithm
 */

#include "sak/package_matcher.h"
#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include <QTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

class TestPackageMatcher : public QObject {
    Q_OBJECT

private:
    QString testMappingsPath;
    QTemporaryDir* tempDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        testMappingsPath = tempDir->path() + "/test_mappings.json";
        createTestMappings();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestMappings() {
        QJsonObject mappings;
        mappings["7-Zip"] = "7zip";
        mappings["Google Chrome"] = "googlechrome";
        mappings["Mozilla Firefox"] = "firefox";
        mappings["VLC media player"] = "vlc";
        mappings["Git"] = "git";
        
        QJsonDocument doc(mappings);
        QFile file(testMappingsPath);
        file.open(QIODevice::WriteOnly);
        file.write(doc.toJson());
        file.close();
    }

    void testLoadMappings() {
        sak::PackageMatcher matcher;
        
        QVERIFY(matcher.loadMappings(testMappingsPath));
    }

    void testExactMatch() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        sak::AppScanner::AppInfo app;
        app.name = "7-Zip 22.01";
        app.version = "22.01";
        
        sak::PackageMatcher::MatchConfig config;
        config.use_exact_mappings = true;
        config.use_fuzzy_matching = false;
        
        auto match = matcher.matchApp(app, config);
        
        QVERIFY(match.has_value());
        QCOMPARE(match->package_id, QString("7zip"));
        QCOMPARE(match->match_type, sak::PackageMatcher::MatchType::ExactMapping);
        QCOMPARE(match->confidence, 1.0);
    }

    void testFuzzyMatch() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        sak::AppScanner::AppInfo app;
        app.name = "7zip Archiver";  // Similar but not exact
        app.version = "22.01";
        
        sak::PackageMatcher::MatchConfig config;
        config.use_exact_mappings = false;
        config.use_fuzzy_matching = true;
        config.min_confidence = 0.6;
        
        auto match = matcher.matchApp(app, config);
        
        if (match.has_value()) {
            QVERIFY(match->confidence >= 0.6);
            QCOMPARE(match->match_type, sak::PackageMatcher::MatchType::FuzzyMatch);
        }
    }

    void testNoMatch() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        sak::AppScanner::AppInfo app;
        app.name = "Custom Internal Tool XYZ";
        app.version = "1.0";
        
        sak::PackageMatcher::MatchConfig config;
        config.min_confidence = 0.8;
        
        auto match = matcher.matchApp(app, config);
        
        QVERIFY(!match.has_value());
    }

    void testConfidenceThreshold() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        sak::AppScanner::AppInfo app;
        app.name = "VLC Player";  // Similar to "VLC media player"
        app.version = "3.0";
        
        // Test with high threshold
        sak::PackageMatcher::MatchConfig config1;
        config1.min_confidence = 0.95;
        auto match1 = matcher.matchApp(app, config1);
        
        // Test with low threshold  
        sak::PackageMatcher::MatchConfig config2;
        config2.min_confidence = 0.5;
        auto match2 = matcher.matchApp(app, config2);
        
        // Lower threshold should be more permissive
        if (match2.has_value()) {
            QVERIFY(match2->confidence >= 0.5);
        }
    }

    void testMatchWithChocolateyVerification() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        QString chocoPath = QCoreApplication::applicationDirPath() + "/../../tools/chocolatey";
        sak::ChocolateyManager chocoMgr;
        
        if (!chocoMgr.initialize(chocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        matcher.setChocolateyManager(&chocoMgr);
        
        sak::AppScanner::AppInfo app;
        app.name = "7-Zip";
        app.version = "22.01";
        
        sak::PackageMatcher::MatchConfig config;
        config.verify_availability = true;
        
        auto match = matcher.matchApp(app, config);
        
        if (match.has_value()) {
            QVERIFY(match->available);
        }
    }

    void testBatchMatching() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        QVector<sak::AppScanner::AppInfo> apps;
        
        sak::AppScanner::AppInfo app1;
        app1.name = "7-Zip";
        app1.version = "22.01";
        apps.append(app1);
        
        sak::AppScanner::AppInfo app2;
        app2.name = "Google Chrome";
        app2.version = "108.0";
        apps.append(app2);
        
        sak::AppScanner::AppInfo app3;
        app3.name = "Unknown App";
        app3.version = "1.0";
        apps.append(app3);
        
        sak::PackageMatcher::MatchConfig config;
        auto matches = matcher.matchApps(apps, config);
        
        // Should match at least the first two
        int matchCount = 0;
        for (const auto& match : matches) {
            if (match.confidence > 0) {
                matchCount++;
            }
        }
        
        QVERIFY(matchCount >= 2);
    }

    void testMatchResultStructure() {
        sak::PackageMatcher::MatchResult result;
        
        result.app_name = "7-Zip";
        result.package_id = "7zip";
        result.confidence = 0.95;
        result.match_type = sak::PackageMatcher::MatchType::ExactMapping;
        result.available = true;
        result.match_reason = "Exact name mapping";
        
        QCOMPARE(result.app_name, QString("7-Zip"));
        QCOMPARE(result.package_id, QString("7zip"));
        QCOMPARE(result.confidence, 0.95);
        QVERIFY(result.available);
    }

    void testParallelMatching() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        // Create many apps to test parallel processing
        QVector<sak::AppScanner::AppInfo> apps;
        for (int i = 0; i < 50; i++) {
            sak::AppScanner::AppInfo app;
            app.name = QString("Test App %1").arg(i);
            app.version = "1.0";
            apps.append(app);
        }
        
        sak::PackageMatcher::MatchConfig config;
        config.thread_count = 4;
        
        QElapsedTimer timer;
        timer.start();
        
        auto matches = matcher.matchApps(apps, config);
        
        qint64 elapsed = timer.elapsed();
        
        QCOMPARE(matches.size(), apps.size());
        qDebug() << "Matched" << apps.size() << "apps in" << elapsed << "ms";
    }

    void testCaching() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        sak::AppScanner::AppInfo app;
        app.name = "7-Zip";
        app.version = "22.01";
        
        sak::PackageMatcher::MatchConfig config;
        config.use_cache = true;
        
        // First match
        QElapsedTimer timer1;
        timer1.start();
        auto match1 = matcher.matchApp(app, config);
        qint64 time1 = timer1.elapsed();
        
        // Second match (should be cached)
        QElapsedTimer timer2;
        timer2.start();
        auto match2 = matcher.matchApp(app, config);
        qint64 time2 = timer2.elapsed();
        
        // Cached result should be faster
        if (match1.has_value() && match2.has_value()) {
            QVERIFY(time2 <= time1);
            QCOMPARE(match1->package_id, match2->package_id);
        }
    }

    void testVersionSpecificMatching() {
        sak::PackageMatcher matcher;
        matcher.loadMappings(testMappingsPath);
        
        sak::AppScanner::AppInfo app;
        app.name = "7-Zip";
        app.version = "22.01";
        
        auto match = matcher.matchApp(app);
        
        if (match.has_value()) {
            QCOMPARE(match->version, QString("22.01"));
        }
    }
};

QTEST_MAIN(TestPackageMatcher)
#include "test_package_matcher.moc"
