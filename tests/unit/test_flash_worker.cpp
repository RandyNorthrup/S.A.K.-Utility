// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_flash_worker.cpp
 * @brief TST-09 -- Unit tests for FlashWorker validation, structs, and early-exit paths.
 *
 * Tests:
 *  - ValidationResult / ValidationMode / ImageMetadata defaults & logic
 *  - FlashWorker construction, getters, and setters
 *  - Early failure when ImageSource::open() fails (mock)
 *  - Cancellation flag propagation
 *
 * No admin privileges or physical drives required -- all tests exercise
 * logic that fires before raw disk writes.
 */

#include "sak/flash_worker.h"

#include <QEventLoop>
#include <QSignalSpy>
#include <QTimer>
#include <QtTest>

#include <limits>
#include <memory>

// ===========================================================================
// Mock Image Source -- always-failing or configurable
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
        md.name = QStringLiteral("mock.iso");
        md.path = QStringLiteral("/mock/mock.iso");
        md.format = sak::ImageFormat::ISO;
        md.size = m_fakeSize;
        return md;
    }

    QString calculateChecksum() override { return QString(); }

    void setFakeSize(qint64 sz) { m_fakeSize = sz; }

private:
    bool m_shouldOpen;
    bool m_open;
    qint64 m_fakeSize = 1024 * 1024;  // 1 MiB
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

    // ---- persisted setting -> ValidationMode (R5-G20-1) ----
    void validationModeFromSetting_mapsEveryDialogValue();
    void validationModeFromSetting_rejectsUnknown();

    // ---- ImageMetadata::isValid() ----
    void imageMetadataEmptyPathInvalid();
    void imageMetadataZeroSizeInvalid();
    void imageMetadataUnknownFormatInvalid();
    void imageMetadataValidReturnsTrue();
    void imageMetadataDefaultScalarsZeroed();

    // ---- FlashWorker construction & getters ----
    void constructorStoresTargetDevice();
    void constructorDefaultBytesWritten();
    void constructorDefaultSpeed();

    // ---- Setters ----
    void setVerificationEnabledToggle();
    void setBufferSizeCustom();

    // ---- Cancellation ----
    void requestStopSetsFlag();
    void stopRequestedFalseByDefault();

    // ---- Early failure: ImageSource open fails ----
    void executeFailsWhenImageOpenFails();

    // ---- Early failure: device open fails (non-admin) ----
    void executeFailsWhenDeviceInvalid();

    // ---- P07-54: sector-size-aware write padding (512e vs 4Kn) ----
    void padToSectorSizeAlreadyAligned512();
    void padToSectorSizePads512();
    void padToSectorSizePads4Kn();
    void padToSectorSizeAlreadyAligned4Kn();
    void padToSectorSizeRejectsBogusSectorSize();
    void padToSectorSizeRejectsNegativeBytesRead();

    // ---- B4-04/06: device-capacity fit + sector-aligned read length ----
    void alignUpToSectorSizeAlreadyAligned();
    void alignUpToSectorSizeRoundsUp();
    void alignUpToSectorSizeRejectsBogusSectorSize();
    void alignUpToSectorSizeRejectsOverflow();
    void imageFitsDeviceUnderAndExact();
    void imageFitsDeviceRejectsOversize();
    void imageFitsDeviceRejectsUnknownCapacity();
    void imageFitsDeviceRejectsUnknownImageSize();
};

// ===========================================================================
// ValidationResult struct
// ===========================================================================

void FlashWorkerTests::validationResultDefaults() {
    sak::ValidationResult vr;
    QCOMPARE(vr.passed, false);
    QCOMPARE(vr.mismatchOffset, static_cast<qint64>(-1));
    QCOMPARE(vr.corruptedBlocks, 0);
    QCOMPARE(vr.verificationSpeed, 0.0);
    QVERIFY(vr.sourceChecksum.isEmpty());
    QVERIFY(vr.targetChecksum.isEmpty());
    QVERIFY(vr.errors.isEmpty());
}

void FlashWorkerTests::validationResultFieldAssignment() {
    sak::ValidationResult vr;
    vr.passed = true;
    vr.sourceChecksum = QStringLiteral("abc123");
    vr.targetChecksum = QStringLiteral("abc123");
    vr.mismatchOffset = 0;
    vr.corruptedBlocks = 5;
    vr.verificationSpeed = 123.45;
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

void FlashWorkerTests::validationModeEnumValues() {
    // All three values must exist and be distinct.
    QVERIFY(sak::ValidationMode::Full != sak::ValidationMode::Sample);
    QVERIFY(sak::ValidationMode::Sample != sak::ValidationMode::Skip);
    QVERIFY(sak::ValidationMode::Full != sak::ValidationMode::Skip);
}

// ===========================================================================
// ImageMetadata::isValid()
// ===========================================================================

void FlashWorkerTests::imageMetadataEmptyPathInvalid() {
    sak::ImageMetadata md;
    md.path = QString();
    md.size = 1024;
    md.format = sak::ImageFormat::ISO;
    QVERIFY(!md.isValid());
}

void FlashWorkerTests::imageMetadataZeroSizeInvalid() {
    sak::ImageMetadata md;
    md.path = QStringLiteral("/some/path.iso");
    md.size = 0;
    md.format = sak::ImageFormat::ISO;
    QVERIFY(!md.isValid());
}

void FlashWorkerTests::imageMetadataUnknownFormatInvalid() {
    sak::ImageMetadata md;
    md.path = QStringLiteral("/some/path.iso");
    md.size = 1024;
    md.format = sak::ImageFormat::Unknown;
    QVERIFY(!md.isValid());
}

void FlashWorkerTests::imageMetadataValidReturnsTrue() {
    sak::ImageMetadata md;
    md.path = QStringLiteral("/some/path.iso");
    md.size = 1024;
    md.format = sak::ImageFormat::ISO;
    QVERIFY(md.isValid());
}

// P07-23: the scalar fields must be deterministically zeroed by default; a compressed source that
// never assigns uncompressedSize previously left it indeterminate and size() returned garbage.
void FlashWorkerTests::imageMetadataDefaultScalarsZeroed() {
    const sak::ImageMetadata md;
    QCOMPARE(md.size, static_cast<qint64>(0));
    QCOMPARE(md.uncompressedSize, static_cast<qint64>(0));
    QCOMPARE(md.isCompressed, false);
    QVERIFY(md.format == sak::ImageFormat::Unknown);
}

// ===========================================================================
// FlashWorker construction & getters
// ===========================================================================

void FlashWorkerTests::constructorStoresTargetDevice() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));
    QCOMPARE(worker.targetDevice(), QStringLiteral("\\\\.\\PhysicalDrive99"));
}

void FlashWorkerTests::constructorDefaultBytesWritten() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));
    QCOMPARE(worker.bytesWritten(), static_cast<qint64>(0));
}

void FlashWorkerTests::constructorDefaultSpeed() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("\\\\.\\PhysicalDrive99"));
    QCOMPARE(worker.speedMBps(), 0.0);
}

// ===========================================================================
// Setters
// ===========================================================================

// NOTE: setValidationModeFull/Sample/Skip used to live here. All three were
// `worker.setValidationMode(<enum>); QVERIFY(true);` -- FlashWorker exposes no getter for
// m_validationMode, so they would have passed against an empty setter body, and they were
// the only callers of setValidationMode() in the unit suite. The one remaining caller is
// tests/certification/flash_live_certifier.cpp:265 -- also a test. FlashCoordinator forwards
// verification and buffer size but never the validation mode, so the user-facing setting is
// not wired to the worker at all; that is a product defect tracked separately, not a reason
// to keep a test that cannot fail.
//
// The two setters below are still exercised because FlashCoordinator DOES call them; they
// remain unassertable for the same "no getter" reason, which is tracked separately.

void FlashWorkerTests::setVerificationEnabledToggle() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    // The default is already true, so observe the intermediate false first: pinning only the final
    // true would pass even if the setter were a no-op. QVERIFY(true) asserted nothing.
    worker.setVerificationEnabled(false);
    QCOMPARE(worker.verificationEnabled(), false);
    worker.setVerificationEnabled(true);
    QCOMPARE(worker.verificationEnabled(), true);
}

void FlashWorkerTests::setBufferSizeCustom() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    worker.setBufferSize(128 * 1024 * 1024);  // 128 MiB
    QVERIFY(true);
}

// ===========================================================================
// Cancellation
// ===========================================================================

void FlashWorkerTests::requestStopSetsFlag() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));

    QVERIFY(!worker.stopRequested());
    worker.requestStop();
    QVERIFY(worker.stopRequested());
}

void FlashWorkerTests::stopRequestedFalseByDefault() {
    auto src = std::make_unique<MockImageSource>(false);
    FlashWorker worker(std::move(src), QStringLiteral("X"));
    QVERIFY(!worker.stopRequested());
}

// ===========================================================================
// Early-exit failure paths
// ===========================================================================

void FlashWorkerTests::executeFailsWhenImageOpenFails() {
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

    // Exactly one failure report, carrying the code the caller switches on. A count-only
    // check would still pass if execute() bailed for some entirely different reason.
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.first().at(0).toInt(), static_cast<int>(sak::error_code::file_not_found));

    // ...and exactly one human-readable error naming the stage that failed. Nothing must be
    // emitted twice: the coordinator counts a drive as failed once per signal.
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toString(), QStringLiteral("Failed to open image source"));
}

void FlashWorkerTests::executeFailsWhenDeviceInvalid() {
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

    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.first().at(0).toInt(), static_cast<int>(sak::error_code::file_not_found));

    // The image opened fine, so the failure MUST be attributed to the device stage -- this is
    // what distinguishes this test from executeFailsWhenImageOpenFails, which shares the code.
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(errorSpy.first().at(0).toString(), QStringLiteral("Failed to open target device"));
}

// ===========================================================================
// P07-54: raw-device write padding aligns to the target's logical sector size.
// A 512-only assumption fails every unbuffered write on a 4Kn (4096-byte
// logical sector) disk; padToSectorSize now takes the queried sector size.
// ===========================================================================

void FlashWorkerTests::padToSectorSizeAlreadyAligned512() {
    QByteArray buf(1024, 'x');
    qint64 bytesRead = 1024;  // exact multiple of 512
    QVERIFY(FlashWorker::padToSectorSize(buf, bytesRead, 512));
    QCOMPARE(bytesRead, static_cast<qint64>(1024));
    QCOMPARE(buf.size(), 1024);
}

void FlashWorkerTests::padToSectorSizePads512() {
    QByteArray buf(4096, 'x');
    qint64 bytesRead = 513;                          // one byte past a 512 boundary
    QVERIFY(FlashWorker::padToSectorSize(buf, bytesRead, 512));
    QCOMPARE(bytesRead, static_cast<qint64>(1024));  // rounded up to 2 sectors
    QCOMPARE(buf.size(), 1024);
    // The pad must begin exactly at bytesRead: the 513 valid bytes survive and only the tail is
    // zeroed. A whole-buffer memset also leaves size 1024 with buf.at(1023) == '\0' -- and would
    // write 513 bytes of zeroes over real image data on every short read.
    QCOMPARE(buf.left(513), QByteArray(513, 'x'));
    QCOMPARE(buf.mid(513), QByteArray(1024 - 513, '\0'));
}

void FlashWorkerTests::padToSectorSizePads4Kn() {
    QByteArray buf(16'384, 'x');
    qint64 bytesRead = 513;                          // aligned to 512 but NOT to 4096
    QVERIFY(FlashWorker::padToSectorSize(buf, bytesRead, 4096));
    QCOMPARE(bytesRead, static_cast<qint64>(4096));  // one full 4Kn sector
    QCOMPARE(buf.size(), 4096);
    // Same content contract at 4Kn geometry: only bytes past bytesRead are zeroed.
    QCOMPARE(buf.left(513), QByteArray(513, 'x'));
    QCOMPARE(buf.mid(513), QByteArray(4096 - 513, '\0'));
}

void FlashWorkerTests::padToSectorSizeAlreadyAligned4Kn() {
    QByteArray buf(8192, 'x');
    qint64 bytesRead = 8192;  // exact multiple of 4096
    QVERIFY(FlashWorker::padToSectorSize(buf, bytesRead, 4096));
    QCOMPARE(bytesRead, static_cast<qint64>(8192));
    QCOMPARE(buf.size(), 8192);
}

void FlashWorkerTests::padToSectorSizeRejectsBogusSectorSize() {
    QByteArray buf(1024, 'x');
    qint64 bytesRead = 500;
    QVERIFY(!FlashWorker::padToSectorSize(buf, bytesRead, 0));
    QVERIFY(!FlashWorker::padToSectorSize(buf, bytesRead, -512));
}

// A negative valid-byte count must be REFUSED, not padded: -100 % 512 != 0 would
// otherwise reach the zero-fill loop and write buffer[i] at negative indices
// (out-of-bounds / undefined behaviour). A real read never yields a negative count.
void FlashWorkerTests::padToSectorSizeRejectsNegativeBytesRead() {
    QByteArray buf(1024, 'x');
    qint64 bytesRead = -100;
    QVERIFY(!FlashWorker::padToSectorSize(buf, bytesRead, 512));
    QCOMPARE(bytesRead, static_cast<qint64>(-100));  // left unchanged on refusal
    qint64 aligned = -512;                           // negative but a sector multiple
    QVERIFY(!FlashWorker::padToSectorSize(buf, aligned, 512));
}

// ===========================================================================
// B4-04 / B4-06: an unbuffered device read length must be sector-aligned, and
// an oversized image must be rejected before the write clobbers the device.
// ===========================================================================

void FlashWorkerTests::alignUpToSectorSizeAlreadyAligned() {
    QCOMPARE(FlashWorker::alignUpToSectorSize(4096, 512), static_cast<qint64>(4096));
    QCOMPARE(FlashWorker::alignUpToSectorSize(0, 512), static_cast<qint64>(0));
    QCOMPARE(FlashWorker::alignUpToSectorSize(8192, 4096), static_cast<qint64>(8192));
}

void FlashWorkerTests::alignUpToSectorSizeRoundsUp() {
    QCOMPARE(FlashWorker::alignUpToSectorSize(513, 512), static_cast<qint64>(1024));
    QCOMPARE(FlashWorker::alignUpToSectorSize(1, 512), static_cast<qint64>(512));
    QCOMPARE(FlashWorker::alignUpToSectorSize(4097, 4096), static_cast<qint64>(8192));
}

void FlashWorkerTests::alignUpToSectorSizeRejectsBogusSectorSize() {
    QCOMPARE(FlashWorker::alignUpToSectorSize(1024, 0), static_cast<qint64>(-1));
    QCOMPARE(FlashWorker::alignUpToSectorSize(1024, -512), static_cast<qint64>(-1));
    QCOMPARE(FlashWorker::alignUpToSectorSize(-1, 512), static_cast<qint64>(-1));
}

void FlashWorkerTests::alignUpToSectorSizeRejectsOverflow() {
    // (max/512) sectors already occupy the whole range; rounding a non-aligned
    // value above that must fail rather than wrap negative.
    const qint64 huge = std::numeric_limits<qint64>::max() - 1;
    QCOMPARE(FlashWorker::alignUpToSectorSize(huge, 512), static_cast<qint64>(-1));
}

void FlashWorkerTests::imageFitsDeviceUnderAndExact() {
    QVERIFY(FlashWorker::imageFitsDevice(1000, 2000));  // fits
    QVERIFY(FlashWorker::imageFitsDevice(2000, 2000));  // exact fit
    QVERIFY(FlashWorker::imageFitsDevice(0, 0));        // empty image on empty device
}

void FlashWorkerTests::imageFitsDeviceRejectsOversize() {
    QVERIFY(!FlashWorker::imageFitsDevice(2001, 2000));
    QVERIFY(!FlashWorker::imageFitsDevice(1LL << 40, (1LL << 40) - 1));
}

void FlashWorkerTests::imageFitsDeviceRejectsUnknownCapacity() {
    // Fail closed: a device whose capacity cannot be queried (-1) is REFUSED, not
    // written best-effort. Assuming "fits" would let an oversized image overwrite
    // the whole device before failing at end-of-media.
    QVERIFY(!FlashWorker::imageFitsDevice(1LL << 40, -1));
    QVERIFY(!FlashWorker::imageFitsDevice(0, -1));
}

void FlashWorkerTests::imageFitsDeviceRejectsUnknownImageSize() {
    // Fail closed on an undetermined image size (-1), e.g. a compressed stream
    // whose decompressed length the decompressor cannot report. Without a known
    // size the gate cannot bound the write, so it must refuse.
    QVERIFY(!FlashWorker::imageFitsDevice(-1, 1LL << 40));
    QVERIFY(!FlashWorker::imageFitsDevice(-1, 0));
}

// ===========================================================================
// R5-G20-1: the Image Flasher settings dialog persists "full" / "quick" / "none" and the
// worker takes a ValidationMode. Nothing mapped between them, and nothing read the setting
// at all, so the user's verification choice was collected and discarded on every flash.
// These pin the mapping for all three dialog values, which is what makes the wiring
// verifiable without driving the GUI.
// ===========================================================================

void FlashWorkerTests::validationModeFromSetting_mapsEveryDialogValue() {
    // The literals must match image_flasher_settings_dialog.cpp's combo userData exactly.
    QCOMPARE(sak::validationModeFromSetting(QStringLiteral("full")).value(),
             sak::ValidationMode::Full);
    QCOMPARE(sak::validationModeFromSetting(QStringLiteral("quick")).value(),
             sak::ValidationMode::Sample);
    QCOMPARE(sak::validationModeFromSetting(QStringLiteral("none")).value(),
             sak::ValidationMode::Skip);
}

void FlashWorkerTests::validationModeFromSetting_rejectsUnknown() {
    // Unknown input must be reported, not silently resolved: the caller logs the offending
    // value and falls back to the MOST thorough mode, so a corrupted setting can never
    // quietly downgrade verification on a destructive write.
    QVERIFY(!sak::validationModeFromSetting(QString()).has_value());
    QVERIFY(!sak::validationModeFromSetting(QStringLiteral("Full")).has_value());  // case
    QVERIFY(!sak::validationModeFromSetting(QStringLiteral("sample")).has_value());
    QVERIFY(!sak::validationModeFromSetting(QStringLiteral("skip")).has_value());
    QVERIFY(!sak::validationModeFromSetting(QStringLiteral("garbage")).has_value());
}

// ===========================================================================

QTEST_MAIN(FlashWorkerTests)
#include "test_flash_worker.moc"
