// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_uninstall_controller.cpp
/// @brief Unit tests for AdvancedUninstallController — state machine, queue,
///        settings, input validation, accessor/mutator pairs

#include "sak/advanced_uninstall_controller.h"
#include "sak/advanced_uninstall_types.h"
#include "sak/config_manager.h"
#include "sak/restore_point_manager.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using sak::AdvancedUninstallController;
using sak::ProgramInfo;
using sak::ScanLevel;
using sak::UninstallQueueItem;

class AdvancedUninstallControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Initial State ──
    void initialState_isIdle();
    void initialPrograms_empty();
    void initialQueue_empty();

    // ── Accessor / Mutator Pairs ──
    void autoRestorePoint_setAndGet();
    void autoCleanSafe_setAndGet();
    void safeLeftovers_filtersSafeOnlyAndSelects();
    void nonSafeLeftovers_keepsReviewAndRisky();
    void autoCleanableLeftovers_excludesIrreversibleSafeTypes();
    void defaultScanLevel_setAndGet();
    void showSystemComponents_setAndGet();
    void defaultScanLevel_allValues();

    // ── Restore Point Manager ──
    void restorePointManager_notNull();

    // ── Queue Management ──
    void addToQueue_appendsItem();
    void addToQueue_multipleItems();
    void addToQueue_emptyNameRejected();
    void removeFromQueue_valid();
    void removeFromQueue_invalidNegative();
    void removeFromQueue_invalidTooLarge();
    void clearQueue_emptiesQueue();
    void queueItem_defaultStatus();

    // ── State Guards ──
    void uninstallProgram_rejectsWhenBusy();
    void forceUninstall_rejectsWhenBusy();
    void removeRegistryEntry_rejectsWhenBusy();
    void startBatch_rejectsEmptyQueue();
    void startBatch_setsUninstallingState();

    // ── Input Validation ──
    void uninstallProgram_rejectsEmptyName();
    void forceUninstall_rejectsEmptyName();
    void removeRegistryEntry_rejectsEmptyKeyPath();

    // ── Settings Persistence ──
    void saveAndLoadSettings_roundTrip();

    // ── State Machine ──
    void stateChanged_emitsSignal();
    void refreshPrograms_changesState();

    // ── Cancel When Idle ──
    void cancelOperation_whenIdle_noOp();
};

// ── Initial State ───────────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::initialState_isIdle() {
    AdvancedUninstallController ctrl;
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::initialPrograms_empty() {
    AdvancedUninstallController ctrl;
    QVERIFY(ctrl.programs().isEmpty());
}

void AdvancedUninstallControllerTests::initialQueue_empty() {
    AdvancedUninstallController ctrl;
    QVERIFY(ctrl.queue().isEmpty());
}

// ── Accessor / Mutator Pairs ────────────────────────────────────────────────

void AdvancedUninstallControllerTests::autoRestorePoint_setAndGet() {
    AdvancedUninstallController ctrl;

    ctrl.setAutoRestorePoint(false);
    QVERIFY(!ctrl.autoRestorePoint());

    ctrl.setAutoRestorePoint(true);
    QVERIFY(ctrl.autoRestorePoint());
}

void AdvancedUninstallControllerTests::autoCleanSafe_setAndGet() {
    AdvancedUninstallController ctrl;

    ctrl.setAutoCleanSafe(false);
    QVERIFY(!ctrl.autoCleanSafe());

    ctrl.setAutoCleanSafe(true);
    QVERIFY(ctrl.autoCleanSafe());
}

void AdvancedUninstallControllerTests::safeLeftovers_filtersSafeOnlyAndSelects() {
    // auto-clean-safe only ever removes SAFE-classified leftovers; Review/Risky always stay for the
    // human. safeLeftovers() is the pure filter that enforces that, marking each kept item
    // selected.
    QVector<sak::LeftoverItem> items;
    sak::LeftoverItem safeFile;
    safeFile.type = sak::LeftoverItem::Type::File;
    safeFile.risk = sak::LeftoverItem::RiskLevel::Safe;
    safeFile.path = QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.log");
    safeFile.selected = false;
    items.append(safeFile);

    sak::LeftoverItem reviewItem;
    reviewItem.type = sak::LeftoverItem::Type::Folder;
    reviewItem.risk = sak::LeftoverItem::RiskLevel::Review;
    reviewItem.path = QStringLiteral("C:\\ProgramData\\Shared");
    items.append(reviewItem);

    sak::LeftoverItem riskyItem;
    riskyItem.type = sak::LeftoverItem::Type::RegistryKey;
    riskyItem.risk = sak::LeftoverItem::RiskLevel::Risky;
    riskyItem.path = QStringLiteral("HKLM\\SOFTWARE\\AcmeCorp");
    items.append(riskyItem);

    const QVector<sak::LeftoverItem> safe = AdvancedUninstallController::safeLeftovers(items);
    QCOMPARE(safe.size(), 1);
    QCOMPARE(safe.first().path, QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.log"));
    QVERIFY(safe.first().selected);  // marked selected so CleanupWorker acts on it
    // Empty in -> empty out (no auto-clean when nothing is safe).
    QVERIFY(AdvancedUninstallController::safeLeftovers({}).isEmpty());
}

void AdvancedUninstallControllerTests::nonSafeLeftovers_keepsReviewAndRisky() {
    // After a Safe auto-clean, the Review/Risky items must be surfaced back to the panel so they do
    // not vanish. nonSafeLeftovers() is the complement of safeLeftovers().
    QVector<sak::LeftoverItem> items;
    sak::LeftoverItem safeFile;
    safeFile.type = sak::LeftoverItem::Type::File;
    safeFile.risk = sak::LeftoverItem::RiskLevel::Safe;
    safeFile.path = QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.log");
    items.append(safeFile);

    sak::LeftoverItem reviewItem;
    reviewItem.type = sak::LeftoverItem::Type::Folder;
    reviewItem.risk = sak::LeftoverItem::RiskLevel::Review;
    reviewItem.path = QStringLiteral("C:\\ProgramData\\Shared");
    items.append(reviewItem);

    sak::LeftoverItem riskyItem;
    riskyItem.type = sak::LeftoverItem::Type::RegistryKey;
    riskyItem.risk = sak::LeftoverItem::RiskLevel::Risky;
    riskyItem.path = QStringLiteral("HKLM\\SOFTWARE\\AcmeCorp");
    items.append(riskyItem);

    const QVector<sak::LeftoverItem> rest = AdvancedUninstallController::nonSafeLeftovers(items);
    QCOMPARE(rest.size(), 2);  // the Review + Risky items, never the Safe one
    for (const sak::LeftoverItem& item : rest) {
        QVERIFY(item.risk != sak::LeftoverItem::RiskLevel::Safe);
    }
    // safeLeftovers + nonSafeLeftovers partition the input (1 + 2 == 3).
    QCOMPARE(AdvancedUninstallController::safeLeftovers(items).size() + rest.size(), items.size());
}

void AdvancedUninstallControllerTests::autoCleanableLeftovers_excludesIrreversibleSafeTypes() {
    using T = sak::LeftoverItem::Type;
    // An AUTOMATIC (no-review) delete must be undoable, so auto-clean only takes Recycle-Bin-able
    // types even when an irreversible type is classified Safe.
    QVERIFY(AdvancedUninstallController::isRecoverableLeftoverType([] {
        sak::LeftoverItem i;
        i.type = T::File;
        return i;
    }()));
    QVERIFY(AdvancedUninstallController::isRecoverableLeftoverType([] {
        sak::LeftoverItem i;
        i.type = T::Folder;
        return i;
    }()));
    QVERIFY(!AdvancedUninstallController::isRecoverableLeftoverType([] {
        sak::LeftoverItem i;
        i.type = T::RegistryKey;
        return i;
    }()));
    QVERIFY(!AdvancedUninstallController::isRecoverableLeftoverType([] {
        sak::LeftoverItem i;
        i.type = T::Service;
        return i;
    }()));
    // A registry StartupEntry (has a value name) is irreversible; a file-shortcut one is not.
    QVERIFY(!AdvancedUninstallController::isRecoverableLeftoverType([] {
        sak::LeftoverItem i;
        i.type = T::StartupEntry;
        i.registryValueName = QStringLiteral("Run");
        return i;
    }()));

    QVector<sak::LeftoverItem> items;
    sak::LeftoverItem safeFile;
    safeFile.type = T::File;
    safeFile.risk = sak::LeftoverItem::RiskLevel::Safe;
    safeFile.path = QStringLiteral("C:\\Program Files\\AcmeCorp\\App\\stale.log");
    items.append(safeFile);
    sak::LeftoverItem safeRegKey;  // SAFE but irreversible -> must NOT be auto-cleaned
    safeRegKey.type = T::RegistryKey;
    safeRegKey.risk = sak::LeftoverItem::RiskLevel::Safe;
    safeRegKey.path = QStringLiteral("HKCU\\Software\\AcmeCorp");
    items.append(safeRegKey);

    const QVector<sak::LeftoverItem> cleanable =
        AdvancedUninstallController::autoCleanableLeftovers(items);
    QCOMPARE(cleanable.size(), 1);  // only the file, never the Safe registry key
    QCOMPARE(cleanable.first().type, T::File);
    QVERIFY(cleanable.first().selected);
}

void AdvancedUninstallControllerTests::defaultScanLevel_setAndGet() {
    AdvancedUninstallController ctrl;

    ctrl.setDefaultScanLevel(ScanLevel::Safe);
    QCOMPARE(ctrl.defaultScanLevel(), ScanLevel::Safe);

    ctrl.setDefaultScanLevel(ScanLevel::Advanced);
    QCOMPARE(ctrl.defaultScanLevel(), ScanLevel::Advanced);

    ctrl.setDefaultScanLevel(ScanLevel::Moderate);
    QCOMPARE(ctrl.defaultScanLevel(), ScanLevel::Moderate);
}

void AdvancedUninstallControllerTests::showSystemComponents_setAndGet() {
    AdvancedUninstallController ctrl;

    ctrl.setShowSystemComponents(true);
    QVERIFY(ctrl.showSystemComponents());

    ctrl.setShowSystemComponents(false);
    QVERIFY(!ctrl.showSystemComponents());
}

void AdvancedUninstallControllerTests::defaultScanLevel_allValues() {
    AdvancedUninstallController ctrl;

    // Verify all ScanLevel values work correctly
    for (int i = 0; i <= 2; ++i) {
        auto level = static_cast<ScanLevel>(i);
        ctrl.setDefaultScanLevel(level);
        QCOMPARE(ctrl.defaultScanLevel(), level);
    }
}

// ── Restore Point Manager ───────────────────────────────────────────────────

void AdvancedUninstallControllerTests::restorePointManager_notNull() {
    AdvancedUninstallController ctrl;
    QVERIFY(ctrl.restorePointManager() != nullptr);
}

// ── Queue Management ────────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::addToQueue_appendsItem() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Test App";
    ctrl.addToQueue(prog, ScanLevel::Moderate, true);

    QCOMPARE(ctrl.queue().size(), 1);
    QCOMPARE(ctrl.queue()[0].program.displayName, "Test App");
    QCOMPARE(ctrl.queue()[0].scanLevel, ScanLevel::Moderate);
    QVERIFY(ctrl.queue()[0].autoCleanSafeLeftovers);
    QCOMPARE(ctrl.queue()[0].status, UninstallQueueItem::Status::Queued);
}

void AdvancedUninstallControllerTests::addToQueue_multipleItems() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog1;
    prog1.displayName = "App A";
    ctrl.addToQueue(prog1, ScanLevel::Safe, false);

    ProgramInfo prog2;
    prog2.displayName = "App B";
    ctrl.addToQueue(prog2, ScanLevel::Advanced, true);

    ProgramInfo prog3;
    prog3.displayName = "App C";
    ctrl.addToQueue(prog3, ScanLevel::Moderate, true);

    QCOMPARE(ctrl.queue().size(), 3);
    QCOMPARE(ctrl.queue()[0].program.displayName, "App A");
    QCOMPARE(ctrl.queue()[1].program.displayName, "App B");
    QCOMPARE(ctrl.queue()[2].program.displayName, "App C");
    QCOMPARE(ctrl.queue()[0].scanLevel, ScanLevel::Safe);
    QCOMPARE(ctrl.queue()[1].scanLevel, ScanLevel::Advanced);
    QVERIFY(!ctrl.queue()[0].autoCleanSafeLeftovers);
}

void AdvancedUninstallControllerTests::addToQueue_emptyNameRejected() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    // displayName left empty
    ctrl.addToQueue(prog, ScanLevel::Moderate, true);

    // Queue should remain empty
    QVERIFY(ctrl.queue().isEmpty());

    // Status message should have been emitted
    QCOMPARE(statusSpy.count(), 1);
}

void AdvancedUninstallControllerTests::removeFromQueue_valid() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog1;
    prog1.displayName = "Keep";
    ctrl.addToQueue(prog1, ScanLevel::Safe, true);

    ProgramInfo prog2;
    prog2.displayName = "Remove";
    ctrl.addToQueue(prog2, ScanLevel::Moderate, true);

    ProgramInfo prog3;
    prog3.displayName = "Keep Too";
    ctrl.addToQueue(prog3, ScanLevel::Advanced, true);

    ctrl.removeFromQueue(1);  // Remove "Remove"

    QCOMPARE(ctrl.queue().size(), 2);
    QCOMPARE(ctrl.queue()[0].program.displayName, "Keep");
    QCOMPARE(ctrl.queue()[1].program.displayName, "Keep Too");
}

void AdvancedUninstallControllerTests::removeFromQueue_invalidNegative() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Stay";
    ctrl.addToQueue(prog, ScanLevel::Safe, true);

    ctrl.removeFromQueue(-1);

    // Should be unchanged
    QCOMPARE(ctrl.queue().size(), 1);
}

void AdvancedUninstallControllerTests::removeFromQueue_invalidTooLarge() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Stay";
    ctrl.addToQueue(prog, ScanLevel::Safe, true);

    ctrl.removeFromQueue(5);

    // Should be unchanged
    QCOMPARE(ctrl.queue().size(), 1);
}

void AdvancedUninstallControllerTests::clearQueue_emptiesQueue() {
    AdvancedUninstallController ctrl;

    for (int i = 0; i < 5; ++i) {
        ProgramInfo prog;
        prog.displayName = QString("App %1").arg(i);
        ctrl.addToQueue(prog, ScanLevel::Moderate, true);
    }
    QCOMPARE(ctrl.queue().size(), 5);

    ctrl.clearQueue();
    QVERIFY(ctrl.queue().isEmpty());
}

void AdvancedUninstallControllerTests::queueItem_defaultStatus() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Test";
    ctrl.addToQueue(prog, ScanLevel::Safe, false);

    // Queue items should default to Queued status
    QCOMPARE(ctrl.queue()[0].status, UninstallQueueItem::Status::Queued);
}

// ── State Guards ────────────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::uninstallProgram_rejectsWhenBusy() {
    AdvancedUninstallController ctrl;

    // Start an enumeration to leave Idle
    ctrl.refreshPrograms();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    prog.displayName = "TestApp";
    ctrl.uninstallProgram(prog, ScanLevel::Moderate, false, false);

    // Should have emitted a rejection status message
    QVERIFY(statusSpy.count() >= 1);

    // Still in Enumerating state, not Uninstalling
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    // Cancel enumeration for cleanup
    ctrl.cancelOperation();
}

void AdvancedUninstallControllerTests::forceUninstall_rejectsWhenBusy() {
    AdvancedUninstallController ctrl;

    ctrl.refreshPrograms();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    prog.displayName = "TestApp";
    ctrl.forceUninstall(prog, ScanLevel::Moderate, false);

    QVERIFY(statusSpy.count() >= 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    ctrl.cancelOperation();
}

void AdvancedUninstallControllerTests::removeRegistryEntry_rejectsWhenBusy() {
    AdvancedUninstallController ctrl;

    ctrl.refreshPrograms();

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    prog.displayName = "TestApp";
    prog.registryKeyPath = "HKLM\\SOFTWARE\\Test";
    ctrl.removeRegistryEntry(prog);

    QVERIFY(statusSpy.count() >= 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    ctrl.cancelOperation();
}

void AdvancedUninstallControllerTests::startBatch_rejectsEmptyQueue() {
    AdvancedUninstallController ctrl;
    QVERIFY(ctrl.queue().isEmpty());

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ctrl.startBatchUninstall(false);

    // Should have emitted "Batch queue is empty" status
    QVERIFY(statusSpy.count() >= 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::startBatch_setsUninstallingState() {
    // A batch must OWN the busy state for the whole run: the instant it starts the controller must
    // be Uninstalling (not Idle), so a concurrent refresh / manual clean / second uninstall cannot
    // slip in between queue items. (createRestorePoint=false so the batch is not aborted on a
    // platform without System Restore.)
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("BatchApp");  // no uninstallString -> native step fails fast
    ctrl.addToQueue(prog, ScanLevel::Safe, false);

    ctrl.startBatchUninstall(false);

    // The worker runs on its own thread and signals back via queued connections that are not
    // delivered during this synchronous body, so the state observed here is the batch start state.
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Uninstalling);

    ctrl.cancelOperation();  // request stop; destructor joins the worker
}

// ── Input Validation ────────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::uninstallProgram_rejectsEmptyName() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    // displayName left empty
    ctrl.uninstallProgram(prog, ScanLevel::Moderate, false, false);

    QVERIFY(statusSpy.count() >= 1);
    // Should remain idle
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::forceUninstall_rejectsEmptyName() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    ctrl.forceUninstall(prog, ScanLevel::Moderate, false);

    QVERIFY(statusSpy.count() >= 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::removeRegistryEntry_rejectsEmptyKeyPath() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    prog.displayName = "TestApp";
    // registryKeyPath left empty
    ctrl.removeRegistryEntry(prog);

    QVERIFY(statusSpy.count() >= 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

// ── Settings Persistence ────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::saveAndLoadSettings_roundTrip() {
    // Set non-default values and save
    {
        AdvancedUninstallController ctrl;
        ctrl.setAutoRestorePoint(false);
        ctrl.setAutoCleanSafe(false);
        ctrl.setDefaultScanLevel(ScanLevel::Advanced);
        ctrl.setShowSystemComponents(true);
        ctrl.saveSettings();
    }

    // Load a new controller and verify settings persisted
    {
        AdvancedUninstallController ctrl;
        // loadSettings() is called in constructor, so values should be loaded
        QVERIFY(!ctrl.autoRestorePoint());
        QVERIFY(!ctrl.autoCleanSafe());
        QCOMPARE(ctrl.defaultScanLevel(), ScanLevel::Advanced);
        QVERIFY(ctrl.showSystemComponents());
    }

    // Restore defaults
    {
        AdvancedUninstallController ctrl;
        ctrl.setAutoRestorePoint(true);
        ctrl.setAutoCleanSafe(true);
        ctrl.setDefaultScanLevel(ScanLevel::Moderate);
        ctrl.setShowSystemComponents(false);
        ctrl.saveSettings();
    }
}

// ── State Machine ───────────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::stateChanged_emitsSignal() {
    AdvancedUninstallController ctrl;

    QSignalSpy stateSpy(&ctrl, &AdvancedUninstallController::stateChanged);

    ctrl.refreshPrograms();

    // Should have transitioned to Enumerating
    QVERIFY(stateSpy.count() >= 1);
    auto newState = stateSpy[0][0].value<AdvancedUninstallController::State>();
    QCOMPARE(newState, AdvancedUninstallController::State::Enumerating);

    ctrl.cancelOperation();
}

void AdvancedUninstallControllerTests::refreshPrograms_changesState() {
    AdvancedUninstallController ctrl;
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);

    ctrl.refreshPrograms();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    ctrl.cancelOperation();
}

// ── Cancel When Idle ────────────────────────────────────────────────────────

void AdvancedUninstallControllerTests::cancelOperation_whenIdle_noOp() {
    AdvancedUninstallController ctrl;
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);

    // Should not crash or change state
    ctrl.cancelOperation();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

QTEST_GUILESS_MAIN(AdvancedUninstallControllerTests)

#include "test_advanced_uninstall_controller.moc"
