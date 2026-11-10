// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_async_tool_runner.h"

#include "sak/logger.h"

#include <QtConcurrent>

#include <utility>

namespace sak::ai {

AiAsyncToolRunner::AiAsyncToolRunner(QObject* parent) : QObject(parent) {
    connect(&m_watcher,
            &QFutureWatcher<QJsonObject>::finished,
            this,
            &AiAsyncToolRunner::onWatcherFinished);
}

AiAsyncToolRunner::~AiAsyncToolRunner() {
    // Do not leave a pool task writing into a destroyed watcher: wait it out.
    if (m_running) {
        // Raise the cancellation token first so cooperative Work observes it and returns
        // promptly, shortening the join below. QtConcurrent::run has no built-in
        // cancellation, so this token is the only lever we have over an in-flight task.
        m_cancel->store(true, std::memory_order_relaxed);
        // SAK-ALLOW-BLOCKING: Work that ignores the token cannot be cancelled, and it writes
        // into m_watcher, which is destroyed the moment this body returns. Giving up on the
        // wait would hand the pool a dangling watcher AND risk the task touching owner state
        // that is being torn down, so we still join. Callers cancel the inner op first so the
        // task returns promptly.
        m_watcher.waitForFinished();
    }
}

bool AiAsyncToolRunner::start(Work work) {
    if (m_running || !work) {
        return false;
    }
    m_running = true;
    m_attached = true;
    // Lower the token for the new job. Safe without synchronization: start() is refused
    // while m_running is true, and m_running only clears in onWatcherFinished after the
    // previous Work callable returned, so no prior task is still polling this flag.
    m_cancel->store(false, std::memory_order_relaxed);
    m_watcher.setFuture(QtConcurrent::run(std::move(work)));
    return true;
}

void AiAsyncToolRunner::detach() {
    m_attached = false;
    // Signal cooperative Work to stop. Without this, detach() only hides the result and
    // lets the task keep running; a QtConcurrent::run task has no other cancellation path.
    m_cancel->store(true, std::memory_order_relaxed);
}

void AiAsyncToolRunner::onWatcherFinished() {
    m_running = false;
    const bool attached = m_attached;
    m_attached = true;

    // QFuture::result() rethrows whatever the Work callable threw, and this is a slot
    // invoked during Qt event delivery, so an escaping exception would terminate the
    // process rather than fail the tool call. Callers are expected to shape their own
    // failure result (they know the call id and tool name); this is the backstop that
    // keeps a caller that does not from taking the application down with it. drained()
    // must still fire either way or anything gating on isRunning() waits forever.
    QJsonObject result;
    bool have_result = false;
    try {
        result = m_watcher.result();
        have_result = true;
    } catch (const std::exception& e) {
        sak::logError("AiAsyncToolRunner: work threw and produced no result: {}", e.what());
    } catch (...) {
        sak::logError("AiAsyncToolRunner: work threw a non-std exception and produced no result");
    }

    if (attached && have_result) {
        Q_EMIT finished(result);
    }
    // Always announce that the pool task left, even when its result was dropped: a
    // detached job emits nothing else, so without this a caller that treats a running
    // runner as busy (a cancelled-but-still-executing mutation) would never learn that
    // it finally went idle.
    Q_EMIT drained();
}

}  // namespace sak::ai
