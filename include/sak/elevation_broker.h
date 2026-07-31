// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevation_broker.h
/// @brief Manages lifecycle of the elevated helper process and IPC pipe

#include "sak/elevated_pipe_protocol.h"
#include "sak/error_codes.h"

#include <QJsonObject>
#include <QObject>
#include <QString>

#include <atomic>
#include <expected>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sak {

/// @brief Result from an elevated task execution
struct ElevatedTaskResult {
    bool success{false};
    QJsonObject data;
    QString error_message;
};

/// @brief Manages the elevated helper process and named pipe communication
///
/// The broker lazily launches `sak_elevated_helper.exe` via UAC prompt,
/// connects over a named pipe, dispatches tasks, and relays progress.
/// The helper auto-exits after an inactivity timeout.
///
/// Thread-safety: executeTask() runs synchronously on a worker thread while
/// cancelCurrentTask() may be invoked from the GUI thread. The current task id
/// is guarded by m_task_state_mutex and every pipe write is serialized by
/// m_send_mutex so a concurrent cancel cannot tear the id or interleave frames.
class ElevationBroker : public QObject {
    Q_OBJECT

public:
    explicit ElevationBroker(QObject* parent = nullptr);
    ~ElevationBroker() override;

    // Disable copy/move (QObject)
    ElevationBroker(const ElevationBroker&) = delete;
    ElevationBroker& operator=(const ElevationBroker&) = delete;

    /// @brief Check if the helper is currently running and connected
    [[nodiscard]] bool isConnected() const;

    /// @brief Execute a task in the elevated helper (synchronous)
    ///
    /// If the helper is not running, launches it (triggers UAC prompt).
    /// Sends the task, waits for completion, returns the result.
    ///
    /// @param task_id  Registered task name (must be in helper allowlist)
    /// @param reason   Human-readable reason shown in UAC context
    /// @param payload  Task-specific JSON parameters
    /// @return Task result or error code
    [[nodiscard]] auto executeTask(const QString& task_id,
                                   const QString& reason,
                                   const QJsonObject& payload = {})
        -> std::expected<ElevatedTaskResult, sak::error_code>;

    /// @brief Request cancellation of the currently running task
    void cancelCurrentTask();

    /// @brief Shut down the helper process gracefully
    void shutdown();

Q_SIGNALS:
    /// @brief Progress update from the elevated helper
    void progressUpdated(int percent, const QString& status);

    /// @brief Helper process connected and ready
    void helperReady();

    /// @brief Helper process disconnected or crashed
    void helperDisconnected();

private:
    /// @brief Launch the helper process with UAC elevation
    [[nodiscard]] auto launchHelper() -> std::expected<void, sak::error_code>;

    /// @brief Connect to the helper's named pipe
    [[nodiscard]] auto connectPipe() -> std::expected<void, sak::error_code>;

    /// @brief Ensure the helper is running, connected, and ready
    [[nodiscard]] auto ensureConnected() -> std::expected<void, sak::error_code>;

    /// @brief Handle a single message in the task response loop
    [[nodiscard]] auto handleTaskMessage(const PipeMessage& msg, const QString& task_id)
        -> std::expected<ElevatedTaskResult, sak::error_code>;

    /// @brief Send raw bytes to the pipe
    [[nodiscard]] bool sendRaw(const QByteArray& data);

    /// @brief Read a single framed message from the pipe
    [[nodiscard]] auto readMessage() -> std::expected<PipeMessage, sak::error_code>;

    /// @brief Read exactly N bytes from the pipe
    [[nodiscard]] bool readExact(char* buffer, int size, int timeout_ms);

#ifdef _WIN32
    /// @brief Peek at available bytes in the pipe (0 if none or broken)
    [[nodiscard]] DWORD peekAvailable() const;

    /// @brief Check if the helper process is still running
    [[nodiscard]] bool isHelperAlive() const;
#endif

    /// @brief Close the pipe handle and helper process
    void cleanup();

    /// @brief Find the helper executable path
    [[nodiscard]] static auto findHelperPath() -> std::expected<QString, sak::error_code>;

    /// @brief Set the current task id under m_task_state_mutex
    void setCurrentTaskId(const QString& id);

    QString m_pipe_name;
    QString m_current_task_id;
    mutable std::mutex m_task_state_mutex;
    std::mutex m_send_mutex;

#ifdef _WIN32
    // B5-06: the pipe handle is read on the GUI/AI thread (isConnected / cancel /
    // shutdown) while the worker thread connects, reads, and tears it down.
    // A plain HANDLE read concurrently with a write is a data race (UB); make it
    // atomic so every access is well-defined. m_send_mutex still serializes the
    // WriteFile-vs-CloseHandle sequence in sendRaw()/cleanup() -- the atomic only
    // makes the lock-free reads (isConnected, readExact/peek) safe; it does not
    // replace that mutex.
    std::atomic<HANDLE> m_pipe_handle{INVALID_HANDLE_VALUE};
    HANDLE m_helper_process{nullptr};
#endif
};

}  // namespace sak
