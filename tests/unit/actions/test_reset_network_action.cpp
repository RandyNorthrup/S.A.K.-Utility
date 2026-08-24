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

    // Non-terminating errors are promoted to terminating and caught. The preference must be in
    // force BEFORE the (unguarded) Get-NetAdapter enumeration -- that enumeration carries no
    // -ErrorAction of its own, so the top-level preference alone makes an enumeration failure
    // terminating.
    const qsizetype pref_at = script.indexOf(QStringLiteral("$ErrorActionPreference = 'Stop'"));
    const qsizetype enum_at = script.indexOf(QStringLiteral("Get-NetAdapter"));
    QVERIFY(pref_at >= 0);
    QVERIFY(enum_at > pref_at);

    // The guard flags must be bound to the Restart-NetAdapter invocation ITSELF: moving
    // -ErrorAction Stop onto the Get-NetAdapter enumeration, or dropping -Confirm:$false, leaves
    // the per-adapter restart non-terminating / able to block on a confirmation prompt, while a
    // script-wide substring check stays green.
    const QStringList script_lines = script.split(QLatin1Char('\n'));
    QString restart_line;
    for (const QString& line : script_lines) {
        if (line.contains(QStringLiteral("Restart-NetAdapter"))) {
            QVERIFY2(restart_line.isEmpty(), "expected exactly one Restart-NetAdapter invocation");
            restart_line = line;
        }
    }
    QVERIFY(!restart_line.isEmpty());
    QVERIFY(restart_line.contains(QStringLiteral("-Name $_.Name")));
    QVERIFY(restart_line.contains(QStringLiteral("-Confirm:$false")));
    QVERIFY(restart_line.contains(QStringLiteral("-ErrorAction Stop")));

    // The catch arm must do more than exist and exit 1: it must write the exception message to
    // stderr, because the caller logs std_err and that is the ONLY cause the operator ever sees
    // behind "exit 1".
    const qsizetype catch_at = script.indexOf(QStringLiteral("} catch {"));
    QVERIFY(catch_at >= 0);
    const QString catch_arm = script.mid(catch_at);
    QVERIFY(catch_arm.contains(QStringLiteral("Write-Error $_.Exception.Message")));
    QVERIFY(catch_arm.contains(QStringLiteral("exit 1")));
    // A failure exits non-zero so the caller records it (vs. the old exit-0 pipeline).
    // Pin WHICH arm exits WHICH code: the sole `exit 1` lives inside the catch arm and
    // the sole `exit 0` is the clean path that precedes it. Swapping the two codes in
    // reset_network_action.cpp:46/:49 turns both ordering checks red.
    const qsizetype catch_pos = script.indexOf(QStringLiteral("} catch {"));
    QVERIFY(catch_pos > 0);
    QVERIFY(script.count(QStringLiteral("exit 0")) == 1);
    QVERIFY(script.count(QStringLiteral("exit 1")) == 1);
    QVERIFY(script.indexOf(QStringLiteral("exit 0")) < catch_pos);  // success arm exits 0
    QVERIFY(script.indexOf(QStringLiteral("exit 1")) > catch_pos);  // failure arm exits 1
    // Still actually restarts the up adapters -- and ONLY the up ones. The filter is
    // load-bearing because of the -ErrorAction Stop above: an unfiltered Get-NetAdapter throws on
    // the first Disabled/Disconnected adapter, so the catch exits 1 on a healthy machine AND the
    // remaining real adapters are never restarted.
    QVERIFY(script.contains(QStringLiteral("Restart-NetAdapter -Name $_.Name")));
    QVERIFY(script.contains(
        QStringLiteral("Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object {")));
}

void ResetNetworkActionTests::stepFailed_truthTable() {
    QVERIFY(!Action::stepFailed(false, 0));  // clean success
    QVERIFY(Action::stepFailed(true, 0));    // timed out (even with exit 0)
    QVERIFY(Action::stepFailed(false, 1));   // non-zero exit
    QVERIFY(Action::stepFailed(true, 5));    // both
    // NEGATIVE exit codes are the ones that matter most and the only positive value tested was
    // 1: -1 is the fail-closed sentinel of a ProcessResult that never ran, and 0xC0000005
    // arrives as a negative int for a crashed child. A guard written `exit_code > 0` treats both
    // as success and reports a network reset that never happened.
    QVERIFY(Action::stepFailed(false, -1));
    QVERIFY(Action::stepFailed(false, -1'073'741'819));
}

QTEST_GUILESS_MAIN(ResetNetworkActionTests)
#include "test_reset_network_action.moc"
