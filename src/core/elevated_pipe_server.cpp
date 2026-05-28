// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevated_pipe_server.cpp
/// @brief Implements the named pipe server for the elevated helper

#include "sak/elevated_pipe_server.h"

#include "sak/logger.h"

#include <QThread>

#ifdef _WIN32
#include <windows.h>

#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace sak {

#ifdef _WIN32
namespace {
bool createPipeSecurityAttributes(SECURITY_ATTRIBUTES& attributes,
                                  PSECURITY_DESCRIPTOR& descriptor) {
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = FALSE;

    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;BA)(A;;GRGW;;;BU)", SDDL_REVISION_1, &descriptor, nullptr)) {
        sak::logError("ElevatedPipeServer: failed to create security descriptor: {}",
                      GetLastError());
        return false;
    }

    attributes.lpSecurityDescriptor = descriptor;
    return true;
}

HANDLE createServerPipe(const QString& pipe_name, SECURITY_ATTRIBUTES& attributes) {
    const std::wstring wide_name = pipe_name.toStdWString();
    return CreateNamedPipeW(wide_name.c_str(),
                            PIPE_ACCESS_DUPLEX,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                            1,
                            static_cast<DWORD>(kPipeMaxPayload),
                            static_cast<DWORD>(kPipeMaxPayload),
                            0,
                            &attributes);
}

bool waitForPipeClient(HANDLE pipe_handle) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) {
        sak::logError("ElevatedPipeServer: CreateEventW failed");
        return false;
    }

    const BOOL connected = ConnectNamedPipe(pipe_handle, &ov);
    const DWORD last_error = GetLastError();
    bool ok = connected || last_error == ERROR_PIPE_CONNECTED;

    if (!ok && last_error == ERROR_IO_PENDING) {
        ok = WaitForSingleObject(ov.hEvent, kPipeConnectTimeoutMs) == WAIT_OBJECT_0;
        if (!ok) {
            sak::logError("ElevatedPipeServer: connection timed out");
        }
    } else if (!ok) {
        sak::logError("ElevatedPipeServer: ConnectNamedPipe failed: {}", last_error);
    }

    CloseHandle(ov.hEvent);
    return ok;
}
}  // namespace
#endif

// ======================================================================
// Construction / Destruction
// ======================================================================

ElevatedPipeServer::ElevatedPipeServer(const QString& pipe_name, qint64 parent_pid, QObject* parent)
    : QObject(parent), m_pipe_name(pipe_name), m_parent_pid(parent_pid) {}

ElevatedPipeServer::~ElevatedPipeServer() {
    stop();
}

// ======================================================================
// Public API
// ======================================================================

bool ElevatedPipeServer::start() {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!createPipeSecurityAttributes(sa, sd)) {
        return false;
    }

    m_pipe_handle = createServerPipe(m_pipe_name, sa);
    LocalFree(sd);

    if (m_pipe_handle == INVALID_HANDLE_VALUE) {
        sak::logError("ElevatedPipeServer: CreateNamedPipeW failed: {}", GetLastError());
        return false;
    }

    sak::logInfo("ElevatedPipeServer: waiting for client on '{}'", m_pipe_name.toStdString());
    if (!waitForPipeClient(m_pipe_handle)) {
        stop();
        return false;
    }

    if (!validateClient()) {
        sak::logError("ElevatedPipeServer: client validation failed");
        stop();
        return false;
    }

    sak::logInfo("ElevatedPipeServer: client connected and validated");
    return true;
#else
    return false;
#endif
}

void ElevatedPipeServer::sendProgress(int percent, const QString& status) {
    if (!sendRaw(buildProgressUpdate(percent, status))) {
        sak::logWarning("ElevatedPipeServer: sendProgress({}) failed; pipe may be broken", percent);
    }
}

void ElevatedPipeServer::sendResult(bool success, const QJsonObject& data) {
    if (!sendRaw(buildTaskResult(success, data))) {
        sak::logWarning("ElevatedPipeServer: sendResult(success={}) failed; pipe may be broken",
                        success);
    }
}

void ElevatedPipeServer::sendError(int code, const QString& message) {
    if (!sendRaw(buildTaskError(code, message))) {
        sak::logWarning("ElevatedPipeServer: sendError(code={}) failed; pipe may be broken", code);
    }
}

void ElevatedPipeServer::sendReady() {
    if (!sendRaw(buildReady())) {
        sak::logWarning("ElevatedPipeServer: sendReady() failed; pipe may be broken");
    }
}

auto ElevatedPipeServer::readMessage() -> std::expected<PipeMessage, sak::error_code> {
    char header[kPipeHeaderSize];
    if (!readExact(header, kPipeHeaderSize, kHelperTimeoutMs)) {
        return std::unexpected(sak::error_code::helper_connection_failed);
    }

    uint32_t payload_len =
        static_cast<uint8_t>(header[kPipeFrameLengthByte0]) |
        (static_cast<uint8_t>(header[kPipeFrameLengthByte1]) << kPipeFrameByteShift1) |
        (static_cast<uint8_t>(header[kPipeFrameLengthByte2]) << kPipeFrameByteShift2) |
        (static_cast<uint8_t>(header[kPipeFrameLengthByte3]) << kPipeFrameByteShift3);
    auto type = static_cast<PipeMessageType>(static_cast<uint8_t>(header[kPipeFrameTypeByte]));

    if (payload_len > kPipeMaxPayload) {
        sak::logError("ElevatedPipeServer: message too large: {}", payload_len);
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

bool ElevatedPipeServer::isConnected() const {
#ifdef _WIN32
    return m_pipe_handle != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

void ElevatedPipeServer::stop() {
#ifdef _WIN32
    if (m_pipe_handle != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(m_pipe_handle);
        CloseHandle(m_pipe_handle);
        m_pipe_handle = INVALID_HANDLE_VALUE;
    }
#endif
}

// ======================================================================
// Private
// ======================================================================

bool ElevatedPipeServer::sendRaw(const QByteArray& data) {
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

bool ElevatedPipeServer::readExact(char* buffer, int size, int timeout_ms) {
#ifdef _WIN32
    if (m_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    int total_read = 0;
    int elapsed = 0;
    constexpr int kPollMs = 50;

    while (total_read < size && elapsed < timeout_ms) {
        DWORD available = 0;
        if (!PeekNamedPipe(m_pipe_handle, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }

        if (available == 0) {
            QThread::msleep(kPollMs);
            elapsed += kPollMs;
            continue;
        }

        DWORD to_read = qMin(static_cast<DWORD>(size - total_read), available);
        DWORD bytes_read = 0;
        if (!ReadFile(m_pipe_handle, buffer + total_read, to_read, &bytes_read, nullptr)) {
            return false;
        }
        total_read += static_cast<int>(bytes_read);
        elapsed = 0;
    }

    return total_read == size;
#else
    (void)buffer;
    (void)size;
    (void)timeout_ms;
    return false;
#endif
}

bool ElevatedPipeServer::validateClient() const {
#ifdef _WIN32
    if (m_parent_pid <= 0) {
        return true;  // No parent PID specified — skip validation
    }

    // Get the client process ID from the pipe
    ULONG client_pid = 0;
    if (!GetNamedPipeClientProcessId(m_pipe_handle, &client_pid)) {
        sak::logWarning("ElevatedPipeServer: could not get client PID: {}", GetLastError());
        return false;
    }

    if (static_cast<qint64>(client_pid) != m_parent_pid) {
        sak::logError(
            "ElevatedPipeServer: client PID {} does not match "
            "expected parent PID {}",
            client_pid,
            m_parent_pid);
        return false;
    }

    return true;
#else
    return false;
#endif
}

}  // namespace sak
