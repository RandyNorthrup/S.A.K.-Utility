// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevation_broker.cpp
/// @brief Implements the ElevationBroker — client-side IPC with elevated helper

#include "sak/elevation_broker.h"

#include "sak/layout_constants.h"
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

namespace {
constexpr int kHelperExitWaitMs = kTimerStatusBriefMs;

#ifdef _WIN32
// Resolve the full image path of a process by PID via PROCESS_QUERY_LIMITED_INFORMATION.
// Returns an empty string on any failure so callers fail closed.
QString processImagePath(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        sak::logWarning("ElevationBroker: OpenProcess({}) failed: {}", pid, GetLastError());
        return {};
    }
    wchar_t buffer[MAX_PATH];
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, buffer, &size);
    CloseHandle(proc);
    if (!ok) {
        sak::logWarning("ElevationBroker: QueryFullProcessImageNameW failed: {}", GetLastError());
        return {};
    }
    return QString::fromWCharArray(buffer, static_cast<int>(size));
}
#endif
}  // namespace

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
    return m_pipe_handle.load(std::memory_order_acquire) != INVALID_HANDLE_VALUE;
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
    setCurrentTaskId(task_id);
    QByteArray request = buildTaskRequest(task_id, payload);
    if (!sendRaw(request)) {
        sak::logError("ElevationBroker: failed to send task request");
        cleanup();
        return std::unexpected(sak::error_code::helper_connection_failed);
    }

    // Read messages until we get a result or error. This method is deliberately
    // synchronous and does not spin a nested Qt event loop; callers that need UI
    // responsiveness must dispatch it from a worker thread.
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
        result.error_message = result.data.value(QStringLiteral("error_message")).toString();
        setCurrentTaskId(QString());
        sak::logInfo("ElevationBroker: task '{}' completed (success={})",
                     task_id.toStdString(),
                     result.success);
        return result;
    }

    if (msg.type == PipeMessageType::TaskError) {
        ElevatedTaskResult result;
        result.success = false;
        result.error_message = msg.json["message"].toString();
        setCurrentTaskId(QString());
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

void ElevationBroker::setCurrentTaskId(const QString& id) {
    std::lock_guard<std::mutex> lock(m_task_state_mutex);
    m_current_task_id = id;
}

void ElevationBroker::cancelCurrentTask() {
    // Snapshot the id under the lock: this runs on the GUI thread while the
    // worker thread may be clearing it in handleTaskMessage/cleanup.
    QString task_id;
    {
        std::lock_guard<std::mutex> lock(m_task_state_mutex);
        task_id = m_current_task_id;
    }
    if (!isConnected() || task_id.isEmpty()) {
        return;
    }

    QByteArray cancel = buildCancelRequest(task_id);
    if (!sendRaw(cancel)) {
        sak::logWarning("ElevationBroker: cancel send failed for '{}'", task_id.toStdString());
    } else {
        sak::logInfo("ElevationBroker: cancel requested for '{}'", task_id.toStdString());
    }
}

void ElevationBroker::shutdown() {
#ifdef _WIN32
    if (isConnected()) {
        QByteArray msg = buildShutdown();
        if (!sendRaw(msg)) {
            sak::logWarning("ElevationBroker: shutdown send failed; tearing down anyway");
        } else {
            sak::logInfo("ElevationBroker: shutdown sent to helper");
        }
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
        const HANDLE handle = CreateFileW(
            wide_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        m_pipe_handle.store(handle, std::memory_order_release);

        if (handle != INVALID_HANDLE_VALUE) {
            // Before trusting the pipe, prove its server IS the helper we launched.
            // An attacker could have raced us to create a pipe of this name; the
            // per-session nonce makes that hard, but we still fail closed on mismatch.
            auto verified = verifyPipeServer(handle);
            if (!verified) {
                return std::unexpected(verified.error());
            }
            // Set pipe to message mode
            DWORD mode = PIPE_READMODE_BYTE;
            SetNamedPipeHandleState(handle, &mode, nullptr, nullptr);
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

#ifdef _WIN32
auto ElevationBroker::verifyPipeServer(HANDLE handle) -> std::expected<void, sak::error_code> {
    ULONG server_pid = 0;
    if (!GetNamedPipeServerProcessId(handle, &server_pid)) {
        sak::logError("ElevationBroker: GetNamedPipeServerProcessId failed: {}", GetLastError());
        return std::unexpected(sak::error_code::helper_connection_failed);
    }
    const qint64 expected_pid = static_cast<qint64>(GetProcessId(m_helper_process));
    if (!serverPidMatchesHelper(expected_pid, static_cast<qint64>(server_pid))) {
        sak::logError("ElevationBroker: pipe server PID {} does not match launched helper PID {}",
                      server_pid,
                      expected_pid);
        return std::unexpected(sak::error_code::helper_connection_failed);
    }
    return verifyServerImage(server_pid);
}

auto ElevationBroker::verifyServerImage(unsigned long server_pid)
    -> std::expected<void, sak::error_code> {
    // Bind identity to the image too: the server MUST run from the helper executable
    // we resolved, not merely hold the expected PID. Fail closed if either path is
    // unresolvable.
    auto expected_image = findHelperPath();
    if (!expected_image) {
        return std::unexpected(expected_image.error());
    }
    const QString server_image = processImagePath(static_cast<DWORD>(server_pid));
    if (!imagePathsMatch(*expected_image, server_image)) {
        sak::logError("ElevationBroker: pipe server image '{}' does not match helper '{}'",
                      server_image.toStdString(),
                      expected_image->toStdString());
        return std::unexpected(sak::error_code::helper_connection_failed);
    }
    return {};
}
#endif

// ======================================================================
// Private — Pipe I/O
// ======================================================================

bool ElevationBroker::sendRaw(const QByteArray& data) {
#ifdef _WIN32
    // Serialize every pipe write: the worker thread sends the task request while
    // the GUI thread may send a cancel frame; interleaved WriteFile calls on a
    // byte-mode pipe would corrupt the framing.
    std::lock_guard<std::mutex> lock(m_send_mutex);
    const HANDLE handle = m_pipe_handle.load(std::memory_order_acquire);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    BOOL ok = WriteFile(
        handle, data.constData(), static_cast<DWORD>(data.size()), &bytes_written, nullptr);

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

    uint32_t payload_len =
        static_cast<uint8_t>(header[kPipeFrameLengthByte0]) |
        (static_cast<uint8_t>(header[kPipeFrameLengthByte1]) << kPipeFrameByteShift1) |
        (static_cast<uint8_t>(header[kPipeFrameLengthByte2]) << kPipeFrameByteShift2) |
        (static_cast<uint8_t>(header[kPipeFrameLengthByte3]) << kPipeFrameByteShift3);

    auto type = static_cast<PipeMessageType>(static_cast<uint8_t>(header[kPipeFrameTypeByte]));

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
    if (m_pipe_handle.load(std::memory_order_acquire) == INVALID_HANDLE_VALUE) {
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
        {
            // Pin the handle across the ReadFile under m_send_mutex: cleanup() stores
            // INVALID and CloseHandle under the same lock, so the close cannot land
            // between this load and the ReadFile. peekAvailable() already reported
            // available>0, so this read does not block long enough to starve a
            // concurrent cancel (which only sends between polls, outside the lock).
            std::lock_guard<std::mutex> lock(m_send_mutex);
            const HANDLE handle = m_pipe_handle.load(std::memory_order_acquire);
            if (handle == INVALID_HANDLE_VALUE ||
                !ReadFile(handle, buffer + total_read, to_read, &bytes_read, nullptr)) {
                return false;
            }
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
    // Pin the handle across PeekNamedPipe under m_send_mutex so a concurrent
    // cleanup() close (which holds the same lock) cannot tear it mid-peek.
    std::lock_guard<std::mutex> lock(m_send_mutex);
    const HANDLE handle = m_pipe_handle.load(std::memory_order_acquire);
    if (handle == INVALID_HANDLE_VALUE ||
        !PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) {
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
    {
        // Close the pipe handle under the SAME lock sendRaw() holds. cancelCurrentTask()
        // can drive sendRaw() from a different thread (the GUI/AI thread) than the one
        // running the task loop that calls cleanup() on a lost connection; without this
        // lock CloseHandle could land between sendRaw's INVALID_HANDLE_VALUE check and its
        // WriteFile, writing to a closed (possibly reused) handle -- a data race on a
        // non-atomic HANDLE. Holding m_send_mutex serializes the close against any write.
        std::lock_guard<std::mutex> lock(m_send_mutex);
        const HANDLE handle = m_pipe_handle.load(std::memory_order_acquire);
        if (handle != INVALID_HANDLE_VALUE) {
            // Invalidate BEFORE closing so a concurrent lock-free reader
            // (readExact/peek) loads INVALID and bails rather than racing the
            // close on the same handle value.
            m_pipe_handle.store(INVALID_HANDLE_VALUE, std::memory_order_release);
            CloseHandle(handle);
        }
    }
    if (m_helper_process) {
        // Give the helper a moment to exit cleanly
        WaitForSingleObject(m_helper_process, kHelperExitWaitMs);
        CloseHandle(m_helper_process);
        m_helper_process = nullptr;
    }
#endif
    m_pipe_name.clear();
    setCurrentTaskId(QString());
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
