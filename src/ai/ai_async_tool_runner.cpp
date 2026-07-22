// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_async_tool_runner.h"

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
        m_watcher.waitForFinished();
    }
}

bool AiAsyncToolRunner::start(Work work) {
    if (m_running || !work) {
        return false;
    }
    m_running = true;
    m_attached = true;
    m_watcher.setFuture(QtConcurrent::run(std::move(work)));
    return true;
}

void AiAsyncToolRunner::detach() {
    m_attached = false;
}

void AiAsyncToolRunner::onWatcherFinished() {
    m_running = false;
    const bool attached = m_attached;
    m_attached = true;
    const QJsonObject result = m_watcher.result();
    if (attached) {
        Q_EMIT finished(result);
    }
}

}  // namespace sak::ai
