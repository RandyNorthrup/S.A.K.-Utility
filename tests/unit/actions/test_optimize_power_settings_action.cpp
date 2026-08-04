// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_optimize_power_settings_action.cpp
/// @brief Unit test for OptimizePowerSettingsAction pure seam (Codex B6-18):
///        "already optimized" must be decided by GUID, so a custom plan merely
///        named like "High Performance" is not mistaken for the built-in one.

#include "sak/actions/optimize_power_settings_action.h"

#include <QtTest/QtTest>

using sak::OptimizePowerSettingsAction;

class OptimizePowerSettingsActionTests : public QObject {
    Q_OBJECT

    using Action = OptimizePowerSettingsAction;

private Q_SLOTS:
    void isHighPerformanceGuid_matchesBuiltinsByGuid();
    void discoveryPermitsActivation_failsClosedWithoutDiscovery();
};

void OptimizePowerSettingsActionTests::isHighPerformanceGuid_matchesBuiltinsByGuid() {
    // Built-in High Performance (SCHEME_MIN) and Ultimate Performance, any case,
    // with surrounding whitespace.
    QVERIFY(Action::isHighPerformanceGuid(QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c")));
    QVERIFY(
        Action::isHighPerformanceGuid(QStringLiteral("  8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C  ")));
    QVERIFY(Action::isHighPerformanceGuid(QStringLiteral("e9a42b02-d5df-448d-aa00-03f14749eb61")));

    // A custom plan's GUID (even if the plan is NAMED "High Performance") is not
    // a built-in high-performance scheme.
    QVERIFY(!Action::isHighPerformanceGuid(QStringLiteral("11111111-2222-3333-4444-555555555555")));
    QVERIFY(!Action::isHighPerformanceGuid(QString()));
    QVERIFY(!Action::isHighPerformanceGuid(QStringLiteral("High Performance")));
}

void OptimizePowerSettingsActionTests::discoveryPermitsActivation_failsClosedWithoutDiscovery() {
    // CODEX_REVIEW_4 M-B3-22: a plan may be activated ONLY when powercfg discovery succeeded and
    // returned at least one plan. A failed discovery (or an empty list) must fail closed so the
    // optimizer never mutates the active plan on a hard-coded/guessed GUID.
    QVERIFY(!Action::discoveryPermitsActivation(false, 5));  // discovery failed -> no mutation
    QVERIFY(!Action::discoveryPermitsActivation(false, 0));
    QVERIFY(!Action::discoveryPermitsActivation(true, 0));   // ran but found nothing -> no mutation
    QVERIFY(Action::discoveryPermitsActivation(true, 1));    // ran and found plans -> may activate
    QVERIFY(Action::discoveryPermitsActivation(true, 42));
}

QTEST_GUILESS_MAIN(OptimizePowerSettingsActionTests)
#include "test_optimize_power_settings_action.moc"
