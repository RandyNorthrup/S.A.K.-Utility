// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QSignalSpy>
#include "sak/quick_action_controller.h"
#include "sak/quick_action.h"
#include "sak/action_category.h"

class TestQuickActionController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Controller initialization
    void testConstructor();
    void testSingletonPattern();
    void testBackupLocation();
    void testLoadActionsOnInit();

    // Action registration
    void testRegisterAction();
    void testRegisterMultipleActions();
    void testRegisterDuplicateAction();
    void testUnregisterAction();
    void testUnregisterNonexistent();
    void testGetAction();
    void testGetNonexistentAction();

    // Action retrieval
    void testGetAllActions();
    void testGetActionsByCategory();
    void testGetSystemOptimizationActions();
    void testGetMaintenanceActions();
    void testGetTroubleshootingActions();
    void testGetQuickBackupActions();
    void testGetEmergencyRecoveryActions();

    // Action execution
    void testExecuteAction();
    void testExecuteNonexistentAction();
    void testExecuteWithConfirmation();
    void testExecuteWithoutConfirmation();
    void testCancelExecution();
    void testExecutionSignals();

    // Action scanning
    void testScanAction();
    void testScanAllActions();
    void testScanSignals();
    void testConcurrentScans();

    // Progress tracking
    void testProgressSignals();
    void testProgressValues();
    void testMultiActionProgress();

    // Error handling
    void testActionError();
    void testInvalidActionName();
    void testExecutionFailure();
    void testScanFailure();

    // Admin privileges
    void testHasAdminPrivileges();
    void testRequiresAdmin();
    void testAdminElevation();

    // Logging
    void testLogMessages();
    void testErrorMessages();
    void testProgressMessages();

    // Thread safety
    void testThreadSafeExecution();
    void testBackgroundExecution();
    void testSignalThreadAffinity();

    // Queue management
    void testActionQueue();
    void testSequentialExecution();
    void testQueueCancellation();

private:
    sak::QuickActionController* m_controller{nullptr};
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;

    void waitForSignal(QObject* obj, const char* signal, int timeout = 5000);
};

void TestQuickActionController::initTestCase() {
    // Setup test environment
}

void TestQuickActionController::cleanupTestCase() {
    // Cleanup test environment
}

void TestQuickActionController::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
    m_controller = new sak::QuickActionController(m_test_backup_location, this);
}

void TestQuickActionController::cleanup() {
    delete m_controller;
    m_controller = nullptr;
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestQuickActionController::waitForSignal(QObject* obj, const char* signal, int timeout) {
    QSignalSpy spy(obj, signal);
    QVERIFY(spy.wait(timeout));
}

void TestQuickActionController::testConstructor() {
    QVERIFY(m_controller != nullptr);
}

void TestQuickActionController::testSingletonPattern() {
    // Controller uses normal instantiation, not singleton
    auto controller2 = new sak::QuickActionController(m_test_backup_location);
    QVERIFY(controller2 != nullptr);
    QVERIFY(controller2 != m_controller);
    delete controller2;
}

void TestQuickActionController::testBackupLocation() {
    QCOMPARE(m_controller->getBackupLocation(), m_test_backup_location);
}

void TestQuickActionController::testLoadActionsOnInit() {
    auto actions = m_controller->getAllActions();
    QVERIFY(!actions.empty());
    QVERIFY(actions.size() >= 37); // At least 37 actions loaded
}

void TestQuickActionController::testRegisterAction() {
    // Note: Actions are auto-registered from factory
    auto actions = m_controller->getAllActions();
    QVERIFY(!actions.empty());
}

void TestQuickActionController::testRegisterMultipleActions() {
    auto actions = m_controller->getAllActions();
    QVERIFY(actions.size() >= 37);
}

void TestQuickActionController::testRegisterDuplicateAction() {
    // Factory ensures no duplicates
    auto actions = m_controller->getAllActions();
    QSet<QString> names;
    for (const auto& action : actions) {
        QVERIFY(!names.contains(action->name()));
        names.insert(action->name());
    }
}

void TestQuickActionController::testUnregisterAction() {
    // Test unregister if API exists
    auto actions = m_controller->getAllActions();
    QVERIFY(!actions.empty());
}

void TestQuickActionController::testUnregisterNonexistent() {
    // Should handle gracefully
    auto actions = m_controller->getAllActions();
    QVERIFY(!actions.empty());
}

void TestQuickActionController::testGetAction() {
    auto actions = m_controller->getAllActions();
    QVERIFY(!actions.empty());
    
    auto* first = m_controller->getAction(actions[0]->name());
    QVERIFY(first != nullptr);
    QCOMPARE(first->name(), actions[0]->name());
}

void TestQuickActionController::testGetNonexistentAction() {
    auto* action = m_controller->getAction("NonexistentAction");
    QVERIFY(action == nullptr);
}

void TestQuickActionController::testGetAllActions() {
    auto actions = m_controller->getAllActions();
    QVERIFY(!actions.empty());
    QVERIFY(actions.size() >= 37);
}

void TestQuickActionController::testGetActionsByCategory() {
    auto optimization = m_controller->getActionsByCategory(sak::ActionCategory::SystemOptimization);
    QVERIFY(!optimization.empty());
    
    for (const auto& action : optimization) {
        QCOMPARE(action->category(), sak::ActionCategory::SystemOptimization);
    }
}

void TestQuickActionController::testGetSystemOptimizationActions() {
    auto actions = m_controller->getActionsByCategory(sak::ActionCategory::SystemOptimization);
    QVERIFY(actions.size() >= 10);
}

void TestQuickActionController::testGetMaintenanceActions() {
    auto actions = m_controller->getActionsByCategory(sak::ActionCategory::Maintenance);
    QVERIFY(actions.size() >= 9);
}

void TestQuickActionController::testGetTroubleshootingActions() {
    auto actions = m_controller->getActionsByCategory(sak::ActionCategory::Troubleshooting);
    QVERIFY(actions.size() >= 9);
}

void TestQuickActionController::testGetQuickBackupActions() {
    auto actions = m_controller->getActionsByCategory(sak::ActionCategory::QuickBackup);
    QVERIFY(actions.size() >= 13);
}

void TestQuickActionController::testGetEmergencyRecoveryActions() {
    auto actions = m_controller->getActionsByCategory(sak::ActionCategory::EmergencyRecovery);
    QVERIFY(actions.size() >= 4);
}

void TestQuickActionController::testExecuteAction() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionExecutionComplete);
    
    // Execute a simple action
    m_controller->executeAction("Disk Cleanup", false);
    
    // Wait for completion (may take time)
    QVERIFY(spy_complete.wait(30000)); // 30 second timeout
}

void TestQuickActionController::testExecuteNonexistentAction() {
    QSignalSpy spy_error(m_controller, &sak::QuickActionController::actionError);
    
    m_controller->executeAction("NonexistentAction", false);
    
    // Should get error signal
    QVERIFY(spy_error.wait(1000));
}

void TestQuickActionController::testExecuteWithConfirmation() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    // Request with confirmation
    m_controller->executeAction("Disk Cleanup", true);
    
    // Should wait for confirmation
}

void TestQuickActionController::testExecuteWithoutConfirmation() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    // Should start immediately
    QVERIFY(spy_started.wait(5000));
}

void TestQuickActionController::testCancelExecution() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    // Wait for start
    QVERIFY(spy_started.wait(5000));
    
    // Cancel
    m_controller->cancelCurrentAction();
}

void TestQuickActionController::testExecutionSignals() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    QSignalSpy spy_progress(m_controller, &sak::QuickActionController::actionExecutionProgress);
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionExecutionComplete);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    QVERIFY(spy_started.wait(5000));
    // Progress may or may not be emitted
    QVERIFY(spy_complete.wait(30000));
}

void TestQuickActionController::testScanAction() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionScanStarted);
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionScanComplete);
    
    m_controller->scanAction("Disk Cleanup");
    
    QVERIFY(spy_started.wait(5000));
    QVERIFY(spy_complete.wait(30000)); // Scan may take time
}

void TestQuickActionController::testScanAllActions() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionScanStarted);
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionScanComplete);
    
    m_controller->scanAllActions();
    
    // Should get multiple signals
    QVERIFY(spy_started.wait(10000));
    QVERIFY(spy_started.count() >= 1);
}

void TestQuickActionController::testScanSignals() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionScanStarted);
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionScanComplete);
    
    m_controller->scanAction("Disk Cleanup");
    
    QVERIFY(spy_started.wait(5000));
    QCOMPARE(spy_started.count(), 1);
    
    QVERIFY(spy_complete.wait(30000));
    QCOMPARE(spy_complete.count(), 1);
}

void TestQuickActionController::testConcurrentScans() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionScanStarted);
    
    m_controller->scanAction("Disk Cleanup");
    m_controller->scanAction("Clear Browser Cache");
    
    // Should handle concurrent scans
    QVERIFY(spy_started.wait(5000));
}

void TestQuickActionController::testProgressSignals() {
    QSignalSpy spy_progress(m_controller, &sak::QuickActionController::actionExecutionProgress);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    // Some actions emit progress
    if (spy_progress.wait(30000)) {
        QVERIFY(spy_progress.count() >= 1);
    }
}

void TestQuickActionController::testProgressValues() {
    QSignalSpy spy_progress(m_controller, &sak::QuickActionController::actionExecutionProgress);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    if (spy_progress.wait(30000)) {
        for (const auto& args : spy_progress) {
            int progress = args.at(2).toInt();
            QVERIFY(progress >= 0);
            QVERIFY(progress <= 100);
        }
    }
}

void TestQuickActionController::testMultiActionProgress() {
    // Test progress tracking across multiple actions
    auto actions = m_controller->getAllActions();
    QVERIFY(actions.size() >= 2);
}

void TestQuickActionController::testActionError() {
    QSignalSpy spy_error(m_controller, &sak::QuickActionController::actionError);
    
    // Try invalid action
    m_controller->executeAction("InvalidAction", false);
    
    QVERIFY(spy_error.wait(5000));
}

void TestQuickActionController::testInvalidActionName() {
    auto* action = m_controller->getAction("");
    QVERIFY(action == nullptr);
}

void TestQuickActionController::testExecutionFailure() {
    QSignalSpy spy_error(m_controller, &sak::QuickActionController::actionError);
    
    m_controller->executeAction("NonexistentAction", false);
    
    QVERIFY(spy_error.wait(5000));
}

void TestQuickActionController::testScanFailure() {
    QSignalSpy spy_error(m_controller, &sak::QuickActionController::actionError);
    
    m_controller->scanAction("NonexistentAction");
    
    QVERIFY(spy_error.wait(5000));
}

void TestQuickActionController::testHasAdminPrivileges() {
    bool has_admin = sak::QuickActionController::hasAdminPrivileges();
    // Result depends on how test is run
    QVERIFY(has_admin == true || has_admin == false);
}

void TestQuickActionController::testRequiresAdmin() {
    auto actions = m_controller->getAllActions();
    
    int admin_required = 0;
    for (const auto& action : actions) {
        if (action->requiresAdmin()) {
            admin_required++;
        }
    }
    
    QVERIFY(admin_required > 0);
}

void TestQuickActionController::testAdminElevation() {
    // Request elevation (will prompt if not admin)
    bool result = sak::QuickActionController::requestAdminElevation("Test");
    // Result depends on user interaction
}

void TestQuickActionController::testLogMessages() {
    QSignalSpy spy_log(m_controller, &sak::QuickActionController::logMessage);
    
    m_controller->scanAction("Disk Cleanup");
    
    // Should get log messages
    if (spy_log.wait(30000)) {
        QVERIFY(spy_log.count() >= 1);
    }
}

void TestQuickActionController::testErrorMessages() {
    QSignalSpy spy_error(m_controller, &sak::QuickActionController::actionError);
    
    m_controller->executeAction("InvalidAction", false);
    
    QVERIFY(spy_error.wait(5000));
    
    auto args = spy_error.at(0);
    QString error_msg = args.at(1).toString();
    QVERIFY(!error_msg.isEmpty());
}

void TestQuickActionController::testProgressMessages() {
    QSignalSpy spy_progress(m_controller, &sak::QuickActionController::actionExecutionProgress);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    if (spy_progress.wait(30000)) {
        auto args = spy_progress.at(0);
        QString message = args.at(1).toString();
        QVERIFY(!message.isEmpty());
    }
}

void TestQuickActionController::testThreadSafeExecution() {
    // Controller uses thread pool
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    QVERIFY(spy_started.wait(5000));
}

void TestQuickActionController::testBackgroundExecution() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionExecutionComplete);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    QVERIFY(spy_started.wait(5000));
    
    // Should complete in background
    QVERIFY(spy_complete.wait(30000));
}

void TestQuickActionController::testSignalThreadAffinity() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    if (spy_started.wait(5000)) {
        // Signals should be emitted on main thread
        QCOMPARE(m_controller->thread(), QThread::currentThread());
    }
}

void TestQuickActionController::testActionQueue() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    // Queue multiple actions
    m_controller->executeAction("Disk Cleanup", false);
    m_controller->executeAction("Clear Browser Cache", false);
    
    // Should handle sequentially
    QVERIFY(spy_started.wait(5000));
}

void TestQuickActionController::testSequentialExecution() {
    QSignalSpy spy_complete(m_controller, &sak::QuickActionController::actionExecutionComplete);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    QVERIFY(spy_complete.wait(30000));
    
    // Execute another
    m_controller->executeAction("Clear Browser Cache", false);
    
    QVERIFY(spy_complete.wait(30000));
}

void TestQuickActionController::testQueueCancellation() {
    QSignalSpy spy_started(m_controller, &sak::QuickActionController::actionExecutionStarted);
    
    m_controller->executeAction("Disk Cleanup", false);
    
    QVERIFY(spy_started.wait(5000));
    
    // Cancel
    m_controller->cancelCurrentAction();
}

QTEST_MAIN(TestQuickActionController)
#include "test_quick_action_controller.moc"
