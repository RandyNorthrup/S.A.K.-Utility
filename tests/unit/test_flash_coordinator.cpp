// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QTest>
#include <QSignalSpy>
#include "sak/flash_coordinator.h"

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
    void testSetBufferCount();

    // ── State queries ───────────────────────────────────────
    void testIsFlashingWhenIdle();
    void testStateWhenIdle();

    // ── startFlash guards ───────────────────────────────────
    void testStartFlashEmptyDrives();
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
    QCOMPARE(p.bytesWritten,
             static_cast<qint64>(0));
    QCOMPARE(p.totalBytes,
             static_cast<qint64>(0));
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

void TestFlashCoordinator::testSetBufferCount() {
    // Should not crash
    m_coord->setBufferCount(8);
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
    QSignalSpy spy(m_coord.get(),
                   &FlashCoordinator::flashError);

    bool result = m_coord->startFlash(
        "C:/test.iso", QStringList{});
    QVERIFY(!result);
    QVERIFY(spy.count() >= 1);
}

void TestFlashCoordinator::testCancelWhenIdle() {
    // Cancel when not flashing should be safe
    m_coord->cancel();
    QCOMPARE(m_coord->state(), sak::FlashState::Idle);
}

QTEST_MAIN(TestFlashCoordinator)
#include "test_flash_coordinator.moc"
