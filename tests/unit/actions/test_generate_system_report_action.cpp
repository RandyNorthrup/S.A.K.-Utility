// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_generate_system_report_action.cpp
/// @brief Unit tests for GenerateSystemReportAction pure seams (Codex B6-15):
///        a collector that fails to run must be surfaced, and a saved-but-empty
///        report must not be reported as a successful comprehensive report.

#include "sak/actions/generate_system_report_action.h"

#include <QtTest/QtTest>

using sak::GenerateSystemReportAction;

class GenerateSystemReportActionTests : public QObject {
    Q_OBJECT

    using Action = GenerateSystemReportAction;

private Q_SLOTS:
    void collectorFailed_truthTable();
    void reportGenerationSucceeded_requiresSaveAndCollectors();
};

void GenerateSystemReportActionTests::collectorFailed_truthTable() {
    QVERIFY(!Action::collectorFailed(false, 0));  // clean run
    QVERIFY(Action::collectorFailed(true, 0));    // timed out
    QVERIFY(Action::collectorFailed(false, 1));   // non-zero exit
}

void GenerateSystemReportActionTests::reportGenerationSucceeded_requiresSaveAndCollectors() {
    QVERIFY(Action::reportGenerationSucceeded(true, true));  // wrote + all collectors ran
    // Saved, but a collector failed -> must NOT be a success (the B6-15 bug).
    QVERIFY(!Action::reportGenerationSucceeded(true, false));
    // Write failed -> failure regardless.
    QVERIFY(!Action::reportGenerationSucceeded(false, true));
    QVERIFY(!Action::reportGenerationSucceeded(false, false));
}

QTEST_GUILESS_MAIN(GenerateSystemReportActionTests)
#include "test_generate_system_report_action.moc"
