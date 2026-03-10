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
