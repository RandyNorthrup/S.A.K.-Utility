// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file worker_base.cpp
/// @brief Implements the base worker thread class with cancellation and error handling

#include "sak/worker_base.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QtGlobal>

#include <cstdlib>
#include <stdexcept>

WorkerBase::WorkerBase(QObject* parent) : QThread(parent) {}

WorkerBase::~WorkerBase() {
    // Last-resort join. Derived workers should have already called stopAndJoin()
    // from their own destructor (while their members were still alive); this is
    // the safety net for a direct WorkerBase or a derived dtor that forgot.
    stopAndJoin();
}

void WorkerBase::stopAndJoin() noexcept {
    if (!isRunning()) {
        return;
    }
    requestStop();
    if (wait(sak::kTimeoutThreadShutdownMs)) {
        return;
    }
    // Cooperative stop failed. terminate() is a last resort (it can corrupt the
    // worker's own state), but a still-live thread about to run execute() into
    // freed members is worse. If even the post-terminate join fails the thread
    // is genuinely still running -- there is no safe way to proceed past this
    // point, so abort loudly rather than silently returning into a
    // use-after-free that would corrupt memory unpredictably.
    sak::logError("Worker thread did not stop within 15s -- forcing termination");
    terminate();
    if (!wait(sak::kTimeoutThreadTerminateMs)) {
        sak::logError(
            "Worker thread did not terminate after 5s -- aborting to avoid "
            "use-after-free on freed worker state");
        std::abort();
    }
}

void WorkerBase::requestStop() noexcept {
    m_stop_requested.store(true, std::memory_order_release);
    requestInterruption();
}

bool WorkerBase::stopRequested() const noexcept {
    return m_stop_requested.load(std::memory_order_acquire);
}

bool WorkerBase::isExecuting() const noexcept {
    return m_is_running.load(std::memory_order_acquire);
}

void WorkerBase::run() {
    m_is_running.store(true, std::memory_order_release);
    // Do NOT clear m_stop_requested here: a requestStop() issued between start()
    // and the thread actually entering run() would otherwise be lost. It defaults
    // to false at construction, so a fresh worker starts un-cancelled either way.

    Q_EMIT started();

    try {
        auto result = execute();

        m_is_running.store(false, std::memory_order_release);

        if (m_stop_requested.load(std::memory_order_acquire)) {
            Q_EMIT cancelled();
        } else if (result) {
            Q_EMIT finished();
        } else {
            Q_EMIT failed(static_cast<int>(result.error()),
                          QString::fromStdString(std::string(sak::to_string(result.error()))));
        }
    } catch (const std::exception& e) {
        m_is_running.store(false, std::memory_order_release);
        sak::logError("Worker thread threw exception: {}", e.what());
        Q_EMIT failed(static_cast<int>(sak::error_code::internal_error),
                      QString("Unhandled exception: %1").arg(e.what()));
    } catch (...) {
        // Final safety net. This deliberately does NOT rethrow.
        //
        // It used to rethrow under #ifndef NDEBUG as a "fail fast in development"
        // measure. That was wrong in two ways. First, this is the top frame of a
        // worker thread, so a rethrow does not reach a handler -- it reaches
        // std::terminate and aborts the process. Second, failed() has already been
        // emitted at that point, so every observer has been told the error was
        // handled and the process then dies anyway; the two statements contradict
        // each other. The reporting below IS the fail-closed behaviour: the run is
        // marked not-running, the cause is logged, and the caller is told the
        // operation failed with internal_error.
        //
        // It also made the Debug configuration unable to run its own test suite
        // (exceptionSafety_unknownException aborted with exit 3 and could only ever
        // pass in Release), which blocked every sanitizer run.
        m_is_running.store(false, std::memory_order_release);
        sak::logError("Worker thread threw unknown exception");
        Q_EMIT failed(static_cast<int>(sak::error_code::internal_error),
                      QStringLiteral("Unhandled unknown exception"));
    }
}

bool WorkerBase::checkStop() const {
    if (stopRequested()) {
        sak::logInfo("Worker cancellation requested");
        return true;
    }
    return false;
}

void WorkerBase::reportProgress(int current, int total, const QString& message) {
    if (total <= 0 || current < 0) {
        return;
    }
    Q_EMIT progress(current, total, message);
}
