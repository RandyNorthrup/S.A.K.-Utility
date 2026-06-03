// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_controller.h
/// @brief Controller for Partition Manager UI.

#pragma once

#include "sak/partition_executor.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_operation_queue.h"
#include "sak/storage_inventory_worker.h"

#include <QFutureWatcher>
#include <QObject>

#include <memory>

namespace sak {

class PartitionManagerController : public QObject {
    Q_OBJECT

public:
    explicit PartitionManagerController(QObject* parent = nullptr);
    ~PartitionManagerController() override;

    [[nodiscard]] const PartitionInventory& inventory() const noexcept;
    [[nodiscard]] const PartitionOperationQueue& queue() const noexcept;
    [[nodiscard]] PartitionManagerState state() const noexcept;

public Q_SLOTS:
    void refreshInventory();
    void queueOperation(PartitionOperationType type,
                        const PartitionTarget& target,
                        const QJsonObject& payload = {});
    void undo();
    void redo();
    void discardQueue();
    void applyQueue(bool dry_run = false, bool use_elevation = true);
    void cancel();

#ifdef SAK_PARTITION_MANAGER_PANEL_TEST_HOOKS
    void setTestInventory(const PartitionInventory& inventory);
#endif

Q_SIGNALS:
    void inventoryChanged(const sak::PartitionInventory& inventory);
    void queueChanged(const QVector<sak::PartitionOperation>& operations);
    void stateChanged(sak::PartitionManagerState state);
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);
    void executionFinished(const sak::PartitionExecutionResult& result);

private:
    PartitionOperationPlanner m_planner;
    PartitionOperationQueue m_queue;
    std::shared_ptr<PartitionExecutor> m_apply_executor;
    PartitionInventory m_inventory;
    PartitionInventory m_apply_before_inventory;
    QFutureWatcher<PartitionExecutionResult>* m_apply_watcher{nullptr};
    PartitionManagerState m_state{PartitionManagerState::Idle};
    bool m_apply_dry_run{false};

    [[nodiscard]] bool applyIsRunning() const;
    void setState(PartitionManagerState state);
    void emitQueueChanged();
    void finishApplyQueue(QFutureWatcher<PartitionExecutionResult>* watcher);
};

}  // namespace sak
