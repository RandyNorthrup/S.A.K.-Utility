// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_controller.cpp
/// @brief Partition Manager controller implementation.

#include "sak/partition_manager_controller.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/partition_report_generator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QtConcurrent>
#include <QThread>

namespace sak {

namespace {
std::shared_ptr<PartitionExecutor> makeAsyncExecutor() {
    return std::shared_ptr<PartitionExecutor>(
        new PartitionExecutor(), [](PartitionExecutor* executor) {
            if (!executor) {
                return;
            }
            auto* target_thread = executor->thread();
            if (target_thread && target_thread != QThread::currentThread() &&
                target_thread->isRunning()) {
                QMetaObject::invokeMethod(executor, &QObject::deleteLater, Qt::QueuedConnection);
                return;
            }
            delete executor;
        });
}

QString runtimeFilesystemManifestPath() {
    // Root of trust for the elevated filesystem tools: resolve only from the trusted
    // application directory, never a CWD-relative manifest an attacker could plant.
    return PartitionFileSystemToolManifest::resolveRuntimeManifestPath();
}

// Cancel-then-JOIN a future watcher and delete it, instead of detaching it. The
// caller must have already issued any cooperative cancel so waitForFinished()
// returns promptly. Disconnecting before the wait keeps the finished slot from
// firing on a half-destroyed owner; waitForFinished() does not spin the event
// loop, so no re-entrancy.
template <typename ResultT>
void cancelJoinDeleteWatcher(QFutureWatcher<ResultT>*& watcher, QObject* owner) {
    if (!watcher) {
        return;
    }
    QObject::disconnect(watcher, nullptr, owner, nullptr);
    // SAK-ALLOW-BLOCKING: `delete watcher` follows on the next line, so abandoning the wait
    // would free the watcher underneath a running task. The caller has already issued the
    // cooperative cancel this relies on (see the contract above).
    watcher->waitForFinished();
    delete watcher;
    watcher = nullptr;
}
}  // namespace

PartitionManagerController::PartitionManagerController(QObject* parent) : QObject(parent) {}

PartitionManagerController::~PartitionManagerController() {
    // JOIN the outstanding apply/check futures instead of detaching them, so
    // destructive disk work (elevated diskpart/format) cannot keep mutating the
    // disk after the controller and its panel are gone. cancel() first so the
    // in-flight op / elevated broker is interrupted and the join returns promptly.
    if (m_apply_executor) {
        m_apply_executor->cancel();
    }
    cancelJoinDeleteWatcher(m_apply_watcher, this);
    // The apply run lambda has released its executor shared_ptr copy by now, so
    // the executor is destroyed here on the GUI thread.
    m_apply_executor.reset();
    cancelJoinDeleteWatcher(m_file_system_check_watcher, this);
}

const PartitionInventory& PartitionManagerController::inventory() const noexcept {
    return m_inventory;
}

const PartitionOperationQueue& PartitionManagerController::queue() const noexcept {
    return m_queue;
}

PartitionManagerState PartitionManagerController::state() const noexcept {
    return m_state;
}

PartitionManagerState PartitionManagerController::inventoryReadyState(
    const PartitionInventory& inventory) {
    return inventory.hasPartitionEnumerationFailure() ? PartitionManagerState::ReadyPartial
                                                      : PartitionManagerState::Ready;
}

QString PartitionManagerController::inventoryStatusMessage(const PartitionInventory& inventory) {
    QStringList failed;
    for (const auto& disk : inventory.disks) {
        if (disk.partition_enumeration_failed) {
            failed.append(QString::number(disk.disk_number));
        }
    }
    if (failed.isEmpty()) {
        return QStringLiteral("Partition Manager: inventory ready");
    }
    return QStringLiteral(
               "Partition Manager: inventory INCOMPLETE -- partition enumeration "
               "failed for disk(s) %1; operations on them are refused until a "
               "refresh succeeds")
        .arg(failed.join(QStringLiteral(", ")));
}

void PartitionManagerController::refreshInventory() {
    if (m_state == PartitionManagerState::RefreshingInventory) {
        Q_EMIT statusMessage(QStringLiteral("Partition Manager: inventory refresh already running"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    setState(PartitionManagerState::RefreshingInventory);
    Q_EMIT statusMessage(QStringLiteral("Partition Manager: refreshing storage inventory"),
                         sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(0, kPercentMax);

    auto* watcher = new QFutureWatcher<PartitionInventory>(this);
    connect(watcher, &QFutureWatcher<PartitionInventory>::finished, this, [this, watcher]() {
        m_inventory = watcher->result();
        logInfo("Partition Manager inventory scan returned {} disk(s), {} warning(s)",
                m_inventory.disks.size(),
                m_inventory.warnings.size());
        for (const auto& warning : m_inventory.warnings) {
            logWarning("Partition Manager inventory warning: {}", warning.toStdString());
            // The scan's blockers belong in front of the USER, not only in the app log.
            Q_EMIT logOutput(warning);
        }
        m_queue.setBaseLayoutHash(m_inventory.layout_hash);
        // Ready only when the inventory is authoritative; a disk whose partition enumeration
        // failed downgrades the reported state instead of passing partial data off as complete.
        setState(inventoryReadyState(m_inventory));
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(inventoryStatusMessage(m_inventory), sak::kTimerStatusDefaultMs);
        watcher->deleteLater();
    });
    watcher->setFuture(
        QtConcurrent::run([]() { return StorageInventoryWorker::scanCurrentSystem(); }));
}

void PartitionManagerController::queueOperation(PartitionOperationType type,
                                                const PartitionTarget& target,
                                                const QJsonObject& payload) {
    setState(PartitionManagerState::PlanningOperation);
    auto operation = PartitionOperationPlanner::makeOperation(type, target, payload);
    auto preview = m_planner.previewOperation(m_inventory, operation);
    m_queue.addPreview(preview);
    setState(PartitionManagerState::QueueDirty);
    emitQueueChanged();
    const QString msg = preview.canApply()
                            ? QStringLiteral("Partition operation queued")
                            : QStringLiteral("Partition operation queued with blockers");
    Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
}

void PartitionManagerController::undo() {
    if (m_queue.undo()) {
        emitQueueChanged();
    }
}

void PartitionManagerController::redo() {
    if (m_queue.redo()) {
        emitQueueChanged();
    }
}

void PartitionManagerController::discardQueue() {
    m_queue.discard();
    emitQueueChanged();
    // Emptying the queue must not silently promote a partial inventory back to a clean Ready.
    setState(inventoryReadyState(m_inventory));
}

void PartitionManagerController::applyQueue(bool dry_run, bool use_elevation) {
    if (applyIsRunning()) {
        Q_EMIT statusMessage(QStringLiteral("Partition operation already running"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    if (!revalidateBeforeApply(dry_run)) {
        return;
    }

    setState(use_elevation && !dry_run ? PartitionManagerState::AwaitingElevation
                                       : PartitionManagerState::Applying);
    m_apply_before_inventory = m_inventory;
    m_apply_dry_run = dry_run;
    const auto operations = m_queue.operations();
    const bool shouldUseElevation = use_elevation && !dry_run;
    auto executor = makeAsyncExecutor();
    connect(executor.get(),
            &PartitionExecutor::progressUpdated,
            this,
            [this](int percent, const QString& status) {
                Q_EMIT progressUpdate(percent, kPercentMax);
                Q_EMIT statusMessage(status, sak::kTimerStatusDefaultMs);
            });
    connect(executor.get(),
            &PartitionExecutor::logMessage,
            this,
            &PartitionManagerController::logOutput);
    m_apply_executor = executor;
    auto* watcher = new QFutureWatcher<PartitionExecutionResult>(this);
    m_apply_watcher = watcher;
    connect(watcher, &QFutureWatcher<PartitionExecutionResult>::finished, this, [this, watcher]() {
        finishApplyQueue(watcher);
    });
    watcher->setFuture(QtConcurrent::run([executor, operations, dry_run, shouldUseElevation]() {
        return executor->execute(operations, dry_run, shouldUseElevation);
    }));
}

bool PartitionManagerController::revalidateBeforeApply(bool dry_run) {
    // A real (non-dry-run) apply re-scans live storage right before dispatching.
    // The queue was planned against a cached inventory, so a disk hot-swap since
    // the last manual refresh (same disk number, different serial) would let the
    // batch run against the wrong physical disk. The queue's planned base hash is
    // compared against a fresh live hash (which folds in every disk serial), so any
    // change fails closed instead of wiping the replacement disk.
    const PartitionInventory live = dry_run ? m_inventory
                                            : StorageInventoryWorker::scanCurrentSystem();
    if (m_queue.canApply(live.layout_hash)) {
        if (!dry_run) {
            m_inventory = live;
        }
        return true;
    }
    if (!dry_run && live.layout_hash != m_inventory.layout_hash) {
        m_inventory = live;
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT statusMessage(
            QStringLiteral("Storage changed since planning; refresh and re-review before applying"),
            sak::kTimerStatusDefaultMs);
        return false;
    }
    Q_EMIT statusMessage(QStringLiteral("Partition queue cannot apply: refresh or fix blockers"),
                         sak::kTimerStatusDefaultMs);
    return false;
}

void PartitionManagerController::finishApplyQueue(
    QFutureWatcher<PartitionExecutionResult>* watcher) {
    if (watcher == nullptr) {
        return;
    }
    auto result = watcher->result();
    setState(PartitionManagerState::Verifying);
    if (!result.dry_run) {
        // Re-scan after ANY real apply, cancelled or not. The executor observes cancellation only
        // between steps and has no rollback, so a cancelled batch may have already destroyed
        // partitions; restoring the pre-apply snapshot here would fabricate an "unchanged"
        // before/after in the report and leave the panel showing partitions that no longer exist.
        m_inventory = StorageInventoryWorker::scanCurrentSystem();
    } else {
        m_inventory = m_apply_before_inventory;
    }

    const PartitionReportGenerator report_generator;
    result.report_html =
        report_generator.generateHtml(m_apply_before_inventory, m_inventory, result);
    result.report_json =
        report_generator.generateJson(m_apply_before_inventory, m_inventory, result);
    if (result.success && !m_apply_dry_run) {
        m_queue.discard();
    }
    // The post-apply re-scan is held to the same honesty bar: a disk that stopped enumerating
    // during the apply reports ReadyPartial, never a clean Ready.
    setState(result.cancelled ? PartitionManagerState::Cancelled
                              : (result.success ? inventoryReadyState(m_inventory)
                                                : PartitionManagerState::Failed));
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
    Q_EMIT executionFinished(result);
    if (m_apply_watcher == watcher) {
        m_apply_watcher = nullptr;
    }
    m_apply_executor.reset();
    watcher->deleteLater();
}

void PartitionManagerController::cancel() {
    if (!applyIsRunning()) {
        Q_EMIT statusMessage(QStringLiteral("No partition operation is running"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    if (m_apply_executor) {
        m_apply_executor->cancel();
    }
    setState(PartitionManagerState::Cancelled);
    Q_EMIT statusMessage(QStringLiteral("Partition operation cancellation requested"),
                         sak::kTimerStatusDefaultMs);
}

void PartitionManagerController::runReadOnlyFileSystemCheck(const QString& file_system,
                                                            const QString& target_path) {
    // Gate on the watcher's lifetime, not isFinished() (which flips true on the worker thread
    // before the queued finished slot nulls the pointer on the GUI thread) so a second check
    // cannot dispatch in that gap. m_file_system_check_watcher is cleared only after cleanup.
    if (m_file_system_check_watcher != nullptr) {
        Q_EMIT statusMessage(QStringLiteral("Filesystem check already running"),
                             sak::kTimerStatusDefaultMs);
        return;
    }

    const QString manifestPath = runtimeFilesystemManifestPath();
    const QString toolsRoot = QFileInfo(manifestPath).absolutePath();
    const PartitionFileSystemReadOnlyCheckRequest request{.manifest_path = manifestPath,
                                                          .tools_root = toolsRoot,
                                                          .file_system = file_system,
                                                          .target_path = target_path,
                                                          .timeout_ms = 0};
    Q_EMIT statusMessage(QStringLiteral("Running read-only %1 filesystem check").arg(file_system),
                         0);
    auto* watcher = new QFutureWatcher<PartitionFileSystemToolRunResult>(this);
    m_file_system_check_watcher = watcher;
    connect(watcher,
            &QFutureWatcher<PartitionFileSystemToolRunResult>::finished,
            this,
            [this, watcher]() {
                const auto result = watcher->result();
                if (!result.std_out.trimmed().isEmpty()) {
                    Q_EMIT logOutput(result.std_out.trimmed());
                }
                if (!result.std_err.trimmed().isEmpty()) {
                    Q_EMIT logOutput(result.std_err.trimmed());
                }
                if (result.blocked) {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check blocked: %1")
                                             .arg(result.blockers.join(QStringLiteral("; "))),
                                         sak::kTimerStatusDefaultMs);
                } else if (result.success) {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check complete"),
                                         sak::kTimerStatusDefaultMs);
                } else {
                    Q_EMIT statusMessage(QStringLiteral("Filesystem check failed with exit code %1")
                                             .arg(result.exit_code),
                                         sak::kTimerStatusDefaultMs);
                }
                Q_EMIT fileSystemCheckFinished(result);
                if (m_file_system_check_watcher == watcher) {
                    m_file_system_check_watcher = nullptr;
                }
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(
        [request]() { return PartitionFileSystemToolRunner::runReadOnlyCheck(request); }));
}

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
void PartitionManagerController::setTestInventory(const PartitionInventory& inventory) {
    m_inventory = inventory;
    m_queue.setBaseLayoutHash(m_inventory.layout_hash);
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
}

void PartitionManagerController::setTestState(PartitionManagerState state) {
    setState(state);
}
#endif

void PartitionManagerController::setState(PartitionManagerState state) {
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged(state);
}

void PartitionManagerController::emitQueueChanged() {
    Q_EMIT queueChanged(m_queue.operations());
}

bool PartitionManagerController::applyIsRunning() const {
    // Gate on the watcher's LIFETIME, not QFuture::isFinished(). isFinished() flips true on
    // the worker thread the instant execute() returns, but finishApplyQueue -- which nulls
    // m_apply_watcher and resets m_apply_executor -- runs later on the GUI thread via the
    // queued finished signal. Using isFinished() would report "not running" in that gap and
    // let a second applyQueue() dispatch a concurrent, destructive batch. m_apply_watcher is
    // set at dispatch and cleared only after that cleanup, so it fails closed across the gap.
    return m_apply_watcher != nullptr;
}

}  // namespace sak
