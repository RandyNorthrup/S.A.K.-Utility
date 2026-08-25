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
    void testStartFlashRejectsAliasedPhysicalDriveTargets();
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
    // The rows above are the region where a naive local "free slots" formula agrees
    // with the policy, so they cannot distinguish routing from re-deriving. These
    // three sit on the policy's saturating edges (flasher_policy.h:70-78) and are the
    // ones that actually prove the forwarder: a re-derivation gets every one wrong.
    // A ceiling below 1 is raised to 1, never 0 -- 0 would leave the run unable to
    // ever start a worker.
    QCOMPARE(FlashCoordinator::startableWorkerCount(0, 0, 3), 1);
    // Nothing queued starts nothing, and never a negative count.
    QCOMPARE(FlashCoordinator::startableWorkerCount(4, 0, -1), 0);
    // A negative running count is clamped to 0 rather than inflating the free slots
    // past the ceiling.
    QCOMPARE(FlashCoordinator::startableWorkerCount(2, -3, 5), 2);
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

    // The empty-target refusal must happen BEFORE beginFlashClaim() publishes
    // Validating (flash_coordinator.cpp:241-245 ordered ahead of :250-256), because
    // nothing on this early-return path releases a claim. If the guard were reordered
    // after the claim, m_state would stay Validating forever and isFlashing() would
    // report busy, so every later startFlash would be refused with "A flash run is
    // already in progress" -- invisible to the return value and the error text above.
    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
    QVERIFY(!m_coord->isFlashing());

    // ...and the coordinator is genuinely reusable: a second call reaches its OWN
    // empty-list guard rather than being turned away by a stale re-entry claim.
    QVERIFY(!m_coord->startFlash("C:/test.iso", QStringList{}));
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).first().toString(), QStringLiteral("No target drives specified"));
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
// String equality is NOT device identity: "\\.\PhysicalDriveN" and "\\?\PhysicalDriveN"
// are different strings naming ONE disk, so they slip past the string-equality guard.
// Pin the SECOND, independent guard (flash_coordinator.cpp:1260-1269) and its wiring --
// without it these two targets are accepted and two raw writers interleave on one disk.
void TestFlashCoordinator::testStartFlashRejectsAliasedPhysicalDriveTargets() {
    // Pure seam: same disk number reached by two different path spellings.
    QCOMPARE(FlashCoordinator::firstDuplicatePhysicalDrive(
                 {QStringLiteral("\\\\.\\PhysicalDrive5"),
                  QStringLiteral("\\\\?\\PhysicalDrive5")}),
             QStringLiteral("\\\\?\\PhysicalDrive5"));
    // Distinct disks -> no duplicate.
    QVERIFY(FlashCoordinator::firstDuplicatePhysicalDrive(
                {QStringLiteral("\\\\.\\PhysicalDrive5"), QStringLiteral("\\\\.\\PhysicalDrive6")})
                .isEmpty());
    // Unparseable identity is skipped, not folded together (validateSingleTarget refuses those).
    QVERIFY(FlashCoordinator::firstDuplicatePhysicalDrive(
                {QStringLiteral("\\\\.\\C:"), QStringLiteral("\\\\.\\D:")})
                .isEmpty());

    // Wiring: the guard must actually run inside validateTargets, ahead of per-target
    // validation. A real (readable) image so validateImagePath passes.
    QTemporaryFile img;
    QVERIFY(img.open());
    img.write("dummy image payload");
    img.flush();

    QSignalSpy spy(m_coord.get(), &FlashCoordinator::flashError);
    const bool result = m_coord->startFlash(img.fileName(),
                                            QStringList{QStringLiteral("\\\\.\\PhysicalDrive99"),
                                                        QStringLiteral("\\\\?\\PhysicalDrive99")});

    QVERIFY(!result);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(0).first().toString(),
             QStringLiteral("Duplicate target device: \\\\?\\PhysicalDrive99 names a physical "
                            "disk that is already in the target list"));
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

    // Non-adjacent duplicate: the seen-set spans the WHOLE prefix, not just the previous
    // element. Ticking drive 5, drive 6, then drive 5 again is the shape the real selection
    // path produces; an adjacent-pair scan would wave it through.
    QCOMPARE(FlashCoordinator::firstDuplicateTarget({QStringLiteral("\\\\.\\PhysicalDrive5"),
                                                     QStringLiteral("\\\\.\\PhysicalDrive6"),
                                                     QStringLiteral("\\\\.\\PhysicalDrive5")}),
             QStringLiteral("\\\\.\\PhysicalDrive5"));

    // The normalized key is remembered ACROSS the gap too, and the value returned is the LATER
    // occurrence verbatim (that string is what reaches the user-facing flashError), not the
    // first one and not the normalized key.
    QCOMPARE(FlashCoordinator::firstDuplicateTarget({QStringLiteral("\\\\.\\PhysicalDrive7"),
                                                     QStringLiteral("\\\\.\\PhysicalDrive8"),
                                                     QStringLiteral("  \\\\.\\PHYSICALDRIVE7 ")}),
             QStringLiteral("  \\\\.\\PHYSICALDRIVE7 "));

    // Three distinct targets stay accepted (the seen-set does not false-positive).
    QVERIFY(FlashCoordinator::firstDuplicateTarget({QStringLiteral("\\\\.\\PhysicalDrive1"),
                                                    QStringLiteral("\\\\.\\PhysicalDrive2"),
                                                    QStringLiteral("\\\\.\\PhysicalDrive3")})
                .isEmpty());
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
    // Cancel with no run in flight must be a COMPLETE no-op, not merely state-neutral.
    // The isFlashing() guard (src/core/flash_coordinator.cpp:622-624) returns BEFORE the
    // locked state write (:646) AND before every announcement (stateChanged :660,
    // emitTerminalOutcome/flashCompleted :662). state() alone only sees the write, so it
    // cannot tell a no-op apart from a cancel broadcast to every listener. The Image
    // Flasher panel drives its UI off exactly those signals (src/gui/image_flasher_panel.cpp
    // :125 stateChanged, :133 flashCompleted), so an idle coordinator that announced
    // Cancelled would render a finished run with nothing behind it. Pin the channels.
    QSignalSpy stateSpy(m_coord.get(), &FlashCoordinator::stateChanged);
    QSignalSpy completedSpy(m_coord.get(), &FlashCoordinator::flashCompleted);
    QSignalSpy errorSpy(m_coord.get(), &FlashCoordinator::flashError);
    QVERIFY(stateSpy.isValid());
    QVERIFY(completedSpy.isValid());
    QVERIFY(errorSpy.isValid());

    m_coord->cancel();

    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
    QVERIFY(!m_coord->isFlashing());
    QCOMPARE(m_coord->progress().state, sak::FlashState::Idle);
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(completedSpy.count(), 0);
    QCOMPARE(errorSpy.count(), 0);
}

QTEST_MAIN(TestFlashCoordinator)
#include "test_flash_coordinator.moc"
