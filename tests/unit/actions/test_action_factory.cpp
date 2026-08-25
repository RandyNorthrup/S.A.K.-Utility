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

#include <QHash>
#include <QSet>
#include <QTest>

#include <vector>

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

    // Category distribution -- every category has at least one action
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
    // Every description() is a compile-time literal, so pin the exact text. A !isEmpty() floor
    // lets a truncated blurb through -- notably the two adjacent split literals in
    // reset_network_action.h and backup_bitlocker_keys_action.h, where dropping a half leaves a
    // non-empty but wrong string that a .contains() check also fails to catch.
    const QHash<QString, QString> expected = {
        {QStringLiteral("BitLocker Key Backup"),
         QStringLiteral("Backup BitLocker recovery keys for all encrypted volumes")},
        {QStringLiteral("Check Disk Errors"), QStringLiteral("Run CHKDSK scan on all drives")},
        {QStringLiteral("Generate System Report"),
         QStringLiteral("Create comprehensive system report")},
        {QStringLiteral("Optimize Power Settings"),
         QStringLiteral("Switch to High Performance power plan")},
        {QStringLiteral("Reset Network Settings"),
         QStringLiteral("Reset TCP/IP, DNS, Winsock and Windows Firewall rules, and restart "
                        "network adapters")},
        {QStringLiteral("Screenshot Settings"),
         QStringLiteral("Capture screenshots of Windows Settings")},
        {QStringLiteral("Verify System Files"),
         QStringLiteral("Run SFC and DISM to repair system files")},
    };

    // The table must cover the factory exactly -- a new action cannot slip past unpinned.
    QCOMPARE(static_cast<int>(expected.size()), static_cast<int>(m_actions.size()));

    for (const auto& action : m_actions) {
        const QString action_name = action->name();
        QVERIFY2(
            expected.contains(action_name),
            qPrintable(
                QStringLiteral("No expected description pinned for action '%1'").arg(action_name)));
        QCOMPARE(action->description(), expected.value(action_name));
    }
}

void TestActionFactory::testAllCategoriesValid() {
    // The old disjunction listed ALL FOUR enumerators of QuickAction::ActionCategory
    // (quick_action.h:37-42), so every in-range value satisfied it -- it could only ever catch an
    // out-of-range static_cast, which no action performs. Pin the ROUTING instead, in
    // initTestCase() construction order, the same convention testRequiresAdminIsBool uses.
    // category() is the single source both for the AI app-action descriptor's category string
    // (app_action_bridge.cpp:196) and for the panel's getActionsByCategory() grouping
    // (quick_action_controller.cpp:256), so a mis-route files a destructive recovery action under
    // routine maintenance in every surface that lists it.
    struct Expected {
        QString name;
        QuickAction::ActionCategory category;
    };
    const std::vector<Expected> expected = {
        {QStringLiteral("BitLocker Key Backup"), QuickAction::ActionCategory::EmergencyRecovery},
        {QStringLiteral("Check Disk Errors"), QuickAction::ActionCategory::Maintenance},
        {QStringLiteral("Generate System Report"), QuickAction::ActionCategory::Troubleshooting},
        {QStringLiteral("Optimize Power Settings"),
         QuickAction::ActionCategory::SystemOptimization},
        {QStringLiteral("Reset Network Settings"), QuickAction::ActionCategory::Maintenance},
        {QStringLiteral("Screenshot Settings"), QuickAction::ActionCategory::EmergencyRecovery},
        {QStringLiteral("Verify System Files"), QuickAction::ActionCategory::Maintenance},
    };
    QCOMPARE(m_actions.size(), expected.size());
    for (size_t i = 0; i < m_actions.size(); ++i) {
        QCOMPARE(m_actions[i]->name(), expected[i].name);
        QCOMPARE(m_actions[i]->category(), expected[i].category);
    }
}

void TestActionFactory::testRequiresAdminIsBool() {
    // `val == true || val == false` is a tautology over a bool: it holds for every
    // implementation, so the elevation contract was entirely unpinned. Pin it exactly, in
    // initTestCase() construction order -- which action needs admin decides whether the run is
    // launched elevated at all.
    struct Expected {
        QString name;
        bool requires_admin;
    };
    const std::vector<Expected> expected = {
        {QStringLiteral("BitLocker Key Backup"), true},
        {QStringLiteral("Check Disk Errors"), true},
        {QStringLiteral("Generate System Report"), false},
        {QStringLiteral("Optimize Power Settings"), false},
        {QStringLiteral("Reset Network Settings"), true},
        {QStringLiteral("Screenshot Settings"), false},
        {QStringLiteral("Verify System Files"), true},
    };
    QCOMPARE(m_actions.size(), expected.size());
    for (size_t i = 0; i < m_actions.size(); ++i) {
        QCOMPARE(m_actions[i]->name(), expected[i].name);
        QCOMPARE(m_actions[i]->requiresAdmin(), expected[i].requires_admin);
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

    // The shipped distribution is deterministic and knowable, so pin the census rather than a
    // floor. `n > 0` only rejects a category emptied ENTIRELY, so an action drifting into an
    // already-populated bucket stayed invisible -- and the trailing total could not fail either:
    // countByCategory() sums over all four enumerators while testAllCategoriesValid has already
    // accepted every value as in-range, so it merely re-derives the fixture size from the fixture.
    const std::vector<int> expected_counts = {1, 3, 1, 2};
    QCOMPARE(categories.size(), expected_counts.size());

    int categorized_total = 0;
    for (size_t i = 0; i < categories.size(); ++i) {
        const int n = countByCategory(categories[i]);
        QVERIFY2(n == expected_counts[i],
                 qPrintable(QStringLiteral("Category %1 holds %2 actions, expected %3")
                                .arg(static_cast<int>(categories[i]))
                                .arg(n)
                                .arg(expected_counts[i])));
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
        const QuickAction::ScanResult& scan = action->lastScanResult();
        QVERIFY2(
            !scan.applicable,
            qPrintable(
                QStringLiteral("Action '%1' initial scan claims applicable").arg(action->name())));

        // The panel renders bytes/count/ETA as the pre-scan estimate, so a dropped or wrong
        // default member initializer would show garbage before any scan runs. Pin the whole
        // default-constructed struct, not just `applicable`.
        QCOMPARE(scan.bytes_affected, Q_INT64_C(0));
        QCOMPARE(scan.files_count, Q_INT64_C(0));
        QCOMPARE(scan.estimated_duration_ms, Q_INT64_C(0));
        QVERIFY2(scan.summary.isEmpty(),
                 qPrintable(QStringLiteral("Action '%1' initial scan summary is '%2'")
                                .arg(action->name(), scan.summary)));
        QVERIFY2(scan.details.isEmpty(),
                 qPrintable(QStringLiteral("Action '%1' initial scan details is '%2'")
                                .arg(action->name(), scan.details)));
        QVERIFY2(scan.warning.isEmpty(),
                 qPrintable(QStringLiteral("Action '%1' initial scan warning is '%2'")
                                .arg(action->name(), scan.warning)));
    }
}

void TestActionFactory::testInitialExecutionResultNotSuccess() {
    for (const auto& action : m_actions) {
        // A freshly constructed action has run nothing, so EVERY field of the default
        // ExecutionResult must read clean: the constructor sets none of them, and the panels
        // publish these counters before any run.
        const QuickAction::ExecutionResult& result = action->lastExecutionResult();
        QVERIFY2(!result.success,
                 qPrintable(QStringLiteral("Action '%1' initial execution claims success")
                                .arg(action->name())));
        QVERIFY2(
            result.message.isEmpty(),
            qPrintable(
                QStringLiteral("Action '%1' initial execution has a message").arg(action->name())));
        QCOMPARE(result.bytes_processed, Q_INT64_C(0));
        QCOMPARE(result.files_processed, Q_INT64_C(0));
        QCOMPARE(result.duration_ms, Q_INT64_C(0));
        QVERIFY2(result.output_path.isEmpty(),
                 qPrintable(QStringLiteral("Action '%1' initial execution has an output_path")
                                .arg(action->name())));
        QVERIFY2(
            result.log.isEmpty(),
            qPrintable(
                QStringLiteral("Action '%1' initial execution has a log").arg(action->name())));
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
