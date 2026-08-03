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

    // ── Construction defaults ───────────────────────────────
    void testInitialState();
    void testInitialProgress();
    void testVerificationEnabledByDefault();

    // ── Configuration ───────────────────────────────────────
    void testSetVerification();
    void testSetBufferSize();

    // ── State queries ───────────────────────────────────────
    void testIsFlashingWhenIdle();
    void testStateWhenIdle();

    // ── startFlash guards ───────────────────────────────────
    void testStartFlashEmptyDrives();
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
    QVERIFY(spy.count() >= 1);
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
    bool sawDuplicate = false;
    for (const QList<QVariant>& args : spy) {
        if (!args.isEmpty() &&
            args.first().toString().contains(QStringLiteral("Duplicate"), Qt::CaseInsensitive)) {
            sawDuplicate = true;
        }
    }
    QVERIFY2(sawDuplicate, "expected a Duplicate target device flashError");
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
    QVERIFY(!FlashCoordinator::firstDuplicateTarget({QStringLiteral("\\\\.\\PhysicalDrive4"),
                                                     QStringLiteral("  \\\\.\\PHYSICALDRIVE4 ")})
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
    // Cancel when not flashing should be safe
    m_coord->cancel();
    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
}

QTEST_MAIN(TestFlashCoordinator)
#include "test_flash_coordinator.moc"
