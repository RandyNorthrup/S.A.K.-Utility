// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_duplicate_finder_worker.cpp
/// @brief Unit tests for DuplicateFinderWorker (TST-11)

#include "sak/duplicate_finder_worker.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#ifdef _WIN32
#include <windows.h>
#endif

class DuplicateFinderWorkerTests : public QObject {
    Q_OBJECT

private:
    void createFile(const QString& dir, const QString& name, const QByteArray& content) {
        QFile f(QDir(dir).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    }

private Q_SLOTS:
    void findsExactDuplicates() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QByteArray content("This is duplicate content for testing purposes.");
        createFile(tmpDir.path(), "file_a.txt", content);
        createFile(tmpDir.path(), "file_b.txt", content);
        createFile(tmpDir.path(), "unique.txt", "This is unique.");

        DuplicateFinderWorker::Config config;
        config.scanDirectories << tmpDir.path();
        config.minimum_file_size = 0;
        config.recursive_scan = false;
        DuplicateFinderWorker worker(config);

        int duplicateCount = 0;
        qint64 wastedSpace = 0;
        connect(&worker,
                &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64 wasted) {
                    duplicateCount = count;
                    wastedSpace = wasted;
                });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10'000));

        QCOMPARE(duplicateCount, 1);
        QCOMPARE(wastedSpace, static_cast<qint64>(content.size()));
        // A clean scan hashed every file: nothing dropped (no false-positive count).
        QCOMPARE(worker.filesUnhashed(), 0);
    }

    // B6-21: a file that cannot be hashed (here: exclusively locked) must be
    // COUNTED as unhashed, not silently dropped so the scan looks complete.
    // (Windows-only mechanism; this codebase targets Windows/MSVC.)
    void unhashableLockedFileIsCounted() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        createFile(tmpDir.path(), "readable.txt", "readable content here");
        const QString lockedPath = QDir(tmpDir.path()).filePath("locked.bin");
        createFile(tmpDir.path(), "locked.bin", "locked payload xyz");

        // Open with NO sharing, so any subsequent read open (the hasher's) fails.
        const std::wstring wpath = lockedPath.toStdWString();
        HANDLE handle = CreateFileW(wpath.c_str(),
                                    GENERIC_READ,
                                    0,  // dwShareMode = 0 -> exclusive
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);

        DuplicateFinderWorker::Config config;
        config.scanDirectories << tmpDir.path();
        config.minimum_file_size = 0;
        config.recursive_scan = false;
        DuplicateFinderWorker worker(config);

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        const bool finished = spy.wait(10'000);
        CloseHandle(handle);
        QVERIFY(finished);

        // The locked file could not be hashed -> surfaced, not hidden.
        QCOMPARE(worker.filesUnhashed(), 1);
    }

    void noDuplicatesWhenAllUnique() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        createFile(tmpDir.path(), "alpha.txt", "alpha content");
        createFile(tmpDir.path(), "beta.txt", "beta content");
        createFile(tmpDir.path(), "gamma.txt", "gamma content");

        DuplicateFinderWorker::Config config;
        config.scanDirectories << tmpDir.path();
        config.minimum_file_size = 0;
        config.recursive_scan = false;
        DuplicateFinderWorker worker(config);

        int duplicateCount = -1;
        connect(&worker,
                &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10'000));

        QCOMPARE(duplicateCount, 0);
    }

    void respectsMinimumFileSize() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QByteArray small("small");
        createFile(tmpDir.path(), "s1.txt", small);
        createFile(tmpDir.path(), "s2.txt", small);

        DuplicateFinderWorker::Config config;
        config.scanDirectories << tmpDir.path();
        config.minimum_file_size = 1024;
        config.recursive_scan = false;
        DuplicateFinderWorker worker(config);

        int duplicateCount = -1;
        connect(&worker,
                &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10'000));

        QCOMPARE(duplicateCount, 0);
    }

    void recursiveScanFindsInSubdirs() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QDir(tmpDir.path()).mkdir("sub");
        QByteArray content("duplicate in subdirectory");
        createFile(tmpDir.path(), "root.txt", content);
        createFile(QDir(tmpDir.path()).filePath("sub"), "sub.txt", content);

        DuplicateFinderWorker::Config config;
        config.scanDirectories << tmpDir.path();
        config.minimum_file_size = 0;
        config.recursive_scan = true;
        DuplicateFinderWorker worker(config);

        int duplicateCount = -1;
        connect(&worker,
                &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10'000));

        QCOMPARE(duplicateCount, 1);
    }

    void cancellationFlag() {
        DuplicateFinderWorker::Config config;
        config.scanDirectories << "C:\\nonexistent";
        DuplicateFinderWorker worker(config);
        worker.requestStop();
        QVERIFY(worker.stopRequested());
    }
};

QTEST_MAIN(DuplicateFinderWorkerTests)
#include "test_duplicate_finder_worker.moc"
