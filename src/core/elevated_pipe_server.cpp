// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevated_pipe_server.cpp
/// @brief Implements the named pipe server for the elevated helper

#include "sak/elevated_pipe_server.h"

#include "sak/logger.h"

#include <QDir>
#include <QFileInfo>
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

    // Administrators get full control; Builtin Users get read/write. The BU ACE
    // CANNOT be dropped: the client (the main app) ships an asInvoker manifest,
    // so it connects with a NON-elevated (Users) token and needs GRGW to open the
    // pipe. The per-process gate is validateClient(), which is fail-closed on a
    // missing parent PID (B5-04) and pins the exact launching process by PID --
    // a stronger control than any user-level DACL. The pipe name is also a
    // per-session nonce.
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;BA)(A;;GRGW;;;BU)", SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        sak::logError("ElevatedPipeServer: failed to create security descriptor: {}",
                      GetLastError());
        return false;
    }

    attributes.lpSecurityDescriptor = descriptor;
    return true;
}

HANDLE createServerPipe(const QString& pipe_name, SECURITY_ATTRIBUTES& attributes) {
    const std::wstring wide_name = pipe_name.toStdWString();
    // FILE_FLAG_OVERLAPPED so ConnectNamedPipe returns ERROR_IO_PENDING and the overlapped wait
    // (and its timeout) in waitForPipeClient can actually fire; a blocking handle would ignore
    // the OVERLAPPED and hang forever if the parent dies before connecting.
    // FILE_FLAG_FIRST_PIPE_INSTANCE so creation fails with ERROR_ALREADY_EXISTS if a pipe of this
    // name is already open: an attacker who pre-created the (per-session-nonce) name to impersonate
    // the server cannot make us silently attach to their instance -- we fail closed instead.
    // PIPE_REJECT_REMOTE_CLIENTS refuses any connection arriving over the network: this privileged
    // IPC is local-only and the pipe DACL admits Builtin Users, so a remote peer must never reach
    // it.
    return CreateNamedPipeW(
        wide_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        static_cast<DWORD>(kPipeMaxPayload),
        static_cast<DWORD>(kPipeMaxPayload),
        0,
        &attributes);
}

bool waitForPipeClient(HANDLE pipe_handle) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        sak::logError("ElevatedPipeServer: CreateEventW failed");
        return false;
    }

    const BOOL connected = ConnectNamedPipe(pipe_handle, &ov);
    const DWORD last_error = GetLastError();
    bool ok = (connected != 0) || last_error == ERROR_PIPE_CONNECTED;

    if (!ok && last_error == ERROR_IO_PENDING) {
        ok = WaitForSingleObject(ov.hEvent, kPipeConnectTimeoutMs) == WAIT_OBJECT_0;
        if (!ok) {
            sak::logError("ElevatedPipeServer: connection timed out");
            // Cancel the still-pending connect and let it settle before the event is closed, so
            // the OVERLAPPED/event is not freed while the kernel may still write to it.
            CancelIoEx(pipe_handle, &ov);
            WaitForSingleObject(ov.hEvent, INFINITE);
        }
    } else if (!ok) {
        sak::logError("ElevatedPipeServer: ConnectNamedPipe failed: {}", last_error);
    }

    CloseHandle(ov.hEvent);
    return ok;
}

// Perform one overlapped read or write with a timeout. The pipe is created OVERLAPPED, so every
// ReadFile/WriteFile MUST supply an OVERLAPPED (passing nullptr is undefined). Returns the byte
// count transferred, or -1 on any error/timeout (cancelling a pending op before freeing the
// event) so callers fail closed instead of blocking or reading garbage.
int overlappedPipeIo(HANDLE handle, bool is_write, char* buffer, DWORD size, DWORD timeout_ms) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        sak::logError("ElevatedPipeServer: CreateEventW failed");
        return -1;
    }
    BOOL ok = is_write ? WriteFile(handle, buffer, size, nullptr, &ov)
                       : ReadFile(handle, buffer, size, nullptr, &ov);
    if ((ok == 0) && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, timeout_ms) == WAIT_OBJECT_0) {
            ok = TRUE;
        } else {
            CancelIoEx(handle, &ov);
            WaitForSingleObject(ov.hEvent, INFINITE);
        }
    }
    DWORD transferred = 0;
    if (ok != 0) {
        ok = GetOverlappedResult(handle, &ov, &transferred, FALSE);
    }
    CloseHandle(ov.hEvent);
    return (ok != 0) ? static_cast<int>(transferred) : -1;
}

// Resolve the full image path of a process by PID via PROCESS_QUERY_LIMITED_INFORMATION.
// Returns an empty string on any failure so callers fail closed.
QString processImagePath(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) {
        sak::logWarning("ElevatedPipeServer: OpenProcess({}) failed: {}", pid, GetLastError());
        return {};
    }
    wchar_t buffer[MAX_PATH];
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(proc, 0, buffer, &size);
    CloseHandle(proc);
    if (ok == 0) {
        sak::logWarning("ElevatedPipeServer: QueryFullProcessImageNameW failed: {}",
                        GetLastError());
        return {};
    }
    return QString::fromWCharArray(buffer, static_cast<int>(size));
}

// The image path the client is REQUIRED to be: the main app executable colocated with
// this helper (both ship in the same directory). Empty on failure so callers fail closed.
QString expectedClientImagePath() {
    wchar_t module[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        sak::logWarning("ElevatedPipeServer: GetModuleFileNameW failed: {}", GetLastError());
        return {};
    }
    const QString helper_path = QString::fromWCharArray(module, static_cast<int>(len));
    const QString dir = QFileInfo(helper_path).absolutePath();
    return QDir(dir).absoluteFilePath(QStringLiteral("sak_utility.exe"));
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

    const DecodedFrameHeader decoded = decodeFrameHeader(header);
    if (!decoded.length_within_cap) {
        sak::logError("ElevatedPipeServer: message too large: {}", decoded.payload_len);
        return std::unexpected(sak::error_code::helper_connection_failed);
    }
    const uint32_t payload_len = decoded.payload_len;
    const auto type = decoded.type;

    QByteArray payload;
    if (payload_len > 0) {
        payload.resize(static_cast<int>(payload_len));
        if (!readExact(payload.data(), static_cast<int>(payload_len), kPipeIoTimeoutMs)) {
            return std::unexpected(sak::error_code::helper_connection_failed);
        }
    }

    PipeMessage message = parsePayload(type, payload);
    if (!message.valid) {
        // parsePayload already enforces the per-type field schema and refuses an
        // unknown/out-of-range type; surface that as a read failure so a malformed
        // frame can never occupy a successful result the caller would then process.
        sak::logError("ElevatedPipeServer: rejecting malformed message (type={})",
                      static_cast<int>(type));
        return std::unexpected(sak::error_code::helper_connection_failed);
    }
    return message;
}

bool ElevatedPipeServer::isConnected() const {
#ifdef _WIN32
    return m_pipe_handle != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

bool ElevatedPipeServer::hasPendingMessage() const {
#ifdef _WIN32
    if (m_pipe_handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD available = 0;
    if (PeekNamedPipe(m_pipe_handle, nullptr, 0, nullptr, &available, nullptr) == 0) {
        return false;
    }
    return available > 0;
#else
    return false;
#endif
}

ElevatedPipeServer::PipePoll ElevatedPipeServer::pollPipe() const {
#ifdef _WIN32
    if (m_pipe_handle == INVALID_HANDLE_VALUE) {
        return PipePoll::Broken;
    }
    DWORD available = 0;
    const BOOL peek_ok = PeekNamedPipe(m_pipe_handle, nullptr, 0, nullptr, &available, nullptr);
    return classifyPeek(peek_ok != 0, available);
#else
    return PipePoll::Broken;
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
    // A framed message is [kPipeHeaderSize header][<= kPipeMaxPayload payload]. An
    // empty buffer means frameMessage() tripped its own over-cap guard (build*
    // returned {}), and anything past the ceiling would truncate through the DWORD
    // size below. Refuse WITHOUT writing so the byte stream stays synchronized.
    if (data.size() < kPipeHeaderSize ||
        data.size() > static_cast<qsizetype>(kPipeHeaderSize) + kPipeMaxPayload) {
        sak::logError("ElevatedPipeServer: refusing to send malformed frame of {} bytes",
                      static_cast<long long>(data.size()));
        return false;
    }
    const int written = overlappedPipeIo(m_pipe_handle,
                                         true,
                                         const_cast<char*>(data.constData()),
                                         static_cast<DWORD>(data.size()),
                                         kPipeIoTimeoutMs);
    if (written != data.size()) {
        // A short or failed write leaves a partial frame in the byte-stream pipe;
        // any later frame would be read against those stray bytes and desynchronize
        // the protocol. Fail closed by tearing the connection down.
        sak::logError("ElevatedPipeServer: sendRaw wrote {} of {} bytes; closing pipe",
                      written,
                      static_cast<long long>(data.size()));
        stop();
        return false;
    }
    return true;
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
        if (PeekNamedPipe(m_pipe_handle, nullptr, 0, nullptr, &available, nullptr) == 0) {
            return false;
        }

        if (available == 0) {
            QThread::msleep(kPollMs);
            elapsed += kPollMs;
            continue;
        }

        DWORD const to_read = qMin(static_cast<DWORD>(size - total_read), available);
        const int bytes_read = overlappedPipeIo(
            m_pipe_handle, false, buffer + total_read, to_read, static_cast<DWORD>(timeout_ms));
        if (bytes_read < 0) {
            return false;
        }
        total_read += bytes_read;
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
        // B5-04: fail closed. Previously a missing/invalid parent PID skipped
        // validation and accepted ANY client -- combined with a pipe DACL that
        // permits Builtin Users, that disabled the only per-process gate. A
        // legitimate launch always passes --parent-pid, so refusing here closes
        // the gap without affecting the real flow.
        sak::logError("ElevatedPipeServer: no valid parent PID -- refusing client");
        return false;
    }

    // Get the client process ID from the pipe
    ULONG client_pid = 0;
    if (GetNamedPipeClientProcessId(m_pipe_handle, &client_pid) == 0) {
        sak::logWarning("ElevatedPipeServer: could not get client PID: {}", GetLastError());
        return false;
    }

    if (!clientPidMatchesParent(m_parent_pid, static_cast<qint64>(client_pid))) {
        sak::logError(
            "ElevatedPipeServer: client PID {} does not match "
            "expected parent PID {}",
            client_pid,
            m_parent_pid);
        return false;
    }

    return validateClientImage(client_pid);
#else
    return false;
#endif
}

#ifdef _WIN32
bool ElevatedPipeServer::validateClientImage(unsigned long client_pid) const {
    // A PID match alone can be forged by an unrelated process that inherited the
    // recycled parent PID. Bind identity to the image path as well: the client MUST
    // be the main app executable that ships alongside this helper. Fail closed if
    // either path cannot be resolved.
    const QString expected_image = expectedClientImagePath();
    const QString client_image = processImagePath(static_cast<DWORD>(client_pid));
    if (!clientImageMatchesExpected(expected_image, client_image)) {
        sak::logError("ElevatedPipeServer: client image '{}' does not match expected '{}'",
                      client_image.toStdString(),
                      expected_image.toStdString());
        return false;
    }
    return true;
}
#endif

}  // namespace sak
