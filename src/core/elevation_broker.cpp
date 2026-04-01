// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevation_broker.cpp
/// @brief Implements the ElevationBroker — client-side IPC with elevated helper

#include "sak/elevation_broker.h"

#include "sak/logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

namespace sak {

// ======================================================================
// Construction / Destruction
// ======================================================================

ElevationBroker::ElevationBroker(QObject* parent) : QObject(parent) {}

ElevationBroker::~ElevationBroker() {
    shutdown();
}

// ======================================================================
// Public API
// ======================================================================

bool ElevationBroker::isConnected() const {
#ifdef _WIN32
    return m_pipe_handle != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

auto ElevationBroker::executeTask(const QString& task_id,
                                  const QString& reason,
                                  const QJsonObject& payload)
    -> std::expected<ElevatedTaskResult, sak::error_code> {
    Q_ASSERT(!task_id.isEmpty());
    (void)reason;  // Reserved for future UAC context display

    sak::logInfo("ElevationBroker: executing task '{}'", task_id.toStdString());

    if (!isConnected()) {
        auto conn = ensureConnected();
        if (!conn) {
            return std::unexpected(conn.error());
        }
    }

    // Send task request
    m_current_task_id = task_id;
    QByteArray request = buildTaskRequest(task_id, payload);
    if (!sendRaw(request)) {
        sak::logError("ElevationBroker: failed to send task request");
        cleanup();
        return std::unexpected(sak::error_code::helper_connection_failed);
    }

    // Read messages until we get a result or error
    while (true) {
        auto msg = readMessage();
        if (!msg) {
            sak::logError("ElevationBroker: lost connection during task");
            cleanup();
            return std::unexpected(sak::error_code::helper_crashed);
        }

        if (msg->type == PipeMessageType::ProgressUpdate) {
            int percent = msg->json["percent"].toInt(0);
            QString status = msg->json["status"].toString();
            Q_EMIT progressUpdated(percent, status);
            continue;
        }

        return handleTaskMessage(*msg, task_id);
    }
}

auto ElevationBroker::ensureConnected() -> std::expected<void, sak::error_code> {
    auto launch = launchHelper();
    if (!launch) {
        return std::unexpected(launch.error());
    }

    auto conn = connectPipe();
    if (!conn) {
        cleanup();
        return std::unexpected(conn.error());
    }

    auto ready = readMessage();
    if (!ready || ready->type != PipeMessageType::Ready) {
        sak::logError("ElevationBroker: helper did not send Ready message");
        cleanup();
        return std::unexpected(sak::error_code::helper_connection_failed);
    }
    sak::logInfo("ElevationBroker: helper is ready");
    Q_EMIT helperReady();
    return {};
}

auto ElevationBroker::handleTaskMessage(const PipeMessage& msg, const QString& task_id)
    -> std::expected<ElevatedTaskResult, sak::error_code> {
    if (msg.type == PipeMessageType::TaskResult) {
        ElevatedTaskResult result;
        result.success = msg.json["success"].toBool(false);
        result.data = msg.json["data"].toObject();
        m_current_task_id.clear();
        sak::logInfo("ElevationBroker: task '{}' completed (success={})",
                     task_id.toStdString(),
                     result.success);
        return result;
    }

    if (msg.type == PipeMessageType::TaskError) {
        ElevatedTaskResult result;
        result.success = false;
        result.error_message = msg.json["message"].toString();
        m_current_task_id.clear();
        int code = msg.json["code"].toInt(0);
        sak::logError("ElevationBroker: task '{}' failed: {} (code {})",
                      task_id.toStdString(),
                      result.error_message.toStdString(),
                      code);
        return result;
    }

    sak::logWarning("ElevationBroker: unexpected message type {}", static_cast<int>(msg.type));
    return std::unexpected(sak::error_code::helper_connection_failed);
}

void ElevationBroker::cancelCurrentTask() {
    if (!isConnected() || m_current_task_id.isEmpty()) {
        return;
    }

    QByteArray cancel = buildCancelRequest(m_current_task_id);
    (void)sendRaw(cancel);
    sak::logInfo("ElevationBroker: cancel requested for '{}'", m_current_task_id.toStdString());
}

void ElevationBroker::shutdown() {
#ifdef _WIN32
    if (isConnected()) {
        QByteArray msg = buildShutdown();
        (void)sendRaw(msg);
        sak::logInfo("ElevationBroker: shutdown sent to helper");
    }
    cleanup();
#endif
}

// ======================================================================
// Private — Helper Launch
// ======================================================================

auto ElevationBroker::launchHelper() -> std::expected<void, sak::error_code> {
#ifdef _WIN32
    auto helper_path = findHelperPath();
    if (!helper_path) {
        return std::unexpected(helper_path.error());
    }

    // Generate unique pipe name for this session
    m_pipe_name = generatePipeName();

    sak::logInfo("ElevationBroker: launching helper at '{}' with pipe '{}'",
                 helper_path->toStdString(),
                 m_pipe_name.toStdString());

    // Pass pipe name and parent PID as arguments
    QString args = QString("--pipe \"%1\" --parent-pid %2")
                       .arg(m_pipe_name)
                       .arg(QCoreApplication::applicationPid());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = reinterpret_cast<LPCWSTR>(helper_path->utf16());
    std::wstring wide_args = args.toStdWString();
    sei.lpParameters = wide_args.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei)) {
        DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) {
            sak::logInfo("ElevationBroker: user cancelled UAC prompt");
            return std::unexpected(sak::error_code::elevation_denied);
        }
        sak::logError("ElevationBroker: ShellExecuteExW failed: {}", error);
        return std::unexpected(sak::error_code::elevation_failed);
    }

    m_helper_process = sei.hProcess;
    sak::logInfo("ElevationBroker: helper launched successfully");
    return {};
#else
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

// ======================================================================
// Private — Named Pipe Connection
// ======================================================================

auto ElevationBroker::connectPipe() -> std::expected<void, sak::error_code> {
#ifdef _WIN32
    std::wstring wide_name = m_pipe_name.toStdWString();

    // Wait for the helper to create the pipe (with timeout)
    constexpr int kRetryIntervalMs = 100;
    int elapsed = 0;

    while (elapsed < kPipeConnectTimeoutMs) {
        m_pipe_handle = CreateFileW(
            wide_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

        if (m_pipe_handle != INVALID_HANDLE_VALUE) {
            // Set pipe to message mode
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(m_pipe_handle, &mode, nullptr, nullptr);
            sak::logInfo("ElevationBroker: connected to pipe");
            return {};
        }

        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
            sak::logError("ElevationBroker: pipe connect error: {}", error);
            return std::unexpected(sak::error_code::helper_connection_failed);
        }

        // Check if helper process is still alive
        DWORD exit_code = 0;
        if (GetExitCodeProcess(m_helper_process, &exit_code) && exit_code != STILL_ACTIVE) {
            sak::logError("ElevationBroker: helper exited with code {}", exit_code);
            return std::unexpected(sak::error_code::helper_crashed);
        }

        QThread::msleep(kRetryIntervalMs);
        elapsed += kRetryIntervalMs;
    }

    sak::logError("ElevationBroker: pipe connection timed out");
    return std::unexpected(sak::error_code::elevation_timeout);
#else
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

// ======================================================================
// Private — Pipe I/O
// ======================================================================

bool ElevationBroker::sendRaw(const QByteArray& data) {
#ifdef _WIN32
    if (m_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    BOOL ok = WriteFile(
        m_pipe_handle, data.constData(), static_cast<DWORD>(data.size()), &bytes_written, nullptr);

    return ok && static_cast<int>(bytes_written) == data.size();
#else
    (void)data;
    return false;
#endif
}

auto ElevationBroker::readMessage() -> std::expected<PipeMessage, sak::error_code> {
    // Read header: 4 bytes length + 1 byte type
    char header[kPipeHeaderSize];
    if (!readExact(header, kPipeHeaderSize, kPipeIoTimeoutMs)) {
        return std::unexpected(sak::error_code::helper_connection_failed);
    }

    uint32_t payload_len = static_cast<uint8_t>(header[0]) |
                           (static_cast<uint8_t>(header[1]) << 8) |
                           (static_cast<uint8_t>(header[2]) << 16) |
                           (static_cast<uint8_t>(header[3]) << 24);

    auto type = static_cast<PipeMessageType>(static_cast<uint8_t>(header[4]));

    if (payload_len > kPipeMaxPayload) {
        sak::logError("ElevationBroker: message too large: {} bytes", payload_len);
        return std::unexpected(sak::error_code::helper_connection_failed);
    }

    QByteArray payload;
    if (payload_len > 0) {
        payload.resize(static_cast<int>(payload_len));
        if (!readExact(payload.data(), static_cast<int>(payload_len), kPipeIoTimeoutMs)) {
            return std::unexpected(sak::error_code::helper_connection_failed);
        }
    }

    return parsePayload(type, payload);
}

bool ElevationBroker::readExact(char* buffer, int size, int timeout_ms) {
#ifdef _WIN32
    if (m_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    int total_read = 0;
    int elapsed = 0;
    constexpr int kPollIntervalMs = 50;

    while (total_read < size && elapsed < timeout_ms) {
        DWORD available = peekAvailable();
        if (available == 0) {
            if (!isHelperAlive()) {
                return false;
            }
            QThread::msleep(kPollIntervalMs);
            elapsed += kPollIntervalMs;
            continue;
        }

        DWORD to_read = qMin(static_cast<DWORD>(size - total_read), available);
        DWORD bytes_read = 0;
        if (!ReadFile(m_pipe_handle, buffer + total_read, to_read, &bytes_read, nullptr)) {
            return false;
        }
        total_read += static_cast<int>(bytes_read);
        elapsed = 0;  // Reset timeout on successful read
    }

    return total_read == size;
#else
    (void)buffer;
    (void)size;
    (void)timeout_ms;
    return false;
#endif
}

DWORD ElevationBroker::peekAvailable() const {
    DWORD available = 0;
    if (!PeekNamedPipe(m_pipe_handle, nullptr, 0, nullptr, &available, nullptr)) {
        return 0;
    }
    return available;
}

bool ElevationBroker::isHelperAlive() const {
    DWORD exit_code = 0;
    if (m_helper_process && GetExitCodeProcess(m_helper_process, &exit_code) &&
        exit_code != STILL_ACTIVE) {
        return false;
    }
    return true;
}

// ======================================================================
// Private — Cleanup
// ======================================================================

void ElevationBroker::cleanup() {
#ifdef _WIN32
    if (m_pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe_handle);
        m_pipe_handle = INVALID_HANDLE_VALUE;
    }
    if (m_helper_process) {
        // Give the helper a moment to exit cleanly
        WaitForSingleObject(m_helper_process, 2000);
        CloseHandle(m_helper_process);
        m_helper_process = nullptr;
    }
#endif
    m_pipe_name.clear();
    m_current_task_id.clear();
    Q_EMIT helperDisconnected();
}

// ======================================================================
// Private — Utility
// ======================================================================

auto ElevationBroker::findHelperPath() -> std::expected<QString, sak::error_code> {
    QString app_dir = QCoreApplication::applicationDirPath();
    QString helper = QDir(app_dir).filePath("sak_elevated_helper.exe");

    if (QFileInfo::exists(helper)) {
        return helper;
    }

    sak::logError("ElevationBroker: helper not found at '{}'", helper.toStdString());
    return std::unexpected(sak::error_code::file_not_found);
}

}  // namespace sak
