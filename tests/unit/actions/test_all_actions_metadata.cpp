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
#include <QStringList>
#include <QTest>

#include <array>

using namespace sak;

namespace {

// The shipped catalog size, mirroring the registration list at app_action_service.cpp:26-32.
constexpr int kExpectedActionCount = 7;

// Every ActionStatus that is NOT in-flight. QuickAction::cancel() guards on a whitelist of
// exactly two states (quick_action.cpp:43-45), so these five are its entire false arm.
constexpr std::array<QuickAction::ActionStatus, 5> kNotInFlightStatuses{
    QuickAction::ActionStatus::Idle,
    QuickAction::ActionStatus::Ready,
    QuickAction::ActionStatus::Success,
    QuickAction::ActionStatus::Failed,
    QuickAction::ActionStatus::Cancelled};

}  // namespace

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

    static void verifyCancelLeavesStatusAlone(QuickAction* action,
                                              QuickAction::ActionStatus resting);

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
    // The exact catalog, not a floor. Every per-action slot below reaches its subject through
    // findByName(), which fails loudly when an entry goes MISSING but is completely blind to an
    // EXTRA one -- so !empty() left this file's own docblock claim ("every action returns the
    // exact expected name, category, and admin flag") true only of the seven names spelled out
    // below, while an eighth action could ship with nothing asserted about its name,
    // description, category or elevation flag. Pinning the NAME SET closes both directions.
    QCOMPARE(static_cast<int>(m_actions.size()), kExpectedActionCount);
    QStringList names;
    names.reserve(static_cast<qsizetype>(m_actions.size()));
    for (const auto& action : m_actions) {
        names << action->name();
    }
    names.sort();
    QStringList expected{QStringLiteral("BitLocker Key Backup"),
                         QStringLiteral("Check Disk Errors"),
                         QStringLiteral("Generate System Report"),
                         QStringLiteral("Optimize Power Settings"),
                         QStringLiteral("Reset Network Settings"),
                         QStringLiteral("Screenshot Settings"),
                         QStringLiteral("Verify System Files")};
    expected.sort();
    QCOMPARE(names, expected);
}

// cancel() must leave a NON-in-flight action exactly as it found it -- status untouched and,
// because setStatus() is never reached, no statusChanged emission at all.
void TestAllActionsMetadata::verifyCancelLeavesStatusAlone(QuickAction* action,
                                                           QuickAction::ActionStatus resting) {
    action->clearCancellation();
    action->updateStatus(resting);
    QSignalSpy status_spy(action, &QuickAction::statusChanged);
    QVERIFY(status_spy.isValid());

    action->cancel();

    QVERIFY2(action->status() == resting,
             qPrintable(QStringLiteral("cancel() moved '%1' off the non-in-flight status %2")
                            .arg(action->name())
                            .arg(static_cast<int>(resting))));
    QVERIFY2(status_spy.count() == 0,
             qPrintable(QStringLiteral("cancel() emitted statusChanged for '%1' resting at %2")
                            .arg(action->name())
                            .arg(static_cast<int>(resting))));
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

        // Scanning IS in-flight: cancel() must transition to Cancelled AND announce it. The
        // status read alone is the wrong observable: statusChanged is emitted from exactly one
        // place (quick_action.cpp:26, inside setStatus) and is the only channel by which any
        // observer learns of the transition -- the controller hooks it at
        // quick_action_controller.cpp:209 and nothing polls status() -- so the Scanning path
        // could reach the right status by a route that notifies nobody, exactly as the Running
        // arm below already guards against.
        action->clearCancellation();
        action->updateStatus(QuickAction::ActionStatus::Scanning);
        QSignalSpy scanning_spy(action.get(), &QuickAction::statusChanged);
        QVERIFY(scanning_spy.isValid());
        action->cancel();
        QVERIFY2(action->status() == QuickAction::ActionStatus::Cancelled,
                 qPrintable("cancel() during Scanning left '" + action->name() + "' un-cancelled"));
        QCOMPARE(scanning_spy.count(), 1);
        QCOMPARE(scanning_spy.at(0).at(0).value<QuickAction::ActionStatus>(),
                 QuickAction::ActionStatus::Cancelled);

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

        // The FALSE arm, exhaustively. quick_action.cpp:43 is a WHITELIST of two states, and
        // proving its false side with only Idle and Success left the guard rewritable as a
        // blacklist over those two -- while the states production actually parks in went
        // untested. Ready is where every scan ends (check_disk_errors_action.cpp:89 and its
        // six siblings) and is precisely where a technician sits reading the scan estimate,
        // the single most likely moment for a Cancel click; Failed is where every bad run
        // ends. Cancelled covers a second cancel arriving after the first.
        for (const QuickAction::ActionStatus resting : kNotInFlightStatuses) {
            verifyCancelLeavesStatusAlone(action.get(), resting);
        }
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
