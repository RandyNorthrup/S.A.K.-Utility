// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/flash_coordinator.h"

#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>

/**
 * @brief Unit tests for FlashCoordinator.
 *
 * Covers default construction, configuration setters,
 * state queries, and guard logic in startFlash.
 * Does NOT test actual flashing (requires drives).
 */
class TestFlashCoordinator : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();

    // -- Construction defaults -------------------------------
    void testInitialState();
    void testInitialProgress();
    void testVerificationEnabledByDefault();

    // -- Configuration ---------------------------------------
    void testSetVerification();
    void testSetBufferSize();
    void testDefaultConcurrencyAndEject();
    void testSetMaxConcurrentWrites();
    void testSetMaxConcurrentWritesRejectsBelowOne();
    void testSetEjectOnCompletion();
    void testStartableWorkerCountSeam();

    // -- State queries ---------------------------------------
    void testIsFlashingWhenIdle();
    void testStateWhenIdle();

    // -- startFlash guards -----------------------------------
    void testStartFlashEmptyDrives();
    void testStartFlashRejectsZeroLengthImage();
    void testStartFlashRejectsDuplicateTargets();
    void testFirstDuplicateTargetSeam();
    void testParsePhysicalDriveNumberSeam();
    void testCancelWhenIdle();

private:
    std::unique_ptr<FlashCoordinator> m_coord;
};

void TestFlashCoordinator::init() {
    m_coord = std::make_unique<FlashCoordinator>();
}

// ============================================================================
// Construction defaults
// ============================================================================

void TestFlashCoordinator::testInitialState() {
    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
}

void TestFlashCoordinator::testInitialProgress() {
    auto p = m_coord->progress();
    QCOMPARE(p.state, sak::FlashState::Idle);
    QCOMPARE(p.percentage, 0.0);
    QCOMPARE(p.bytesWritten, static_cast<qint64>(0));
    QCOMPARE(p.totalBytes, static_cast<qint64>(0));
    QCOMPARE(p.speedMBps, 0.0);
    QCOMPARE(p.activeDrives, 0);
    QCOMPARE(p.failedDrives, 0);
    QCOMPARE(p.completedDrives, 0);
}

void TestFlashCoordinator::testVerificationEnabledByDefault() {
    QVERIFY(m_coord->isVerificationEnabled());
}

// ============================================================================
// Configuration
// ============================================================================

void TestFlashCoordinator::testSetVerification() {
    m_coord->setVerificationEnabled(false);
    QVERIFY(!m_coord->isVerificationEnabled());
    m_coord->setVerificationEnabled(true);
    QVERIFY(m_coord->isVerificationEnabled());
}

void TestFlashCoordinator::testSetBufferSize() {
    // Should not crash
    constexpr qint64 size128MB = 128LL * 1024 * 1024;
    m_coord->setBufferSize(size128MB);
}

void TestFlashCoordinator::testDefaultConcurrencyAndEject() {
    // The defaults must match what the settings dialog shows an untouched install,
    // or the dialog describes behaviour the coordinator does not have.
    QCOMPARE(m_coord->maxConcurrentWrites(), 1);
    // Ejecting is opt-in: a coordinator nobody configured must not start pulling
    // drives out from under the caller.
    QVERIFY(!m_coord->ejectOnCompletion());
}

void TestFlashCoordinator::testSetMaxConcurrentWrites() {
    m_coord->setMaxConcurrentWrites(4);
    QCOMPARE(m_coord->maxConcurrentWrites(), 4);
    m_coord->setMaxConcurrentWrites(1);
    QCOMPARE(m_coord->maxConcurrentWrites(), 1);
}

void TestFlashCoordinator::testSetMaxConcurrentWritesRejectsBelowOne() {
    // A ceiling below one would start no worker at all, so the run could never
    // finish. Refuse it and keep the previous ceiling rather than silently
    // substituting a number the caller did not ask for.
    m_coord->setMaxConcurrentWrites(3);
    m_coord->setMaxConcurrentWrites(0);
    QCOMPARE(m_coord->maxConcurrentWrites(), 3);
    m_coord->setMaxConcurrentWrites(-8);
    QCOMPARE(m_coord->maxConcurrentWrites(), 3);
}

void TestFlashCoordinator::testSetEjectOnCompletion() {
    m_coord->setEjectOnCompletion(true);
    QVERIFY(m_coord->ejectOnCompletion());
    m_coord->setEjectOnCompletion(false);
    QVERIFY(!m_coord->ejectOnCompletion());
}

void TestFlashCoordinator::testStartableWorkerCountSeam() {
    // The scheduling decision the coordinator makes each time a drive finishes.
    // Exhaustively covered in test_flasher_policy; this asserts the coordinator
    // really routes through it rather than re-deriving the answer.
    QCOMPARE(FlashCoordinator::startableWorkerCount(1, 0, 3), 1);
    QCOMPARE(FlashCoordinator::startableWorkerCount(1, 1, 2), 0);
    QCOMPARE(FlashCoordinator::startableWorkerCount(4, 1, 5), 3);
    QCOMPARE(FlashCoordinator::startableWorkerCount(4, 0, 0), 0);
}

// ============================================================================
// State queries
// ============================================================================

void TestFlashCoordinator::testIsFlashingWhenIdle() {
    QVERIFY(!m_coord->isFlashing());
}

void TestFlashCoordinator::testStateWhenIdle() {
    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
}

// ============================================================================
// startFlash guards
// ============================================================================

void TestFlashCoordinator::testStartFlashEmptyDrives() {
    QSignalSpy spy(m_coord.get(), &FlashCoordinator::flashError);

    bool result = m_coord->startFlash("C:/test.iso", QStringList{});
    QVERIFY(!result);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("No target drives specified"));
}

// A 0-byte image must be refused BEFORE any drive work: it would write nothing yet
// pass verification (source==target==SHA-512 of empty), reporting a false success.
// The guard fires in validateImagePath, before target validation, so no drive is
// needed to exercise it headlessly.
void TestFlashCoordinator::testStartFlashRejectsZeroLengthImage() {
    QTemporaryFile img;
    QVERIFY(img.open());
    img.close();  // created, but left at 0 bytes

    QSignalSpy spy(m_coord.get(), &FlashCoordinator::flashError);
    const bool result = m_coord->startFlash(img.fileName(),
                                            QStringList{QStringLiteral("\\\\.\\PhysicalDrive99")});

    QVERIFY(!result);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().first().toString(),
             QStringLiteral("Image file is empty (0 bytes); nothing to flash"));
    QCOMPARE(m_coord->state(), sak::FlashState::Failed);
}

void TestFlashCoordinator::testStartFlashRejectsDuplicateTargets() {
    // Two workers writing the SAME physical disk concurrently corrupt each
    // other. A real (readable) image is needed so validateImagePath passes and
    // execution reaches the duplicate-target guard in validateTargets.
    QTemporaryFile img;
    QVERIFY(img.open());
    img.write("dummy image payload");
    img.flush();

    QSignalSpy spy(m_coord.get(), &FlashCoordinator::flashError);
    const QString drive = QStringLiteral("\\\\.\\PhysicalDrive99");
    const bool result = m_coord->startFlash(img.fileName(), QStringList{drive, drive});

    QVERIFY(!result);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).first().toString(),
             QStringLiteral("Duplicate target device: \\\\.\\PhysicalDrive99"));
    QCOMPARE(spy.at(1).first().toString(), QStringLiteral("Target validation failed"));
}

void TestFlashCoordinator::testFirstDuplicateTargetSeam() {
    // Distinct targets -> empty (no duplicate).
    QVERIFY(FlashCoordinator::firstDuplicateTarget(
                {QStringLiteral("\\\\.\\PhysicalDrive1"), QStringLiteral("\\\\.\\PhysicalDrive2")})
                .isEmpty());
    QVERIFY(FlashCoordinator::firstDuplicateTarget({}).isEmpty());

    // Exact duplicate -> returns the offending path.
    QCOMPARE(FlashCoordinator::firstDuplicateTarget({QStringLiteral("\\\\.\\PhysicalDrive3"),
                                                     QStringLiteral("\\\\.\\PhysicalDrive3")}),
             QStringLiteral("\\\\.\\PhysicalDrive3"));

    // Case- and whitespace-insensitive: the same disk written two ways is still a dup.
    // The dedup KEY is normalized (trim+lower) but the RETURN is the caller's verbatim string,
    // which is surfaced into the user-facing flashError -- pin that verbatim contract.
    QCOMPARE(FlashCoordinator::firstDuplicateTarget({QStringLiteral("\\\\.\\PhysicalDrive4"),
                                                     QStringLiteral("  \\\\.\\PHYSICALDRIVE4 ")}),
             QStringLiteral("  \\\\.\\PHYSICALDRIVE4 "));
}

void TestFlashCoordinator::testParsePhysicalDriveNumberSeam() {
    // Well-formed device paths -> the numeric index.
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QStringLiteral("\\\\.\\PhysicalDrive0")),
             0);
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QStringLiteral("\\\\.\\PhysicalDrive7")),
             7);
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QStringLiteral("\\\\.\\PhysicalDrive42")),
             42);

    // No parseable index -> -1 (the re-online / guard paths then fail closed).
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QStringLiteral("\\\\.\\PhysicalDrive")),
             -1);
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QStringLiteral("\\\\.\\C:")), -1);
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QString()), -1);
    QCOMPARE(FlashCoordinator::parsePhysicalDriveNumber(QStringLiteral("PhysicalDriveX")), -1);
}

void TestFlashCoordinator::testCancelWhenIdle() {
    // Cancel when not flashing should be safe
    m_coord->cancel();
    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
}

QTEST_MAIN(TestFlashCoordinator)
#include "test_flash_coordinator.moc"
