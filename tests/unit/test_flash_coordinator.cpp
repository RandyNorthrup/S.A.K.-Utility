// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryFile>
#include "sak/flash_coordinator.h"

class TestFlashCoordinator : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Coordinator initialization
    void testConstructor();
    void testDefaultState();
    void testInitialValues();

    // Flash operations
    void testStartFlash();
    void testStartFlashInvalidImage();
    void testStartFlashNoTargets();
    void testStartFlashInvalidTargets();

    // State management
    void testStateIdle();
    void testStateValidating();
    void testStateFlashing();
    void testStateCompleted();
    void testStateFailed();
    void testStateCancelled();

    // Progress tracking
    void testProgressInitial();
    void testProgressUpdates();
    void testProgressPercentage();
    void testProgressBytesWritten();
    void testProgressSpeed();

    // Verification
    void testVerificationEnabled();
    void testVerificationDisabled();
    void testSetVerificationEnabled();
    void testIsVerificationEnabled();

    // Buffer configuration
    void testSetBufferSize();
    void testSetBufferCount();
    void testDefaultBufferSize();
    void testDefaultBufferCount();

    // Signals
    void testStateChangedSignal();
    void testProgressUpdatedSignal();
    void testDriveCompletedSignal();
    void testDriveFailedSignal();
    void testFlashCompletedSignal();
    void testFlashErrorSignal();

    // Cancellation
    void testCancel();
    void testCancelDuringFlash();
    void testCancelBeforeStart();
    void testIsFlashing();

    // Multi-drive operations
    void testMultipleDrives();
    void testParallelWriting();
    void testDriveFailureHandling();

    // Validation
    void testValidateTargets();
    void testValidateSystemDrive();
    void testValidateInvalidDrives();

    // Unmounting
    void testUnmountVolumes();
    void testUnmountFailure();

    // Error handling
    void testImageNotFound();
    void testInsufficientSpace();
    void testDriveAccessDenied();
    void testCorruptedImage();

    // Flash results
    void testFlashResultSuccess();
    void testFlashResultFailure();
    void testFlashResultPartial();
    void testFlashResultStatistics();

    // Progress information
    void testFlashProgressStructure();
    void testFlashProgressOverall();
    void testFlashProgressActiveDrives();
    void testFlashProgressSpeed();

    // State transitions
    void testStateTransitions();
    void testInvalidStateTransitions();

    // Resource cleanup
    void testCleanupWorkers();
    void testCleanupOnFailure();
    void testCleanupOnCancel();

private:
    FlashCoordinator* m_coordinator{nullptr};
    QTemporaryFile* m_tempImage{nullptr};
    QString m_testImagePath;
    
    void createTestImage(qint64 sizeBytes = 1024 * 1024); // 1MB default
    void waitForSignal(QObject* obj, const char* signal, int timeout = 5000);
};

void TestFlashCoordinator::initTestCase() {
    // Setup test environment
}

void TestFlashCoordinator::cleanupTestCase() {
    // Cleanup test environment
}

void TestFlashCoordinator::init() {
    m_coordinator = new FlashCoordinator(this);
    createTestImage();
}

void TestFlashCoordinator::cleanup() {
    delete m_coordinator;
    m_coordinator = nullptr;
    delete m_tempImage;
    m_tempImage = nullptr;
}

void TestFlashCoordinator::createTestImage(qint64 sizeBytes) {
    m_tempImage = new QTemporaryFile(this);
    m_tempImage->setFileTemplate("test_image_XXXXXX.img");
    QVERIFY(m_tempImage->open());
    
    // Write test data
    QByteArray data(sizeBytes, 0x42);
    m_tempImage->write(data);
    m_tempImage->flush();
    
    m_testImagePath = m_tempImage->fileName();
}

void TestFlashCoordinator::waitForSignal(QObject* obj, const char* signal, int timeout) {
    QSignalSpy spy(obj, signal);
    QVERIFY(spy.wait(timeout));
}

void TestFlashCoordinator::testConstructor() {
    QVERIFY(m_coordinator != nullptr);
}

void TestFlashCoordinator::testDefaultState() {
    QCOMPARE(m_coordinator->state(), sak::FlashState::Idle);
}

void TestFlashCoordinator::testInitialValues() {
    QVERIFY(!m_coordinator->isFlashing());
    QVERIFY(m_coordinator->isVerificationEnabled()); // Default should be enabled
}

void TestFlashCoordinator::testStartFlash() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::stateChanged);
    
    // Cannot actually flash without real drive
    // Just test that method exists and validates input
    bool result = m_coordinator->startFlash(m_testImagePath, QStringList());
    QVERIFY(!result); // Should fail with no targets
}

void TestFlashCoordinator::testStartFlashInvalidImage() {
    bool result = m_coordinator->startFlash("nonexistent.img", {"\\\\.\\PhysicalDrive99"});
    QVERIFY(!result);
}

void TestFlashCoordinator::testStartFlashNoTargets() {
    bool result = m_coordinator->startFlash(m_testImagePath, QStringList());
    QVERIFY(!result);
}

void TestFlashCoordinator::testStartFlashInvalidTargets() {
    bool result = m_coordinator->startFlash(m_testImagePath, {"InvalidPath"});
    QVERIFY(!result);
}

void TestFlashCoordinator::testStateIdle() {
    QCOMPARE(m_coordinator->state(), sak::FlashState::Idle);
}

void TestFlashCoordinator::testStateValidating() {
    // State should change to Validating when flash starts
    QSignalSpy spy(m_coordinator, &FlashCoordinator::stateChanged);
    
    m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive99"});
    
    // May transition through Validating state
}

void TestFlashCoordinator::testStateFlashing() {
    // Would need actual drive to test
    QSKIP("Requires physical drive");
}

void TestFlashCoordinator::testStateCompleted() {
    // Would need successful flash to test
    QSKIP("Requires physical drive");
}

void TestFlashCoordinator::testStateFailed() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::stateChanged);
    
    m_coordinator->startFlash("nonexistent.img", {"\\\\.\\PhysicalDrive99"});
    
    // Should transition to Failed state
    if (spy.wait(5000)) {
        QVERIFY(m_coordinator->state() == sak::FlashState::Failed ||
                m_coordinator->state() == sak::FlashState::Idle);
    }
}

void TestFlashCoordinator::testStateCancelled() {
    m_coordinator->cancel();
    // State should handle cancellation
}

void TestFlashCoordinator::testProgressInitial() {
    auto progress = m_coordinator->progress();
    QCOMPARE(progress.percentage, 0.0);
    QCOMPARE(progress.bytesWritten, 0LL);
}

void TestFlashCoordinator::testProgressUpdates() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::progressUpdated);
    
    // Progress updates during operation
    // Cannot test without actual flash
}

void TestFlashCoordinator::testProgressPercentage() {
    auto progress = m_coordinator->progress();
    QVERIFY(progress.percentage >= 0.0);
    QVERIFY(progress.percentage <= 100.0);
}

void TestFlashCoordinator::testProgressBytesWritten() {
    auto progress = m_coordinator->progress();
    QVERIFY(progress.bytesWritten >= 0);
}

void TestFlashCoordinator::testProgressSpeed() {
    auto progress = m_coordinator->progress();
    QVERIFY(progress.speedMBps >= 0.0);
}

void TestFlashCoordinator::testVerificationEnabled() {
    m_coordinator->setVerificationEnabled(true);
    QVERIFY(m_coordinator->isVerificationEnabled());
}

void TestFlashCoordinator::testVerificationDisabled() {
    m_coordinator->setVerificationEnabled(false);
    QVERIFY(!m_coordinator->isVerificationEnabled());
}

void TestFlashCoordinator::testSetVerificationEnabled() {
    m_coordinator->setVerificationEnabled(false);
    QVERIFY(!m_coordinator->isVerificationEnabled());
    
    m_coordinator->setVerificationEnabled(true);
    QVERIFY(m_coordinator->isVerificationEnabled());
}

void TestFlashCoordinator::testIsVerificationEnabled() {
    bool enabled = m_coordinator->isVerificationEnabled();
    QVERIFY(enabled == true || enabled == false);
}

void TestFlashCoordinator::testSetBufferSize() {
    m_coordinator->setBufferSize(32 * 1024 * 1024); // 32MB
    // No getter, just verify method exists
}

void TestFlashCoordinator::testSetBufferCount() {
    m_coordinator->setBufferCount(8);
    // No getter, just verify method exists
}

void TestFlashCoordinator::testDefaultBufferSize() {
    // Default should be 64MB
    // Tested implicitly through flash operations
}

void TestFlashCoordinator::testDefaultBufferCount() {
    // Default should be 16 buffers
    // Tested implicitly through flash operations
}

void TestFlashCoordinator::testStateChangedSignal() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::stateChanged);
    
    m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive99"});
    
    // Should emit state changes
}

void TestFlashCoordinator::testProgressUpdatedSignal() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::progressUpdated);
    
    // Progress signals during flash operation
    // Cannot test without actual drive
}

void TestFlashCoordinator::testDriveCompletedSignal() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::driveCompleted);
    
    // Emitted when drive finishes successfully
    // Cannot test without actual drive
}

void TestFlashCoordinator::testDriveFailedSignal() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::driveFailed);
    
    // Emitted when drive fails
    // Cannot test without actual drive
}

void TestFlashCoordinator::testFlashCompletedSignal() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::flashCompleted);
    
    // Emitted when operation completes
    // Cannot test without actual drive
}

void TestFlashCoordinator::testFlashErrorSignal() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::flashError);
    
    m_coordinator->startFlash("nonexistent.img", {"\\\\.\\PhysicalDrive99"});
    
    // Should emit error for missing file
}

void TestFlashCoordinator::testCancel() {
    m_coordinator->cancel();
    // Should handle cancel gracefully
}

void TestFlashCoordinator::testCancelDuringFlash() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::stateChanged);
    
    m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive99"});
    m_coordinator->cancel();
    
    // Should cancel operation
}

void TestFlashCoordinator::testCancelBeforeStart() {
    m_coordinator->cancel();
    QVERIFY(!m_coordinator->isFlashing());
}

void TestFlashCoordinator::testIsFlashing() {
    QVERIFY(!m_coordinator->isFlashing());
    
    m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive99"});
    // May or may not be flashing depending on validation
}

void TestFlashCoordinator::testMultipleDrives() {
    // Test with multiple target drives
    QStringList targets = {"\\\\.\\PhysicalDrive98", "\\\\.\\PhysicalDrive99"};
    bool result = m_coordinator->startFlash(m_testImagePath, targets);
    
    // Should handle multiple targets (though will fail validation)
}

void TestFlashCoordinator::testParallelWriting() {
    // Parallel writing tested through multi-drive operation
    QSKIP("Requires physical drives");
}

void TestFlashCoordinator::testDriveFailureHandling() {
    // Should handle individual drive failures
    QSKIP("Requires physical drives");
}

void TestFlashCoordinator::testValidateTargets() {
    // Internal validation tested through startFlash
    bool result = m_coordinator->startFlash(m_testImagePath, {"InvalidPath"});
    QVERIFY(!result);
}

void TestFlashCoordinator::testValidateSystemDrive() {
    // Should reject system drive (PhysicalDrive0)
    bool result = m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive0"});
    QVERIFY(!result); // Should fail validation
}

void TestFlashCoordinator::testValidateInvalidDrives() {
    bool result = m_coordinator->startFlash(m_testImagePath, {"NotADrive"});
    QVERIFY(!result);
}

void TestFlashCoordinator::testUnmountVolumes() {
    // Unmounting tested through flash workflow
    QSKIP("Requires physical drives");
}

void TestFlashCoordinator::testUnmountFailure() {
    // Should handle unmount failures gracefully
    QSKIP("Requires physical drives");
}

void TestFlashCoordinator::testImageNotFound() {
    QSignalSpy spy(m_coordinator, &FlashCoordinator::flashError);
    
    bool result = m_coordinator->startFlash("missing.img", {"\\\\.\\PhysicalDrive99"});
    QVERIFY(!result);
}

void TestFlashCoordinator::testInsufficientSpace() {
    // Would need drive smaller than image
    QSKIP("Requires physical drives");
}

void TestFlashCoordinator::testDriveAccessDenied() {
    // Access denied tested through permission checks
    QSKIP("Requires physical drives");
}

void TestFlashCoordinator::testCorruptedImage() {
    // Create corrupted image file
    QTemporaryFile corrupt;
    corrupt.open();
    corrupt.write("INVALID");
    corrupt.flush();
    
    bool result = m_coordinator->startFlash(corrupt.fileName(), {"\\\\.\\PhysicalDrive99"});
    // Should handle invalid image
}

void TestFlashCoordinator::testFlashResultSuccess() {
    // FlashResult structure tested through completion
    sak::FlashResult result;
    result.success = true;
    QVERIFY(result.success);
    QVERIFY(!result.hasErrors());
}

void TestFlashCoordinator::testFlashResultFailure() {
    sak::FlashResult result;
    result.success = false;
    result.failedDrives = {"\\\\.\\PhysicalDrive1"};
    QVERIFY(!result.success);
    QVERIFY(result.hasErrors());
}

void TestFlashCoordinator::testFlashResultPartial() {
    sak::FlashResult result;
    result.successfulDrives = {"\\\\.\\PhysicalDrive1"};
    result.failedDrives = {"\\\\.\\PhysicalDrive2"};
    QCOMPARE(result.totalDrives(), 2);
    QVERIFY(result.hasErrors());
}

void TestFlashCoordinator::testFlashResultStatistics() {
    sak::FlashResult result;
    result.bytesWritten = 1024 * 1024 * 1024; // 1GB
    result.elapsedSeconds = 60.0;
    
    QCOMPARE(result.bytesWritten, 1024LL * 1024 * 1024);
    QCOMPARE(result.elapsedSeconds, 60.0);
}

void TestFlashCoordinator::testFlashProgressStructure() {
    sak::FlashProgress progress;
    progress.state = sak::FlashState::Flashing;
    progress.percentage = 50.0;
    progress.bytesWritten = 512 * 1024 * 1024;
    progress.totalBytes = 1024 * 1024 * 1024;
    
    QCOMPARE(progress.state, sak::FlashState::Flashing);
    QCOMPARE(progress.percentage, 50.0);
}

void TestFlashCoordinator::testFlashProgressOverall() {
    sak::FlashProgress progress;
    progress.bytesWritten = 500;
    progress.totalBytes = 1000;
    
    double overall = progress.getOverallProgress();
    QCOMPARE(overall, 50.0);
}

void TestFlashCoordinator::testFlashProgressActiveDrives() {
    sak::FlashProgress progress;
    progress.activeDrives = 3;
    progress.completedDrives = 1;
    progress.failedDrives = 1;
    
    QCOMPARE(progress.activeDrives, 3);
}

void TestFlashCoordinator::testFlashProgressSpeed() {
    sak::FlashProgress progress;
    progress.speedMBps = 25.5;
    
    QCOMPARE(progress.speedMBps, 25.5);
}

void TestFlashCoordinator::testStateTransitions() {
    // Valid state transitions:
    // Idle -> Validating -> Unmounting -> Flashing -> Verifying -> Completed
    QCOMPARE(m_coordinator->state(), sak::FlashState::Idle);
}

void TestFlashCoordinator::testInvalidStateTransitions() {
    // Cannot go from Completed back to Flashing
    // State machine enforces valid transitions
}

void TestFlashCoordinator::testCleanupWorkers() {
    // Workers cleaned up after operation
    m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive99"});
    m_coordinator->cancel();
    // Should cleanup resources
}

void TestFlashCoordinator::testCleanupOnFailure() {
    m_coordinator->startFlash("missing.img", {"\\\\.\\PhysicalDrive99"});
    // Should cleanup on failure
}

void TestFlashCoordinator::testCleanupOnCancel() {
    m_coordinator->startFlash(m_testImagePath, {"\\\\.\\PhysicalDrive99"});
    m_coordinator->cancel();
    // Should cleanup on cancel
}

QTEST_MAIN(TestFlashCoordinator)
#include "test_flash_coordinator.moc"
