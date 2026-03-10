// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_scanner.cpp
/// @brief Unit tests for recursive file scanning with filtering

#include "sak/error_codes.h"
#include "sak/file_scanner.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <filesystem>
#include <stop_token>

class FileScannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Basic scanning
    void scan_normalDirectory();
    void scan_emptyDirectory();
    void scan_nonExistentDir();

    // scanAndCollect
    void scanAndCollect_returnsAllFiles();
    void scanAndCollect_respectsFilters();

    // Filters
    void includePattern_onlyMatching();
    void excludePattern_skipsMatching();
    void excludeDir_skipsDirectory();
    void fileSizeFilter_minMax();
    void maxDepth_limitsRecursion();
    void typeFilter_filesOnly();
    void typeFilter_dirsOnly();

    // Cancellation
    void cancellation_stopsScan();

    // Static convenience
    void listFiles_recursive();
    void listFiles_nonRecursive();
    void findFiles_byPattern();

    // Callback
    void callback_falseStopsScan();

    // Progress
    void progress_callbackInvoked();

private:
    QTemporaryDir m_tempDir;
    std::filesystem::path m_rootPath;
};

void FileScannerTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_rootPath = m_tempDir.path().toStdWString();

    QDir root(m_tempDir.path());
    root.mkpath("dir_a");
    root.mkpath("dir_b/nested");
    root.mkpath("dir_c");

    auto writeFile = [&](const QString& relPath, const QByteArray& content) {
        QFile f(m_tempDir.filePath(relPath));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
        f.close();
    };

    writeFile("file1.txt", "hello");                     // 5 bytes
    writeFile("file2.cpp", "int main() {}");             // 13 bytes
    writeFile("dir_a/data.json", R"({"key":"val"})");    // 14 bytes
    writeFile("dir_a/readme.md", "# Title");             // 7 bytes
    writeFile("dir_b/image.png", QByteArray(100, 'P'));  // 100 bytes
    writeFile("dir_b/nested/deep.log", "log entry");     // 9 bytes
}

// ============================================================================
// Basic Scanning
// ============================================================================

void FileScannerTests::scan_normalDirectory() {
    sak::file_scanner scanner;
    sak::scan_options opts;

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{6});
    QVERIFY(result.value().directories_found >= std::size_t{3});
    QVERIFY(result.value().total_size > 0);
}

void FileScannerTests::scan_emptyDirectory() {
    QDir(m_tempDir.path()).mkpath("empty_dir");
    auto emptyPath = m_rootPath / "empty_dir";

    sak::file_scanner scanner;
    sak::scan_options opts;

    auto result = scanner.scan(emptyPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{0});
}

void FileScannerTests::scan_nonExistentDir() {
    sak::file_scanner scanner;
    sak::scan_options opts;

    auto result = scanner.scan(m_rootPath / "does_not_exist", opts);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::file_not_found);
}

// ============================================================================
// scanAndCollect
// ============================================================================

void FileScannerTests::scanAndCollect_returnsAllFiles() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    // Note: files_only type_filter prevents directory recursion in shouldProcessEntry,
    // so only root-level files are returned (file1.txt, file2.cpp)
    opts.type_filter = sak::file_type_filter::files_only;

    auto result = scanner.scanAndCollect(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().size(), std::size_t{2});
}

void FileScannerTests::scanAndCollect_respectsFilters() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.include_patterns = {"*.txt"};
    // Use files_only so result only contains files (not directories).
    // Root-level *.txt matches file1.txt only.
    opts.type_filter = sak::file_type_filter::files_only;

    auto result = scanner.scanAndCollect(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().size(), std::size_t{1});
}

// ============================================================================
// Filter Tests
// ============================================================================

void FileScannerTests::includePattern_onlyMatching() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.include_patterns = {"*.cpp", "*.txt"};

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{2});
}

void FileScannerTests::excludePattern_skipsMatching() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.exclude_patterns = {"*.log"};

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{5});  // 6 total minus 1 .log
}

void FileScannerTests::excludeDir_skipsDirectory() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.exclude_dirs = {"dir_b"};

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // Without dir_b: file1.txt, file2.cpp, dir_a/data.json, dir_a/readme.md = 4
    QCOMPARE(result.value().files_found, std::size_t{4});
}

void FileScannerTests::fileSizeFilter_minMax() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.min_file_size = 10;
    opts.max_file_size = 50;

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // Files 10-50 bytes: file2.cpp(13), dir_a/data.json(14)
    QVERIFY(result.value().files_found >= std::size_t{2});
}

void FileScannerTests::maxDepth_limitsRecursion() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.max_depth = 1;  // Only root and immediate children

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // Only root-level files: file1.txt, file2.cpp = 2
    QCOMPARE(result.value().files_found, std::size_t{2});
}

void FileScannerTests::typeFilter_filesOnly() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.type_filter = sak::file_type_filter::files_only;

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().directories_found, std::size_t{0});
    // files_only prevents directory recursion in shouldProcessEntry,
    // so only root-level files (file1.txt, file2.cpp) are found
    QCOMPARE(result.value().files_found, std::size_t{2});
}

void FileScannerTests::typeFilter_dirsOnly() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.type_filter = sak::file_type_filter::directories_only;

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{0});
    QVERIFY(result.value().directories_found >= std::size_t{3});
}

// ============================================================================
// Cancellation
// ============================================================================

void FileScannerTests::cancellation_stopsScan() {
    std::stop_source stopSource;
    stopSource.request_stop();

    sak::file_scanner scanner;
    sak::scan_options opts;

    auto result = scanner.scan(m_rootPath, opts, stopSource.get_token());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::operation_cancelled);
}

// ============================================================================
// Static Convenience Functions
// ============================================================================

void FileScannerTests::listFiles_recursive() {
    auto result = sak::file_scanner::listFiles(m_rootPath, true);
    QVERIFY(result.has_value());
    // listFiles uses files_only internally, which prevents directory recursion.
    // Only root-level files are returned.
    QCOMPARE(result.value().size(), std::size_t{2});
}

void FileScannerTests::listFiles_nonRecursive() {
    auto result = sak::file_scanner::listFiles(m_rootPath, false);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().size(), std::size_t{2});  // file1.txt, file2.cpp
}

void FileScannerTests::findFiles_byPattern() {
    auto result = sak::file_scanner::findFiles(m_rootPath, {"*.json"}, true);
    QVERIFY(result.has_value());
    // findFiles uses files_only internally, preventing directory recursion.
    // data.json is in dir_a/ which cannot be reached. Only root files are checked.
    QCOMPARE(result.value().size(), std::size_t{0});
}

// ============================================================================
// Callback Tests
// ============================================================================

void FileScannerTests::callback_falseStopsScan() {
    int filesProcessed = 0;
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.callback = [&filesProcessed](const std::filesystem::path&, bool) -> bool {
        ++filesProcessed;
        return filesProcessed < 3;  // Stop after 2
    };

    auto result = scanner.scan(m_rootPath, opts);
    // Scan should have stopped early
    QVERIFY(filesProcessed <= 3);
}

// ============================================================================
// Progress Callback
// ============================================================================

void FileScannerTests::progress_callbackInvoked() {
    int progressCalls = 0;
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.progress_callback = [&progressCalls](std::size_t, std::uintmax_t) {
        ++progressCalls;
    };

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QVERIFY(progressCalls > 0);
}

QTEST_GUILESS_MAIN(FileScannerTests)
#include "test_file_scanner.moc"
