// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thread_custody.cpp
/// @brief Keeps an overrunning thread and the state it uses alive until the thread finishes.

#include "sak/thread_custody.h"

#include "sak/logger.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace sak {

namespace {

// Reaping runs after the thread emitted finished(), so this join only waits out the last few
// instructions of QThread's own epilogue. A thread still running past it stays in custody.
constexpr unsigned long kFinishedThreadJoinMs = 1000;

struct RetainedThread {
    quint64 id{0};
    QThread* thread{nullptr};
    std::shared_ptr<void> state;
    QString reason;
};

struct Custody {
    QMutex mutex;
    std::vector<RetainedThread> threads;
    quint64 next_id{1};
};

// Deliberately never destroyed: a static destructor would run at exit and destroy state that a
// still-running retained thread is using -- the exact teardown custody exists to prevent.
Custody& custody() {
    static auto* const instance = new Custody;
    return *instance;
}

std::optional<RetainedThread> takeRetained(quint64 id) {
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    const auto found = std::ranges::find(registry.threads, id, &RetainedThread::id);
    if (found == registry.threads.end()) {
        return std::nullopt;
    }
    RetainedThread taken = std::move(*found);
    registry.threads.erase(found);
    return taken;
}

void putBack(RetainedThread retained) {
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    registry.threads.push_back(std::move(retained));
}

// Runs on the application thread, queued after the retained thread's finished().
void reap(quint64 id) {
    std::optional<RetainedThread> retained = takeRetained(id);
    if (!retained) {
        return;  // Already reaped: finished() and the post-connect isFinished() check both fired.
    }
    if (!retained->thread->wait(kFinishedThreadJoinMs)) {
        logError("Retained thread signalled finished but did not exit; keeping it in custody: {}",
                 retained->reason.toStdString());
        putBack(std::move(*retained));
        return;
    }
    // Destroyed outside the registry lock: a destructor may hand something else to custody.
    retained->state.reset();
}

}  // namespace

void ThreadCustody::retain(QThread* thread, std::shared_ptr<void> state, const QString& reason) {
    Q_ASSERT(thread != nullptr && state);
    if (thread == nullptr || !state) {
        logError("Thread custody was handed no thread or no state for: {}", reason.toStdString());
        return;
    }
    logError(
        "Thread did not stop within its cooperative deadline; it and the state it uses are "
        "retained until it finishes: {}",
        reason.toStdString());
    quint64 id = 0;
    {
        Custody& registry = custody();
        const QMutexLocker lock(&registry.mutex);
        id = registry.next_id++;
        registry.threads.push_back(RetainedThread{
            .id = id, .thread = thread, .state = std::move(state), .reason = reason});
    }
    QCoreApplication* const application = QCoreApplication::instance();
    if (application == nullptr) {
        return;  // No event loop will ever reap it; process exit ends the thread.
    }
    QObject::connect(
        thread, &QThread::finished, application, [id]() { reap(id); }, Qt::QueuedConnection);
    // The thread may have finished between the caller's bounded wait and the connect above.
    if (thread->isFinished()) {
        QMetaObject::invokeMethod(application, [id]() { reap(id); }, Qt::QueuedConnection);
    }
}

bool ThreadCustody::release(std::unique_ptr<QThread> thread,
                            const std::function<void()>& request_stop,
                            int stop_wait_ms) noexcept {
    if (!thread || !thread->isRunning()) {
        return true;  // Destroyed here; nothing is running in it.
    }
    if (request_stop) {
        request_stop();
    }
    if (thread->wait(static_cast<unsigned long>(std::max(stop_wait_ms, 0)))) {
        return true;
    }
    // Report + contain (owner decision 2026-10-24) instead of terminate() or an unbounded wait.
    const QString reason = QStringLiteral("%1 still running %2 ms after its stop request")
                               .arg(QString::fromLatin1(thread->metaObject()->className()))
                               .arg(stop_wait_ms);
    // The owner gave it up: nothing it emits from now on may reach that owner.
    QObject::disconnect(thread.get(), nullptr, nullptr, nullptr);
    // A QObject parent would otherwise delete it from underneath custody.
    thread->setParent(nullptr);
    // Custody destroys it on the application thread once it finishes.
    const QCoreApplication* const application = QCoreApplication::instance();
    if (application != nullptr && thread->thread() != application->thread()) {
        thread->moveToThread(application->thread());
    }
    QThread* const raw = thread.get();
    retain(raw, std::shared_ptr<QThread>(std::move(thread)), reason);
    return false;
}

void ThreadCustody::joinOrFailInDestructor(QThread& thread, int stop_wait_ms) noexcept {
    if (thread.wait(static_cast<unsigned long>(std::max(stop_wait_ms, 0)))) {
        return;
    }
    logError(
        "{} still running {} ms after its stop request was destroyed directly instead of "
        "released into thread custody",
        thread.metaObject()->className(),
        stop_wait_ms);
    qFatal(
        "%s destroyed while its thread still runs; release it through ThreadCustody (or "
        "WorkerBase::release) so it is retained instead",
        thread.metaObject()->className());
}

int ThreadCustody::retainedCount() {
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    return static_cast<int>(registry.threads.size());
}

QStringList ThreadCustody::retainedReasons() {
    Custody& registry = custody();
    const QMutexLocker lock(&registry.mutex);
    QStringList reasons;
    reasons.reserve(static_cast<qsizetype>(registry.threads.size()));
    for (const RetainedThread& retained : registry.threads) {
        reasons.append(retained.reason);
    }
    return reasons;
}

}  // namespace sak
