// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apply_worker.h
/// @brief Runs PartitionExecutor::execute on its own worker thread so a live,
/// ELEVATED partition apply is cancellable and can never wedge shutdown.

#pragma once

#include "sak/partition_executor.h"
#include "sak/partition_manager_types.h"
#include "sak/worker_base.h"

#include <QVector>

namespace sak {

/// Wraps a single PartitionExecutor::execute() call in a WorkerBase thread.
///
/// The AI assistant's partition.apply_operation must not call the BLOCKING, elevated
/// PartitionExecutor directly on the tool worker thread: an in-flight elevated run
/// (ShellExecuteEx runas) has no reachable cancel and would leave the Stop button inert
/// and block teardown. Running it here instead makes it drivable exactly like the flash
/// path: connect finished()/failed()/cancelled() to an AsyncActionInvocation, and the
/// bounded WorkerBase shutdown (requestStop + wait + terminate) plus the cooperative
/// PartitionExecutor::cancel() (forwarded from cancelExecution() and the destructor) cap
/// how long a run can hold the thread.
class PartitionApplyWorker : public WorkerBase {
    Q_OBJECT

public:
    PartitionApplyWorker(QVector<PartitionOperation> operations,
                         bool dry_run,
                         bool use_elevation,
                         QObject* parent = nullptr);
    ~PartitionApplyWorker() override;

    /// Cooperatively cancel an in-flight run: requests WorkerBase stop AND forwards to
    /// PartitionExecutor::cancel() (thread-safe -- sets the cancel flag and cancels the
    /// elevation task). Safe to call from any thread; a no-op if nothing is running.
    void cancelExecution();

    /// The execution result. Valid only after finished()/cancelled() has been delivered
    /// (emitted after execute() returns, so reading it from the queued slot on the caller
    /// thread happens-after the worker wrote it).
    [[nodiscard]] const PartitionExecutionResult& result() const noexcept { return m_result; }

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    PartitionExecutor m_executor;
    QVector<PartitionOperation> m_operations;
    bool m_dry_run;
    bool m_use_elevation;
    PartitionExecutionResult m_result;
};

}  // namespace sak
