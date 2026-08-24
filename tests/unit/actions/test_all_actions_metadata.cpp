// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/generate_system_report_action.h"
#include "sak/actions/optimize_power_settings_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/screenshot_settings_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/quick_action.h"

#include <QMap>
#include <QSignalSpy>
#include <QTest>

using namespace sak;

/**
 * @brief Per-action metadata and behavior validation.
 *
 * Validates that every action returns the exact expected
 * name, category, and admin flag. Also exercises cancel
 * behavior and status transitions for all actions.
 */
class TestAllActionsMetadata : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // -- Metadata -- exact expected values ----------------------
    void testOptimizePowerSettings();

    void testVerifySystemFiles();
    void testCheckDiskErrors();
    void testResetNetwork();

    void testGenerateSystemReport();

    void testScreenshotSettings();
    void testBackupBitlockerKeys();

    // -- Cancel behavior for all actions ------------------------
    void testCancelAllActionsWhenIdle();

    // -- Status remains Idle after cancel -----------------------
    void testStatusAfterCancel();

    void cleanupTestCase();

private:
    std::vector<std::unique_ptr<QuickAction>> m_actions;

    QuickAction* findByName(const QString& name) const;

    void verifyAction(const QString& expectedName,
                      const QString& expectedDescription,
                      QuickAction::ActionCategory expectedCat,
                      bool expectedAdmin);

    std::vector<std::unique_ptr<QuickAction>> createAllActions() const;
};

std::vector<std::unique_ptr<QuickAction>> TestAllActionsMetadata::createAllActions() const {
    const auto backup = QStringLiteral("C:/SAK_Test_Backups");
    std::vector<std::unique_ptr<QuickAction>> actions;
    actions.push_back(std::make_unique<BackupBitlockerKeysAction>(backup));
    actions.push_back(std::make_unique<CheckDiskErrorsAction>());
    actions.push_back(std::make_unique<GenerateSystemReportAction>(backup));
    actions.push_back(std::make_unique<OptimizePowerSettingsAction>());
    actions.push_back(std::make_unique<ResetNetworkAction>());
    actions.push_back(std::make_unique<ScreenshotSettingsAction>(backup));
    actions.push_back(std::make_unique<VerifySystemFilesAction>());
    return actions;
}

void TestAllActionsMetadata::initTestCase() {
    m_actions = createAllActions();
    QVERIFY(!m_actions.empty());
}

QuickAction* TestAllActionsMetadata::findByName(const QString& name) const {
    for (const auto& a : m_actions) {
        if (a->name() == name) {
            return a.get();
        }
    }
    return nullptr;
}

void TestAllActionsMetadata::verifyAction(const QString& expectedName,
                                          const QString& expectedDescription,
                                          QuickAction::ActionCategory expectedCat,
                                          bool expectedAdmin) {
    auto* action = findByName(expectedName);
    QVERIFY2(action != nullptr, qPrintable("Action not found: " + expectedName));

    // findByName() returns the FIRST name-equal entry, so re-comparing action->name() to
    // expectedName is vacuous -- it re-checks the very field the lookup matched on. Pin the
    // premise the lookup rests on instead: exactly ONE catalog entry carries this name, so the
    // instance under test is unambiguous and a shadowed sibling cannot go uninspected.
    int name_matches = 0;
    for (const auto& candidate : m_actions) {
        if (candidate->name() == expectedName) {
            ++name_matches;
        }
    }
    QCOMPARE(name_matches, 1);
    // Descriptions are fixed literals and are what the technician reads before authorizing a
    // destructive run: pin the exact text, case included. A case-insensitive substring match
    // accepts a truncated or reworded blurb, and accepts the wrong case in a product name.
    QCOMPARE(action->description(), expectedDescription);
    QCOMPARE(action->category(), expectedCat);
    QCOMPARE(action->requiresAdmin(), expectedAdmin);
    QCOMPARE(action->status(), QuickAction::ActionStatus::Idle);
}

// ============================================================================
// System Optimization (1)
// ============================================================================

void TestAllActionsMetadata::testOptimizePowerSettings() {
    verifyAction("Optimize Power Settings",
                 "Switch to High Performance power plan",
                 QuickAction::ActionCategory::SystemOptimization,
                 false);
}

// ============================================================================
// Maintenance (3)
// ============================================================================

void TestAllActionsMetadata::testVerifySystemFiles() {
    verifyAction("Verify System Files",
                 "Run SFC and DISM to repair system files",
                 QuickAction::ActionCategory::Maintenance,
                 true);
}

void TestAllActionsMetadata::testCheckDiskErrors() {
    verifyAction("Check Disk Errors",
                 "Run CHKDSK scan on all drives",
                 QuickAction::ActionCategory::Maintenance,
                 true);
}

void TestAllActionsMetadata::testResetNetwork() {
    verifyAction("Reset Network Settings",
                 "Reset TCP/IP, DNS, Winsock and Windows Firewall rules, and restart network "
                 "adapters",
                 QuickAction::ActionCategory::Maintenance,
                 true);
}

// ============================================================================
// Troubleshooting (1)
// ============================================================================

void TestAllActionsMetadata::testGenerateSystemReport() {
    verifyAction("Generate System Report",
                 "Create comprehensive system report",
                 QuickAction::ActionCategory::Troubleshooting,
                 false);
}

// ============================================================================
// Emergency Recovery (2)
// ============================================================================

void TestAllActionsMetadata::testScreenshotSettings() {
    verifyAction("Screenshot Settings",
                 "Capture screenshots of Windows Settings",
                 QuickAction::ActionCategory::EmergencyRecovery,
                 false);
}

void TestAllActionsMetadata::testBackupBitlockerKeys() {
    verifyAction("BitLocker Key Backup",
                 "Backup BitLocker recovery keys for all encrypted volumes",
                 QuickAction::ActionCategory::EmergencyRecovery,
                 true);
}

// ============================================================================
// Cancel behavior
// ============================================================================

void TestAllActionsMetadata::testCancelAllActionsWhenIdle() {
    // The old disjunction accepted BOTH candidate outcomes, so it held for every
    // implementation. QuickAction::cancel() only transitions to Cancelled from Scanning or
    // Running (src/core/quick_action.cpp:43) and no concrete action overrides the virtual, so
    // every fixture action being Idle here leaves exactly ONE correct post-state. Because
    // setStatus() is never reached, statusChanged must not fire at all -- a subclass that
    // fabricated Cancelled for something that never ran would drive the panel's button state
    // off a bogus Idle->Cancelled emission.
    for (const auto& action : m_actions) {
        QSignalSpy status_spy(action.get(), &QuickAction::statusChanged);
        QVERIFY(status_spy.isValid());

        action->cancel();

        QVERIFY2(action->status() == QuickAction::ActionStatus::Idle,
                 qPrintable("cancel() from Idle must leave '" + action->name() +
                            "' exactly Idle, not fabricate Cancelled; got status " +
                            QString::number(static_cast<int>(action->status()))));
        QVERIFY2(status_spy.count() == 0,
                 qPrintable("cancel() from Idle must not emit statusChanged for '" +
                            action->name() + "'; got " + QString::number(status_spy.count()) +
                            " emission(s)"));
    }
}

void TestAllActionsMetadata::testStatusAfterCancel() {
    auto fresh = createAllActions();

    for (const auto& action : fresh) {
        // The old assertion was satisfied by the fixture's own pre-state: a freshly built
        // action is Idle before cancel() is ever called, so it proved nothing about cancel().
        QCOMPARE(action->status(), QuickAction::ActionStatus::Idle);

        // Idle is NOT in-flight: cancel() must not fabricate a Cancelled status for an action
        // that never ran (the false arm of the quick_action.cpp:43 guard).
        action->cancel();
        QVERIFY2(action->status() == QuickAction::ActionStatus::Idle,
                 qPrintable("cancel() from Idle changed status of '" + action->name() + "'"));

        // Scanning IS in-flight: cancel() must transition to Cancelled.
        action->clearCancellation();
        action->updateStatus(QuickAction::ActionStatus::Scanning);
        action->cancel();
        QVERIFY2(action->status() == QuickAction::ActionStatus::Cancelled,
                 qPrintable("cancel() during Scanning left '" + action->name() + "' un-cancelled"));

        // Running IS in-flight: cancel() must transition to Cancelled AND emit statusChanged --
        // that signal is what releases the panel's Cancel button.
        action->clearCancellation();
        action->updateStatus(QuickAction::ActionStatus::Running);
        QSignalSpy status_spy(action.get(), &QuickAction::statusChanged);
        action->cancel();
        QCOMPARE(action->status(), QuickAction::ActionStatus::Cancelled);
        QCOMPARE(status_spy.count(), 1);
        QCOMPARE(status_spy.at(0).at(0).value<QuickAction::ActionStatus>(),
                 QuickAction::ActionStatus::Cancelled);

        // A terminal status is NOT in-flight: cancel() must leave it alone.
        action->clearCancellation();
        action->updateStatus(QuickAction::ActionStatus::Success);
        action->cancel();
        QCOMPARE(action->status(), QuickAction::ActionStatus::Success);
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void TestAllActionsMetadata::cleanupTestCase() {
    m_actions.clear();
}

QTEST_MAIN(TestAllActionsMetadata)
#include "test_all_actions_metadata.moc"
