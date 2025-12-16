// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for path_utils
 * Tests path manipulation, validation, and sanitization
 */

#include "sak/path_utils.h"
#include <QTest>
#include <QTemporaryDir>
#include <QDir>

class TestPathUtils : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Setup before all tests
    }

    void cleanupTestCase() {
        // Cleanup after all tests
    }

    void init() {
        // Setup before each test
    }

    void cleanup() {
        // Cleanup after each test
    }

    // Test path validation
    void testIsValidPath() {
        QVERIFY(sak::PathUtils::isValidPath("C:\\Windows\\System32"));
        QVERIFY(sak::PathUtils::isValidPath("C:/Users/Test/Documents"));
        QVERIFY(!sak::PathUtils::isValidPath(""));
        QVERIFY(!sak::PathUtils::isValidPath("C:\\Invalid<>Path"));
        QVERIFY(!sak::PathUtils::isValidPath("CON"));  // Reserved name
        QVERIFY(!sak::PathUtils::isValidPath("C:\\Path\\NUL\\File"));
    }

    void testNormalizePath() {
        QString normalized = sak::PathUtils::normalizePath("C:/Users/Test\\Documents/../Downloads");
        QCOMPARE(normalized, QString("C:/Users/Test/Downloads"));
        
        normalized = sak::PathUtils::normalizePath("C:\\Windows\\..\\Program Files");
        QCOMPARE(normalized, QString("C:/Program Files"));
    }

    void testSanitizeFilename() {
        QCOMPARE(sak::PathUtils::sanitizeFilename("file<name>.txt"), QString("filename.txt"));
        QCOMPARE(sak::PathUtils::sanitizeFilename("file:name|test.txt"), QString("filenametest.txt"));
        QCOMPARE(sak::PathUtils::sanitizeFilename("valid_file-123.txt"), QString("valid_file-123.txt"));
    }

    void testGetRelativePath() {
        QString base = "C:/Users/Test/Documents";
        QString target = "C:/Users/Test/Documents/Projects/SAK";
        QString relative = sak::PathUtils::getRelativePath(base, target);
        QCOMPARE(relative, QString("Projects/SAK"));
    }

    void testJoinPaths() {
        QCOMPARE(sak::PathUtils::joinPaths("C:/Users", "Test", "Documents"), 
                 QString("C:/Users/Test/Documents"));
        QCOMPARE(sak::PathUtils::joinPaths("C:\\Users\\", "\\Test"), 
                 QString("C:/Users/Test"));
    }

    void testExpandEnvironmentVariables() {
        QString path = "%USERPROFILE%\\Documents";
        QString expanded = sak::PathUtils::expandEnvironmentVariables(path);
        QVERIFY(expanded.contains("Documents"));
        QVERIFY(!expanded.contains("%USERPROFILE%"));
    }

    void testGetFileSize() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString testFile = tempDir.path() + "/test.txt";
        QFile file(testFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Test content");
        file.close();
        
        qint64 size = sak::PathUtils::getFileSize(testFile);
        QCOMPARE(size, qint64(12));
    }

    void testGetDirectorySize() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        // Create test files
        QFile file1(tempDir.path() + "/file1.txt");
        QVERIFY(file1.open(QIODevice::WriteOnly));
        file1.write("Content1");
        file1.close();
        
        QDir(tempDir.path()).mkdir("subdir");
        QFile file2(tempDir.path() + "/subdir/file2.txt");
        QVERIFY(file2.open(QIODevice::WriteOnly));
        file2.write("Content22");
        file2.close();
        
        qint64 size = sak::PathUtils::getDirectorySize(tempDir.path());
        QCOMPARE(size, qint64(17)); // 8 + 9 bytes
    }

    void testEnsureDirectoryExists() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString testPath = tempDir.path() + "/new/nested/directory";
        QVERIFY(sak::PathUtils::ensureDirectoryExists(testPath));
        QVERIFY(QDir(testPath).exists());
    }

    void testIsSafeToDelete() {
        // Should never delete system directories
        QVERIFY(!sak::PathUtils::isSafeToDelete("C:\\Windows"));
        QVERIFY(!sak::PathUtils::isSafeToDelete("C:\\Program Files"));
        QVERIFY(!sak::PathUtils::isSafeToDelete("C:\\"));
        
        // User directories should be safe
        QVERIFY(sak::PathUtils::isSafeToDelete("C:\\Users\\Test\\Documents\\temp"));
    }
};

QTEST_MAIN(TestPathUtils)
#include "test_path_utils.moc"
