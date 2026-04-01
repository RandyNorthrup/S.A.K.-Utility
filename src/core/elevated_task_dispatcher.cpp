// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevated_task_dispatcher.cpp
/// @brief Implements the task dispatcher for the elevated helper

#include "sak/elevated_task_dispatcher.h"

#include "sak/logger.h"

namespace sak {

void ElevatedTaskDispatcher::registerHandler(const QString& task_id, TaskHandler handler) {
    Q_ASSERT(!task_id.isEmpty());
    Q_ASSERT(handler);
    m_handlers[task_id.toStdString()] = std::move(handler);
    sak::logInfo("ElevatedTaskDispatcher: registered handler for '{}'", task_id.toStdString());
}

bool ElevatedTaskDispatcher::isAllowed(const QString& task_id) const {
    return m_handlers.contains(task_id.toStdString());
}

auto ElevatedTaskDispatcher::dispatch(const QString& task_id,
                                      const QJsonObject& payload,
                                      ProgressCallback progress,
                                      CancelCheck is_cancelled)
    -> std::expected<TaskHandlerResult, sak::error_code> {
    Q_ASSERT(!task_id.isEmpty());

    auto it = m_handlers.find(task_id.toStdString());
    if (it == m_handlers.end()) {
        sak::logError("ElevatedTaskDispatcher: task '{}' not in allowlist", task_id.toStdString());
        return std::unexpected(sak::error_code::task_not_allowed);
    }

    sak::logInfo("ElevatedTaskDispatcher: dispatching '{}'", task_id.toStdString());

    try {
        return it->second(payload, std::move(progress), std::move(is_cancelled));
    } catch (const std::exception& ex) {
        sak::logError("ElevatedTaskDispatcher: task '{}' threw exception: {}",
                      task_id.toStdString(),
                      ex.what());
        TaskHandlerResult result;
        result.success = false;
        result.error_message = QString::fromStdString(ex.what());
        return result;
    }
}

int ElevatedTaskDispatcher::handlerCount() const {
    return static_cast<int>(m_handlers.size());
}

}  // namespace sak
