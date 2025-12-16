// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for FileScanner
 * Tests file scanning, filtering, and pattern matching
 */

#include "sak/file_scanner.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

class TestFileScanner : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString testPath;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        testPath = tempDir->path();
        
        // Create test directory structure
        QDir dir(testPath);
        dir.mkpath("subdir1");
        dir.mkpath("subdir2/nested");
        
        // Create test files
        QFile::copy(":/test_data/test.txt", testPath + "/file1.txt");
        QFile::copy(":/test_data/test.txt", testPath + "/file2.log");
        QFile::copy(":/test_data/test.txt", testPath + "/subdir1/file3.txt");
        QFile::copy(":/test_data/test.txt", testPath + "/subdir2/file4.doc");
        QFile::copy(":/test_data/test.txt", testPath + "/subdir2/nested/file5.txt");
        
        // Alternative: Create files directly
        QStringList files = {
            "/file1.txt", "/file2.log", 
            "/subdir1/file3.txt", "/subdir2/file4.doc",
            "/subdir2/nested/file5.txt"
        };
        
        for (const QString& file : files) {
            QFile f(testPath + file);
            if (f.open(QIODevice::WriteOnly)) {
                f.write("Test content");
                f.close();
            }
        }
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void testScanAllFiles() {
        sak::FileScanner scanner;
        auto files = scanner.scan(testPath);
        
        QVERIFY(files.size() >= 5);
    }

    void testScanWithExtensionFilter() {
        sak::FileScanner scanner;
        scanner.setExtensionFilter(QStringList() << "*.txt");
        
        auto files = scanner.scan(testPath);
        
        for (const auto& file : files) {
            QVERIFY(file.endsWith(".txt"));
        }
        QCOMPARE(files.size(), 3); // file1.txt, file3.txt, file5.txt
    }

    void testScanWithMultipleExtensions() {
        sak::FileScanner scanner;
        scanner.setExtensionFilter(QStringList() << "*.txt" << "*.log");
        
        auto files = scanner.scan(testPath);
        
        QCOMPARE(files.size(), 4); // .txt and .log files
    }

    void testRecursiveScan() {
        sak::FileScanner scanner;
        scanner.setRecursive(true);
        
        auto files = scanner.scan(testPath);
        QVERIFY(files.size() >= 5);
        
        // Check that nested file is included
        bool foundNested = false;
        for (const auto& file : files) {
            if (file.contains("nested")) {
                foundNested = true;
                break;
            }
        }
        QVERIFY(foundNested);
    }

    void testNonRecursiveScan() {
        sak::FileScanner scanner;
        scanner.setRecursive(false);
        
        auto files = scanner.scan(testPath);
        
        // Should only find files in root directory
        for (const auto& file : files) {
            QFileInfo info(file);
            QCOMPARE(info.dir().path(), testPath);
        }
    }

    void testExcludeDirectories() {
        sak::FileScanner scanner;
        scanner.setExcludeDirectories(QStringList() << "subdir2");
        
        auto files = scanner.scan(testPath);
        
        for (const auto& file : files) {
            QVERIFY(!file.contains("subdir2"));
        }
    }

    void testExcludePatterns() {
        sak::FileScanner scanner;
        scanner.setExcludePatterns(QStringList() << "*nested*");
        
        auto files = scanner.scan(testPath);
        
        for (const auto& file : files) {
            QVERIFY(!file.contains("nested"));
        }
    }

    void testMinMaxFileSize() {
        sak::FileScanner scanner;
        scanner.setMinFileSize(5);  // 5 bytes minimum
        scanner.setMaxFileSize(100); // 100 bytes maximum
        
        auto files = scanner.scan(testPath);
        
        for (const auto& file : files) {
            QFileInfo info(file);
            QVERIFY(info.size() >= 5);
            QVERIFY(info.size() <= 100);
        }
    }

    void testDateFilter() {
        sak::FileScanner scanner;
        QDateTime yesterday = QDateTime::currentDateTime().addDays(-1);
        QDateTime tomorrow = QDateTime::currentDateTime().addDays(1);
        
        scanner.setModifiedAfter(yesterday);
        scanner.setModifiedBefore(tomorrow);
        
        auto files = scanner.scan(testPath);
        
        for (const auto& file : files) {
            QFileInfo info(file);
            QVERIFY(info.lastModified() >= yesterday);
            QVERIFY(info.lastModified() <= tomorrow);
        }
    }

    void testHiddenFiles() {
        // Create hidden file (on Windows, requires attribute setting)
        QString hiddenFile = testPath + "/.hidden";
        QFile file(hiddenFile);
        file.open(QIODevice::WriteOnly);
        file.close();
        
        sak::FileScanner scanner;
        scanner.setIncludeHidden(false);
        auto files1 = scanner.scan(testPath);
        
        scanner.setIncludeHidden(true);
        auto files2 = scanner.scan(testPath);
        
        QVERIFY(files2.size() >= files1.size());
    }

    void testFollowSymlinks() {
        sak::FileScanner scanner;
        
        scanner.setFollowSymlinks(false);
        auto files1 = scanner.scan(testPath);
        
        scanner.setFollowSymlinks(true);
        auto files2 = scanner.scan(testPath);
        
        // Results should be same if no symlinks exist
        QVERIFY(files1.size() <= files2.size());
    }

    void testScanProgress() {
        sak::FileScanner scanner;
        
        int progressCount = 0;
        QObject::connect(&scanner, &sak::FileScanner::progress, 
                        [&progressCount](int current, int total) {
            progressCount++;
            QVERIFY(current <= total);
        });
        
        scanner.scan(testPath);
        QVERIFY(progressCount > 0);
    }

    void testCancelScan() {
        sak::FileScanner scanner;
        
        QTimer::singleShot(100, &scanner, &sak::FileScanner::cancel);
        
        auto files = scanner.scan(testPath);
        
        // Should have been cancelled
        QVERIFY(scanner.wasCancelled());
    }
};

QTEST_MAIN(TestFileScanner)
#include "test_file_scanner.moc"
