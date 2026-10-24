// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_uninstall_controller.cpp
/// @brief Unit tests for AdvancedUninstallController -- state machine, queue,
///        settings, input validation, accessor/mutator pairs

#include "sak/advanced_uninstall_controller.h"
#include "sak/advanced_uninstall_types.h"
#include "sak/config_manager.h"
#include "sak/restore_point_manager.h"
#include "sak/uninstall_worker.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QtTest/QtTest>

using sak::AdvancedUninstallController;
using sak::ProgramInfo;
using sak::ScanLevel;
using sak::UninstallQueueItem;

class AdvancedUninstallControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Initial State --
    void initialState_isIdle();
    void initialPrograms_empty();
    void initialQueue_empty();

    // -- Accessor / Mutator Pairs --
    void autoRestorePoint_setAndGet();
    void autoCleanSafe_setAndGet();
    void autoCleanPreference_isSeparateFromOperationState();
    void autoCleanableLeftovers_excludesIrreversibleSafeTypes();
    void autoCleanableLeftovers_excludesProtectedTargets();
    void cleanupIdentity_isTypedExactAndCasePreserving();
    void defaultScanLevel_setAndGet();
    void defaultScanLevel_rejectsInvalid();
    void showSystemComponents_setAndGet();
    void defaultScanLevel_allValues();

    // -- Queue Management --
    void addToQueue_appendsItem();
    void addToQueue_multipleItems();
    void addToQueue_emptyNameRejected();
    void addToQueue_invalidScanLevelRejected();
    void addToQueue_rejectsDuplicateAndBound();
    void removeFromQueue_valid();
    void removeFromQueue_invalidNegative();
    void removeFromQueue_invalidTooLarge();
    void clearQueue_emptiesQueue();
    void queueItem_defaultStatus();
    void queueMutation_refusedDuringBatch();

    // -- State Guards --
    void uninstallProgram_rejectsWhenBusy();
    void forceUninstall_rejectsWhenBusy();
    void removeRegistryEntry_rejectsWhenBusy();
    void startBatch_rejectsEmptyQueue();
    void startBatch_setsUninstallingState();
    void startBatch_exposesFailureTerminalAndCheckedCaller();

    // -- Input Validation --
    void uninstallProgram_rejectsEmptyName();
    void forceUninstall_rejectsEmptyName();
    void removeRegistryEntry_rejectsEmptyKeyPath();
    void removeRegistryEntry_rejectsNonHiveRooted();
    void operationScanLevels_rejectInvalid();

    // -- Settings Persistence --
    void saveAndLoadSettings_roundTrip();
    void settingsPersistence_checksHealthAndSync();
    void settingsError_isDeferredForObservers();

    // -- State Machine --
    void stateChanged_emitsSignal();
    void refreshPrograms_changesState();
    void enumerationCancel_emitsTerminalAndBlocksOverlap();
    void enumerationStopping_blocksDestructiveAdmission();
    void leftoverScan_entersState();
    void cleanupFailure_emitsTerminal();
    void cleanupRefusals_areTypedAndCounted();
    void workerCallbacks_requireCurrentGeneration();
    void failureSignals_preserveErrorCodes();
    void batchItemFailure_preservesDetailsThroughTerminal();
    void automaticCleanupTerminal_precedesBatchFailure();
    void deferredAutoClean_mergesSuccessfulAndFailedItems();
    void deferredAutoClean_terminalFailureMergesPartialProgress();
    void batchCompletion_waitsForCleanupAndCarriesResult();
    void batchWorker_usesCompleteSignalSet();
    void batchCancel_emitsCancelledQueueStatus();
    void registryOnly_connectsCancellation();
    void enumThreadFailedJoin_detachesSafely();
    void obsoleteHelpersAndMembers_areAbsent();
    void obsoleteControllerApis_areAbsent();
    void stateMachineDocs_describeActivityBranches();

    // -- Cancel When Idle --
    void cancelOperation_whenIdle_noOp();

    // R5-P7-9: registry-derived uninstall program trust policy
    void uninstallProgramPathTrusted_refusesSearchOrderResolvableForms();
    void uninstallProgramPathTrusted_refusesUncAndTraversalAndNonExe();
    void uninstallProgramPathTrusted_acceptsQualifiedLocalExe();
    void silentMsiCommand_namesMsiexecByAbsoluteSystem32Path();
};

namespace {

QString readRepoSource(const QString& relative_path) {
    const QDir repo_root(QStringLiteral(SAK_REPO_ROOT));
    QFile file(repo_root.filePath(relative_path));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

}  // namespace

// -- Initial State -----------------------------------------------------------

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

// -- Accessor / Mutator Pairs ------------------------------------------------

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

void AdvancedUninstallControllerTests::autoCleanPreference_isSeparateFromOperationState() {
    AdvancedUninstallController ctrl;
    ctrl.setAutoCleanSafe(true);
    ctrl.m_currentAutoCleanSafe = false;

    // Per-run forced/UWP state must not overwrite the persisted preference exposed to the panel.
    QVERIFY(ctrl.autoCleanSafe());
    QVERIFY(!ctrl.m_currentAutoCleanSafe);
    QVERIFY(ctrl.saveSettings());
    QCOMPARE(sak::ConfigManager::instance()
                 .getValue(QStringLiteral("advuninstall/auto_clean_safe"))
                 .toBool(),
             true);
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString dialog =
        readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel_dialogs.cpp"));
    QVERIFY(source.contains(QStringLiteral("m_currentAutoCleanSafe = auto_clean_safe")));
    QVERIFY(source.contains(QStringLiteral("m_currentAutoCleanSafe = false")));
    QVERIFY(
        source.contains(QStringLiteral("cfg.setValue(\"advuninstall/auto_clean_safe\", "
                                       "m_autoCleanSafePreference)")));
    QVERIFY(dialog.contains(QStringLiteral("addSettingsAutoCleanupGroup(")));
    QVERIFY(dialog.contains(
        QStringLiteral("m_controller->setAutoCleanSafe(auto_clean_check->isChecked())")));
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

void AdvancedUninstallControllerTests::autoCleanableLeftovers_excludesProtectedTargets() {
    sak::LeftoverItem protected_file;
    protected_file.type = sak::LeftoverItem::Type::File;
    protected_file.risk = sak::LeftoverItem::RiskLevel::Safe;
    protected_file.deletable = true;
    protected_file.path = QStringLiteral("C:\\Windows\\System32\\not-a-leftover.tmp");

    QVERIFY(AdvancedUninstallController::autoCleanableLeftovers({protected_file}).isEmpty());
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    QVERIFY(source.contains(QStringLiteral("cleanupItemRefusal(item).isEmpty()")));
    QVERIFY(header.contains(
        QStringLiteral("[[nodiscard]] static QVector<LeftoverItem> autoCleanableLeftovers(")));
}

void AdvancedUninstallControllerTests::cleanupIdentity_isTypedExactAndCasePreserving() {
    sak::LeftoverItem value_a;
    value_a.type = sak::LeftoverItem::Type::RegistryValue;
    value_a.path = QStringLiteral("HKCU\\Software\\Acme");
    value_a.registryValueName = QStringLiteral("RunA");
    sak::LeftoverItem value_b = value_a;
    QVERIFY(AdvancedUninstallController::sameCleanupItemIdentity(value_a, value_b));

    value_b.registryValueName = QStringLiteral("RunB");
    QVERIFY(!AdvancedUninstallController::sameCleanupItemIdentity(value_a, value_b));
    value_b = value_a;
    value_b.registryValueName = QStringLiteral("runa");
    QVERIFY(!AdvancedUninstallController::sameCleanupItemIdentity(value_a, value_b));
    value_b = value_a;
    value_b.type = sak::LeftoverItem::Type::StartupEntry;
    QVERIFY(!AdvancedUninstallController::sameCleanupItemIdentity(value_a, value_b));

    sak::LeftoverItem firewall_a;
    firewall_a.type = sak::LeftoverItem::Type::FirewallRule;
    firewall_a.path = QStringLiteral("Acme Rule");
    firewall_a.firewallDirection = QStringLiteral("in");
    sak::LeftoverItem firewall_b = firewall_a;
    firewall_b.firewallDirection = QStringLiteral("out");
    QVERIFY(!AdvancedUninstallController::sameCleanupItemIdentity(firewall_a, firewall_b));

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    QVERIFY2(!source.isEmpty() && !header.isEmpty(), "controller sources could not be read");
    for (const QString& field : {QStringLiteral("lhs.type == rhs.type"),
                                 QStringLiteral("lhs.path == rhs.path"),
                                 QStringLiteral("lhs.registryValueName == rhs.registryValueName"),
                                 QStringLiteral("lhs.firewallDirection == rhs.firewallDirection"),
                                 QStringLiteral("lhs.firewallProfile == rhs.firewallProfile"),
                                 QStringLiteral("lhs.firewallProgram == rhs.firewallProgram")}) {
        QVERIFY2(source.contains(field),
                 qPrintable(QStringLiteral("typed cleanup identity omitted: %1").arg(field)));
    }
    QVERIFY(header.contains(QStringLiteral("void itemCleaned(sak::LeftoverItem item")));
    QVERIFY(header.contains(QStringLiteral("void itemCleanupError(sak::LeftoverItem item")));

    AdvancedUninstallController ctrl;
    ctrl.appendUniqueBatchAutoCleanItems({value_a, value_a});
    QCOMPARE(ctrl.m_batchAutoCleanItems.size(), 1);
    value_b = value_a;
    value_b.registryValueName = QStringLiteral("RunB");
    ctrl.appendUniqueBatchAutoCleanItems({value_b});
    QCOMPARE(ctrl.m_batchAutoCleanItems.size(), 2);
    QVERIFY(
        source.contains(QStringLiteral("appendUniqueBatchAutoCleanItems(autoCleanableLeftovers(")));
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

void AdvancedUninstallControllerTests::defaultScanLevel_rejectsInvalid() {
    AdvancedUninstallController ctrl;
    ctrl.setDefaultScanLevel(ScanLevel::Moderate);

    // An out-of-range enum (e.g. a bad numeric cast) must be refused and the prior valid level
    // kept, rather than stored as a value that would drive an invalid scan.
    ctrl.setDefaultScanLevel(static_cast<ScanLevel>(99));
    QCOMPARE(ctrl.defaultScanLevel(), ScanLevel::Moderate);

    ctrl.setDefaultScanLevel(static_cast<ScanLevel>(-1));
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

// -- Queue Management --------------------------------------------------------

void AdvancedUninstallControllerTests::addToQueue_appendsItem() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Test App";
    QVERIFY(ctrl.addToQueue(prog, ScanLevel::Moderate, true));

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
    QVERIFY(ctrl.addToQueue(prog1, ScanLevel::Safe, false));

    ProgramInfo prog2;
    prog2.displayName = "App B";
    QVERIFY(ctrl.addToQueue(prog2, ScanLevel::Advanced, true));

    ProgramInfo prog3;
    prog3.displayName = "App C";
    QVERIFY(ctrl.addToQueue(prog3, ScanLevel::Moderate, true));

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
    QVERIFY(!ctrl.addToQueue(prog, ScanLevel::Moderate, true));

    // Queue should remain empty
    QVERIFY(ctrl.queue().isEmpty());

    // Status message should have been emitted
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Cannot add to queue: program name is empty."));
}

void AdvancedUninstallControllerTests::addToQueue_invalidScanLevelRejected() {
    AdvancedUninstallController ctrl;
    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);
    ProgramInfo prog;
    prog.displayName = QStringLiteral("Invalid Scan App");

    QVERIFY(!ctrl.addToQueue(prog, static_cast<ScanLevel>(99), true));

    QVERIFY(ctrl.queue().isEmpty());
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(),
             QStringLiteral("Cannot add to queue: scan level is invalid."));
}

void AdvancedUninstallControllerTests::addToQueue_rejectsDuplicateAndBound() {
    AdvancedUninstallController ctrl;
    ProgramInfo program;
    program.displayName = QStringLiteral("Acme App");
    program.publisher = QStringLiteral("Acme");
    program.displayVersion = QStringLiteral("1.0");
    QVERIFY(ctrl.addToQueue(program, ScanLevel::Safe, false));
    QVERIFY(!ctrl.addToQueue(program, ScanLevel::Advanced, true));
    QCOMPARE(ctrl.queue().size(), 1);
    ProgramInfo distinct_install = program;
    distinct_install.installLocation = QStringLiteral("C:\\Apps\\Acme-Portable");
    QVERIFY(ctrl.addToQueue(distinct_install, ScanLevel::Safe, false));
    QCOMPARE(ctrl.queue().size(), 2);

    QVERIFY(ctrl.clearQueue());
    for (int index = 0; index < AdvancedUninstallController::kMaxBatchQueueItems; ++index) {
        ProgramInfo unique;
        unique.displayName = QStringLiteral("App %1").arg(index);
        QVERIFY(ctrl.addToQueue(unique, ScanLevel::Safe, false));
    }
    ProgramInfo overflow;
    overflow.displayName = QStringLiteral("Overflow App");
    QVERIFY(!ctrl.addToQueue(overflow, ScanLevel::Safe, false));
    QCOMPARE(ctrl.queue().size(), AdvancedUninstallController::kMaxBatchQueueItems);

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY(source.contains(QStringLiteral("sameQueuedProgram(queued.program, program)")));
    QVERIFY(source.contains(QStringLiteral("m_queue.size() >= kMaxBatchQueueItems")));
    QVERIFY(header.contains(QStringLiteral("[[nodiscard]] bool addToQueue(")));
    QVERIFY(header.contains(QStringLiteral("[[nodiscard]] bool clearQueue()")));
    QVERIFY(panel.contains(QStringLiteral("if (!m_controller->addToQueue(")));
    QVERIFY(panel.contains(QStringLiteral("if (!m_controller->clearQueue())")));
}

void AdvancedUninstallControllerTests::removeFromQueue_valid() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog1;
    prog1.displayName = "Keep";
    QVERIFY(ctrl.addToQueue(prog1, ScanLevel::Safe, true));

    ProgramInfo prog2;
    prog2.displayName = "Remove";
    QVERIFY(ctrl.addToQueue(prog2, ScanLevel::Moderate, true));

    ProgramInfo prog3;
    prog3.displayName = "Keep Too";
    QVERIFY(ctrl.addToQueue(prog3, ScanLevel::Advanced, true));

    QVERIFY(ctrl.removeFromQueue(1));  // Remove "Remove"

    QCOMPARE(ctrl.queue().size(), 2);
    QCOMPARE(ctrl.queue()[0].program.displayName, "Keep");
    QCOMPARE(ctrl.queue()[1].program.displayName, "Keep Too");
}

void AdvancedUninstallControllerTests::removeFromQueue_invalidNegative() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Stay";
    QVERIFY(ctrl.addToQueue(prog, ScanLevel::Safe, true));

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);
    QVERIFY(!ctrl.removeFromQueue(-1));

    QCOMPARE(ctrl.queue().size(), 1);
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(),
             QStringLiteral("Cannot remove queue item: index is invalid."));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QVERIFY(source.contains(QStringLiteral("if (index < 0 || index >= m_queue.size())")));
}

void AdvancedUninstallControllerTests::removeFromQueue_invalidTooLarge() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Stay";
    QVERIFY(ctrl.addToQueue(prog, ScanLevel::Safe, true));

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);
    QVERIFY(!ctrl.removeFromQueue(5));

    QCOMPARE(ctrl.queue().size(), 1);
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy.at(0).at(0).toString(),
             QStringLiteral("Cannot remove queue item: index is invalid."));
}

void AdvancedUninstallControllerTests::clearQueue_emptiesQueue() {
    AdvancedUninstallController ctrl;

    for (int i = 0; i < 5; ++i) {
        ProgramInfo prog;
        prog.displayName = QString("App %1").arg(i);
        QVERIFY(ctrl.addToQueue(prog, ScanLevel::Moderate, true));
    }
    QCOMPARE(ctrl.queue().size(), 5);

    QVERIFY(ctrl.clearQueue());
    QVERIFY(ctrl.queue().isEmpty());
}

void AdvancedUninstallControllerTests::queueItem_defaultStatus() {
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = "Test";
    QVERIFY(ctrl.addToQueue(prog, ScanLevel::Safe, false));

    // Queue items should default to Queued status
    QCOMPARE(ctrl.queue()[0].status, UninstallQueueItem::Status::Queued);
}

void AdvancedUninstallControllerTests::queueMutation_refusedDuringBatch() {
    // While a batch is running the queue is indexed by m_batchIndex; mutating it mid-flight would
    // shift indices and misattribute a completion to the wrong program. add/remove/clear must all
    // be refused until the batch ends. (create_restore_point=false so batch is not aborted on a
    // platform without System Restore; the worker's completion signals are queued and not delivered
    // during this synchronous body, so m_batchIndex stays >= 0 throughout.)
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("BatchApp");  // no uninstallString -> native step fails fast
    QVERIFY(ctrl.addToQueue(prog, ScanLevel::Safe, false));

    QVERIFY(ctrl.startBatchUninstall(false));
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Uninstalling);

    ProgramInfo sneaky;
    sneaky.displayName = QStringLiteral("SneakyApp");
    QVERIFY(!ctrl.addToQueue(sneaky, ScanLevel::Safe, false));
    QCOMPARE(ctrl.queue().size(), 1);  // add refused mid-batch

    QVERIFY(!ctrl.removeFromQueue(0));
    QCOMPARE(ctrl.queue().size(), 1);  // remove refused mid-batch

    QVERIFY(!ctrl.clearQueue());
    QCOMPARE(ctrl.queue().size(), 1);  // clear refused mid-batch

    ctrl.cancelOperation();            // request stop; destructor joins the worker
}

// -- State Guards ------------------------------------------------------------

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
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Another operation is already in progress."));

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

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Another operation is already in progress."));
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

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Another operation is already in progress."));
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    ctrl.cancelOperation();
}

void AdvancedUninstallControllerTests::startBatch_rejectsEmptyQueue() {
    AdvancedUninstallController ctrl;
    QVERIFY(ctrl.queue().isEmpty());

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);
    QSignalSpy failedSpy(&ctrl, &AdvancedUninstallController::batchFailed);

    QVERIFY(!ctrl.startBatchUninstall(false));

    // Should have emitted "Batch queue is empty" status
    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(), QStringLiteral("Batch queue is empty."));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("Batch queue is empty."));
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::startBatch_setsUninstallingState() {
    // A batch must OWN the busy state for the whole run: the instant it starts the controller must
    // be Uninstalling (not Idle), so a concurrent refresh / manual clean / second uninstall cannot
    // slip in between queue items. (create_restore_point=false so batch is not aborted on a
    // platform without System Restore.)
    AdvancedUninstallController ctrl;

    ProgramInfo prog;
    prog.displayName = QStringLiteral("BatchApp");  // no uninstallString -> native step fails fast
    QVERIFY(ctrl.addToQueue(prog, ScanLevel::Safe, false));

    QVERIFY(ctrl.startBatchUninstall(false));

    // The worker runs on its own thread and signals back via queued connections that are not
    // delivered during this synchronous body, so the state observed here is the batch start state.
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Uninstalling);

    ctrl.cancelOperation();  // request stop; destructor joins the worker
}

void AdvancedUninstallControllerTests::startBatch_exposesFailureTerminalAndCheckedCaller() {
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString dialog =
        readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel_dialogs.cpp"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY2(!header.isEmpty() && !source.isEmpty() && !dialog.isEmpty() && !panel.isEmpty(),
             "batch start consumer sources could not be read");
    QVERIFY(header.contains(
        QStringLiteral("[[nodiscard]] bool startBatchUninstall(bool create_restore_point)")));
    QVERIFY(header.contains(QStringLiteral("void batchFailed(const QString& error)")));
    QVERIFY(source.contains(QStringLiteral("Q_EMIT batchFailed(")));
    QVERIFY(dialog.contains(
        QStringLiteral("if (!m_controller->startBatchUninstall(restore_check->isChecked()))")));
    QVERIFY2(!dialog.contains(QStringLiteral("setOperationRunning(true);\n        logMessage(")),
             "panel still becomes busy before batch admission succeeds");
    QVERIFY(panel.contains(QStringLiteral("&AdvancedUninstallPanel::onBatchFailed")));
    QVERIFY(panel.contains(QStringLiteral("tr(\"Batch Uninstall Failed\")")));

    AdvancedUninstallController ctrl;
    ProgramInfo program;
    program.displayName = QStringLiteral("Busy Batch");
    QVERIFY(ctrl.addToQueue(program, ScanLevel::Safe, false));
    ctrl.setState(AdvancedUninstallController::State::Enumerating);
    QSignalSpy failedSpy(&ctrl, &AdvancedUninstallController::batchFailed);
    QVERIFY(!ctrl.startBatchUninstall(false));
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);
    QCOMPARE(source.count(QStringLiteral("Q_EMIT batchFailed(error);")), 3);
    QVERIFY(source.contains(
        QStringLiteral("Cannot start batch while cancelled enumeration is still stopping.")));
    QVERIFY(panel.contains(QStringLiteral(
        "m_controller->currentState() == AdvancedUninstallController::State::Idle")));
    const qsizetype batch_failed_handler =
        panel.indexOf(QStringLiteral("void AdvancedUninstallPanel::onBatchFailed"));
    const qsizetype idle_guard = panel.indexOf(
        QStringLiteral("m_controller->currentState() == AdvancedUninstallController::State::Idle"),
        batch_failed_handler);
    const qsizetype auto_cleanup_reset =
        panel.indexOf(QStringLiteral("m_auto_cleanup_in_progress = false"), idle_guard);
    QVERIFY(batch_failed_handler >= 0 && idle_guard > batch_failed_handler &&
            auto_cleanup_reset > idle_guard);
}

// -- Input Validation --------------------------------------------------------

void AdvancedUninstallControllerTests::uninstallProgram_rejectsEmptyName() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    // displayName left empty
    ctrl.uninstallProgram(prog, ScanLevel::Moderate, false, false);

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Cannot uninstall: program name is empty."));
    // Should remain idle
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::forceUninstall_rejectsEmptyName() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    ctrl.forceUninstall(prog, ScanLevel::Moderate, false);

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Cannot uninstall: program name is empty."));
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::removeRegistryEntry_rejectsEmptyKeyPath() {
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    prog.displayName = "TestApp";
    // registryKeyPath left empty
    ctrl.removeRegistryEntry(prog);

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(), QStringLiteral("Cannot remove: no registry key path."));
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::removeRegistryEntry_rejectsNonHiveRooted() {
    // Defense in depth: a registry key path that is not rooted at a recognized hive is malformed
    // and must be refused before a destructive RegistryOnly worker is spawned. The controller stays
    // Idle.
    AdvancedUninstallController ctrl;

    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);

    ProgramInfo prog;
    prog.displayName = QStringLiteral("TestApp");
    prog.registryKeyPath = QStringLiteral("SOFTWARE\\Acme");  // no HKxx\\ root
    ctrl.removeRegistryEntry(prog);

    QCOMPARE(statusSpy.count(), 1);
    QCOMPARE(statusSpy[0][0].toString(),
             QStringLiteral("Cannot remove: registry key path is not hive-rooted."));
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::operationScanLevels_rejectInvalid() {
    AdvancedUninstallController ctrl;
    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);
    QSignalSpy startedSpy(&ctrl, &AdvancedUninstallController::uninstallStarted);
    ProgramInfo prog;
    prog.displayName = QStringLiteral("Invalid Scan App");

    ctrl.uninstallProgram(prog, static_cast<ScanLevel>(-1), false, false);
    ctrl.forceUninstall(prog, static_cast<ScanLevel>(99), false);

    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
    QCOMPARE(startedSpy.count(), 0);
    QCOMPARE(statusSpy.count(), 2);
    QCOMPARE(statusSpy.at(0).at(0).toString(),
             QStringLiteral("Cannot uninstall: scan level is invalid."));
    QCOMPARE(statusSpy.at(1).at(0).toString(),
             QStringLiteral("Cannot force uninstall: scan level is invalid."));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QCOMPARE(source.count(QStringLiteral("if (!isValidScanLevel(scan_level))")), 3);
}

// -- Settings Persistence ----------------------------------------------------

void AdvancedUninstallControllerTests::saveAndLoadSettings_roundTrip() {
    // Set non-default values and save
    {
        AdvancedUninstallController ctrl;
        ctrl.setAutoRestorePoint(false);
        ctrl.setAutoCleanSafe(false);
        ctrl.setDefaultScanLevel(ScanLevel::Advanced);
        ctrl.setShowSystemComponents(true);
        QVERIFY(ctrl.saveSettings());
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
        QVERIFY(ctrl.saveSettings());
    }
}

void AdvancedUninstallControllerTests::settingsPersistence_checksHealthAndSync() {
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString dialog =
        readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel_dialogs.cpp"));
    QVERIFY2(!source.isEmpty() && !header.isEmpty() && !dialog.isEmpty(),
             "Advanced Uninstall settings sources could not be read");
    QVERIFY(header.contains(QStringLiteral("[[nodiscard]] bool loadSettings()")));
    QVERIFY(header.contains(QStringLiteral("[[nodiscard]] bool saveSettings()")));
    QVERIFY(header.contains(QStringLiteral("void settingsError(")));
    QVERIFY(source.contains(QStringLiteral("if (!cfg.isHealthy())")));
    QVERIFY(source.contains(QStringLiteral("if (!cfg.sync())")));
    QVERIFY(dialog.contains(QStringLiteral("if (!m_controller->saveSettings())")));
    QVERIFY(dialog.contains(QStringLiteral("tr(\"Settings Not Saved\")")));
}

void AdvancedUninstallControllerTests::settingsError_isDeferredForObservers() {
    AdvancedUninstallController ctrl;
    QSignalSpy errorSpy(&ctrl, &AdvancedUninstallController::settingsError);

    ctrl.deferSettingsError(QStringLiteral("injected settings error"));

    QCOMPARE(errorSpy.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 1000);
    QCOMPARE(errorSpy.at(0).at(0).toString(), QStringLiteral("injected settings error"));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QVERIFY(source.contains(QStringLiteral("QTimer::singleShot(0, this")));
    QCOMPARE(source.count(QStringLiteral("deferSettingsError(error)")), 4);
}

// -- State Machine -----------------------------------------------------------

void AdvancedUninstallControllerTests::stateChanged_emitsSignal() {
    AdvancedUninstallController ctrl;

    QSignalSpy stateSpy(&ctrl, &AdvancedUninstallController::stateChanged);

    ctrl.refreshPrograms();

    // Should have transitioned to Enumerating
    QCOMPARE(stateSpy.count(), 1);
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

void AdvancedUninstallControllerTests::enumerationCancel_emitsTerminalAndBlocksOverlap() {
    AdvancedUninstallController ctrl;
    QSignalSpy cancelledSpy(&ctrl, &AdvancedUninstallController::enumerationCancelled);
    QSignalSpy statusSpy(&ctrl, &AdvancedUninstallController::statusMessage);
    bool cancelled_at_idle = false;
    connect(&ctrl, &AdvancedUninstallController::enumerationCancelled, &ctrl, [&]() {
        cancelled_at_idle = ctrl.currentState() == AdvancedUninstallController::State::Idle;
    });
    ctrl.refreshPrograms();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);

    ctrl.cancelOperation();

    QCOMPARE(cancelledSpy.count(), 0);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);
    ctrl.refreshPrograms();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);
    QCOMPARE(statusSpy.last().at(0).toString(),
             QStringLiteral("Waiting for cancelled enumeration to stop."));
    QTRY_COMPARE_WITH_TIMEOUT(ctrl.currentState(), AdvancedUninstallController::State::Idle, 5000);
    QCOMPARE(cancelledSpy.count(), 1);
    QVERIFY(cancelled_at_idle);

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY(source.contains(QStringLiteral("Q_EMIT enumerationCancelled()")));
    QVERIFY(source.contains(QStringLiteral("m_enumerationStopping = true")));
    const qsizetype cancel_begin =
        source.indexOf(QStringLiteral("void AdvancedUninstallController::cancelOperation"));
    const qsizetype cancel_end = source.indexOf(QStringLiteral("// -- Batch Uninstall"),
                                                cancel_begin);
    QVERIFY(cancel_begin >= 0 && cancel_end > cancel_begin);
    const QString cancel_function = source.mid(cancel_begin, cancel_end - cancel_begin);
    QVERIFY(!cancel_function.contains(QStringLiteral("setState(State::Idle)")));
    QVERIFY(!cancel_function.contains(QStringLiteral("Q_EMIT enumerationCancelled()")));
    const qsizetype finished_begin =
        source.indexOf(QStringLiteral("onEnumerationFinished(int generation"));
    const qsizetype failed_begin =
        source.indexOf(QStringLiteral("onEnumerationFailed(int generation"), finished_begin);
    const qsizetype uninstall_begin =
        source.indexOf(QStringLiteral("onUninstallComplete(UninstallReport"), failed_begin);
    QVERIFY(finished_begin >= 0 && failed_begin > finished_begin && uninstall_begin > failed_begin);
    const QString finished_function = source.mid(finished_begin, failed_begin - finished_begin);
    const QString failed_function = source.mid(failed_begin, uninstall_begin - failed_begin);
    for (const QString& function : {finished_function, failed_function}) {
        const qsizetype stopping_cleared =
            function.indexOf(QStringLiteral("m_enumerationStopping = false"));
        const qsizetype branch_return = function.indexOf(QStringLiteral("return;"),
                                                         stopping_cleared);
        const qsizetype idle = function.indexOf(QStringLiteral("setState(State::Idle)"),
                                                stopping_cleared);
        const qsizetype cancelled =
            function.indexOf(QStringLiteral("Q_EMIT enumerationCancelled()"), stopping_cleared);
        QVERIFY(stopping_cleared >= 0 && branch_return > stopping_cleared);
        QVERIFY(idle > stopping_cleared && cancelled > idle && branch_return > cancelled);
    }
    QVERIFY(panel.contains(QStringLiteral("&AdvancedUninstallPanel::onEnumerationCancelled")));
    QVERIFY(panel.contains(QStringLiteral("Program enumeration cancelled.")));
}

void AdvancedUninstallControllerTests::enumerationStopping_blocksDestructiveAdmission() {
    AdvancedUninstallController ctrl;
    ctrl.m_enumerationStopping = true;
    ctrl.setState(AdvancedUninstallController::State::Enumerating);
    ProgramInfo program;
    program.displayName = QStringLiteral("Blocked App");
    sak::LeftoverItem item;
    item.type = sak::LeftoverItem::Type::File;
    item.path = QStringLiteral("C:\\Temp\\Acme\\stale.tmp");
    item.selected = true;
    QSignalSpy uninstallSpy(&ctrl, &AdvancedUninstallController::uninstallStarted);
    QSignalSpy cleanupSpy(&ctrl, &AdvancedUninstallController::cleanupStarted);
    QSignalSpy batchFailedSpy(&ctrl, &AdvancedUninstallController::batchFailed);

    ctrl.uninstallProgram(program, ScanLevel::Safe, false, false);
    ctrl.cleanLeftovers({item});
    QVERIFY(ctrl.addToQueue(program, ScanLevel::Safe, false));
    QVERIFY(!ctrl.startBatchUninstall(false));

    QCOMPARE(uninstallSpy.count(), 0);
    QCOMPARE(cleanupSpy.count(), 0);
    QCOMPARE(batchFailedSpy.count(), 1);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Enumerating);
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QCOMPARE(source.count(QStringLiteral("refuseWhileEnumerationStopping(")), 6);
    ctrl.m_enumerationStopping = false;
    ctrl.setState(AdvancedUninstallController::State::Idle);
}

void AdvancedUninstallControllerTests::leftoverScan_entersState() {
    AdvancedUninstallController ctrl;
    QSignalSpy stateSpy(&ctrl, &AdvancedUninstallController::stateChanged);
    QSignalSpy scanSpy(&ctrl, &AdvancedUninstallController::leftoverScanStarted);
    ctrl.setState(AdvancedUninstallController::State::Uninstalling);
    stateSpy.clear();

    ctrl.onLeftoverScanStarted(ScanLevel::Advanced);

    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::LeftoverScanning);
    QCOMPARE(stateSpy.count(), 1);
    QCOMPARE(stateSpy.at(0).at(0).value<AdvancedUninstallController::State>(),
             AdvancedUninstallController::State::LeftoverScanning);
    QCOMPARE(scanSpy.count(), 1);
    QCOMPARE(scanSpy.at(0).at(0).value<ScanLevel>(), ScanLevel::Advanced);

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QCOMPARE(source.count(QStringLiteral("onLeftoverScanStarted(level)")),
             1);  // one generation-guarded shared wiring path serves single and batch workers
}

void AdvancedUninstallControllerTests::cleanupFailure_emitsTerminal() {
    AdvancedUninstallController ctrl;
    QSignalSpy failedSpy(&ctrl, &AdvancedUninstallController::cleanupFailed);
    QSignalSpy finishedSpy(&ctrl, &AdvancedUninstallController::cleanupFinished);
    ctrl.setState(AdvancedUninstallController::State::Cleaning);

    ctrl.onCleanupWorkerFailed(7, QStringLiteral("injected cleanup failure"));

    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(0).toInt(), 7);
    QCOMPARE(failedSpy.at(0).at(1).toString(), QStringLiteral("injected cleanup failure"));
    QCOMPARE(finishedSpy.count(), 0);
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY(source.contains(QStringLiteral("Q_EMIT cleanupFailed(error_code, message)")));
    QVERIFY(panel.contains(QStringLiteral("&AdvancedUninstallPanel::onCleanupFailed")));
    QVERIFY(panel.contains(QStringLiteral("tr(\"Cleanup Failed\")")));
}

void AdvancedUninstallControllerTests::cleanupRefusals_areTypedAndCounted() {
    AdvancedUninstallController ctrl;
    sak::LeftoverItem protected_item;
    protected_item.type = sak::LeftoverItem::Type::File;
    protected_item.path = QStringLiteral("C:\\Windows\\System32\\protected.tmp");
    protected_item.selected = true;
    QSignalSpy errorSpy(&ctrl, &AdvancedUninstallController::itemCleanupError);
    QSignalSpy itemSpy(&ctrl, &AdvancedUninstallController::itemCleaned);
    QSignalSpy finishedSpy(&ctrl, &AdvancedUninstallController::cleanupFinished);

    ctrl.cleanLeftovers({protected_item});

    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(qvariant_cast<sak::LeftoverItem>(errorSpy.at(0).at(0)).path, protected_item.path);
    QVERIFY(!errorSpy.at(0).at(1).toString().isEmpty());
    QCOMPARE(itemSpy.count(), 1);
    QCOMPARE(qvariant_cast<sak::LeftoverItem>(itemSpy.at(0).at(0)).type,
             sak::LeftoverItem::Type::File);
    QCOMPARE(itemSpy.at(0).at(1).toBool(), false);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(finishedSpy.at(0).at(1).toInt(), 1);

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QVERIFY(source.contains(QStringLiteral("failed += m_pendingCleanupRefused")));
    QVERIFY(source.contains(QStringLiteral("reportCleanupRefusals(screen.refused)")));
    QVERIFY(source.contains(QStringLiteral("automatic cleanup refused %1 protected item(s)")));

    AdvancedUninstallController partial_ctrl;
    QSignalSpy partialFinishedSpy(&partial_ctrl, &AdvancedUninstallController::cleanupFinished);
    partial_ctrl.m_pendingCleanupRefused = 2;
    partial_ctrl.onCleanupComplete(1, 3, 64);
    QCOMPARE(partialFinishedSpy.count(), 1);
    QCOMPARE(partialFinishedSpy.at(0).at(0).toInt(), 1);
    QCOMPARE(partialFinishedSpy.at(0).at(1).toInt(), 5);

    AdvancedUninstallController batch_ctrl;
    batch_ctrl.m_batchAutoCleanItems = {protected_item};
    QSignalSpy batchFinishedSpy(&batch_ctrl, &AdvancedUninstallController::batchFinished);
    QSignalSpy batchStatusSpy(&batch_ctrl, &AdvancedUninstallController::statusMessage);
    batch_ctrl.finishBatchUninstall();
    QCOMPARE(batchFinishedSpy.count(), 1);
    QCOMPARE(batchFinishedSpy.at(0).at(3).toInt(), 1);
    QVERIFY(batchStatusSpy.last().at(0).toString().contains(QStringLiteral("refused 1 protected")));

    sak::LeftoverItem irreversible_registry;
    irreversible_registry.type = sak::LeftoverItem::Type::RegistryKey;
    irreversible_registry.path = QStringLiteral("HKCU\\Software\\Acme\\Obsolete");
    irreversible_registry.selected = true;
    const auto manual_screen = ctrl.screenCleanupItems({irreversible_registry});
    QCOMPARE(manual_screen.allowed.size(), 1);
    QVERIFY(manual_screen.refused.isEmpty());
    const qsizetype screen_begin =
        source.indexOf(QStringLiteral("AdvancedUninstallController::screenCleanupItems"));
    const qsizetype screen_end = source.indexOf(
        QStringLiteral("void AdvancedUninstallController::cleanLeftovers"), screen_begin);
    QVERIFY(screen_begin >= 0 && screen_end > screen_begin);
    QVERIFY(!source.mid(screen_begin, screen_end - screen_begin)
                 .contains(QStringLiteral("isRecoverableLeftoverType")));
    const QString controller_header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString guard_header =
        readRepoSource(QStringLiteral("include/sak/leftover_cleanup_item_guard.h"));
    QVERIFY(controller_header.contains(
        QStringLiteral("Irreversible registry/service/task/firewall targets remain allowed")));
    QVERIFY(guard_header.contains(QStringLiteral("separate caller policy")));
    QVERIFY(!guard_header.contains(QStringLiteral("unrecoverable target")));
}

void AdvancedUninstallControllerTests::workerCallbacks_requireCurrentGeneration() {
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    QVERIFY2(!source.isEmpty() && !header.isEmpty(), "controller sources could not be read");
    QCOMPARE(source.count(QStringLiteral("generation == m_uninstallGeneration")), 10);
    QCOMPARE(source.count(QStringLiteral("generation != m_cleanupGeneration")), 2);
    QCOMPARE(source.count(QStringLiteral("generation == m_cleanupGeneration")), 4);
    QCOMPARE(source.count(QStringLiteral("++m_uninstallGeneration")), 7);
    QCOMPARE(source.count(QStringLiteral("++m_cleanupGeneration")), 4);
    QVERIFY(header.contains(QStringLiteral("int m_uninstallGeneration")));
    QVERIFY(header.contains(QStringLiteral("int m_cleanupGeneration")));
}

void AdvancedUninstallControllerTests::failureSignals_preserveErrorCodes() {
    AdvancedUninstallController ctrl;
    QSignalSpy uninstallSpy(&ctrl, &AdvancedUninstallController::uninstallFailed);
    QSignalSpy cleanupSpy(&ctrl, &AdvancedUninstallController::cleanupFailed);

    ctrl.onUninstallWorkerFailed(37, QStringLiteral("uninstall failure"));
    ctrl.onCleanupWorkerFailed(41, QStringLiteral("cleanup failure"));

    QCOMPARE(uninstallSpy.count(), 1);
    QCOMPARE(uninstallSpy.at(0).at(0).toInt(), 37);
    QCOMPARE(uninstallSpy.at(0).at(1).toString(), QStringLiteral("uninstall failure"));
    QCOMPARE(cleanupSpy.count(), 1);
    QCOMPARE(cleanupSpy.at(0).at(0).toInt(), 41);
    QCOMPARE(cleanupSpy.at(0).at(1).toString(), QStringLiteral("cleanup failure"));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY(source.contains(QStringLiteral("Q_EMIT uninstallFailed(error_code, message)")));
    QVERIFY(source.contains(QStringLiteral("Q_EMIT cleanupFailed(error_code, message)")));
    QVERIFY(panel.contains(QStringLiteral("onUninstallFailed(int error_code")));
    QVERIFY(panel.contains(QStringLiteral("onCleanupFailed(int error_code")));
}

void AdvancedUninstallControllerTests::batchItemFailure_preservesDetailsThroughTerminal() {
    AdvancedUninstallController ctrl;
    ProgramInfo program;
    program.displayName = QStringLiteral("Broken Batch App");
    QVERIFY(ctrl.addToQueue(program, ScanLevel::Safe, false));
    ctrl.m_batchIndex = 0;
    ctrl.m_queue[0].status = UninstallQueueItem::Status::InProgress;
    ctrl.setState(AdvancedUninstallController::State::Uninstalling);
    QStringList order;
    connect(&ctrl,
            &AdvancedUninstallController::batchItemFailed,
            &ctrl,
            [&order](int, int, const QString&) { order.append(QStringLiteral("item")); });
    connect(&ctrl,
            &AdvancedUninstallController::batchFinished,
            &ctrl,
            [&order](int, int, int, int, qint64) { order.append(QStringLiteral("terminal")); });
    QSignalSpy itemFailedSpy(&ctrl, &AdvancedUninstallController::batchItemFailed);
    QSignalSpy batchFinishedSpy(&ctrl, &AdvancedUninstallController::batchFinished);

    ctrl.onUninstallWorkerFailed(73, QStringLiteral("exact worker failure"));

    QCOMPARE(itemFailedSpy.count(), 1);
    QCOMPARE(itemFailedSpy.at(0).at(0).toInt(), 0);
    QCOMPARE(itemFailedSpy.at(0).at(1).toInt(), 73);
    QCOMPARE(itemFailedSpy.at(0).at(2).toString(), QStringLiteral("exact worker failure"));
    QCOMPARE(batchFinishedSpy.count(), 1);
    QCOMPARE(order, QStringList({QStringLiteral("item"), QStringLiteral("terminal")}));
    const UninstallQueueItem failed_item = ctrl.queue().at(0);
    QCOMPARE(failed_item.status, UninstallQueueItem::Status::Failed);
    QCOMPARE(failed_item.errorCode, 73);
    QCOMPARE(failed_item.errorMessage, QStringLiteral("exact worker failure"));

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY(source.contains(QStringLiteral("item.errorCode = error_code")));
    QVERIFY(source.contains(QStringLiteral("item.errorMessage = message")));
    QVERIFY(source.contains(
        QStringLiteral("Q_EMIT batchItemFailed(m_batchIndex, error_code, message)")));
    QVERIFY(header.contains(QStringLiteral("void batchItemFailed(")));
    QVERIFY(panel.contains(QStringLiteral("&AdvancedUninstallController::batchItemFailed")));
    QVERIFY(panel.contains(QStringLiteral("failed (error %2): %3")));
}

void AdvancedUninstallControllerTests::automaticCleanupTerminal_precedesBatchFailure() {
    AdvancedUninstallController failed_ctrl;
    failed_ctrl.m_batchCompletionPending = true;
    QStringList failed_order;
    connect(&failed_ctrl,
            &AdvancedUninstallController::cleanupFailed,
            &failed_ctrl,
            [&failed_order](int, const QString&) {
                failed_order.append(QStringLiteral("cleanup"));
            });
    connect(&failed_ctrl,
            &AdvancedUninstallController::batchFailed,
            &failed_ctrl,
            [&failed_order](const QString&) { failed_order.append(QStringLiteral("batch")); });
    failed_ctrl.onCleanupWorkerFailed(9, QStringLiteral("injected failure"));
    QCOMPARE(failed_order, QStringList({QStringLiteral("cleanup"), QStringLiteral("batch")}));

    AdvancedUninstallController cancelled_ctrl;
    cancelled_ctrl.m_batchCompletionPending = true;
    QStringList cancelled_order;
    connect(&cancelled_ctrl,
            &AdvancedUninstallController::cleanupCancelled,
            &cancelled_ctrl,
            [&cancelled_order]() { cancelled_order.append(QStringLiteral("cleanup")); });
    connect(&cancelled_ctrl,
            &AdvancedUninstallController::batchFailed,
            &cancelled_ctrl,
            [&cancelled_order](const QString&) {
                cancelled_order.append(QStringLiteral("batch"));
            });
    cancelled_ctrl.onCleanupWorkerCancelled();
    QCOMPARE(cancelled_order, QStringList({QStringLiteral("cleanup"), QStringLiteral("batch")}));

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    const qsizetype failed_begin =
        source.indexOf(QStringLiteral("onCleanupWorkerFailed(int error_code"));
    const qsizetype cancelled_begin = source.indexOf(QStringLiteral("onCleanupWorkerCancelled()"),
                                                     failed_begin);
    const qsizetype terminal_end = source.indexOf(QStringLiteral("// -- Private"), cancelled_begin);
    QVERIFY(failed_begin >= 0 && cancelled_begin > failed_begin && terminal_end > cancelled_begin);
    const QString failed_function = source.mid(failed_begin, cancelled_begin - failed_begin);
    const QString cancelled_function = source.mid(cancelled_begin, terminal_end - cancelled_begin);
    const qsizetype cleanup_failed =
        failed_function.indexOf(QStringLiteral("Q_EMIT cleanupFailed("));
    const qsizetype batch_failed =
        failed_function.indexOf(QStringLiteral("failDeferredBatchCleanup("));
    const qsizetype cleanup_cancelled =
        cancelled_function.indexOf(QStringLiteral("Q_EMIT cleanupCancelled()"));
    const qsizetype batch_cancelled =
        cancelled_function.indexOf(QStringLiteral("failDeferredBatchCleanup("));
    QVERIFY(cleanup_failed >= 0 && batch_failed > cleanup_failed);
    QVERIFY(cleanup_cancelled >= 0 && batch_cancelled > cleanup_cancelled);
    QVERIFY(panel.contains(QStringLiteral("cleanup_failure_reported")));
    QVERIFY(panel.contains(QStringLiteral("if (cleanup_failure_reported)")));
}

void AdvancedUninstallControllerTests::deferredAutoClean_mergesSuccessfulAndFailedItems() {
    AdvancedUninstallController ctrl;
    sak::UninstallReport report;
    report.programName = QStringLiteral("Acme");
    report.filesDeleted = 2;
    report.failedDeletions = 3;
    report.totalSpaceRecovered = 10;
    sak::LeftoverItem removed;
    removed.type = sak::LeftoverItem::Type::File;
    removed.path = QStringLiteral("C:\\Temp\\Acme\\removed.tmp");
    removed.sizeBytes = 40;
    sak::LeftoverItem failed;
    failed.type = sak::LeftoverItem::Type::Folder;
    failed.path = QStringLiteral("C:\\Temp\\Acme\\locked");
    failed.sizeBytes = 80;
    report.foundLeftovers = {removed, failed};
    ctrl.m_autoCleanInProgress = true;
    ctrl.m_pendingAutoCleanReport = report;
    ctrl.m_pendingAutoCleanItemCount = 2;
    ctrl.m_autoCleanedItems.append(removed);
    QSignalSpy finishedSpy(&ctrl, &AdvancedUninstallController::uninstallFinished);
    QSignalSpy scanFinishedSpy(&ctrl, &AdvancedUninstallController::leftoverScanFinished);
    QSignalSpy leftoversSpy(&ctrl, &AdvancedUninstallController::leftoversUpdated);
    QSignalSpy cleanupSpy(&ctrl, &AdvancedUninstallController::cleanupFinished);

    ctrl.onCleanupComplete(1, 1, 40);

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(cleanupSpy.count(), 1);
    const auto merged = qvariant_cast<sak::UninstallReport>(finishedSpy.at(0).at(0));
    QCOMPARE(merged.filesDeleted, 3);
    QCOMPARE(merged.foldersDeleted, 0);
    QCOMPARE(merged.failedDeletions, 4);
    QCOMPARE(merged.totalSpaceRecovered, 50);
    QCOMPARE(scanFinishedSpy.count(), 0);
    QCOMPARE(leftoversSpy.count(), 1);
    const auto remaining = qvariant_cast<QVector<sak::LeftoverItem>>(leftoversSpy.at(0).at(0));
    QCOMPARE(remaining.size(), 1);
    QCOMPARE(remaining.first().path, failed.path);
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY(source.contains(
        QStringLiteral("mergeDeferredAutoCleanResult(succeeded, failed, bytes_recovered)")));
    QCOMPARE(source.count(QStringLiteral("Q_EMIT cleanupFinished(")), 2);
    QVERIFY(source.contains(QStringLiteral("Q_EMIT leftoversUpdated(")));
    QVERIFY(header.contains(QStringLiteral("void leftoversUpdated(")));
    QVERIFY(header.contains(
        QStringLiteral("[[nodiscard]] bool maybeAutoCleanSafeLeftovers(UninstallReport& report")));
    QVERIFY(panel.contains(QStringLiteral("&AdvancedUninstallController::leftoversUpdated")));
    QVERIFY(panel.contains(QStringLiteral("if (m_auto_cleanup_in_progress)")));
    QCOMPARE(panel.count(QStringLiteral("Leftover scan complete:")), 1);
}

void AdvancedUninstallControllerTests::deferredAutoClean_terminalFailureMergesPartialProgress() {
    AdvancedUninstallController ctrl;
    sak::UninstallReport report;
    sak::LeftoverItem removed;
    removed.type = sak::LeftoverItem::Type::File;
    removed.path = QStringLiteral("C:\\Temp\\Acme\\removed.tmp");
    removed.sizeBytes = 40;
    sak::LeftoverItem pending;
    pending.type = sak::LeftoverItem::Type::Folder;
    pending.path = QStringLiteral("C:\\Temp\\Acme\\pending");
    pending.sizeBytes = 80;
    report.foundLeftovers = {removed, pending};
    ctrl.m_autoCleanInProgress = true;
    ctrl.m_pendingAutoCleanReport = report;
    ctrl.m_pendingAutoCleanItemCount = 2;
    ctrl.m_autoCleanedItems.append(removed);
    QSignalSpy finishedSpy(&ctrl, &AdvancedUninstallController::uninstallFinished);

    ctrl.onCleanupWorkerFailed(7, QStringLiteral("injected failure"));

    QCOMPARE(finishedSpy.count(), 1);
    const auto merged = qvariant_cast<sak::UninstallReport>(finishedSpy.at(0).at(0));
    QCOMPARE(merged.filesDeleted, 1);
    QCOMPARE(merged.failedDeletions, 1);
    QCOMPARE(merged.totalSpaceRecovered, 40);
    QVERIFY(merged.errorLog.contains(QStringLiteral("Automatic cleanup failed: injected failure")));
}

void AdvancedUninstallControllerTests::batchCompletion_waitsForCleanupAndCarriesResult() {
    AdvancedUninstallController ctrl;
    ctrl.m_batchCompletionPending = true;
    ctrl.m_pendingBatchSucceeded = 2;
    ctrl.m_pendingBatchFailed = 1;
    ctrl.m_batchIndex = -1;
    ctrl.setState(AdvancedUninstallController::State::Cleaning);
    QSignalSpy batchSpy(&ctrl, &AdvancedUninstallController::batchFinished);
    QSignalSpy cleanupSpy(&ctrl, &AdvancedUninstallController::cleanupFinished);

    QCOMPARE(batchSpy.count(), 0);
    ctrl.onCleanupComplete(3, 2, 4096);

    QCOMPARE(batchSpy.count(), 1);
    QCOMPARE(cleanupSpy.count(), 1);
    const auto args = batchSpy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 2);
    QCOMPARE(args.at(1).toInt(), 1);
    QCOMPARE(args.at(2).toInt(), 3);
    QCOMPARE(args.at(3).toInt(), 2);
    QCOMPARE(args.at(4).toLongLong(), 4096);
    QVERIFY(!ctrl.m_batchCompletionPending);
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);

    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const qsizetype begin =
        source.indexOf(QStringLiteral("void AdvancedUninstallController::finishBatchUninstall"));
    const qsizetype end = source.indexOf(
        QStringLiteral("void AdvancedUninstallController::processNextQueueItem"), begin);
    QVERIFY(begin >= 0 && end > begin);
    const QString function = source.mid(begin, end - begin);
    const qsizetype unlock = function.indexOf(QStringLiteral("m_batchIndex = -1"));
    const qsizetype directFinish = function.indexOf(QStringLiteral("Q_EMIT batchFinished"));
    const qsizetype pending = function.indexOf(QStringLiteral("m_batchCompletionPending = true"));
    const qsizetype cleanup = function.indexOf(QStringLiteral("startCleanupWorker("));
    QVERIFY(unlock >= 0 && directFinish > unlock);
    QVERIFY(pending > directFinish && cleanup > pending);
}

void AdvancedUninstallControllerTests::batchWorker_usesCompleteSignalSet() {
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QVERIFY(!source.contains(QStringLiteral("connectBatchUninstallWorkerSignals")));
    QVERIFY(source.contains(QStringLiteral("connectUninstallWorkerSignals(true, generation)")));
    for (const QString& signal : {QStringLiteral("&UninstallWorker::nativeUninstallerStarted"),
                                  QStringLiteral("&UninstallWorker::nativeUninstallerFinished"),
                                  QStringLiteral("&UninstallWorker::restorePointCreated"),
                                  QStringLiteral("&UninstallWorker::leftoverScanProgress")}) {
        QVERIFY2(source.contains(signal), qPrintable(QStringLiteral("missing %1").arg(signal)));
    }
    QVERIFY(source.contains(QStringLiteral("Q_EMIT restorePointCreated(desc)")));
}

void AdvancedUninstallControllerTests::batchCancel_emitsCancelledQueueStatus() {
    AdvancedUninstallController ctrl;
    ProgramInfo program;
    program.displayName = QStringLiteral("Queued App");
    QVERIFY(ctrl.addToQueue(program, ScanLevel::Safe, false));
    ctrl.m_queue[0].status = UninstallQueueItem::Status::InProgress;
    ctrl.m_batchIndex = 0;
    ctrl.setState(AdvancedUninstallController::State::Uninstalling);
    int status_count = 0;
    int status_index = -1;
    UninstallQueueItem::Status emitted_status = UninstallQueueItem::Status::Queued;
    connect(&ctrl,
            &AdvancedUninstallController::queueItemStatusChanged,
            &ctrl,
            [&](int index, UninstallQueueItem::Status status) {
                ++status_count;
                status_index = index;
                emitted_status = status;
            });
    QSignalSpy finishedSpy(&ctrl, &AdvancedUninstallController::batchFinished);
    QSignalSpy failedSpy(&ctrl, &AdvancedUninstallController::batchFailed);
    QSignalSpy cancelledSpy(&ctrl, &AdvancedUninstallController::uninstallCancelled);

    ctrl.onUninstallWorkerCancelled();

    QCOMPARE(status_count, 1);
    QCOMPARE(status_index, 0);
    QCOMPARE(emitted_status, UninstallQueueItem::Status::Cancelled);
    QCOMPARE(finishedSpy.count(), 0);
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.at(0).at(0).toString(), QStringLiteral("Batch uninstall was cancelled."));
    QCOMPARE(cancelledSpy.count(), 1);
    QCOMPARE(ctrl.m_batchIndex, -1);
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    QVERIFY(source.contains(QStringLiteral(
        "Q_EMIT queueItemStatusChanged(m_batchIndex, UninstallQueueItem::Status::Cancelled)")));
    QVERIFY(source.contains(
        QStringLiteral("Q_EMIT batchFailed(QStringLiteral(\"Batch uninstall was cancelled.\"))")));
    QVERIFY(header.contains(QStringLiteral("was cancelled during uninstall")));
}

void AdvancedUninstallControllerTests::registryOnly_connectsCancellation() {
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const qsizetype begin =
        source.indexOf(QStringLiteral("void AdvancedUninstallController::removeRegistryEntry"));
    const qsizetype end = source.indexOf(QStringLiteral("QVector<LeftoverItem>"), begin);
    QVERIFY(begin >= 0 && end > begin);
    const QString function = source.mid(begin, end - begin);
    QVERIFY(function.contains(QStringLiteral("connectUninstallWorkerSignals(false, generation)")));
    QVERIFY(source.contains(QStringLiteral("&UninstallWorker::cancelled")));
    QVERIFY(source.contains(QStringLiteral("onUninstallWorkerCancelled()")));
    const qsizetype cancelBegin =
        source.indexOf(QStringLiteral("void AdvancedUninstallController::cancelOperation"));
    const qsizetype cancelEnd = source.indexOf(QStringLiteral("// -- Batch Uninstall"),
                                               cancelBegin);
    QVERIFY(cancelBegin >= 0 && cancelEnd > cancelBegin);
    const QString cancel = source.mid(cancelBegin, cancelEnd - cancelBegin);
    QVERIFY(cancel.contains(QStringLiteral("m_uninstallWorker->isRunning()")));
    QVERIFY(cancel.contains(QStringLiteral("m_uninstallWorker->requestStop()")));
}

void AdvancedUninstallControllerTests::enumThreadFailedJoin_detachesSafely() {
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const qsizetype begin =
        source.indexOf(QStringLiteral("void AdvancedUninstallController::stopEnumThread"));
    const qsizetype end =
        source.indexOf(QStringLiteral("void AdvancedUninstallController::cleanupWorkers"), begin);
    QVERIFY(begin >= 0 && end > begin);
    const QString function = source.mid(begin, end - begin);
    QVERIFY(function.contains(QStringLiteral("m_enumerator.release()")));
    QVERIFY(function.contains(QStringLiteral("m_enumThread = nullptr")));
    QVERIFY(function.contains(QStringLiteral("stale_thread->setParent(nullptr)")));
    QCOMPARE(function.count(QStringLiteral("&QObject::deleteLater")), 2);
    QVERIFY2(!function.contains(QStringLiteral("terminate()")),
             "forced termination must not replace a failed cooperative join");
}

void AdvancedUninstallControllerTests::obsoleteHelpersAndMembers_areAbsent() {
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    QVERIFY2(!header.isEmpty() && !source.isEmpty(), "controller sources could not be read");
    const QString combined = header + source;
    for (const QString& dead : {QStringLiteral("safeLeftovers("),
                                QStringLiteral("nonSafeLeftovers("),
                                QStringLiteral("m_batchRestorePointCreated"),
                                QStringLiteral("m_enum_generation"),
                                QStringLiteral("m_uninstall_worker"),
                                QStringLiteral("m_cleanup_worker"),
                                QStringLiteral("m_restore_manager"),
                                QStringLiteral("m_autoCleanedPaths")}) {
        QVERIFY2(!combined.contains(dead),
                 qPrintable(QStringLiteral("obsolete controller symbol remains: %1").arg(dead)));
    }
    QVERIFY(header.contains(QStringLiteral("m_enumGeneration")));
    QVERIFY(header.contains(QStringLiteral("m_uninstallWorker")));
    QVERIFY(header.contains(QStringLiteral("m_cleanupWorker")));
    QVERIFY(header.contains(QStringLiteral("m_restoreManager")));
}

void AdvancedUninstallControllerTests::obsoleteControllerApis_areAbsent() {
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    const QString source =
        readRepoSource(QStringLiteral("src/core/advanced_uninstall_controller.cpp"));
    const QString panelHeader =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_panel.h"));
    const QString panel = readRepoSource(QStringLiteral("src/gui/advanced_uninstall_panel.cpp"));
    QVERIFY2(!header.isEmpty() && !source.isEmpty() && !panelHeader.isEmpty() && !panel.isEmpty(),
             "controller API sources could not be read");
    const QString combined = header + source + panelHeader + panel;
    for (const QString& dead : {QStringLiteral("void removeUwpPackage("),
                                QStringLiteral("AdvancedUninstallController::removeUwpPackage"),
                                QStringLiteral("RestorePointManager* restorePointManager("),
                                QStringLiteral("void recycleFallbackItems("),
                                QStringLiteral("AdvancedUninstallPanel::onRecycleFallbackItems"),
                                QStringLiteral("connectBatchUninstallWorkerSignals")}) {
        QVERIFY2(!combined.contains(dead),
                 qPrintable(QStringLiteral("obsolete controller API remains: %1").arg(dead)));
    }
}

void AdvancedUninstallControllerTests::stateMachineDocs_describeActivityBranches() {
    const QString header =
        readRepoSource(QStringLiteral("include/sak/advanced_uninstall_controller.h"));
    QVERIFY2(!header.isEmpty(), "advanced_uninstall_controller.h could not be read");
    QVERIFY(header.contains(QStringLiteral("Activity state, not one linear pipeline")));
    QVERIFY(header.contains(QStringLiteral("enumeration runs Idle -> Enumerating -> Idle")));
    QVERIFY(header.contains(QStringLiteral("may enter LeftoverScanning")));
    QVERIFY(header.contains(QStringLiteral("optional Cleaning")));
    QVERIFY(!header.contains(
        QStringLiteral("Idle -> Enumerating -> Uninstalling -> LeftoverScanning")));
}

// -- Cancel When Idle --------------------------------------------------------

void AdvancedUninstallControllerTests::cancelOperation_whenIdle_noOp() {
    AdvancedUninstallController ctrl;
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);

    // Should not crash or change state
    ctrl.cancelOperation();
    QCOMPARE(ctrl.currentState(), AdvancedUninstallController::State::Idle);
}

// R5-P7-9: an uninstall string comes from the registry Uninstall subtree, which ANY user
// can write (HKCU), and it names the program the worker launches with an ELEVATED token.
// A bare or relative image name is resolved by the CreateProcess search order -- current
// directory and PATH ahead of the real install location -- so a non-admin who registers
// `UninstallString = "setup.exe"` and drops a setup.exe on that search path gets it run as
// administrator. Every form CreateProcess would resolve by searching must be REFUSED.

void AdvancedUninstallControllerTests::
    uninstallProgramPathTrusted_refusesSearchOrderResolvableForms() {
    using sak::UninstallWorker;

    // The finding itself: a bare image name.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("setup.exe")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("msiexec.exe")));
    // Plain relative and explicitly-relative forms.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("tools\\setup.exe")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral(".\\setup.exe")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("./setup.exe")));
    // Drive-relative: resolved against that drive's CURRENT directory, not its root.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:setup.exe")));
    // Root-relative: resolved against the CURRENT drive.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("\\App\\setup.exe")));
    // Nothing at all, or whitespace only.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QString()));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("   ")));
    // An unexpanded environment reference does not name the image its author meant, and
    // CreateProcess will not expand it either.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("%ProgramFiles%\\App\\unins000.exe")));
}

void AdvancedUninstallControllerTests::
    uninstallProgramPathTrusted_refusesUncAndTraversalAndNonExe() {
    using sak::UninstallWorker;

    // A remote share is an untrusted origin for an elevated launch.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("\\\\server\\share\\unins000.exe")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("//server/share/unins000.exe")));
    // A traversal segment moves the image out of the directory the string names.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("C:\\App\\..\\..\\Windows\\System32\\cmd.exe")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\App\\.\\unins.exe")));
    // An alternate-data-stream suffix would otherwise satisfy the ".exe" tail check while
    // the real image is a stream hanging off an innocuous file.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("C:\\App\\readme.txt:payload.exe")));
    // Only a real executable image: a script or an installer package would be handed to a
    // shell or an associated handler under the elevated token.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\App\\unins.bat")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\App\\unins.cmd")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\App\\product.msi")));
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\App\\unins")));
    // A bare drive root names no image at all.
    QVERIFY(!UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\")));
}

void AdvancedUninstallControllerTests::uninstallProgramPathTrusted_acceptsQualifiedLocalExe() {
    using sak::UninstallWorker;

    // The legitimate shape: a fully-qualified local .exe, separators either way, extension
    // case-insensitive, surrounding whitespace tolerated (the registry value is often padded).
    QVERIFY(UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:\\App\\unins000.exe")));
    QVERIFY(UninstallWorker::uninstallProgramPathTrusted(QStringLiteral("C:/App/unins000.exe")));
    QVERIFY(UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("D:\\Program Files\\Vendor App\\Uninstall.EXE")));
    QVERIFY(UninstallWorker::uninstallProgramPathTrusted(
        QStringLiteral("  C:\\Windows\\System32\\msiexec.exe  ")));
}

// R5-P7-9 / R5-P7-22: the MSI removal command this app BUILDS must itself name msiexec by
// its absolute System32 path -- emitting a bare "msiexec.exe" would both re-open the
// search-order hijack and be refused by the trust screen above, breaking MSI uninstalls.
void AdvancedUninstallControllerTests::silentMsiCommand_namesMsiexecByAbsoluteSystem32Path() {
    sak::ProgramInfo msi;
    msi.uninstallString = QStringLiteral("MsiExec.exe /X{12345678-90AB-CDEF-1234-567890ABCDEF}");

    QString cmd;
    QVERIFY(sak::UninstallWorker::buildSilentUninstallCommand(msi, cmd));
    QVERIFY2(!cmd.startsWith(QStringLiteral("msiexec.exe")), qPrintable(cmd));
    QVERIFY2(cmd.startsWith(QLatin1Char('"')), qPrintable(cmd));
    QVERIFY2(cmd.contains(QStringLiteral("\\System32\\msiexec.exe"), Qt::CaseInsensitive),
             qPrintable(cmd));
    // The silent switches and the product GUID must survive the requalification.
    QVERIFY2(cmd.contains(QStringLiteral("/qn")), qPrintable(cmd));
    QVERIFY2(cmd.contains(QStringLiteral("/norestart")), qPrintable(cmd));
    QVERIFY2(cmd.contains(QStringLiteral("{12345678-90AB-CDEF-1234-567890ABCDEF}")),
             qPrintable(cmd));

    // And the program token that command parses down to passes the trust screen, so the
    // hardened launch path accepts our own generated MSI command.
    const int end_quote = cmd.indexOf(QLatin1Char('"'), 1);
    QVERIFY(end_quote > 1);
    // The whole tail after the requalified quoted path is deterministic; the loose contains()
    // above would pass even if the switches were reordered or an extra argument were injected.
    QCOMPARE(cmd.mid(end_quote + 1),
             QStringLiteral(" /x {12345678-90AB-CDEF-1234-567890ABCDEF} /qn /norestart"));
    QVERIFY(sak::UninstallWorker::uninstallProgramPathTrusted(cmd.mid(1, end_quote - 1)));
}

QTEST_GUILESS_MAIN(AdvancedUninstallControllerTests)

#include "test_advanced_uninstall_controller.moc"
