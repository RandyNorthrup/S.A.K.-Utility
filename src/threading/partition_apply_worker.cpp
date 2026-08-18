// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_apply_worker.cpp
/// @brief Worker-thread wrapper around PartitionExecutor::execute.

#include "sak/partition_apply_worker.h"

#include "sak/layout_constants.h"

#include <utility>

namespace sak {

PartitionApplyWorker::PartitionApplyWorker(QVector<PartitionOperation> operations,
                                           bool dry_run,
                                           bool use_elevation,
                                           QObject* parent)
    : WorkerBase(parent)
    , m_operations(std::move(operations))
    , m_dry_run(dry_run)
    , m_use_elevation(use_elevation) {}

PartitionApplyWorker::~PartitionApplyWorker() {
    // Join the thread HERE, while m_executor is still alive. WorkerBase's own destructor
    // runs AFTER this body and AFTER m_executor (a derived member) is destroyed, so if we
    // left the join to it the still-running execute() would use a dead m_executor (UAF).
    // Bounded: cooperative cancel first, then wait, then force-terminate as a backstop.
    //
    // ACCEPTED RESIDUAL (adversarial review, low-probability): cooperative cancel stops an
    // in-flight elevated op (broker cancel frame) so the thread returns well within the
    // wait and terminate() does NOT fire. terminate() can only fire if the thread is stuck
    // in the un-interruptible ShellExecuteEx(runas) UAC launch -- i.e. teardown happened
    // while a just-confirmed apply's UAC prompt is still unanswered. In that window NO disk
    // write has occurred (UAC was not accepted), so there is no data-loss risk; the cost is
    // only that TerminateThread inside SHELL32/COM may leak the stack-local broker's handles
    // or strand a loader lock during an app exit that is already in progress. Bounding
    // teardown is judged better than an unbounded hang on an un-cancellable UAC prompt.
    if (isRunning()) {
        m_executor.cancel();
        // stopAndJoin() is the base's bounded, FAIL-CLOSED join (requestStop -> wait -> terminate
        // -> wait, and std::abort() if even the post-terminate wait fails). It replaces a
        // hand-rolled sequence that IGNORED the final wait and fell through into member
        // destruction -- a use-after-free of m_executor if the thread was not yet reaped. The
        // accepted residual above is unchanged: it bounds WHEN terminate() may fire (no disk write
        // has occurred in that window); this only makes an unreaped thread abort loudly instead of
        // corrupting memory, matching the NetworkProbeWorker destructor.
        stopAndJoin();
    }
}

void PartitionApplyWorker::cancelExecution() {
    requestStop();        // so WorkerBase emits cancelled() rather than finished()
    m_executor.cancel();  // cooperative: stops the elevated task / local script
}

auto PartitionApplyWorker::execute() -> std::expected<void, sak::error_code> {
    // The executor never throws for an operation-level failure; it records success/failure
    // in the result. So the worker itself always "succeeds" at running, and the caller
    // inspects result().success. A genuine crash surfaces via WorkerBase's exception guard.
    m_result = m_executor.execute(m_operations, m_dry_run, m_use_elevation);
    return {};
}

}  // namespace sak
