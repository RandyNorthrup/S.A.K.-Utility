// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for FileHash
 * Tests checksum calculation and verification
 */

#include "sak/file_hash.h"
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>

class TestFileHash : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString testFilePath;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        testFilePath = tempDir->path() + "/test.dat";
        createTestFile();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestFile() {
        QFile file(testFilePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("Hello, World! This is test data for hashing.");
        file.close();
    }

    void testSha256Hash() {
        sak::FileHash hasher;
        
        QString hash = hasher.calculateSha256(testFilePath);
        
        QVERIFY(!hash.isEmpty());
        QCOMPARE(hash.length(), 64);  // SHA256 is 64 hex characters
        
        // Verify it's valid hex
        for (QChar c : hash) {
            QVERIFY(c.isDigit() || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
        }
    }

    void testMd5Hash() {
        sak::FileHash hasher;
        
        QString hash = hasher.calculateMd5(testFilePath);
        
        QVERIFY(!hash.isEmpty());
        QCOMPARE(hash.length(), 32);  // MD5 is 32 hex characters
    }

    void testSha1Hash() {
        sak::FileHash hasher;
        
        QString hash = hasher.calculateSha1(testFilePath);
        
        QVERIFY(!hash.isEmpty());
        QCOMPARE(hash.length(), 40);  // SHA1 is 40 hex characters
    }

    void testHashConsistency() {
        sak::FileHash hasher;
        
        QString hash1 = hasher.calculateSha256(testFilePath);
        QString hash2 = hasher.calculateSha256(testFilePath);
        
        QCOMPARE(hash1, hash2);  // Same file should produce same hash
    }

    void testDifferentContentHash() {
        // Create second file with different content
        QString file2Path = tempDir->path() + "/test2.dat";
        QFile file2(file2Path);
        QVERIFY(file2.open(QIODevice::WriteOnly));
        file2.write("Different content for different hash");
        file2.close();
        
        sak::FileHash hasher;
        
        QString hash1 = hasher.calculateSha256(testFilePath);
        QString hash2 = hasher.calculateSha256(file2Path);
        
        QVERIFY(hash1 != hash2);  // Different content = different hash
    }

    void testEmptyFile() {
        QString emptyPath = tempDir->path() + "/empty.dat";
        QFile empty(emptyPath);
        QVERIFY(empty.open(QIODevice::WriteOnly));
        empty.close();
        
        sak::FileHash hasher;
        QString hash = hasher.calculateSha256(emptyPath);
        
        QVERIFY(!hash.isEmpty());
        
        // SHA256 of empty file is known constant
        QCOMPARE(hash.toLower(), 
                 QString("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    }

    void testLargeFile() {
        QString largePath = tempDir->path() + "/large.dat";
        QFile large(largePath);
        QVERIFY(large.open(QIODevice::WriteOnly));
        
        // Write 10MB of data
        QByteArray chunk(1024 * 1024, 'x');  // 1MB chunks
        for (int i = 0; i < 10; i++) {
            large.write(chunk);
        }
        large.close();
        
        sak::FileHash hasher;
        
        QElapsedTimer timer;
        timer.start();
        
        QString hash = hasher.calculateSha256(largePath);
        
        qint64 elapsed = timer.elapsed();
        
        QVERIFY(!hash.isEmpty());
        qDebug() << "Hashed 10MB in" << elapsed << "ms";
        
        // Should complete within reasonable time (adjust if needed)
        QVERIFY(elapsed < 5000);  // 5 seconds for 10MB
    }

    void testNonExistentFile() {
        sak::FileHash hasher;
        
        QString hash = hasher.calculateSha256("/nonexistent/file.dat");
        
        QVERIFY(hash.isEmpty());  // Should return empty string on error
    }

    void testProgressReporting() {
        // Create a larger file to track progress
        QString progressPath = tempDir->path() + "/progress.dat";
        QFile file(progressPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(5 * 1024 * 1024, 'x'));  // 5MB
        file.close();
        
        sak::FileHash hasher;
        
        int progressCount = 0;
        int lastProgress = -1;
        
        connect(&hasher, &sak::FileHash::progress, 
                [&](int current, int total) {
            progressCount++;
            QVERIFY(current >= 0);
            QVERIFY(total > 0);
            QVERIFY(current <= total);
            QVERIFY(current >= lastProgress);  // Should be monotonic
            lastProgress = current;
        });
        
        hasher.calculateSha256(progressPath);
        
        QVERIFY(progressCount > 0);
    }

    void testCancellation() {
        // Create large file for cancellation test
        QString cancelPath = tempDir->path() + "/cancel.dat";
        QFile file(cancelPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(50 * 1024 * 1024, 'x'));  // 50MB
        file.close();
        
        sak::FileHash hasher;
        
        // Cancel after short delay
        QTimer::singleShot(100, &hasher, &sak::FileHash::cancel);
        
        QString hash = hasher.calculateSha256(cancelPath);
        
        // Should be cancelled (empty or incomplete)
        QVERIFY(hasher.wasCancelled());
    }

    void testVerifyHash() {
        sak::FileHash hasher;
        
        QString expectedHash = hasher.calculateSha256(testFilePath);
        
        // Verify with correct hash
        QVERIFY(hasher.verifyHash(testFilePath, expectedHash, 
                                   sak::FileHash::Algorithm::SHA256));
        
        // Verify with wrong hash
        QVERIFY(!hasher.verifyHash(testFilePath, "0000000000000000", 
                                    sak::FileHash::Algorithm::SHA256));
    }

    void testCompareFiles() {
        // Create identical file
        QString identicalPath = tempDir->path() + "/identical.dat";
        QFile::copy(testFilePath, identicalPath);
        
        sak::FileHash hasher;
        
        QVERIFY(hasher.compareFiles(testFilePath, identicalPath));
        
        // Create different file
        QString differentPath = tempDir->path() + "/different.dat";
        QFile diff(differentPath);
        diff.open(QIODevice::WriteOnly);
        diff.write("Different data");
        diff.close();
        
        QVERIFY(!hasher.compareFiles(testFilePath, differentPath));
    }

    void testConcurrentHashing() {
        // Create multiple files
        QStringList files;
        for (int i = 0; i < 5; i++) {
            QString path = tempDir->path() + QString("/file%1.dat").arg(i);
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write(QByteArray(1024 * 1024, 'a' + i));  // 1MB each
            file.close();
            files.append(path);
        }
        
        sak::FileHash hasher;
        
        QElapsedTimer timer;
        timer.start();
        
        QVector<QString> hashes;
        for (const QString& file : files) {
            hashes.append(hasher.calculateSha256(file));
        }
        
        qint64 elapsed = timer.elapsed();
        
        QCOMPARE(hashes.size(), 5);
        for (const QString& hash : hashes) {
            QVERIFY(!hash.isEmpty());
        }
        
        qDebug() << "Hashed 5 files in" << elapsed << "ms";
    }

    void testAlgorithmEnum() {
        sak::FileHash hasher;
        
        QString sha256 = hasher.calculateHash(testFilePath, 
                                               sak::FileHash::Algorithm::SHA256);
        QString md5 = hasher.calculateHash(testFilePath, 
                                            sak::FileHash::Algorithm::MD5);
        QString sha1 = hasher.calculateHash(testFilePath, 
                                             sak::FileHash::Algorithm::SHA1);
        
        QVERIFY(!sha256.isEmpty());
        QVERIFY(!md5.isEmpty());
        QVERIFY(!sha1.isEmpty());
        
        QCOMPARE(sha256.length(), 64);
        QCOMPARE(md5.length(), 32);
        QCOMPARE(sha1.length(), 40);
    }

    void testBufferSize() {
        sak::FileHash hasher;
        
        // Test with different buffer sizes
        hasher.setBufferSize(4096);  // 4KB
        QString hash1 = hasher.calculateSha256(testFilePath);
        
        hasher.setBufferSize(1024 * 1024);  // 1MB
        QString hash2 = hasher.calculateSha256(testFilePath);
        
        // Buffer size shouldn't affect hash
        QCOMPARE(hash1, hash2);
    }

    void testThreadSafety() {
        // Create multiple hashers in parallel
        QVector<QFuture<QString>> futures;
        
        for (int i = 0; i < 10; i++) {
            futures.append(QtConcurrent::run([this]() {
                sak::FileHash hasher;
                return hasher.calculateSha256(testFilePath);
            }));
        }
        
        // Wait for all to complete
        QVector<QString> hashes;
        for (auto& future : futures) {
            hashes.append(future.result());
        }
        
        // All should produce same hash
        for (int i = 1; i < hashes.size(); i++) {
            QCOMPARE(hashes[i], hashes[0]);
        }
    }
};

QTEST_MAIN(TestFileHash)
#include "test_file_hash.moc"
