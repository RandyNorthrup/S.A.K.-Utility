// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for FlashWorker
 * Tests USB device flashing operations
 */

#include "sak/workers/flash_worker.h"
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>

class TestFlashWorker : public QObject {
    Q_OBJECT

private:
    QTemporaryDir* tempDir;
    QString imageFile;

private slots:
    void initTestCase() {
        tempDir = new QTemporaryDir();
        QVERIFY(tempDir->isValid());
        
        imageFile = tempDir->path() + "/test.iso";
        createTestImage();
    }

    void cleanupTestCase() {
        delete tempDir;
    }

    void createTestImage() {
        QFile file(imageFile);
        file.open(QIODevice::WriteOnly);
        // Create 10MB test image
        file.write(QByteArray(10 * 1024 * 1024, 'x'));
        file.close();
    }

    void testInitialization() {
        sak::FlashWorker worker;
        
        QVERIFY(!worker.isRunning());
        QCOMPARE(worker.getProgress(), 0);
    }

    void testSetImageFile() {
        sak::FlashWorker worker;
        
        worker.setImageFile(imageFile);
        
        QCOMPARE(worker.getImageFile(), imageFile);
    }

    void testSetTargetDevice() {
        sak::FlashWorker worker;
        
        QString device = "/dev/sdb";  // Mock device
        worker.setTargetDevice(device);
        
        QCOMPARE(worker.getTargetDevice(), device);
    }

    void testImageValidation() {
        sak::FlashWorker worker;
        
        worker.setImageFile(imageFile);
        
        bool valid = worker.validateImage();
        QVERIFY(valid);
    }

    void testInvalidImageFile() {
        sak::FlashWorker worker;
        
        worker.setImageFile("/nonexistent/image.iso");
        
        bool valid = worker.validateImage();
        QVERIFY(!valid);
    }

    void testGetImageSize() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        
        qint64 size = worker.getImageSize();
        QCOMPARE(size, qint64(10 * 1024 * 1024));
    }

    void testDeviceCapacityCheck() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        
        // Mock device capacity
        worker.setMockDeviceCapacity(20 * 1024 * 1024);
        
        bool hasCapacity = worker.checkDeviceCapacity();
        QVERIFY(hasCapacity);
    }

    void testInsufficientCapacity() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        
        // Mock device too small
        worker.setMockDeviceCapacity(5 * 1024 * 1024);
        
        bool hasCapacity = worker.checkDeviceCapacity();
        QVERIFY(!hasCapacity);
    }

    void testDryRunMode() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QVERIFY(worker.isDryRun());
        
        QSignalSpy completedSpy(&worker, &sak::FlashWorker::completed);
        
        worker.start();
        
        // Dry run should complete quickly without actual writing
        QVERIFY(completedSpy.wait(5000));
    }

    void testProgressReporting() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QSignalSpy progressSpy(&worker, &sak::FlashWorker::progress);
        
        worker.start();
        
        QVERIFY(progressSpy.wait(5000));
        QVERIFY(progressSpy.count() > 0);
        
        // Progress should be valid
        for (const auto& signal : progressSpy) {
            int progress = signal.at(0).toInt();
            QVERIFY(progress >= 0);
            QVERIFY(progress <= 100);
        }
    }

    void testCancellation() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QSignalSpy cancelledSpy(&worker, &sak::FlashWorker::cancelled);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::FlashWorker::cancel);
        
        QVERIFY(cancelledSpy.wait(5000));
        QVERIFY(worker.wasCancelled());
    }

    void testVerificationEnabled() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setVerificationEnabled(true);
        worker.setDryRun(true);
        
        QVERIFY(worker.isVerificationEnabled());
        
        QSignalSpy verifiedSpy(&worker, &sak::FlashWorker::verified);
        
        worker.start();
        
        // Should emit verified signal after writing
        QVERIFY(verifiedSpy.wait(10000));
    }

    void testStatusMessages() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QSignalSpy statusSpy(&worker, &sak::FlashWorker::statusChanged);
        
        worker.start();
        
        QVERIFY(statusSpy.wait(5000));
        QVERIFY(statusSpy.count() > 0);
    }

    void testSpeedCalculation() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        worker.start();
        
        QTest::qWait(1000);
        
        double speed = worker.getCurrentSpeed();
        QVERIFY(speed >= 0);
    }

    void testElapsedTime() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 elapsed = worker.getElapsedTime();
        QVERIFY(elapsed >= 900);
    }

    void testRemainingTime() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        worker.start();
        
        QTest::qWait(1000);
        
        qint64 remaining = worker.getRemainingTime();
        // Remaining time should be calculated
        QVERIFY(remaining >= 0);
    }

    void testBytesWritten() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QSignalSpy completedSpy(&worker, &sak::FlashWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        qint64 written = worker.getBytesWritten();
        QVERIFY(written > 0);
    }

    void testErrorHandlingNoDevice() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        // No device set
        
        QSignalSpy errorSpy(&worker, &sak::FlashWorker::error);
        
        worker.start();
        
        QVERIFY(errorSpy.wait(5000));
    }

    void testErrorHandlingNoImage() {
        sak::FlashWorker worker;
        worker.setTargetDevice("mock_device");
        // No image set
        
        QSignalSpy errorSpy(&worker, &sak::FlashWorker::error);
        
        worker.start();
        
        QVERIFY(errorSpy.wait(5000));
    }

    void testDeviceLocking() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        worker.start();
        
        // Device should be locked during operation
        QTest::qWait(500);
        QVERIFY(worker.isDeviceLocked());
    }

    void testDeviceUnlockingAfterCompletion() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QSignalSpy completedSpy(&worker, &sak::FlashWorker::completed);
        worker.start();
        completedSpy.wait(10000);
        
        // Device should be unlocked after completion
        QVERIFY(!worker.isDeviceLocked());
    }

    void testBufferSize() {
        sak::FlashWorker worker;
        
        worker.setBufferSize(1024 * 1024);  // 1MB
        
        QCOMPARE(worker.getBufferSize(), 1024 * 1024);
    }

    void testSyncAfterWrite() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setSyncEnabled(true);
        worker.setDryRun(true);
        
        QVERIFY(worker.isSyncEnabled());
        
        QSignalSpy syncedSpy(&worker, &sak::FlashWorker::synced);
        
        worker.start();
        
        // Should emit synced signal after writing
        QVERIFY(syncedSpy.wait(10000));
    }

    void testPauseResume() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setDryRun(true);
        
        QSignalSpy pausedSpy(&worker, &sak::FlashWorker::paused);
        QSignalSpy resumedSpy(&worker, &sak::FlashWorker::resumed);
        
        worker.start();
        
        QTimer::singleShot(500, &worker, &sak::FlashWorker::pause);
        
        if (pausedSpy.wait(2000)) {
            QVERIFY(worker.isPaused());
            
            QTimer::singleShot(500, &worker, &sak::FlashWorker::resume);
            
            QVERIFY(resumedSpy.wait(2000));
            QVERIFY(!worker.isPaused());
        }
    }

    void testHashCalculation() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        
        QString hash = worker.calculateImageHash();
        
        QVERIFY(!hash.isEmpty());
        QCOMPARE(hash.length(), 64);  // SHA256
    }

    void testCompareHashes() {
        sak::FlashWorker worker;
        worker.setImageFile(imageFile);
        worker.setTargetDevice("mock_device");
        worker.setVerificationEnabled(true);
        worker.setDryRun(true);
        
        QSignalSpy verifiedSpy(&worker, &sak::FlashWorker::verified);
        worker.start();
        verifiedSpy.wait(10000);
        
        // In dry run, hashes should match
        QVERIFY(worker.hashesMatch());
    }

    void testMultipleFlashOperations() {
        // Test flashing same image to multiple devices (dry run)
        QVector<sak::FlashWorker*> workers;
        
        for (int i = 0; i < 3; i++) {
            auto* worker = new sak::FlashWorker(this);
            worker->setImageFile(imageFile);
            worker->setTargetDevice(QString("mock_device_%1").arg(i));
            worker->setDryRun(true);
            workers.append(worker);
        }
        
        // Start all workers
        for (auto* worker : workers) {
            worker->start();
        }
        
        // Wait for all to complete
        int completed = 0;
        QEventLoop loop;
        
        for (auto* worker : workers) {
            connect(worker, &sak::FlashWorker::completed, [&]() {
                completed++;
                if (completed == workers.size()) {
                    loop.quit();
                }
            });
        }
        
        QTimer::singleShot(15000, &loop, &QEventLoop::quit);
        loop.exec();
        
        QCOMPARE(completed, workers.size());
        
        qDeleteAll(workers);
    }
};

QTEST_MAIN(TestFlashWorker)
#include "test_flash_worker.moc"
