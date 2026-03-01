// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_duplicate_finder_worker.cpp
/// @brief Unit tests for DuplicateFinderWorker (TST-11)

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "sak/duplicate_finder_worker.h"

class DuplicateFinderWorkerTests : public QObject {
    Q_OBJECT

private:
    void createFile(const QString& dir, const QString& name, const QByteArray& content)
    {
        QFile f(QDir(dir).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    }

private Q_SLOTS:
    void findsExactDuplicates()
    {
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
        connect(&worker, &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64 wasted) {
                    duplicateCount = count;
                    wastedSpace = wasted;
                });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10000));

        QCOMPARE(duplicateCount, 1);
        QCOMPARE(wastedSpace, static_cast<qint64>(content.size()));
    }

    void noDuplicatesWhenAllUnique()
    {
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
        connect(&worker, &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10000));

        QCOMPARE(duplicateCount, 0);
    }

    void respectsMinimumFileSize()
    {
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
        connect(&worker, &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10000));

        QCOMPARE(duplicateCount, 0);
    }

    void recursiveScanFindsInSubdirs()
    {
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
        connect(&worker, &DuplicateFinderWorker::resultsReady,
                [&](const QString&, int count, qint64) { duplicateCount = count; });

        QSignalSpy spy(&worker, &DuplicateFinderWorker::finished);
        worker.start();
        QVERIFY(spy.wait(10000));

        QCOMPARE(duplicateCount, 1);
    }

    void cancellationFlag()
    {
        DuplicateFinderWorker::Config config;
        config.scanDirectories << "C:\\nonexistent";
        DuplicateFinderWorker worker(config);
        worker.requestStop();
        QVERIFY(worker.stopRequested());
    }
};

QTEST_MAIN(DuplicateFinderWorkerTests)
#include "test_duplicate_finder_worker.moc"
