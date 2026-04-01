// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevated_pipe_server.h
/// @brief Named pipe server running inside the elevated helper process

#include "sak/elevated_pipe_protocol.h"
#include "sak/error_codes.h"

#include <QObject>
#include <QString>

#include <expected>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sak {

/// @brief Named pipe server for the elevated helper process
///
/// Creates a named pipe, waits for a single client connection,
/// and provides send/receive primitives for framed messages.
///
/// Thread-Safety: Single-threaded; call from the helper's main thread.
class ElevatedPipeServer : public QObject {
    Q_OBJECT

public:
    /// @param pipe_name  Full pipe path (e.g. "\\\\.\\pipe\\SAK_Elevated_1234_...")
    /// @param parent_pid Expected parent process ID for validation
    explicit ElevatedPipeServer(const QString& pipe_name,
                                qint64 parent_pid,
                                QObject* parent = nullptr);
    ~ElevatedPipeServer() override;

    // Non-copyable
    ElevatedPipeServer(const ElevatedPipeServer&) = delete;
    ElevatedPipeServer& operator=(const ElevatedPipeServer&) = delete;

    /// @brief Create the pipe and wait for the client to connect
    /// @return true if a client connected successfully
    [[nodiscard]] bool start();

    /// @brief Send a progress update to the client
    void sendProgress(int percent, const QString& status);

    /// @brief Send a task result to the client
    void sendResult(bool success, const QJsonObject& data);

    /// @brief Send an error to the client
    void sendError(int code, const QString& message);

    /// @brief Send the Ready message after initialization
    void sendReady();

    /// @brief Read the next framed message from the client
    [[nodiscard]] auto readMessage() -> std::expected<PipeMessage, sak::error_code>;

    /// @brief Check if client is still connected
    [[nodiscard]] bool isConnected() const;

    /// @brief Close the pipe
    void stop();

private:
    /// @brief Send raw bytes
    [[nodiscard]] bool sendRaw(const QByteArray& data);

    /// @brief Read exactly N bytes
    [[nodiscard]] bool readExact(char* buffer, int size, int timeout_ms);

    /// @brief Validate the connecting client's parent PID
    [[nodiscard]] bool validateClient() const;

    QString m_pipe_name;
    qint64 m_parent_pid{0};

#ifdef _WIN32
    HANDLE m_pipe_handle{INVALID_HANDLE_VALUE};
#endif
};

}  // namespace sak
