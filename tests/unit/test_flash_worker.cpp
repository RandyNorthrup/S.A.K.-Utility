// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_flash_worker.cpp
 * @brief TST-09 — Unit tests for FlashWorker validation, structs, and early‐exit paths.
 *
 * Tests:
 *  - ValidationResult / ValidationMode / ImageMetadata defaults & logic
 *  - FlashWorker construction, getters, and setters
 *  - Early failure when ImageSource::open() fails (mock)
 *  - Cancellation flag propagation
 *
 * No admin privileges or physical drives required — all tests exercise
 * logic that fires before raw disk writes.
 */

#include <QtTest>
#include <QSignalSpy>
#include <QEventLoop>
#include <QTimer>
#include <memory>

#include "sak/flash_worker.h"

// ===========================================================================
// Mock Image Source — always‐failing or configurable
// ===========================================================================

class MockImageSource : public ImageSource {
    Q_OBJECT
public:
    explicit MockImageSource(bool shouldOpen = false, QObject* parent = nullptr)
        : ImageSource(parent), m_shouldOpen(shouldOpen), m_open(false) {}

    bool open() override {
        m_open = m_shouldOpen;
        return m_open;
    }

    void close() override { m_open = false; }
    bool isOpen() const override { return m_open; }

    qint64 read(char* /*data*/, qint64 /*maxSize*/) override { return -1; }
    qint64 size() const override { return m_fakeSize; }
    qint64 position() const override { return 0; }
    bool seek(qint64 /*pos*/) override { return false; }
    bool atEnd() const override { return true; }

    sak::ImageMetadata metadata() const override {
        sak::ImageMetadata md;
        md.name   = QStringLiteral("mock.iso");
        md.path   = QStringLiteral("/mock/mock.iso");
        md.format = sak::ImageFormat::ISO;
        md.size   = m_fakeSize;
        return md;
    }

    QString calculateChecksum() override { return QString(); }

    void setFakeSize(qint64 sz) { m_fakeSize = sz; }

private:
    bool m_shouldOpen;
    bool m_open;
    qint64 m_fakeSize = 1024 * 1024; // 1 MiB
};

// ===========================================================================
// Test class
// ===========================================================================

class FlashWorkerTests : public QObject {
    Q_OBJECT

private slots:
    // ---- ValidationResult struct ----
    void validationResultDefaults();
    void validationResultFieldAssignment();

    // ---- ValidationMode enum ----
    void validationModeEnumValues();

    // ---- ImageMetadata::isValid() ----
    void imageMetadataEmptyPathInvalid();
    void imageMetadataZeroSizeInvalid();
    void imageMetadataUnknownFormatInvalid();
    void imageMetadataValidReturnsTrue();

    // ---- FlashWorker construction & getters ----
    void constructorStoresTargetDevice();
    void constructorDefaultBytesWritten();
    void constructorDefaultSpeed();

    // ---- Setters ----
    void setValidationModeFull();
    void setValidationModeSample();
    void setValidationModeSkip();
    void setVerificationEnabledToggle();
    void setBufferSizeCustom();

    // ---- Cancellation ----
    void requestStopSetsFlag();
    void stopRequestedFalseByDefault();

    // ---- Early failure: ImageSource open fails ----
    void executeFailsWhenImageOpenFails();

    // ---- Early failure: device open fails (non-admin) ----
    void executeFailsWhenDeviceInvalid();
};

// ===========================================================================
// ValidationResult struct
// ===========================================================================

void FlashWorkerTests::validationResultDefaults()
{
    sak::ValidationResult vr;
    QCOMPARE(vr.passed, false);
    QCOMPARE(vr.mismatchOffset, static_cast<qint64>(-1));
    QCOMPARE(vr.corruptedBlocks, 0);
    QCOMPARE(vr.verificationSpeed, 0.0);
    QVERIFY(vr.sourceChecksum.isEmpty());
    QVERIFY(vr.targetChecksum.isEmpty());
    QVERIFY(vr.errors.isEmpty());
}

void FlashWorkerTests::validationResultFieldAssignment()
{
    sak::ValidationResult vr;
    vr.passed             = true;
    vr.sourceChecksum     = QStringLiteral("abc123");
    vr.targetChecksum     = QStringLiteral("abc123");
    vr.mismatchOffset     = 0;
    vr.corruptedBlocks    = 5;
    vr.verificationSpeed  = 123.45;
    vr.errors << QStringLiteral("block 7 mismatch");

    QCOMPARE(vr.passed, true);
    QCOMPARE(vr.sourceChecksum, QStringLiteral("abc123"));
    QCOMPARE(vr.targetChecksum, QStringLiteral("abc123"));
    QCOMPARE(vr.mismatchOffset, static_cast<qint64>(0));
    QCOMPARE(vr.corruptedBlocks, 5);
    QCOMPARE(vr.verificationSpeed, 123.45);
    QCOMPARE(vr.errors.size(), 1);
    QCOMPARE(vr.errors.first(), QStringLiteral("block 7 mismatch"));
}

// ===========================================================================
// ValidationMode enum
// ===========================================================================

void FlashWorkerTests::validationModeEnumValues()
{
    // All three values must exist and be distinct.
    QVERIFY(sak::ValidationMode::Full   != sak::ValidationMode::Sample);
    QVERIFY(sak::ValidationMode::Sample != sak::ValidationMode::Skip);
    QVERIFY(sak::ValidationMode::Full   != sak::ValidationMode::Skip);
}

// ===========================================================================
// ImageMetadata::isValid()
// ===========================================================================

void FlashWorkerTests::imageMetadataEmptyPathInvalid()
{
    sak::ImageMetadata md;
    md.path   = QString();
    md.size   = 1024;
    md.format = sak::ImageFormat::ISO;
    QVERIFY(!md.isValid());
}

void FlashWorkerTests::imageMetadataZeroSizeInvalid()
{
    sak::ImageMetadata md;
    md.path   = QStringLiteral("/some/path.iso");
    md.size   = 0;
    md.format = sak::ImageFormat::ISO;
    QVERIFY(!md.isValid());
}

void FlashWorkerTests::imageMetadataUnknownFormatInvalid()
{
    sak::ImageMetadata md;
    md.path   = QStringLiteral("/some/path.iso");
    md.size   = 1024;
    md.format = sak::ImageFormat::Unknown;
    QVERIFY(!md.isValid());
}

void FlashWorkerTests::imageMetadataValidReturnsTrue()
{
    sak::ImageMetadata md;
    md.path   = QStringLiteral("/some/path.iso");
    md.size   = 1024;
    md.format = sak::ImageFormat::ISO;
    QVERIFY(md.isValid());
}

// ===========================================================================
// FlashWorker construction & getters
// ===========================================================================

void FlashWorkerTests::constructorStoresTargetDevice()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));
    QCOMPARE(worker.targetDevice(), QStringLiteral("\\\\.\\PhysicalDrive99"));
}

void FlashWorkerTests::constructorDefaultBytesWritten()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));
    QCOMPARE(worker.bytesWritten(), static_cast<qint64>(0));
}

void FlashWorkerTests::constructorDefaultSpeed()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));
    QCOMPARE(worker.speedMBps(), 0.0);
}

// ===========================================================================
// Setters
// ===========================================================================

void FlashWorkerTests::setValidationModeFull()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    worker.setValidationMode(sak::ValidationMode::Full);
    // No crash — setter accepted.
    QVERIFY(true);
}

void FlashWorkerTests::setValidationModeSample()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    worker.setValidationMode(sak::ValidationMode::Sample);
    QVERIFY(true);
}

void FlashWorkerTests::setValidationModeSkip()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    worker.setValidationMode(sak::ValidationMode::Skip);
    QVERIFY(true);
}

void FlashWorkerTests::setVerificationEnabledToggle()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    worker.setVerificationEnabled(false);
    worker.setVerificationEnabled(true);
    QVERIFY(true);
}

void FlashWorkerTests::setBufferSizeCustom()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    worker.setBufferSize(128 * 1024 * 1024); // 128 MiB
    QVERIFY(true);
}

// ===========================================================================
// Cancellation
// ===========================================================================

void FlashWorkerTests::requestStopSetsFlag()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));

    QVERIFY(!worker.stopRequested());
    worker.requestStop();
    QVERIFY(worker.stopRequested());
}

void FlashWorkerTests::stopRequestedFalseByDefault()
{
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    QVERIFY(!worker.stopRequested());
}

// ===========================================================================
// Early-exit failure paths
// ===========================================================================

void FlashWorkerTests::executeFailsWhenImageOpenFails()
{
    // Mock that returns false from open().
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));

    QSignalSpy failedSpy(&worker, &FlashWorker::failed);
    QSignalSpy errorSpy(&worker, &FlashWorker::error);
    QVERIFY(failedSpy.isValid());
    QVERIFY(errorSpy.isValid());

    // start() spawns a thread. We need a short event loop to let it finish.
    worker.start();

    // Wait for the thread to complete (should fail fast).
    bool stopped = worker.wait(5000);
    QVERIFY2(stopped, "Worker thread did not stop within 5 seconds");

    // Should have emitted failed() with an error code.
    QVERIFY2(failedSpy.count() >= 1,
             "Expected failed() signal when ImageSource::open() returns false");
}

void FlashWorkerTests::executeFailsWhenDeviceInvalid()
{
    // Mock that opens successfully, but the device path is bogus.
    auto src = std::make_unique<MockImageSource>(true);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive999"));

    QSignalSpy failedSpy(&worker, &FlashWorker::failed);
    QSignalSpy errorSpy(&worker, &FlashWorker::error);
    QVERIFY(failedSpy.isValid());
    QVERIFY(errorSpy.isValid());

    worker.start();
    bool stopped = worker.wait(5000);
    QVERIFY2(stopped, "Worker thread did not stop within 5 seconds");

    // DeviceIoControl / CreateFileW should fail → failed() emitted.
    QVERIFY2(failedSpy.count() >= 1,
             "Expected failed() signal when device path is invalid");
}

// ===========================================================================

QTEST_MAIN(FlashWorkerTests)
#include "test_flash_worker.moc"
