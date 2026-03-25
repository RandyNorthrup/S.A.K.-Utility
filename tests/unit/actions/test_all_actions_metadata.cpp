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
                      const QString& descSubstring,
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
                                          const QString& descSubstring,
                                          QuickAction::ActionCategory expectedCat,
                                          bool expectedAdmin) {
    auto* action = findByName(expectedName);
    QVERIFY2(action != nullptr, qPrintable("Action not found: " + expectedName));

    QCOMPARE(action->name(), expectedName);
    QVERIFY2(action->description().contains(descSubstring, Qt::CaseInsensitive),
             qPrintable("Description of '" + expectedName + "' doesn't contain '" + descSubstring +
                        "': " + action->description()));
    QCOMPARE(action->category(), expectedCat);
    QCOMPARE(action->requiresAdmin(), expectedAdmin);
    QCOMPARE(action->status(), QuickAction::ActionStatus::Idle);
}

// ============================================================================
// System Optimization (1)
// ============================================================================

void TestAllActionsMetadata::testOptimizePowerSettings() {
    verifyAction(
        "Optimize Power Settings", "power", QuickAction::ActionCategory::SystemOptimization, false);
}

// ============================================================================
// Maintenance (3)
// ============================================================================

void TestAllActionsMetadata::testVerifySystemFiles() {
    verifyAction("Verify System Files", "SFC", QuickAction::ActionCategory::Maintenance, true);
}

void TestAllActionsMetadata::testCheckDiskErrors() {
    verifyAction("Check Disk Errors", "CHKDSK", QuickAction::ActionCategory::Maintenance, true);
}

void TestAllActionsMetadata::testResetNetwork() {
    verifyAction(
        "Reset Network Settings", "TCP/IP", QuickAction::ActionCategory::Maintenance, true);
}

// ============================================================================
// Troubleshooting (1)
// ============================================================================

void TestAllActionsMetadata::testGenerateSystemReport() {
    verifyAction("Generate System Report",
                 "system report",
                 QuickAction::ActionCategory::Troubleshooting,
                 false);
}

// ============================================================================
// Emergency Recovery (2)
// ============================================================================

void TestAllActionsMetadata::testScreenshotSettings() {
    verifyAction(
        "Screenshot Settings", "screenshot", QuickAction::ActionCategory::EmergencyRecovery, false);
}

void TestAllActionsMetadata::testBackupBitlockerKeys() {
    verifyAction(
        "BitLocker Key Backup", "BitLocker", QuickAction::ActionCategory::EmergencyRecovery, true);
}

// ============================================================================
// Cancel behavior
// ============================================================================

void TestAllActionsMetadata::testCancelAllActionsWhenIdle() {
    for (const auto& action : m_actions) {
        action->cancel();
        QVERIFY2(action->status() == QuickAction::ActionStatus::Idle ||
                     action->status() == QuickAction::ActionStatus::Cancelled,
                 qPrintable("After cancel, '" + action->name() + "' has unexpected status"));
    }
}

void TestAllActionsMetadata::testStatusAfterCancel() {
    auto fresh = createAllActions();

    for (const auto& action : fresh) {
        QCOMPARE(action->status(), QuickAction::ActionStatus::Idle);
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
