// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_duplicate_finder_worker.cpp
/// @brief Unit tests for DuplicateFinderWorker (TST-11)

#include "sak/duplicate_finder_worker.h"

#include <QDir>
#include <QFile>
#include <QScopeGuard>
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

    /// Config that drives the raw/image (FileManagementFileSystemBridge) scan path against a
    /// LOCAL target, so the untrusted-image walk can be exercised without a real disk image.
    static DuplicateFinderWorker::Config makeVirtualConfig(const QString& root_path) {
        DuplicateFinderWorker::Config config;
        config.use_file_system_target = true;
        config.file_system_target.label = QStringLiteral("unit-target");
        config.file_system_target.root_path = root_path;
        config.file_system_target.local_file_system = true;
        config.file_system_target.can_duplicate_scan = true;
        config.minimum_file_size = 0;
        config.recursive_scan = true;
        return config;
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
        // Assert the absolute count rather than spy.wait(): QSignalSpy connects with
        // Qt::DirectConnection, so the worker thread records finished the instant it fires --
        // frequently before the main thread reaches this line -- and wait() then blocks for a
        // SECOND emission that never comes.
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 10'000);

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
        // The lock has to outlive the scan but still be released if an assertion below returns
        // early, otherwise QTemporaryDir cannot remove locked.bin.
        const auto release = qScopeGuard([handle]() { CloseHandle(handle); });

        DuplicateFinderWorker::Config config;
        config.scanDirectories << tmpDir.path();
        config.minimum_file_size = 0;
        config.recursive_scan = false;
        DuplicateFinderWorker worker(config);

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 10'000);

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
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 10'000);

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
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 10'000);

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
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 10'000);

        QCOMPARE(duplicateCount, 1);
    }

    void cancellationFlag() {
        DuplicateFinderWorker::Config config;
        config.scanDirectories << "C:\\nonexistent";
        DuplicateFinderWorker worker(config);
        worker.requestStop();
        QVERIFY(worker.stopRequested());
    }

    // ------------------------------------------------------------------
    // R5 p3_actions-31: the raw/image walk is bounded and never defaults its roots
    // ------------------------------------------------------------------

    void virtualWalkWithinBounds_capsDepthDirectoriesAndFiles() {
        using W = DuplicateFinderWorker;
        // A normal walk is inside every budget.
        QVERIFY(W::virtualWalkWithinBounds(0, 0, 0));
        QVERIFY(W::virtualWalkWithinBounds(W::kVirtualWalkMaxDepth,
                                           W::kVirtualWalkMaxDirectories - 1,
                                           W::kVirtualWalkMaxFiles - 1));
        // One past ANY bound refuses -- a crafted image must not recurse or accumulate
        // without limit just because the other two budgets still have room.
        QVERIFY(!W::virtualWalkWithinBounds(W::kVirtualWalkMaxDepth + 1, 0, 0));
        QVERIFY(!W::virtualWalkWithinBounds(0, W::kVirtualWalkMaxDirectories, 0));
        QVERIFY(!W::virtualWalkWithinBounds(0, 0, W::kVirtualWalkMaxFiles));
        // A nonsensical depth fails closed rather than being read as "shallow".
        QVERIFY(!W::virtualWalkWithinBounds(-1, 0, 0));
    }

    void resolveVirtualRoots_dropsBlanksAndNeverDefaults() {
        QVector<QString> configured{QStringLiteral("  /Users  "),
                                    QString(),
                                    QStringLiteral("   "),
                                    QStringLiteral("/Data")};
        const QVector<QString> roots = DuplicateFinderWorker::resolveVirtualRoots(configured);
        QCOMPARE(roots.size(), 2);
        QCOMPARE(roots.at(0), QStringLiteral("/Users"));
        QCOMPARE(roots.at(1), QStringLiteral("/Data"));

        // Nothing usable resolves to NOTHING -- never to the image root "/". The caller
        // refuses the scan on this empty result.
        QVERIFY(DuplicateFinderWorker::resolveVirtualRoots({}).isEmpty());
        QVERIFY(DuplicateFinderWorker::resolveVirtualRoots({QStringLiteral("  ")}).isEmpty());
    }

    // A scan whose roots resolve to nothing must be REFUSED, not silently retargeted at
    // the file-system root and walked without bound.
    void virtualScanWithNoRootsIsRefused() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        DuplicateFinderWorker::Config config = makeVirtualConfig(tmpDir.path());
        config.virtual_directories = {QStringLiteral("   ")};  // resolves to nothing
        DuplicateFinderWorker worker(config);

        QSignalSpy failedSpy(&worker, &WorkerBase::failed);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 10'000);
        QCOMPARE(failedSpy.takeFirst().at(0).toInt(),
                 static_cast<int>(sak::error_code::invalid_argument));
    }

    // A hierarchy deeper than the walk bound (a corrupt/crafted image can be cyclic or
    // arbitrarily deep) refuses the scan instead of recursing until the stack is gone.
    void virtualScanDeeperThanBoundIsRefused() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        const QString root = QDir(tmpDir.path()).filePath("deep");
        QString chain = root;
        for (int i = 0; i <= DuplicateFinderWorker::kVirtualWalkMaxDepth; ++i) {
            chain = QDir(chain).filePath("d");
        }
        QVERIFY(QDir().mkpath(chain));

        DuplicateFinderWorker::Config config = makeVirtualConfig(tmpDir.path());
        config.virtual_directories = {root};
        DuplicateFinderWorker worker(config);

        QSignalSpy failedSpy(&worker, &WorkerBase::failed);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 30'000);
        QCOMPARE(failedSpy.takeFirst().at(0).toInt(),
                 static_cast<int>(sak::error_code::scan_failed));
    }

    // Control: the bound does not break a normal (shallow) image scan.
    void virtualScanWithinBoundStillFindsDuplicates() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        const QString root = QDir(tmpDir.path()).filePath("shallow");
        QVERIFY(QDir().mkpath(QDir(root).filePath("sub")));
        const QByteArray content("duplicate content inside the virtual target");
        createFile(root, "a.bin", content);
        createFile(QDir(root).filePath("sub"), "b.bin", content);

        DuplicateFinderWorker::Config config = makeVirtualConfig(tmpDir.path());
        config.virtual_directories = {root};
        DuplicateFinderWorker worker(config);

        int duplicateCount = -1;
        connect(&worker,
                &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy finishedSpy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 30'000);
        QCOMPARE(duplicateCount, 1);
    }
};

QTEST_MAIN(DuplicateFinderWorkerTests)
#include "test_duplicate_finder_worker.moc"
