// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_controller.cpp
/// @brief Partition Manager controller implementation.

#include "sak/partition_manager_controller.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/partition_report_generator.h"

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
}  // namespace

PartitionManagerController::PartitionManagerController(QObject* parent) : QObject(parent) {}

PartitionManagerController::~PartitionManagerController() {
    if (m_apply_executor) {
        m_apply_executor->cancel();
    }
    if (m_apply_watcher) {
        auto* watcher = m_apply_watcher;
        m_apply_watcher = nullptr;
        QObject::disconnect(watcher, nullptr, this, nullptr);
        watcher->setParent(nullptr);
        connect(watcher,
                &QFutureWatcher<PartitionExecutionResult>::finished,
                watcher,
                &QObject::deleteLater);
        if (watcher->isFinished()) {
            watcher->deleteLater();
        }
    }
    m_apply_executor.reset();
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
        }
        m_queue.setBaseLayoutHash(m_inventory.layout_hash);
        setState(PartitionManagerState::Ready);
        Q_EMIT inventoryChanged(m_inventory);
        emitQueueChanged();
        Q_EMIT progressUpdate(kPercentMax, kPercentMax);
        Q_EMIT statusMessage(QStringLiteral("Partition Manager: inventory ready"),
                             sak::kTimerStatusDefaultMs);
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
    setState(PartitionManagerState::Ready);
}

void PartitionManagerController::applyQueue(bool dry_run, bool use_elevation) {
    if (applyIsRunning()) {
        Q_EMIT statusMessage(QStringLiteral("Partition operation already running"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    if (!m_queue.canApply(m_inventory.layout_hash)) {
        Q_EMIT statusMessage(QStringLiteral(
                                 "Partition queue cannot apply: refresh or fix blockers"),
                             sak::kTimerStatusDefaultMs);
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

void PartitionManagerController::finishApplyQueue(
    QFutureWatcher<PartitionExecutionResult>* watcher) {
    if (!watcher) {
        return;
    }
    auto result = watcher->result();
    setState(PartitionManagerState::Verifying);
    if (!result.dry_run && !result.cancelled) {
        m_inventory = StorageInventoryWorker::scanCurrentSystem();
    } else {
        m_inventory = m_apply_before_inventory;
    }

    PartitionReportGenerator report_generator;
    result.report_html =
        report_generator.generateHtml(m_apply_before_inventory, m_inventory, result);
    result.report_json =
        report_generator.generateJson(m_apply_before_inventory, m_inventory, result);
    if (result.success && !m_apply_dry_run) {
        m_queue.discard();
    }
    setState(result.cancelled
                 ? PartitionManagerState::Cancelled
                 : (result.success ? PartitionManagerState::Ready : PartitionManagerState::Failed));
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

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
void PartitionManagerController::setTestInventory(const PartitionInventory& inventory) {
    m_inventory = inventory;
    m_queue.setBaseLayoutHash(m_inventory.layout_hash);
    Q_EMIT inventoryChanged(m_inventory);
    emitQueueChanged();
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
    return m_apply_watcher && !m_apply_watcher->isFinished();
}

}  // namespace sak
