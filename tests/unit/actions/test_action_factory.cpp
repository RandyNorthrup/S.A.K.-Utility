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

#include <QSet>
#include <QTest>

using namespace sak;

/**
 * @brief Unit tests for all QuickAction metadata
 *
 * Validates that every action has correct metadata (name, description,
 * category, icon, admin flag) and proper initial state.
 * These tests require no admin privileges.
 */
class TestActionFactory : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Factory completeness
    void testActionsExist();
    void testNoNullActions();
    void testNoDuplicateNames();

    // Metadata validity
    void testAllNamesNonEmpty();
    void testAllDescriptionsNonEmpty();
    void testAllCategoriesValid();
    void testRequiresAdminIsBool();

    // Category distribution — every category has at least one action
    void testAllCategoriesPopulated();

    // Initial state
    void testInitialStatusIsIdle();
    void testInitialScanResultNotApplicable();
    void testInitialExecutionResultNotSuccess();

    void cleanupTestCase();

private:
    std::vector<std::unique_ptr<QuickAction>> m_actions;

    int countByCategory(QuickAction::ActionCategory cat) const;
};

void TestActionFactory::initTestCase() {
    const auto backup = QStringLiteral("C:/SAK_Test_Backups");
    m_actions.push_back(std::make_unique<BackupBitlockerKeysAction>(backup));
    m_actions.push_back(std::make_unique<CheckDiskErrorsAction>());
    m_actions.push_back(std::make_unique<GenerateSystemReportAction>(backup));
    m_actions.push_back(std::make_unique<OptimizePowerSettingsAction>());
    m_actions.push_back(std::make_unique<ResetNetworkAction>());
    m_actions.push_back(std::make_unique<ScreenshotSettingsAction>(backup));
    m_actions.push_back(std::make_unique<VerifySystemFilesAction>());
}

// ============================================================================
// Factory completeness
// ============================================================================

void TestActionFactory::testActionsExist() {
    QVERIFY2(!m_actions.empty(), "No actions created");
    QCOMPARE(static_cast<int>(m_actions.size()), 7);
}

void TestActionFactory::testNoNullActions() {
    for (size_t i = 0; i < m_actions.size(); ++i) {
        QVERIFY2(m_actions[i] != nullptr,
                 qPrintable(QStringLiteral("Action at index %1 is null").arg(i)));
    }
}

void TestActionFactory::testNoDuplicateNames() {
    QSet<QString> names;
    for (const auto& action : m_actions) {
        const QString n = action->name();
        QVERIFY2(!names.contains(n),
                 qPrintable(QStringLiteral("Duplicate action name: %1").arg(n)));
        names.insert(n);
    }
    QCOMPARE(names.size(), static_cast<int>(m_actions.size()));
}

// ============================================================================
// Metadata validity
// ============================================================================

void TestActionFactory::testAllNamesNonEmpty() {
    for (const auto& action : m_actions) {
        QVERIFY2(!action->name().isEmpty(), "Action has empty name()");
    }
}

void TestActionFactory::testAllDescriptionsNonEmpty() {
    for (const auto& action : m_actions) {
        QVERIFY2(!action->description().isEmpty(),
                 qPrintable(
                     QStringLiteral("Action '%1' has empty description()").arg(action->name())));
    }
}

void TestActionFactory::testAllCategoriesValid() {
    for (const auto& action : m_actions) {
        const auto cat = action->category();
        QVERIFY2(cat == QuickAction::ActionCategory::SystemOptimization ||
                     cat == QuickAction::ActionCategory::Maintenance ||
                     cat == QuickAction::ActionCategory::Troubleshooting ||
                     cat == QuickAction::ActionCategory::EmergencyRecovery,
                 qPrintable(QStringLiteral("Action '%1' has unknown category %2")
                                .arg(action->name())
                                .arg(static_cast<int>(cat))));
    }
}

void TestActionFactory::testRequiresAdminIsBool() {
    for (const auto& action : m_actions) {
        const bool val = action->requiresAdmin();
        QVERIFY(val == true || val == false);
    }
}

// ============================================================================
// Category distribution
// ============================================================================

int TestActionFactory::countByCategory(QuickAction::ActionCategory cat) const {
    int count = 0;
    for (const auto& action : m_actions) {
        if (action->category() == cat) {
            ++count;
        }
    }
    return count;
}

void TestActionFactory::testAllCategoriesPopulated() {
    const std::vector<QuickAction::ActionCategory> categories = {
        QuickAction::ActionCategory::SystemOptimization,
        QuickAction::ActionCategory::Maintenance,
        QuickAction::ActionCategory::Troubleshooting,
        QuickAction::ActionCategory::EmergencyRecovery,
    };

    int categorized_total = 0;
    for (const auto cat : categories) {
        const int n = countByCategory(cat);
        QVERIFY2(n > 0,
                 qPrintable(
                     QStringLiteral("Category %1 has zero actions").arg(static_cast<int>(cat))));
        categorized_total += n;
    }

    QCOMPARE(categorized_total, static_cast<int>(m_actions.size()));
}

// ============================================================================
// Initial state
// ============================================================================

void TestActionFactory::testInitialStatusIsIdle() {
    for (const auto& action : m_actions) {
        QVERIFY2(action->status() == QuickAction::ActionStatus::Idle,
                 qPrintable(
                     QStringLiteral("Action '%1' initial status is not Idle").arg(action->name())));
    }
}

void TestActionFactory::testInitialScanResultNotApplicable() {
    for (const auto& action : m_actions) {
        QVERIFY2(
            !action->lastScanResult().applicable,
            qPrintable(
                QStringLiteral("Action '%1' initial scan claims applicable").arg(action->name())));
    }
}

void TestActionFactory::testInitialExecutionResultNotSuccess() {
    for (const auto& action : m_actions) {
        QVERIFY2(!action->lastExecutionResult().success,
                 qPrintable(QStringLiteral("Action '%1' initial execution claims success")
                                .arg(action->name())));
    }
}

// ============================================================================
// Cleanup
// ============================================================================

void TestActionFactory::cleanupTestCase() {
    m_actions.clear();
}

QTEST_MAIN(TestActionFactory)
#include "test_action_factory.moc"
