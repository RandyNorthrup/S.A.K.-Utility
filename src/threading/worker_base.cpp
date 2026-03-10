// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file worker_base.cpp
/// @brief Implements the base worker thread class with cancellation and error handling

#include "sak/worker_base.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QtGlobal>

#include <stdexcept>

WorkerBase::WorkerBase(QObject* parent) : QThread(parent) {}

WorkerBase::~WorkerBase() {
    if (isRunning()) {
        requestStop();
        if (!wait(sak::kTimeoutThreadShutdownMs)) {
            sak::logError("Worker thread did not stop within 15s — forcing termination");
            terminate();
            wait(sak::kTimeoutThreadTerminateMs);
        }
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
    m_stop_requested.store(false, std::memory_order_release);

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
    } catch (...) {  // Final safety net: re-throw in debug builds
        m_is_running.store(false, std::memory_order_release);
        sak::logError("Worker thread threw unknown exception");
        Q_EMIT failed(static_cast<int>(sak::error_code::internal_error),
                      QStringLiteral("Unhandled unknown exception"));
#ifndef NDEBUG
        throw;
#endif
    }
}

bool WorkerBase::checkStop() {
    if (stopRequested()) {
        sak::logInfo("Worker cancellation requested");
        return true;
    }
    return false;
}

void WorkerBase::reportProgress(int current, int total, const QString& message) {
    Q_ASSERT_X(total > 0, "reportProgress", "total must be positive");
    Q_ASSERT_X(current >= 0, "reportProgress", "current must be non-negative");
    Q_EMIT progress(current, total, message);
}
