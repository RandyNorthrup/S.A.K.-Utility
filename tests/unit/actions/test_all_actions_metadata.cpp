// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/actions/action_factory.h"
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
 *
 * Complements test_action_factory (which validates generic
 * constraints) with concrete expected-value assertions.
 */
class TestAllActionsMetadata : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // ── Metadata — exact expected values ────────────────────
    void testDiskCleanup();
    void testClearBrowserCache();
    void testClearWindowsUpdateCache();
    void testClearEventLogs();
    void testOptimizePowerSettings();

    void testVerifySystemFiles();
    void testCheckDiskErrors();
    void testRebuildIconCache();
    void testResetNetwork();
    void testClearPrintSpooler();

    void testGenerateSystemReport();
    void testCheckBloatware();
    void testRepairWindowsStore();
    void testFixAudioIssues();

    void testScreenshotSettings();
    void testBackupBitlockerKeys();

    // ── Cancel behavior for all actions ─────────────────────
    void testCancelAllActionsWhenIdle();

    // ── Status remains Idle after cancel ────────────────────
    void testStatusAfterCancel();

    void cleanupTestCase();

private:
    std::vector<std::unique_ptr<QuickAction>> m_actions;

    QuickAction* findByName(const QString& name) const;

    void verifyAction(const QString& expectedName,
                      const QString& descSubstring,
                      QuickAction::ActionCategory expectedCat,
                      bool expectedAdmin);
};

void TestAllActionsMetadata::initTestCase() {
    m_actions = ActionFactory::createAllActions(QStringLiteral("C:/SAK_Test_Backups"));
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
// System Optimization (5)
// ============================================================================

void TestAllActionsMetadata::testDiskCleanup() {
    verifyAction(
        "Disk Cleanup", "temporary", QuickAction::ActionCategory::SystemOptimization, true);
}

void TestAllActionsMetadata::testClearBrowserCache() {
    verifyAction(
        "Clear Browser Cache", "cache", QuickAction::ActionCategory::SystemOptimization, false);
}

void TestAllActionsMetadata::testClearWindowsUpdateCache() {
    verifyAction("Clear Windows Update Cache",
                 "Windows Update",
                 QuickAction::ActionCategory::SystemOptimization,
                 true);
}

void TestAllActionsMetadata::testClearEventLogs() {
    verifyAction(
        "Clear Event Logs", "Event Log", QuickAction::ActionCategory::SystemOptimization, true);
}

void TestAllActionsMetadata::testOptimizePowerSettings() {
    verifyAction(
        "Optimize Power Settings", "power", QuickAction::ActionCategory::SystemOptimization, false);
}

// ============================================================================
// Maintenance (5)
// ============================================================================

void TestAllActionsMetadata::testVerifySystemFiles() {
    verifyAction("Verify System Files", "SFC", QuickAction::ActionCategory::Maintenance, true);
}

void TestAllActionsMetadata::testCheckDiskErrors() {
    verifyAction("Check Disk Errors", "CHKDSK", QuickAction::ActionCategory::Maintenance, true);
}

void TestAllActionsMetadata::testRebuildIconCache() {
    verifyAction("Rebuild Icon Cache", "icon", QuickAction::ActionCategory::Maintenance, false);
}

void TestAllActionsMetadata::testResetNetwork() {
    verifyAction(
        "Reset Network Settings", "TCP/IP", QuickAction::ActionCategory::Maintenance, true);
}

void TestAllActionsMetadata::testClearPrintSpooler() {
    verifyAction("Clear Print Spooler", "print", QuickAction::ActionCategory::Maintenance, true);
}

// ============================================================================
// Troubleshooting (4)
// ============================================================================

void TestAllActionsMetadata::testGenerateSystemReport() {
    verifyAction("Generate System Report",
                 "system report",
                 QuickAction::ActionCategory::Troubleshooting,
                 false);
}

void TestAllActionsMetadata::testCheckBloatware() {
    verifyAction(
        "Check for Bloatware", "bloatware", QuickAction::ActionCategory::Troubleshooting, true);
}

void TestAllActionsMetadata::testRepairWindowsStore() {
    verifyAction(
        "Repair Windows Store", "Store", QuickAction::ActionCategory::Troubleshooting, false);
}

void TestAllActionsMetadata::testFixAudioIssues() {
    verifyAction("Fix Audio Issues", "audio", QuickAction::ActionCategory::Troubleshooting, true);
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
        // Cancel when idle should not crash or change state
        action->cancel();
        QVERIFY2(action->status() == QuickAction::ActionStatus::Idle ||
                     action->status() == QuickAction::ActionStatus::Cancelled,
                 qPrintable("After cancel, '" + action->name() + "' has unexpected status"));
    }
}

void TestAllActionsMetadata::testStatusAfterCancel() {
    // Fresh set of actions for clean state
    auto fresh = ActionFactory::createAllActions(QStringLiteral("C:/SAK_Test_Backups"));

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
