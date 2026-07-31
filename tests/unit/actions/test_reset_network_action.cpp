// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_reset_network_action.cpp
/// @brief Unit tests for ResetNetworkAction pure seams (Codex B6-12/13):
///        the adapter-reset script must fail closed on a per-adapter error, and
///        a timed-out / non-zero step must count as a failure for the verdict.

#include "sak/actions/reset_network_action.h"

#include <QtTest/QtTest>

using sak::ResetNetworkAction;

class ResetNetworkActionTests : public QObject {
    Q_OBJECT

    using Action = ResetNetworkAction;

private Q_SLOTS:
    // B6-12: Restart-NetAdapter must be forced terminating and exit non-zero on error.
    void buildAdapterResetScript_failsClosed();
    // B6-13: the outcome must treat a timed-out / non-zero step as a failure.
    void stepFailed_truthTable();
};

void ResetNetworkActionTests::buildAdapterResetScript_failsClosed() {
    const QString script = Action::buildAdapterResetScript();

    // Non-terminating errors are promoted to terminating and caught.
    QVERIFY(script.contains(QStringLiteral("$ErrorActionPreference = 'Stop'")));
    QVERIFY(script.contains(QStringLiteral("-ErrorAction Stop")));
    QVERIFY(script.contains(QStringLiteral("catch")));
    // A failure exits non-zero so the caller records it (vs. the old exit-0 pipeline).
    QVERIFY(script.contains(QStringLiteral("exit 1")));
    // Still actually restarts the up adapters.
    QVERIFY(script.contains(QStringLiteral("Restart-NetAdapter")));
}

void ResetNetworkActionTests::stepFailed_truthTable() {
    QVERIFY(!Action::stepFailed(false, 0));  // clean success
    QVERIFY(Action::stepFailed(true, 0));    // timed out (even with exit 0)
    QVERIFY(Action::stepFailed(false, 1));   // non-zero exit
    QVERIFY(Action::stepFailed(true, 5));    // both
}

QTEST_GUILESS_MAIN(ResetNetworkActionTests)
#include "test_reset_network_action.moc"
