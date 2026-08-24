// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_scanner.cpp
/// @brief Unit tests for recursive file scanning with filtering

#include "sak/error_codes.h"
#include "sak/file_scanner.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <filesystem>
#include <stop_token>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

    // Symlink policy
    void symlink_notFollowedWhenDisabled();
    void junction_notFollowedByDefault();
    void canDescendInto_refusesWhenCanonicalUnresolved();

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
    writeFile("dir_a/data.json", R"({"key":"val"})");    // 13 bytes
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
    // Tree has exactly four subdirs (dir_a, dir_b, dir_c, dir_b/nested); root is not counted.
    QCOMPARE(result.value().directories_found, std::size_t{4});
    // Exact byte sum of the six fixtures: 5 + 13 + 13 + 7 + 100 + 9 (raw writes, no CRLF xlate).
    QCOMPARE(result.value().total_size, std::uintmax_t{147});
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
    // files_only filters the emitted entries to regular files but does not stop
    // directory recursion, so all six files across the tree are returned.
    opts.type_filter = sak::file_type_filter::files_only;

    auto result = scanner.scanAndCollect(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().size(), std::size_t{6});
    // Identity, not just arity: the six collected paths are the six fixture FILES, each exactly
    // once, at its real location in the tree. A size-only check stayed green if the collector
    // pushed the containing directory (or the same entry twice) for every emission.
    QStringList collected;
    for (const auto& collected_path : result.value()) {
        collected << QString::fromStdString(
            collected_path.lexically_relative(m_rootPath).generic_string());
    }
    collected.sort();
    QCOMPARE(collected,
             (QStringList{QStringLiteral("dir_a/data.json"),
                          QStringLiteral("dir_a/readme.md"),
                          QStringLiteral("dir_b/image.png"),
                          QStringLiteral("dir_b/nested/deep.log"),
                          QStringLiteral("file1.txt"),
                          QStringLiteral("file2.cpp")}));
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
    // The name is excluded by TWO independent guards -- emission (shouldProcessEntry) and descent
    // (canDescendInto). files_found only proves the descent guard: with the emission guard deleted
    // dir_b is still not walked, so the count stays 4 while dir_b is wrongly REPORTED as a found
    // directory and passed to the callback. Exactly one entry (dir_b itself) is filtered out.
    QCOMPARE(result.value().skipped_by_filter, std::size_t{1});
    // Bytes follow the same exclusion: 5 + 13 + 13 + 7, with dir_b's 109 bytes absent.
    QCOMPARE(result.value().total_size, std::uintmax_t{38});
}

void FileScannerTests::fileSizeFilter_minMax() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.min_file_size = 10;
    opts.max_file_size = 50;

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // Files 10-50 bytes: file2.cpp(13), dir_a/data.json(13); 5/7/9 below min, 100 above max.
    QCOMPARE(result.value().files_found, std::size_t{2});
    // total_size is the sum over the files that PASSED the filter, not the tree total: 13 + 13.
    // A scan that accumulated bytes before the size filter reports 147 and kept files_found == 2.
    QCOMPARE(result.value().total_size, std::uintmax_t{26});
    // The four rejected files are seen-and-rejected, not never-enumerated.
    QCOMPARE(result.value().skipped_by_filter, std::size_t{4});
}

void FileScannerTests::maxDepth_limitsRecursion() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.max_depth = 1;  // Only root and immediate children

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // Only root-level files: file1.txt, file2.cpp = 2
    QCOMPARE(result.value().files_found, std::size_t{2});
    // The depth test gates BOTH emission (shouldProcessEntry) and descent (canDescendInto).
    // Exactly the four depth-1 entries (data.json, readme.md, image.png, nested) are enumerated
    // and rejected; a walk that kept descending and leaned on the emission filter alone also
    // reaches dir_b/nested and records 5.
    QCOMPARE(result.value().skipped_by_filter, std::size_t{4});
    // Bytes come only from the two emitted root files: 5 + 13.
    QCOMPARE(result.value().total_size, std::uintmax_t{18});
}

void FileScannerTests::typeFilter_filesOnly() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.type_filter = sak::file_type_filter::files_only;

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // Directories are not emitted under files_only (so the count stays 0) but
    // are still traversed, so every regular file in the tree is found.
    QCOMPARE(result.value().directories_found, std::size_t{0});
    QCOMPARE(result.value().files_found, std::size_t{6});
}

void FileScannerTests::typeFilter_dirsOnly() {
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.type_filter = sak::file_type_filter::directories_only;
    // scan_emptyDirectory() adds "empty_dir" to the shared root, so exclude it here to keep the
    // catalog independent of test-function execution order (5 in-class vs 4 standalone today).
    opts.exclude_dirs = {"empty_dir"};

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{0});
    // Exact catalog: dir_a, dir_b, dir_c and the nested dir_b/nested.
    QCOMPARE(result.value().directories_found, std::size_t{4});
    // The six regular files plus the excluded empty_dir are enumerated and REJECTED, so they must
    // be counted as filtered rather than silently unvisited.
    QCOMPARE(result.value().skipped_by_filter, std::size_t{7});
    // Bytes are accumulated only for emitted files (updateFileStats), so a directories_only scan
    // reports zero; an accumulation moved ahead of the filter would report the tree's 147.
    QCOMPARE(result.value().total_size, std::uintmax_t{0});
}

// ============================================================================
// Cancellation
// ============================================================================

void FileScannerTests::cancellation_stopsScan() {
    std::stop_source stopSource;
    stopSource.request_stop();

    sak::file_scanner scanner;
    sak::scan_options opts;
    int entriesSeen = 0;
    opts.callback = [&entriesSeen](const std::filesystem::path&, bool) -> bool {
        ++entriesSeen;
        return true;
    };

    auto result = scanner.scan(m_rootPath, opts, stopSource.get_token());
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::operation_cancelled);
    // An already-requested stop must abort BEFORE the first directory is enumerated. scan() also
    // checks the token AFTER the walk, so deleting the pre-enumeration and per-entry checks still
    // yields operation_cancelled: the error code alone never proved the scan stopped early.
    QCOMPARE(entriesSeen, 0);
}

// ============================================================================
// Static Convenience Functions
// ============================================================================

void FileScannerTests::listFiles_recursive() {
    auto result = sak::file_scanner::listFiles(m_rootPath, true);
    QVERIFY(result.has_value());
    // Recursive listing returns every file in the tree (files_only filters the
    // emitted entries, not the traversal).
    QCOMPARE(result.value().size(), std::size_t{6});
}

void FileScannerTests::listFiles_nonRecursive() {
    auto result = sak::file_scanner::listFiles(m_rootPath, false);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().size(), std::size_t{2});  // file1.txt, file2.cpp
}

void FileScannerTests::findFiles_byPattern() {
    auto result = sak::file_scanner::findFiles(m_rootPath, {"*.json"}, true);
    QVERIFY(result.has_value());
    // Recursive find now reaches dir_a/data.json.
    QCOMPARE(result.value().size(), std::size_t{1});
    // WHICH file: the one match is the nested data.json, returned as a full path under the root.
    QCOMPARE(QString::fromStdString(
                 result.value().front().lexically_relative(m_rootPath).generic_string()),
             QStringLiteral("dir_a/data.json"));

    // Other arm of the same entry point: an EMPTY pattern list must fail closed with
    // invalid_argument. Left unchecked it would leave include_patterns empty, which the scan
    // reads as "no filter" and answers a filtered query with every file in the tree.
    auto no_patterns = sak::file_scanner::findFiles(m_rootPath, {}, true);
    QVERIFY(!no_patterns.has_value());
    QCOMPARE(no_patterns.error(), sak::error_code::invalid_argument);
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
    // The callback returns false on its 3rd call (filesProcessed==3), which the scanner turns
    // into operation_cancelled and stops; the root holds >= 3 emittable entries, so exactly 3
    // callbacks fire. <= 3 also passed if the false-return were ignored; == 3 pins the abort.
    QCOMPARE(filesProcessed, 3);
    // The abort must also be REPORTED. A scanner that stopped the walk but handed back a
    // successful -- silently truncated -- statistics block also leaves filesProcessed at 3.
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::operation_cancelled);
}

// ============================================================================
// Progress Callback
// ============================================================================

void FileScannerTests::progress_callbackInvoked() {
    int progressCalls = 0;
    std::uintmax_t lastBytes = 0;
    sak::file_scanner scanner;
    sak::scan_options opts;
    opts.progress_callback = [&progressCalls, &lastBytes](std::size_t files, std::uintmax_t bytes) {
        ++progressCalls;
        // Both arguments are CUMULATIVE running totals, so the file count advances by exactly one
        // per call and the byte total strictly grows (every fixture file is non-empty). Both
        // parameters were unnamed, so only the ARITY of the invocation was ever checked: a
        // callback invoked with (0, 0), with a per-file size instead of the running total, or
        // with the two counters swapped stayed green.
        QCOMPARE(files, static_cast<std::size_t>(progressCalls));
        QVERIFY(bytes > lastBytes);
        lastBytes = bytes;
    };

    auto result = scanner.scan(m_rootPath, opts);
    QVERIFY(result.has_value());
    // progress_callback fires once per regular file, unthrottled, size-gating aside; all six
    // fixtures emit -> exactly 6. > 0 passed on a single stray fire.
    QCOMPARE(progressCalls, 6);
}

void FileScannerTests::symlink_notFollowedWhenDisabled() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();

    // Real subdirectory with one file, plus a symlink pointing back at the root
    // (an ancestor). Following it would either escape the scan or recurse without
    // bound.
    std::filesystem::create_directory(root / "payload");
    {
        QFile f(QString::fromStdWString((root / "payload" / "p.txt").wstring()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("data");
        f.close();
    }
    std::error_code link_ec;
    std::filesystem::create_directory_symlink(root, root / "loop", link_ec);
    if (link_ec) {
        QSKIP("Platform cannot create directory symlinks without privilege");
    }

    // Default (follow_symlinks=false): the symlinked directory is not traversed,
    // so p.txt is found exactly once and the scan terminates.
    sak::file_scanner scanner;
    sak::scan_options opts;
    auto result = scanner.scan(root, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{1});
    // The loop symlink is still EMITTED as a directory (is_directory() resolves it); only the
    // DESCENT is refused, so payload + loop = 2.
    QCOMPARE(result.value().directories_found, std::size_t{2});

    // follow_symlinks=true must still terminate (cycle guard) rather than hang.
    sak::file_scanner following_scanner;
    sak::scan_options following;
    following.follow_symlinks = true;
    auto followed = following_scanner.scan(root, following);
    QVERIFY(followed.has_value());
    // The cycle guard refuses to re-descend the loop symlink (canonical(root) already visited),
    // so p.txt is counted exactly once -- a terminate-but-double-count bug would fail this.
    QCOMPARE(followed.value().files_found, std::size_t{1});
    // WHICH arm refused matters: the silent cycle guard, or the fail-closed canonical() failure
    // that counts an error. Both stop the descent, so files_found cannot separate them.
    QCOMPARE(followed.value().errors_encountered, std::size_t{0});
    QVERIFY(followed.value().is_complete());
    // With the root seeding deleted the walk descends the loop once and re-emits payload+loop
    // (4 dirs) while p.txt is still counted exactly once -- invisible to files_found alone.
    QCOMPARE(followed.value().directories_found, std::size_t{2});
}

void FileScannerTests::junction_notFollowedByDefault() {
#ifdef _WIN32
    // B8-16: a Windows JUNCTION is a reparse point that directory_entry maps to
    // file_type::junction, so is_symlink() misses it. The default scan (follow off)
    // must still refuse to descend into it -- otherwise a junction pointing at an
    // ancestor steers the walk out of the scan root and loops. Junctions need no
    // privilege to create (unlike symlinks), so this always runs on Windows.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();
    std::filesystem::create_directory(root / "payload");
    {
        QFile f(QString::fromStdWString((root / "payload" / "p.txt").wstring()));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("data");
        f.close();
    }

    // Create a junction "jloop" pointing back at the scan root (an ancestor).
    // mklink needs native backslash separators.
    const QString link =
        QDir::toNativeSeparators(QString::fromStdWString((root / "jloop").wstring()));
    const QString target = QDir::toNativeSeparators(QString::fromStdWString(root.wstring()));
    QProcess mklink;
    mklink.start(
        QStringLiteral("cmd"),
        {QStringLiteral("/c"), QStringLiteral("mklink"), QStringLiteral("/J"), link, target});
    QVERIFY(mklink.waitForFinished(10'000));
    QVERIFY2(mklink.exitCode() == 0,
             qPrintable(QStringLiteral("mklink /J failed: %1")
                            .arg(QString::fromLocal8Bit(mklink.readAllStandardError()))));

    // Default (follow_symlinks=false): the junction is NOT descended, so the scan
    // terminates and p.txt is found exactly once (not multiplied by loop levels).
    sak::file_scanner scanner;
    sak::scan_options opts;
    auto result = scanner.scan(root, opts);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().files_found, std::size_t{1});
    // The junction is refused at the DESCENT gate only: it is still enumerated and emitted as a
    // directory (directory_entry::is_directory() resolves the reparse point), so payload + jloop
    // = 2. skip_symlinks (default off) is the separate option that drops such entries entirely,
    // and a regression that turned it on -- or one that counted the refusal as an error -- keeps
    // files_found at 1.
    QCOMPARE(result.value().directories_found, std::size_t{2});
    QCOMPARE(result.value().errors_encountered, std::size_t{0});
    QVERIFY(result.value().is_complete());
#else
    QSKIP("Junctions are a Windows-only concept");
#endif
}

void FileScannerTests::canDescendInto_refusesWhenCanonicalUnresolved() {
#ifdef Q_OS_WIN
    // A follow_symlinks scan breaks cycles by CANONICAL identity. When canonical() fails there
    // is no identity to register, so descending anyway traverses the subtree with the cycle and
    // confinement guard switched off -- exactly what a planted, unreadable junction wants. The
    // gate must fail closed (refuse) and report the error instead of walking blind.
    //
    // The failure is reproduced without any privilege: hold the directory open with dwShareMode
    // 0, so every later open (canonical() opens the file to resolve its final path) fails with
    // ERROR_SHARING_VIOLATION, while the cached directory attributes still report a directory.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::filesystem::path root = dir.path().toStdWString();
    const std::filesystem::path child = root / "locked";
    std::error_code create_ec;
    std::filesystem::create_directory(child, create_ec);
    QVERIFY(!create_ec);

    sak::scan_options opts;
    opts.follow_symlinks = true;
    const std::filesystem::directory_entry entry(child);

    // Control: canonical() resolves, so a first visit is allowed and nothing is counted as an
    // error. This pins that the refusal below comes from the canonicalization failure alone.
    sak::file_scanner open_scanner;
    sak::scan_statistics open_stats;
    QVERIFY(open_scanner.canDescendInto(entry, opts, open_stats, 0));
    QCOMPARE(open_stats.errors_encountered, std::size_t{0});

    const HANDLE locked = CreateFileW(child.wstring().c_str(),
                                      GENERIC_READ,
                                      0,  // no sharing: every subsequent open is refused
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS,
                                      nullptr);
    QVERIFY(locked != INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([locked]() { CloseHandle(locked); });

    std::error_code canonical_ec;
    const std::filesystem::path canonical_probe = std::filesystem::canonical(child, canonical_ec);
    Q_UNUSED(canonical_probe);
    if (!canonical_ec) {
        QSKIP("Platform resolves canonical() on an exclusively opened directory");
    }

    // Reuse the entry captured before the lock: the gate only reads entry.path() and the
    // directory attributes, so this is the same shape a live walk hands it (the directory
    // became unreadable after enumeration, exactly the TOCTOU a planted link exploits).
    sak::file_scanner blocked_scanner;
    sak::scan_statistics blocked_stats;
    QVERIFY(!blocked_scanner.canDescendInto(entry, opts, blocked_stats, 0));
    QCOMPARE(blocked_stats.errors_encountered, std::size_t{1});
#else
    QSKIP("Exclusive directory handles are a Windows-only concept");
#endif
}

QTEST_GUILESS_MAIN(FileScannerTests)
#include "test_file_scanner.moc"
