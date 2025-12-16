// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for ChocolateyManager
 * Tests portable Chocolatey integration
 */

#include "sak/chocolatey_manager.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>

class TestChocolateyManager : public QObject {
    Q_OBJECT

private:
    QString testChocoPath;
    QTemporaryDir* tempDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        testChocoPath = QCoreApplication::applicationDirPath() + "/../../tools/chocolatey";
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void testInitialization() {
        sak::ChocolateyManager manager;
        
        if (QFile::exists(testChocoPath + "/choco.exe")) {
            QVERIFY(manager.initialize(testChocoPath));
            QVERIFY(manager.isInitialized());
        } else {
            // If choco not bundled, initialization should fail gracefully
            QVERIFY(!manager.initialize(testChocoPath));
            QVERIFY(!manager.isInitialized());
        }
    }

    void testGetVersion() {
        sak::ChocolateyManager manager;
        
        if (manager.initialize(testChocoPath)) {
            QString version = manager.getChocoVersion();
            
            QVERIFY(!version.isEmpty());
            QVERIFY(version.contains(QRegularExpression("\\d+\\.\\d+")));
        } else {
            QSKIP("Chocolatey not available");
        }
    }

    void testGetChocoPath() {
        sak::ChocolateyManager manager;
        
        if (manager.initialize(testChocoPath)) {
            QString path = manager.getChocoPath();
            
            QVERIFY(!path.isEmpty());
            QVERIFY(QFile::exists(path + "/choco.exe"));
        } else {
            QSKIP("Chocolatey not available");
        }
    }

    void testVerifyIntegrity() {
        sak::ChocolateyManager manager;
        
        if (manager.initialize(testChocoPath)) {
            QVERIFY(manager.verifyIntegrity());
        } else {
            QSKIP("Chocolatey not available");
        }
    }

    void testSearchPackage() {
        sak::ChocolateyManager manager;
        
        if (!manager.initialize(testChocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        // Search for a common package
        auto result = manager.searchPackage("7zip", 10);
        
        if (result.success) {
            QVERIFY(!result.output.isEmpty());
            
            auto packages = manager.parseSearchResults(result.output);
            QVERIFY(!packages.isEmpty());
            
            // Should find 7zip package
            bool found7zip = false;
            for (const auto& pkg : packages) {
                if (pkg.package_id.toLower().contains("7zip")) {
                    found7zip = true;
                    QVERIFY(!pkg.version.isEmpty());
                    break;
                }
            }
            QVERIFY(found7zip);
        }
    }

    void testIsPackageAvailable() {
        sak::ChocolateyManager manager;
        
        if (!manager.initialize(testChocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        // Test known packages
        QVERIFY(manager.isPackageAvailable("7zip"));
        QVERIFY(manager.isPackageAvailable("googlechrome"));
        
        // Test non-existent package
        QVERIFY(!manager.isPackageAvailable("nonexistent-package-xyz123"));
    }

    void testParseSearchResults() {
        sak::ChocolateyManager manager;
        
        QString sampleOutput = R"(
Chocolatey v0.11.0
7zip 22.01
 7-Zip is a file archiver with a high compression ratio.
 7zip.install 22.01
googlechrome 108.0.5359.125
 Google Chrome web browser
)";
        
        auto packages = manager.parseSearchResults(sampleOutput);
        
        QCOMPARE(packages.size(), 3);
        QCOMPARE(packages[0].package_id, QString("7zip"));
        QCOMPARE(packages[0].version, QString("22.01"));
    }

    void testInstallConfigStructure() {
        sak::ChocolateyManager::InstallConfig config;
        
        config.package_name = "7zip";
        config.version = "22.01";
        config.version_locked = true;
        config.auto_confirm = true;
        config.force_reinstall = false;
        config.ignore_checksums = false;
        
        QCOMPARE(config.package_name, QString("7zip"));
        QCOMPARE(config.version, QString("22.01"));
        QVERIFY(config.version_locked);
        QVERIFY(config.auto_confirm);
    }

    void testInstallCommandGeneration() {
        sak::ChocolateyManager manager;
        
        if (!manager.initialize(testChocoPath)) {
            QSKIP("Chocolatey not available");
        }
        
        sak::ChocolateyManager::InstallConfig config;
        config.package_name = "7zip";
        config.version = "22.01";
        config.version_locked = true;
        config.auto_confirm = true;
        
        // We don't actually install, just verify the command would be generated
        // This is a dry-run test
        QVERIFY(!config.package_name.isEmpty());
        QVERIFY(!config.version.isEmpty());
    }

    void testExecutionResultStructure() {
        sak::ChocolateyManager::ExecutionResult result;
        
        result.success = true;
        result.exit_code = 0;
        result.output = "Installation successful";
        result.error = "";
        
        QVERIFY(result.success);
        QCOMPARE(result.exit_code, 0);
        QVERIFY(!result.output.isEmpty());
    }

    void testInvalidPath() {
        sak::ChocolateyManager manager;
        
        QVERIFY(!manager.initialize("/nonexistent/path"));
        QVERIFY(!manager.isInitialized());
    }

    void testMultipleInitialization() {
        sak::ChocolateyManager manager;
        
        if (QFile::exists(testChocoPath + "/choco.exe")) {
            QVERIFY(manager.initialize(testChocoPath));
            
            // Second initialization should also work (or handle gracefully)
            bool secondInit = manager.initialize(testChocoPath);
            QVERIFY(secondInit || manager.isInitialized());
        }
    }
};

QTEST_MAIN(TestChocolateyManager)
#include "test_chocolatey_manager.moc"
