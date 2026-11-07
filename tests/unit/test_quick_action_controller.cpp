// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_quick_action_controller.cpp
/// @brief The application's one quick-action controller: registration, per-run context (each
///        run's own output folder and owner), queueing, per-owner cancel, the elevated-helper
///        broker it owns, and teardown.

#include "sak/action_constants.h"
#include "sak/elevation_manager.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"
#include "sak/thread_custody.h"

#include "quick_action_fakes.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>
#include <memory>

using namespace sak;
using namespace quick_action_fakes;

namespace {

constexpr int kRunTimeoutMs = 10'000;

// ============================================================================
// Stub actions
// ============================================================================

class ControllerStubAction : public QuickAction {
public:
    explicit ControllerStubAction(const QString& name) : m_name(name) {}

    QString name() const override { return m_name; }
    QString description() const override { return m_name + " description"; }
    QIcon icon() const override { return {}; }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return false; }
    void execute() override {
        setStatus(ActionStatus::Running);
        ExecutionResult r;
        r.success = true;
        r.message = "Done";
        finishWithResult(r, ActionStatus::Success);
    }

private:
    QString m_name;
};

/// Facts a stubborn action reports about itself, shared so they outlive the action.
struct StubbornActionFacts {
    std::atomic<bool> may_finish{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> destroyed{false};
};

/// A run that cannot reach a cancellation point until the test lets it -- a long native call.
class StubbornAction : public ControllerStubAction {
public:
    explicit StubbornAction(std::shared_ptr<StubbornActionFacts> facts)
        : ControllerStubAction(QStringLiteral("Stubborn")), m_facts(std::move(facts)) {}
    ~StubbornAction() override { m_facts->destroyed.store(true, std::memory_order_release); }

    void execute() override {
        m_facts->entered.store(true, std::memory_order_release);
        while (!m_facts->may_finish.load(std::memory_order_acquire)) {
            QThread::msleep(kStubbornPollMs);
        }
        // Reached only by running to completion: a terminated thread never gets here.
        m_facts->completed.store(true, std::memory_order_release);
        ControllerStubAction::execute();
    }

    static constexpr unsigned long kStubbornPollMs = 10;

private:
    std::shared_ptr<StubbornActionFacts> m_facts;
};

/// Every runFinished the controller emits, in order.
struct FinishedRuns {
    QList<QuickActionRun> runs;
    QList<QuickAction::ExecutionResult> results;
    QList<QuickAction::ActionStatus> statuses;

    void watch(QuickActionController& controller) {
        QObject::connect(&controller,
                         &QuickActionController::runFinished,
                         &controller,
                         [this](const QuickActionRun& run,
                                const QuickAction::ExecutionResult& result,
                                QuickAction::ActionStatus status) {
                             runs << run;
                             results << result;
                             statuses << status;
                         });
    }
    [[nodiscard]] int count() const { return static_cast<int>(runs.size()); }
};

QString fileIn(const QTemporaryDir& dir, const QString& action_name) {
    return QDir(dir.path()).filePath(action_name + QStringLiteral(".txt"));
}

}  // namespace

// ============================================================================
// Test class
// ============================================================================

class TestQuickActionController : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    // -- Registration & lookup -------------------------------
    void testRegisterAndGet();
    void testGetUnknownReturnsNull();
    void testGetAllActions();
    void testNoDuplicateRegistration();
    void testElevatedTaskIdsAndPayloadUseHelperContract();
    void testBitLockerPayloadHasNoDefaultDestination();
    void testBitLockerPayloadCarriesAnAcceptedWarningOnlyWhenAsked();
    void testAcceptedWarningReachesThisRunAndNoOther();
    void testOutcomeTextCarriesTheWarning();
    void testElevatedResultCarriesItsWarningAndWhereItSaved();

    // -- Per-run context ----------------------------------------
    void queuedRunsKeepTheirOwnOutputFolders();
    void eachRunNamesItsOwnerAndCarriesItsOwnResult();
    void anActionsDetailReachesTheRunLog();
    void aRequestWhileAnotherRunsIsQueuedAndSaysSo();
    void refusedRequestsFinishAtOnce();
    void cancelRunsOfTouchesOnlyThatOwner();

    // -- The elevated helper's broker ---------------------------
    void elevatedRunsShareOneBrokerAndCarryTheirFolder();
    void elevatedRunRefusesAnUnsafeOutputFolder();
    void anInconclusiveElevatedRunIsNotCalledAFailure();
    void stopReachesOnlyTheOwnersElevatedTask();
    void destroyingTheControllerCancelsTheTaskAndShutsTheHelperDown();

    // -- Teardown --------------------------------------------
    void testDestroyDuringStuckRunRetainsActionInsteadOfBlocking();
    void completionLineCountsBytesOnlyWhenThereAreSome();

private:
    std::unique_ptr<QuickActionController> m_ctrl;
    std::shared_ptr<FakeSystem> m_system;
};

void TestQuickActionController::init() {
    m_system = std::make_shared<FakeSystem>();
    useFakeElevatedHelper(m_system);
    m_ctrl = std::make_unique<QuickActionController>();
}

void TestQuickActionController::cleanup() {
    m_system->hold.store(false);
    m_ctrl.reset();
    useRealElevatedHelper();
    m_system.reset();
}

// ============================================================================
// Registration & lookup
// ============================================================================

void TestQuickActionController::testRegisterAndGet() {
    const QString name =
        m_ctrl->registerAction(std::make_unique<ControllerStubAction>("TestAction1"));
    QCOMPARE(name, "TestAction1");
    QVERIFY(m_ctrl->getAction("TestAction1") != nullptr);
    QCOMPARE(m_ctrl->getAction("TestAction1")->name(), "TestAction1");
}

void TestQuickActionController::testGetUnknownReturnsNull() {
    QVERIFY(m_ctrl->getAction("NoSuchAction") == nullptr);
}

void TestQuickActionController::testGetAllActions() {
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("A1"));
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("A2"));
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("A3"));
    QCOMPARE(static_cast<int>(m_ctrl->getAllActions().size()), 3);
}

void TestQuickActionController::testNoDuplicateRegistration() {
    m_ctrl->registerAction(std::make_unique<ControllerStubAction>("Dup"));
    // A second action with a duplicate name is REJECTED (fail closed): registerAction stores
    // nothing and returns an empty id, so the first "Dup" survives and stays routable.
    QCOMPARE(m_ctrl->registerAction(std::make_unique<ControllerStubAction>("Dup")), QString());
    QCOMPARE(static_cast<int>(m_ctrl->getAllActions().size()), 1);
    QCOMPARE(m_ctrl->getAction("Dup")->name(), QStringLiteral("Dup"));
}

void TestQuickActionController::testElevatedTaskIdsAndPayloadUseHelperContract() {
    using namespace quick_action_elevation;
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kResetNetworkName)),
             QString::fromLatin1(kResetNetworkTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kBackupBitLockerKeysName)),
             QString::fromLatin1(kBackupBitLockerKeysTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kVerifySystemFilesName)),
             QString::fromLatin1(kVerifySystemFilesTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QString::fromLatin1(kCheckDiskErrorsName)),
             QString::fromLatin1(kCheckDiskErrorsTaskId));
    QCOMPARE(elevatedTaskIdForQuickAction(QStringLiteral("Unknown Admin Action")), QString());

    const QJsonObject payload = buildElevatedActionPayload(
        elevatedTaskIdForQuickAction(QString::fromLatin1(kBackupBitLockerKeysName)),
        QStringLiteral("C:/session/artifacts"));
    QCOMPARE(payload.value(QStringLiteral("backup_location")).toString(),
             QStringLiteral("C:/session/artifacts"));
}

void TestQuickActionController::testBitLockerPayloadHasNoDefaultDestination() {
    const QJsonObject payload = buildElevatedActionPayload(
        QString::fromLatin1(quick_action_elevation::kBackupBitLockerKeysTaskId), QString());
    QVERIFY(payload.isEmpty());
}

void TestQuickActionController::testBitLockerPayloadCarriesAnAcceptedWarningOnlyWhenAsked() {
    const QString task = QString::fromLatin1(quick_action_elevation::kBackupBitLockerKeysTaskId);
    const QString key =
        QString::fromLatin1(quick_action_elevation::kOutputFolderWarningAcceptedKey);
    const QString folder = QStringLiteral("E:/keys");
    QVERIFY(!buildElevatedActionPayload(task, folder).contains(key));
    QVERIFY(!buildElevatedActionPayload(task, folder, false).contains(key));
    QCOMPARE(buildElevatedActionPayload(task, folder, true).value(key).toBool(false), true);
    // No other helper task has an output folder, so none has a warning either.
    QVERIFY(buildElevatedActionPayload(
                QString::fromLatin1(quick_action_elevation::kCheckDiskErrorsTaskId), folder, true)
                .isEmpty());
}

// One caller's answer belongs to its own run. The action object is shared, so an answer that stuck
// to it would let the next caller, who asked no one, write keys to an unprotected drive.
void TestQuickActionController::testAcceptedWarningReachesThisRunAndNoOther() {
    QTemporaryDir keys;
    QVERIFY(keys.isValid());
    QVERIFY(registerFakes(*m_ctrl, m_system, {kBitLocker}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);
    const QString key =
        QString::fromLatin1(quick_action_elevation::kOutputFolderWarningAcceptedKey);

    m_ctrl->requestRun({kBitLocker, QStringLiteral("backup_restore"), keys.path(), true});
    m_ctrl->requestRun({kBitLocker, QStringLiteral("assistant#1"), keys.path()});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kRunTimeoutMs);

    const QList<QJsonObject> payloads = m_system->elevatedPayloads();
    QCOMPARE(payloads.size(), 2);
    QCOMPARE(payloads.at(0).value(key).toBool(false), true);
    QVERIFY(!payloads.at(1).contains(key));
    QCOMPARE(m_ctrl->getAction(kBitLocker)->outputFolderWarningAccepted(), false);
}

void TestQuickActionController::testOutcomeTextCarriesTheWarning() {
    QuickAction::ExecutionResult result;
    result.success = true;
    result.output_path = QStringLiteral("E:/keys/run");
    const QuickActionRun run{kBitLocker, QStringLiteral("backup_restore"), {}};
    const QString plain = quickActionOutcomeText(run, result, QuickAction::ActionStatus::Success);
    QCOMPARE(plain, QStringLiteral("Completed: %1 - saved to E:\\keys\\run").arg(kBitLocker));

    result.warning = QStringLiteral("The keys are NOT protected");
    QCOMPARE(quickActionOutcomeText(run, result, QuickAction::ActionStatus::Success),
             plain + QStringLiteral(" - The keys are NOT protected"));
}

// The helper reports over its pipe, so what the panel shows has to survive the trip: where to find
// the keys, and that they are not protected.
void TestQuickActionController::testElevatedResultCarriesItsWarningAndWhereItSaved() {
    QTemporaryDir keys;
    QVERIFY(keys.isValid());
    QVERIFY(registerFakes(*m_ctrl, m_system, {kBitLocker}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);
    sak::ElevatedTaskResult helper;
    helper.success = true;
    helper.data = QJsonObject{{QStringLiteral("message"), QStringLiteral("saved")},
                              {QStringLiteral("warning"), QStringLiteral("not protected")},
                              {QStringLiteral("output_path"), QStringLiteral("E:/keys/run")}};
    m_system->elevated_result = helper;

    m_ctrl->requestRun({kBitLocker, QStringLiteral("backup_restore"), keys.path(), true});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kRunTimeoutMs);
    QCOMPARE(finished.results.at(0).warning, QStringLiteral("not protected"));
    QCOMPARE(finished.results.at(0).output_path, QStringLiteral("E:/keys/run"));
}

// ============================================================================
// Per-run context
// ============================================================================

// The bug a shared controller had to fix first: the output folder was set on the shared action
// object when a caller asked, so a run queued behind another caller's wrote into whichever
// folder was set last. Each run now carries its own folder, applied as the run starts.
void TestQuickActionController::queuedRunsKeepTheirOwnOutputFolders() {
    QTemporaryDir panel_folder;
    QTemporaryDir assistant_folder;
    QVERIFY(panel_folder.isValid() && assistant_folder.isValid());
    QVERIFY(registerFakes(*m_ctrl, m_system, {kOptimizePower, kInventoryReport}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_system->hold.store(true);
    m_ctrl->requestRun({kOptimizePower, QStringLiteral("network"), {}});
    m_ctrl->requestRun({kInventoryReport, QStringLiteral("diagnostics"), panel_folder.path()});
    m_ctrl->requestRun({kInventoryReport, QStringLiteral("assistant#1"), assistant_folder.path()});
    m_system->hold.store(false);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, kRunTimeoutMs);

    QCOMPARE(m_system->inProcessRuns(),
             (QStringList{kOptimizePower + QLatin1Char('|'),
                          kInventoryReport + QLatin1Char('|') + panel_folder.path(),
                          kInventoryReport + QLatin1Char('|') + assistant_folder.path()}));
    QVERIFY(QFile::exists(fileIn(panel_folder, kInventoryReport)));
    QVERIFY(QFile::exists(fileIn(assistant_folder, kInventoryReport)));
    QCOMPARE(finished.results.at(1).output_path, fileIn(panel_folder, kInventoryReport));
    QCOMPARE(finished.results.at(2).output_path, fileIn(assistant_folder, kInventoryReport));
}

namespace {
class DetailedAction : public ControllerStubAction {
public:
    DetailedAction() : ControllerStubAction(QStringLiteral("Detailed")) {}
    void execute() override {
        setStatus(ActionStatus::Running);
        ExecutionResult r;
        r.success = true;
        r.message = "Done";
        r.log = "Drive C: No errors found\nDrive D: Errors found";
        finishWithResult(r, ActionStatus::Success);
    }
};
}  // namespace

// The summary only says how many; which drive is in the detail. Every panel shows runLog in its
// Log, and nothing else ever displayed result.log.
void TestQuickActionController::anActionsDetailReachesTheRunLog() {
    m_ctrl->registerAction(std::make_unique<DetailedAction>());
    QSignalSpy logged(m_ctrl.get(), &QuickActionController::runLog);
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_ctrl->requestRun({QStringLiteral("Detailed"), QStringLiteral("diagnostics"), {}});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kRunTimeoutMs);

    QString lines;
    for (const auto& entry : logged) {
        const QString line = entry.at(1).toString();
        // The panel's Log stamps every line with its time; one with its own ISO stamp read
        // "[06:51:00] [2026-11-05T06:51:00] Optimize Power Settings: ..." (review 2).
        QVERIFY2(line.startsWith(QStringLiteral("Detailed: ")), qPrintable(line));
        lines += line + QLatin1Char('\n');
    }
    QVERIFY2(lines.contains(QStringLiteral("Drive D: Errors found")), qPrintable(lines));
}

void TestQuickActionController::eachRunNamesItsOwnerAndCarriesItsOwnResult() {
    QVERIFY(registerFakes(*m_ctrl, m_system, {kOptimizePower}));
    QSignalSpy started(m_ctrl.get(), &QuickActionController::runStarted);
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_ctrl->requestRun({kOptimizePower, QStringLiteral("diagnostics"), {}});
    m_ctrl->requestRun({kOptimizePower, QStringLiteral("assistant#7"), {}});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kRunTimeoutMs);

    QCOMPARE(started.count(), 2);
    QCOMPARE(finished.runs.at(0).owner, QStringLiteral("diagnostics"));
    QCOMPARE(finished.runs.at(1).owner, QStringLiteral("assistant#7"));
    for (int i = 0; i < 2; ++i) {
        QVERIFY(finished.results.at(i).success);
        QCOMPARE(finished.results.at(i).message, QStringLiteral("fake ok"));
        QCOMPARE(finished.statuses.at(i), QuickAction::ActionStatus::Success);
    }
}

void TestQuickActionController::aRequestWhileAnotherRunsIsQueuedAndSaysSo() {
    QVERIFY(registerFakes(*m_ctrl, m_system, {kOptimizePower, kInventoryReport}));
    QSignalSpy queued(m_ctrl.get(), &QuickActionController::runQueued);
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_system->hold.store(true);
    m_ctrl->requestRun({kOptimizePower, QStringLiteral("network"), {}});
    QVERIFY(queued.isEmpty());
    m_ctrl->requestRun({kInventoryReport, QStringLiteral("diagnostics"), {}});
    QCOMPARE(queued.count(), 1);
    QCOMPARE(queued.at(0).at(0).value<QuickActionRun>().owner, QStringLiteral("diagnostics"));
    QCOMPARE(queued.at(0).at(1).toString(), kOptimizePower);
    QVERIFY(m_ctrl->hasRunsOf(QStringLiteral("diagnostics")));
    QCOMPARE(quickActionQueuedText(queued.at(0).at(0).value<QuickActionRun>(), kOptimizePower),
             QStringLiteral("Queued: System Inventory Report - waits for Optimize Power Settings "
                            "to finish"));

    m_system->hold.store(false);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kRunTimeoutMs);
    QVERIFY(!m_ctrl->hasRunsOf(QStringLiteral("diagnostics")));
    QVERIFY(!m_ctrl->hasRunsOf(QStringLiteral("network")));
}

// Every request ends in exactly one runFinished -- a refused one at once -- so a caller waiting
// on its own run is never left waiting.
void TestQuickActionController::refusedRequestsFinishAtOnce() {
    QVERIFY(registerFakes(*m_ctrl, m_system, {kOptimizePower}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_ctrl->requestRun({QStringLiteral("No Such Action"), QStringLiteral("diagnostics"), {}});
    m_ctrl->requestRun({kOptimizePower, QString(), {}});
    QCOMPARE(finished.count(), 2);
    QVERIFY(!finished.results.at(0).success);
    QCOMPARE(finished.results.at(0).message,
             QStringLiteral("No quick action is registered as \"No Such Action\""));
    QVERIFY(!finished.results.at(1).success);
    QCOMPARE(finished.results.at(1).message,
             QStringLiteral("Refused a quick-action request with no owner"));
    QVERIFY(m_system->inProcessRuns().isEmpty());
}

void TestQuickActionController::cancelRunsOfTouchesOnlyThatOwner() {
    QVERIFY(registerFakes(*m_ctrl, m_system, {kOptimizePower, kInventoryReport}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_system->hold.store(true);
    m_ctrl->requestRun({kOptimizePower, QStringLiteral("network"), {}});
    m_ctrl->requestRun({kInventoryReport, QStringLiteral("diagnostics"), {}});
    m_ctrl->requestRun({kInventoryReport, QStringLiteral("network"), {}});

    // The diagnostics run is queued: it never starts, and finishes as cancelled at once. The
    // network runs -- the running one and the queued one -- are untouched.
    m_ctrl->cancelRunsOf(QStringLiteral("diagnostics"));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.runs.at(0).owner, QStringLiteral("diagnostics"));
    QCOMPARE(finished.statuses.at(0), QuickAction::ActionStatus::Cancelled);
    QVERIFY(m_ctrl->hasRunsOf(QStringLiteral("network")));

    // The network owner's cancel stops its running run and drops its queued one.
    m_ctrl->cancelRunsOf(QStringLiteral("network"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 3, kRunTimeoutMs);
    QVERIFY(!m_ctrl->hasRunsOf(QStringLiteral("network")));
    QCOMPARE(finished.statuses.at(1), QuickAction::ActionStatus::Cancelled);
    QCOMPARE(finished.statuses.at(2), QuickAction::ActionStatus::Cancelled);
    QVERIFY(m_system->inProcessRuns().isEmpty());  // nothing ran to its end
}

// ============================================================================
// The elevated helper's broker
// ============================================================================

void TestQuickActionController::elevatedRunsShareOneBrokerAndCarryTheirFolder() {
    if (ElevationManager::isElevated()) {
        QSKIP("Run unelevated: administrator actions then take the elevated-helper route");
    }
    QTemporaryDir keys;
    QVERIFY(keys.isValid());
    QVERIFY(registerFakes(*m_ctrl, m_system, {kCheckDisk, kBitLocker}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);
    QCOMPARE(m_system->brokers_created.load(), 0);  // nothing elevated has run yet

    m_ctrl->requestRun({kCheckDisk, QStringLiteral("diagnostics"), {}});
    m_ctrl->requestRun({kBitLocker, QStringLiteral("backup_restore"), keys.path()});
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kRunTimeoutMs);

    QVERIFY(finished.results.at(0).success && finished.results.at(1).success);
    QCOMPARE(
        m_system->elevatedTasks(),
        (QStringList{QString::fromLatin1(quick_action_elevation::kCheckDiskErrorsTaskId),
                     QString::fromLatin1(quick_action_elevation::kBackupBitLockerKeysTaskId)}));
    QCOMPARE(m_system->elevatedPayloads().at(1).value(QStringLiteral("backup_location")).toString(),
             keys.path());
    QCOMPARE(m_system->brokers_created.load(), 1);  // one broker for every elevated run
    QCOMPARE(m_system->brokers_destroyed.load(), 0);
}

// SFC on non-English Windows ends this way. The app could not read the result, which says nothing
// about the PC, so neither the Log nor the status line may say it failed.
void TestQuickActionController::anInconclusiveElevatedRunIsNotCalledAFailure() {
    if (ElevationManager::isElevated()) {
        QSKIP("Run unelevated: administrator actions then take the elevated-helper route");
    }
    QVERIFY(registerFakes(*m_ctrl, m_system, {kCheckDisk}));
    ElevatedTaskResult helper_result;
    helper_result.success = false;
    helper_result.data =
        QJsonObject{{QStringLiteral("message"), QStringLiteral("could not read it")},
                    {QStringLiteral("inconclusive"), true}};
    m_system->elevated_result = helper_result;
    QSignalSpy logged(m_ctrl.get(), &QuickActionController::runLog);
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    const QuickActionRun run{kCheckDisk, QStringLiteral("diagnostics"), {}};
    m_ctrl->requestRun(run);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kRunTimeoutMs);

    const QuickAction::ExecutionResult& result = finished.results.at(0);
    QVERIFY(!result.success);
    QVERIFY(result.inconclusive);
    QCOMPARE(quickActionOutcomeText(run, result, QuickAction::ActionStatus::Failed),
             QStringLiteral("Result unknown: ") + kCheckDisk +
                 QStringLiteral(" - could not read it"));
    QStringList lines;
    for (const auto& entry : logged) {
        lines << entry.at(1).toString();
    }
    const QString all = lines.join(QLatin1Char('\n'));
    QVERIFY2(all.contains(QStringLiteral("result unknown: could not read it")), qPrintable(all));
    QVERIFY2(!all.contains(QStringLiteral("Execution failed")), qPrintable(all));

    // The same flag without the rest of the story is not enough to soften a real failure.
    helper_result.data = QJsonObject{{QStringLiteral("message"), QStringLiteral("broken")}};
    m_system->elevated_result = helper_result;
    m_ctrl->requestRun(run);
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, kRunTimeoutMs);
    QVERIFY(!finished.results.at(1).inconclusive);
    QVERIFY(quickActionOutcomeText(run, finished.results.at(1), QuickAction::ActionStatus::Failed)
                .startsWith(QStringLiteral("Failed: ")));
}

void TestQuickActionController::elevatedRunRefusesAnUnsafeOutputFolder() {
    if (ElevationManager::isElevated()) {
        QSKIP("Run unelevated: administrator actions then take the elevated-helper route");
    }
    QVERIFY(registerFakes(*m_ctrl, m_system, {kBitLocker}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_ctrl->requestRun({kBitLocker, QStringLiteral("backup_restore"), QStringLiteral("keys")});
    QCOMPARE(finished.count(), 1);
    QVERIFY(!finished.results.at(0).success);
    QVERIFY2(finished.results.at(0).message.startsWith(
                 QStringLiteral("Refused output folder (needs an absolute local path")),
             qPrintable(finished.results.at(0).message));
    QVERIFY(m_system->elevatedTasks().isEmpty());
}

void TestQuickActionController::stopReachesOnlyTheOwnersElevatedTask() {
    if (ElevationManager::isElevated()) {
        QSKIP("Run unelevated: administrator actions then take the elevated-helper route");
    }
    QVERIFY(registerFakes(*m_ctrl, m_system, {kResetNetwork}));
    FinishedRuns finished;
    finished.watch(*m_ctrl);

    m_system->hold.store(true);
    m_ctrl->requestRun({kResetNetwork, QStringLiteral("network"), {}});
    QTRY_COMPARE_WITH_TIMEOUT(m_system->elevatedTasks().size(), 1, kRunTimeoutMs);

    m_ctrl->cancelRunsOf(QStringLiteral("diagnostics"));  // another caller's Stop
    QTest::qWait(50);
    QCOMPARE(m_system->helper_cancels.load(), 0);
    QCOMPARE(finished.count(), 0);

    m_ctrl->cancelRunsOf(QStringLiteral("network"));
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kRunTimeoutMs);
    QCOMPARE(m_system->helper_cancels.load(), 1);
    QCOMPARE(finished.statuses.at(0), QuickAction::ActionStatus::Cancelled);
    QCOMPARE(finished.results.at(0).message, QStringLiteral("Cancelled by user"));
}

// Application exit destroys the controller: the running elevated task is cancelled (so the
// bounded join does not wait out the helper), and the broker -- the helper's one owner -- is
// destroyed with it, which shuts the helper down.
void TestQuickActionController::destroyingTheControllerCancelsTheTaskAndShutsTheHelperDown() {
    if (ElevationManager::isElevated()) {
        QSKIP("Run unelevated: administrator actions then take the elevated-helper route");
    }
    QVERIFY(registerFakes(*m_ctrl, m_system, {kVerifySystemFiles}));
    m_system->hold.store(true);
    m_ctrl->requestRun({kVerifySystemFiles, QStringLiteral("diagnostics"), {}});
    QTRY_COMPARE_WITH_TIMEOUT(m_system->elevatedTasks().size(), 1, kRunTimeoutMs);

    QElapsedTimer teardown;
    teardown.start();
    m_ctrl.reset();
    QVERIFY2(teardown.elapsed() < kRunTimeoutMs, "teardown waited out the held task");
    QCOMPARE(m_system->helper_cancels.load(), 1);
    QCOMPARE(m_system->brokers_created.load(), 1);
    QCOMPARE(m_system->brokers_destroyed.load(), 1);
}

// ============================================================================
// Teardown
// ============================================================================

// Destroying the controller while a run's thread is stuck used to block the GUI thread with an
// unbounded wait. It must give the thread its bounded join, then report it and retain the thread
// AND the action it runs -- which the controller would otherwise free underneath it -- until the
// run finishes on its own.
void TestQuickActionController::testDestroyDuringStuckRunRetainsActionInsteadOfBlocking() {
    constexpr int kStartTimeoutMs = 5000;
    constexpr qint64 kTeardownBoundMs = 20'000;  // controller's 10 s join plus slack
    constexpr int kReapTimeoutMs = 5000;
    auto facts = std::make_shared<StubbornActionFacts>();
    QVERIFY(!m_ctrl->registerAction(std::make_unique<StubbornAction>(facts)).isEmpty());
    m_ctrl->requestRun({QStringLiteral("Stubborn"), QStringLiteral("diagnostics"), {}});
    QTRY_VERIFY_WITH_TIMEOUT(facts->entered.load(std::memory_order_acquire), kStartTimeoutMs);
    const int custody_before = ThreadCustody::retainedCount();

    QElapsedTimer timer;
    timer.start();
    m_ctrl.reset();
    QVERIFY2(timer.elapsed() < kTeardownBoundMs, "controller teardown waited without bound");
    QVERIFY2(!facts->destroyed.load(std::memory_order_acquire),
             "the running action was freed underneath its thread");
    QCOMPARE(ThreadCustody::retainedCount(), custody_before + 1);
    QVERIFY(ThreadCustody::retainedReasons().constLast().startsWith(
        QStringLiteral("QuickAction thread for 'Stubborn' still running")));

    facts->may_finish.store(true, std::memory_order_release);
    QTRY_VERIFY_WITH_TIMEOUT(facts->destroyed.load(std::memory_order_acquire), kReapTimeoutMs);
    QVERIFY(facts->completed.load(std::memory_order_acquire));
    QCOMPARE(ThreadCustody::retainedCount(), custody_before);
}

// Review 3: Check Disk Errors and Optimize Power ended "(0 bytes in 169s)" / "(0 bytes in 5s)".
void TestQuickActionController::completionLineCountsBytesOnlyWhenThereAreSome() {
    sak::QuickAction::ExecutionResult none;
    none.message = QStringLiteral("Checked 2 drives");
    none.duration_ms = 169'400;
    QCOMPARE(sak::QuickActionController::completionLine(none),
             QStringLiteral("Execution complete: Checked 2 drives (169s)"));
    sak::QuickAction::ExecutionResult some = none;
    some.bytes_processed = 4096;
    QCOMPARE(sak::QuickActionController::completionLine(some),
             QStringLiteral("Execution complete: Checked 2 drives (4096 bytes in 169s)"));
}

QTEST_MAIN(TestQuickActionController)
#include "test_quick_action_controller.moc"
