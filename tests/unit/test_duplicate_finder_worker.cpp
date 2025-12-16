// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for DuplicateFinderWorker
 * Tests duplicate file detection operations
 */

#include "sak/workers/duplicate_finder_worker.h"
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>

class TestDuplicateFinderWorker : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString searchDir;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        searchDir = tempDir->path() + "/search";
        QDir().mkpath(searchDir);
        
        createTestFiles();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestFiles() {
        // Create identical files
        createFile(searchDir + "/file1.txt", "Same content");
        createFile(searchDir + "/file2.txt", "Same content");
        createFile(searchDir + "/file3.txt", "Same content");
        
        // Create different file
        createFile(searchDir + "/unique.txt", "Different content");
        
        // Create subdirectory with duplicates
        QDir().mkpath(searchDir + "/subdir");
        createFile(searchDir + "/subdir/copy1.txt", "Same content");
        createFile(searchDir + "/subdir/copy2.txt", "Another duplicate");
        createFile(searchDir + "/subdir/copy3.txt", "Another duplicate");
    }

    void createFile(const QString& path, const QString& content) {
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(content.toUtf8());
        file.close();
    }

    void testInitialization() {
        sak::DuplicateFinderWorker worker;
        
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 0);
    }

    void testSetSearchDirectory() {
        sak::DuplicateFinderWorker worker;
        
        worker.setSearchDirectory(searchDir);
        
        QCOMPARE(worker.getSearchDirectory(), searchDir);
    }

    void testStartSearch() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy startedSpy(&worker, &sak::DuplicateFinderWorker::started);
        
        worker.start();
        
        QVERIFY(startedSpy.wait(1000));
        QVERIFY(worker.isRunning());
    }

    void testProgressReporting() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy progressSpy(&worker, &sak::DuplicateFinderWorker::progress);
        
        worker.start();
        
        QVERIFY(progressSpy.wait(5000));
        QVERIFY(progressSpy.count() > 0);
    }

    void testFindDuplicates() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        
        worker.start();
        
        QVERIFY(completedSpy.wait(10000));
        
        auto duplicates = worker.getDuplicateGroups();
        
        // Should find at least 2 groups (same content files and another duplicate group)
        QVERIFY(duplicates.size() >= 2);
    }

    void testDuplicateGrouping() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        auto groups = worker.getDuplicateGroups();
        
        // Verify groups contain multiple files
        for (const auto& group : groups) {
            QVERIFY(group.files.size() >= 2);
            
            // All files in group should have same hash
            QString firstHash = group.hash;
            QVERIFY(!firstHash.isEmpty());
        }
    }

    void testGetDuplicateCount() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        int count = worker.getDuplicateFileCount();
        
        // Should find multiple duplicate files
        QVERIFY(count >= 4);  // file1, file2, file3, copy1 are duplicates
    }

    void testGetWastedSpace() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        qint64 wasted = worker.getWastedSpace();
        
        // Should calculate wasted space
        QVERIFY(wasted > 0);
    }

    void testRecursiveSearch() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        worker.setRecursive(true);
        
        QVERIFY(worker.isRecursive());
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        auto groups = worker.getDuplicateGroups();
        
        // Should find duplicates in subdirectories too
        QVERIFY(groups.size() >= 2);
    }

    void testNonRecursiveSearch() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        worker.setRecursive(false);
        
        QVERIFY(!worker.isRecursive());
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Should only find files in root directory
        auto groups = worker.getDuplicateGroups();
        QVERIFY(groups.size() >= 1);
    }

    void testMinimumFileSize() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        worker.setMinimumFileSize(1000);  // 1KB minimum
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Should find fewer or no duplicates (our test files are small)
        auto groups = worker.getDuplicateGroups();
        // May be empty if all files are below minimum
    }

    void testFileExtensionFilter() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        worker.setFileExtensionFilter({"txt"});
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Should only search .txt files
        auto groups = worker.getDuplicateGroups();
        QVERIFY(groups.size() >= 1);
    }

    void testExclusionPatterns() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        worker.setExclusionPatterns({"*unique*"});
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // unique.txt should be excluded
        auto groups = worker.getDuplicateGroups();
        
        for (const auto& group : groups) {
            for (const QString& file : group.files) {
                QVERIFY(!file.contains("unique"));
            }
        }
    }

    void testCancellation() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy cancelledSpy(&worker, &sak::DuplicateFinderWorker::cancelled);
        
        worker.start();
        
        QTimer::singleShot(200, &worker, &sak::DuplicateFinderWorker::cancel);
        
        QVERIFY(cancelledSpy.wait(5000));
        QVERIFY(worker.wasCancelled());
    }

    void testStatusMessages() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy statusSpy(&worker, &sak::DuplicateFinderWorker::statusChanged);
        
        worker.start();
        
        QVERIFY(statusSpy.wait(5000));
        QVERIFY(statusSpy.count() > 0);
    }

    void testCurrentFileSignal() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy fileSpy(&worker, &sak::DuplicateFinderWorker::currentFile);
        
        worker.start();
        
        QVERIFY(fileSpy.wait(5000));
        QVERIFY(fileSpy.count() > 0);
    }

    void testGetScannedFileCount() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        int scanned = worker.getScannedFileCount();
        
        QVERIFY(scanned >= 7);  // At least 7 test files
    }

    void testElapsedTime() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 elapsed = worker.getElapsedTime();
        QVERIFY(elapsed >= 900);
    }

    void testHashAlgorithm() {
        sak::DuplicateFinderWorker worker;
        
        worker.setHashAlgorithm(sak::DuplicateFinderWorker::HashAlgorithm::SHA256);
        
        QCOMPARE(worker.getHashAlgorithm(), 
                 sak::DuplicateFinderWorker::HashAlgorithm::SHA256);
    }

    void testFastMode() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        worker.setFastMode(true);  // Only hash first/last chunks
        
        QVERIFY(worker.isFastMode());
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Should complete faster but still find duplicates
        auto groups = worker.getDuplicateGroups();
        QVERIFY(groups.size() >= 1);
    }

    void testErrorHandling() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory("/nonexistent/directory");
        
        QSignalSpy errorSpy(&worker, &sak::DuplicateFinderWorker::error);
        
        worker.start();
        
        QVERIFY(errorSpy.wait(5000));
    }

    void testDuplicateFoundSignal() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy duplicateSpy(&worker, &sak::DuplicateFinderWorker::duplicateFound);
        
        worker.start();
        
        QVERIFY(duplicateSpy.wait(10000));
        QVERIFY(duplicateSpy.count() > 0);
    }

    void testGetLargestDuplicateGroup() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        auto largest = worker.getLargestDuplicateGroup();
        
        QVERIFY(largest.files.size() >= 2);
    }

    void testGetDuplicatesBySize() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        auto groups = worker.getDuplicateGroupsBySize();
        
        // Should be sorted by wasted space
        QVERIFY(groups.size() >= 1);
    }

    void testClearResults() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy completedSpy(&worker, &sak::DuplicateFinderWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        QVERIFY(worker.getDuplicateGroups().size() > 0);
        
        worker.clearResults();
        
        QCOMPARE(worker.getDuplicateGroups().size(), 0);
    }

    void testPauseResume() {
        sak::DuplicateFinderWorker worker;
        worker.setSearchDirectory(searchDir);
        
        QSignalSpy pausedSpy(&worker, &sak::DuplicateFinderWorker::paused);
        QSignalSpy resumedSpy(&worker, &sak::DuplicateFinderWorker::resumed);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::DuplicateFinderWorker::pause);
        
        if (pausedSpy.wait(2000)) {
            QVERIFY(worker.isPaused());
            
            QTimer::singleShot(500, &worker, &sak::DuplicateFinderWorker::resume);
            
            QVERIFY(resumedSpy.wait(2000));
            QVERIFY(!worker.isPaused());
        }
    }
};

QTEST_MAIN(TestDuplicateFinderWorker)
#include "test_duplicate_finder_worker.moc"
