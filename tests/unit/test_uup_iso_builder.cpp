// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <QTest>
#include "sak/uup_iso_builder.h"

/**
 * @brief Unit tests for UupIsoBuilder.
 *
 * Covers construction defaults, phase queries, and
 * running state. Actual ISO building requires network
 * access and external tools; not tested here.
 */
class TestUupIsoBuilder : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction defaults ───────────────────────────────
    void testInitialPhase();
    void testNotRunningInitially();

    // ── Cancel when idle ────────────────────────────────────
    void testCancelWhenIdle();
};

// ============================================================================
// Construction
// ============================================================================

void TestUupIsoBuilder::testInitialPhase() {
    UupIsoBuilder builder;
    QCOMPARE(builder.currentPhase(),
             UupIsoBuilder::Phase::Idle);
}

void TestUupIsoBuilder::testNotRunningInitially() {
    UupIsoBuilder builder;
    QVERIFY(!builder.isRunning());
}

// ============================================================================
// Cancel
// ============================================================================

void TestUupIsoBuilder::testCancelWhenIdle() {
    UupIsoBuilder builder;
    builder.cancel();
    QCOMPARE(builder.currentPhase(),
             UupIsoBuilder::Phase::Idle);
}

QTEST_MAIN(TestUupIsoBuilder)
#include "test_uup_iso_builder.moc"
