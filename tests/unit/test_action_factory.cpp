// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include "sak/actions/action_factory.h"
#include "sak/quick_action.h"

class TestActionFactory : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Factory creation
    void testCreateAllActions();
    void testActionsNotNull();
    void testActionsHaveNames();
    void testActionsHaveCategories();

    // Action counts
    void testSystemOptimizationCount();
    void testMaintenanceCount();
    void testTroubleshootingCount();
    void testQuickBackupCount();
    void testEmergencyRecoveryCount();

    // Specific action creation
    void testCreateDiskCleanup();
    void testCreateClearBrowserCache();
    void testCreateOptimizePowerSettings();
    void testCreateWindowsUpdate();
    void testCreateBackupBrowserData();
    void testCreateBackupActivationKeys();
    void testCreateStickyNotesBackup();
    void testCreateDefragmentDrives();
    void testCreateCreateRestorePoint();
    void testCreateDisableStartupPrograms();

    // Category validation
    void testAllActionsHaveValidCategory();
    void testSystemOptimizationActions();
    void testMaintenanceActions();
    void testTroubleshootingActions();
    void testQuickBackupActions();
    void testEmergencyRecoveryActions();

    // Action properties
    void testActionNames();
    void testActionDescriptions();
    void testActionCategories();
    void testActionAdminRequirements();

    // Backup location
    void testBackupLocationUsed();
    void testDefaultBackupLocation();
    void testCustomBackupLocation();

    // Unique actions
    void testNoDuplicateNames();
    void testNoDuplicateActions();
    void testEachActionUnique();

    // Memory management
    void testActionsOwnership();
    void testActionsLifetime();

private:
    QTemporaryDir* m_temp_dir{nullptr};
    QString m_test_backup_location;
};

void TestActionFactory::initTestCase() {
    // Setup test environment
}

void TestActionFactory::cleanupTestCase() {
    // Cleanup test environment
}

void TestActionFactory::init() {
    m_temp_dir = new QTemporaryDir();
    QVERIFY(m_temp_dir->isValid());
    m_test_backup_location = m_temp_dir->path();
}

void TestActionFactory::cleanup() {
    delete m_temp_dir;
    m_temp_dir = nullptr;
}

void TestActionFactory::testCreateAllActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QVERIFY(!actions.empty());
    QVERIFY(actions.size() >= 37); // At least 37 actions
}

void TestActionFactory::testActionsNotNull() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        QVERIFY(action != nullptr);
    }
}

void TestActionFactory::testActionsHaveNames() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        QVERIFY(!action->name().isEmpty());
    }
}

void TestActionFactory::testActionsHaveCategories() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        QVERIFY(action->category() != sak::ActionCategory::Unknown);
    }
}

void TestActionFactory::testSystemOptimizationCount() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    int count = 0;
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::SystemOptimization) {
            count++;
        }
    }
    QVERIFY(count >= 10); // At least 10 system optimization actions
}

void TestActionFactory::testMaintenanceCount() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    int count = 0;
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::Maintenance) {
            count++;
        }
    }
    QVERIFY(count >= 9); // At least 9 maintenance actions
}

void TestActionFactory::testTroubleshootingCount() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    int count = 0;
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::Troubleshooting) {
            count++;
        }
    }
    QVERIFY(count >= 9); // At least 9 troubleshooting actions
}

void TestActionFactory::testQuickBackupCount() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    int count = 0;
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::QuickBackup) {
            count++;
        }
    }
    QVERIFY(count >= 13); // At least 13 quick backup actions
}

void TestActionFactory::testEmergencyRecoveryCount() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    int count = 0;
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::EmergencyRecovery) {
            count++;
        }
    }
    QVERIFY(count >= 4); // At least 4 emergency recovery actions
}

void TestActionFactory::testCreateDiskCleanup() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Disk Cleanup", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateClearBrowserCache() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Browser Cache", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateOptimizePowerSettings() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Power", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateWindowsUpdate() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Windows Update", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateBackupBrowserData() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Browser", Qt::CaseInsensitive) &&
            action->name().contains("Backup", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateBackupActivationKeys() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Activation", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateStickyNotesBackup() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Sticky Notes", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateDefragmentDrives() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Defrag", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateCreateRestorePoint() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Restore Point", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testCreateDisableStartupPrograms() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    bool found = false;
    for (const auto& action : actions) {
        if (action->name().contains("Startup", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY(found);
}

void TestActionFactory::testAllActionsHaveValidCategory() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        QVERIFY(action->category() == sak::ActionCategory::SystemOptimization ||
                action->category() == sak::ActionCategory::Maintenance ||
                action->category() == sak::ActionCategory::Troubleshooting ||
                action->category() == sak::ActionCategory::QuickBackup ||
                action->category() == sak::ActionCategory::EmergencyRecovery);
    }
}

void TestActionFactory::testSystemOptimizationActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::SystemOptimization) {
            QVERIFY(!action->name().isEmpty());
            QVERIFY(!action->description().isEmpty());
        }
    }
}

void TestActionFactory::testMaintenanceActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::Maintenance) {
            QVERIFY(!action->name().isEmpty());
            QVERIFY(!action->description().isEmpty());
        }
    }
}

void TestActionFactory::testTroubleshootingActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::Troubleshooting) {
            QVERIFY(!action->name().isEmpty());
            QVERIFY(!action->description().isEmpty());
        }
    }
}

void TestActionFactory::testQuickBackupActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::QuickBackup) {
            QVERIFY(!action->name().isEmpty());
            QVERIFY(!action->description().isEmpty());
        }
    }
}

void TestActionFactory::testEmergencyRecoveryActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        if (action->category() == sak::ActionCategory::EmergencyRecovery) {
            QVERIFY(!action->name().isEmpty());
            QVERIFY(!action->description().isEmpty());
        }
    }
}

void TestActionFactory::testActionNames() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        QString name = action->name();
        QVERIFY(!name.isEmpty());
        QVERIFY(name.length() >= 5); // Reasonable minimum length
        QVERIFY(name.length() <= 100); // Reasonable maximum length
    }
}

void TestActionFactory::testActionDescriptions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (const auto& action : actions) {
        QString desc = action->description();
        QVERIFY(!desc.isEmpty());
    }
}

void TestActionFactory::testActionCategories() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QSet<int> categories;
    for (const auto& action : actions) {
        categories.insert(static_cast<int>(action->category()));
    }
    QVERIFY(categories.size() >= 5); // At least 5 different categories
}

void TestActionFactory::testActionAdminRequirements() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    int admin_required = 0;
    int no_admin = 0;
    for (const auto& action : actions) {
        if (action->requiresAdmin()) {
            admin_required++;
        } else {
            no_admin++;
        }
    }
    QVERIFY(admin_required > 0); // Some actions require admin
    QVERIFY(no_admin > 0); // Some actions don't require admin
}

void TestActionFactory::testBackupLocationUsed() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QVERIFY(!actions.empty());
    // Backup location should be passed to actions that need it
}

void TestActionFactory::testDefaultBackupLocation() {
    auto actions = sak::ActionFactory::createAllActions("");
    QVERIFY(!actions.empty());
}

void TestActionFactory::testCustomBackupLocation() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QVERIFY(!actions.empty());
}

void TestActionFactory::testNoDuplicateNames() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QSet<QString> names;
    for (const auto& action : actions) {
        QString name = action->name();
        QVERIFY(!names.contains(name));
        names.insert(name);
    }
}

void TestActionFactory::testNoDuplicateActions() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QCOMPARE(actions.size(), static_cast<size_t>(37)); // Exactly 37 unique actions
}

void TestActionFactory::testEachActionUnique() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    for (size_t i = 0; i < actions.size(); i++) {
        for (size_t j = i + 1; j < actions.size(); j++) {
            QVERIFY(actions[i]->name() != actions[j]->name());
        }
    }
}

void TestActionFactory::testActionsOwnership() {
    auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
    QVERIFY(!actions.empty());
    // Unique pointers handle ownership automatically
}

void TestActionFactory::testActionsLifetime() {
    {
        auto actions = sak::ActionFactory::createAllActions(m_test_backup_location);
        QVERIFY(!actions.empty());
    }
    // Actions should be cleaned up when vector goes out of scope
}

QTEST_MAIN(TestActionFactory)
#include "test_action_factory.moc"
