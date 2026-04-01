// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevated_task_dispatcher.h
/// @brief Dispatches tasks in the elevated helper to registered handler functions

#include "sak/error_codes.h"

#include <QJsonObject>
#include <QString>

#include <expected>
#include <functional>
#include <string>
#include <unordered_map>

namespace sak {

/// @brief Result returned by a task handler
struct TaskHandlerResult {
    bool success{false};
    QJsonObject data;
    QString error_message;
};

/// @brief Callback for reporting progress from within a task handler
using ProgressCallback = std::function<void(int percent, const QString& status)>;

/// @brief Callback for checking if cancellation was requested
using CancelCheck = std::function<bool()>;

/// @brief A task handler function signature
using TaskHandler = std::function<TaskHandlerResult(
    const QJsonObject& payload, ProgressCallback progress, CancelCheck is_cancelled)>;

/// @brief Manages the compile-time allowlist of tasks and dispatches them
///
/// Only tasks explicitly registered are accepted — any unregistered task
/// is rejected with error_code::task_not_allowed (security hardening).
class ElevatedTaskDispatcher {
public:
    ElevatedTaskDispatcher() = default;

    /// @brief Register a task handler
    /// @param task_id Unique task identifier
    /// @param handler Function to execute for this task
    void registerHandler(const QString& task_id, TaskHandler handler);

    /// @brief Check if a task is in the allowlist
    [[nodiscard]] bool isAllowed(const QString& task_id) const;

    /// @brief Dispatch a task to its registered handler
    /// @param task_id  Task identifier
    /// @param payload  JSON parameters
    /// @param progress Progress callback
    /// @param is_cancelled Cancellation check
    /// @return Handler result or error
    [[nodiscard]] auto dispatch(const QString& task_id,
                                const QJsonObject& payload,
                                ProgressCallback progress,
                                CancelCheck is_cancelled)
        -> std::expected<TaskHandlerResult, sak::error_code>;

    /// @brief Get the number of registered handlers
    [[nodiscard]] int handlerCount() const;

private:
    std::unordered_map<std::string, TaskHandler> m_handlers;
};

}  // namespace sak
