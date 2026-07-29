// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge_pipe.h"

#include "sak/win32mcp/browser_bridge_security.h"
#include "sak/win32mcp/native_messaging.h"

#include <QFile>
#include <QHash>
#include <QtEndian>

#include <tlhelp32.h>

namespace sak::win32mcp {

namespace {

// An ABSOLUTE deadline (a GetTickCount64 tick by which the whole logical operation
// must finish) plus the event that aborts the wait (the server shutdown event). The
// deadline is absolute, not per-call, so a byte-at-a-time dribble cannot keep
// restarting a per-op timer and hold a read open forever (slow-loris).
struct IoDeadline {
    ULONGLONG deadline_tick;
    HANDLE cancel_event;

    [[nodiscard]] DWORD remaining_ms() const {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline_tick) {
            return 0;
        }
        const ULONGLONG left = deadline_tick - now;
        return left > MAXDWORD ? MAXDWORD : static_cast<DWORD>(left);
    }
};

// One overlapped read or write bounded by the absolute deadline and abortable via its
// cancel event. Returns bytes transferred, or -1 on error / timeout / abort.
int overlappedIo(HANDLE pipe, bool is_write, char* buffer, DWORD size, IoDeadline deadline) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return -1;
    }
    DWORD moved = 0;
    const BOOL immediate = is_write ? WriteFile(pipe, buffer, size, &moved, &overlapped)
                                    : ReadFile(pipe, buffer, size, &moved, &overlapped);
    int result = -1;
    if (immediate != FALSE) {
        result = static_cast<int>(moved);
    } else if (GetLastError() == ERROR_IO_PENDING) {
        const HANDLE waits[2] = {overlapped.hEvent, deadline.cancel_event};
        const DWORD count = deadline.cancel_event != nullptr ? 2 : 1;
        const DWORD which = WaitForMultipleObjects(count, waits, FALSE, deadline.remaining_ms());
        if (which == WAIT_OBJECT_0 &&
            GetOverlappedResult(pipe, &overlapped, &moved, FALSE) != FALSE) {
            result = static_cast<int>(moved);
        } else {
            CancelIoEx(pipe, &overlapped);
            WaitForSingleObject(overlapped.hEvent, INFINITE);  // drain before the OVERLAPPED dies
        }
    }
    CloseHandle(overlapped.hEvent);
    return result;
}

bool pipeReadExact(HANDLE pipe, char* buffer, DWORD size, IoDeadline deadline) {
    DWORD offset = 0;
    while (offset < size) {
        const int got = overlappedIo(pipe, false, buffer + offset, size - offset, deadline);
        if (got <= 0) {
            return false;
        }
        offset += static_cast<DWORD>(got);
    }
    return true;
}

bool pipeWriteAll(HANDLE pipe, const char* buffer, DWORD size, IoDeadline deadline) {
    DWORD offset = 0;
    while (offset < size) {
        const int put =
            overlappedIo(pipe, true, const_cast<char*>(buffer) + offset, size - offset, deadline);
        if (put <= 0) {
            return false;
        }
        offset += static_cast<DWORD>(put);
    }
    return true;
}

bool readFrame(HANDLE pipe, IoDeadline deadline, QJsonObject* out) {
    char header[4];
    if (!pipeReadExact(pipe, header, 4, deadline)) {
        return false;
    }
    if (parseFrame(QByteArray(header, 4)).status == NativeFrame::Status::Error) {
        return false;  // zero / over-cap length prefix
    }
    const DWORD length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header));
    QByteArray frame(header, 4);
    QByteArray body(static_cast<int>(length), Qt::Uninitialized);
    if (!pipeReadExact(pipe, body.data(), length, deadline)) {
        return false;
    }
    frame.append(body);
    const NativeFrame parsed = parseFrame(frame);
    if (parsed.status != NativeFrame::Status::Ok) {
        return false;
    }
    *out = parsed.message;
    return true;
}

bool writeFrame(HANDLE pipe, const QJsonObject& message, IoDeadline deadline) {
    const QByteArray frame = encodeFrame(message);
    return pipeWriteAll(pipe, frame.constData(), static_cast<DWORD>(frame.size()), deadline);
}

enum class ConnectResult {
    Connected,
    Shutdown,
    Error
};

// Overlapped ConnectNamedPipe that also wakes on the shutdown event. Distinguishes a
// real client (Connected), a requested shutdown, and a transient failure (Error) so
// the caller can re-arm on Error instead of tearing the accept loop down.
ConnectResult waitForConnection(HANDLE pipe, HANDLE shutdown_event) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        return ConnectResult::Error;
    }
    ConnectResult result = ConnectResult::Error;
    if (ConnectNamedPipe(pipe, &overlapped) != FALSE) {
        result = ConnectResult::Connected;  // rare immediate completion
    } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
        result = ConnectResult::Connected;  // a client connected before we armed
    } else if (GetLastError() == ERROR_IO_PENDING) {
        const HANDLE waits[2] = {overlapped.hEvent, shutdown_event};
        const DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (which == WAIT_OBJECT_0) {
            result = ConnectResult::Connected;
        } else {
            CancelIoEx(pipe, &overlapped);
            WaitForSingleObject(overlapped.hEvent, INFINITE);
            result = which == WAIT_OBJECT_0 + 1 ? ConnectResult::Shutdown : ConnectResult::Error;
        }
    }
    CloseHandle(overlapped.hEvent);
    return result;
}

QString ownModulePath() {
    wchar_t buffer[MAX_PATH * 2] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH * 2);
    return QString::fromWCharArray(buffer, static_cast<int>(length)).toLower();
}

QString clientImagePath(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t buffer[MAX_PATH * 2] = {0};
    DWORD size = MAX_PATH * 2;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    return ok != FALSE ? QString::fromWCharArray(buffer, static_cast<int>(size)).toLower()
                       : QString();
}

// Best-effort walk up the process tree looking for an ancestor image (e.g.
// chrome.exe). Toolhelp parent pids can be stale/reused, so this is defense in depth,
// not a hard boundary.
bool hasAncestorImage(DWORD pid, const QString& target_basename_lower, int max_depth) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    QHash<DWORD, DWORD> parent;
    QHash<DWORD, QString> image;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            parent.insert(entry.th32ProcessID, entry.th32ParentProcessID);
            image.insert(entry.th32ProcessID, QString::fromWCharArray(entry.szExeFile).toLower());
        } while (Process32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);

    DWORD current = pid;
    for (int depth = 0; depth < max_depth && current != 0; ++depth) {
        const auto it = parent.constFind(current);
        if (it == parent.constEnd()) {
            break;
        }
        if (image.value(it.value()) == target_basename_lower) {
            return true;
        }
        current = it.value();
    }
    return false;
}

}  // namespace

BrowserBridgePipeServer::BrowserBridgePipeServer(Options options) : options_(std::move(options)) {
    rendezvous_path_ = options_.rendezvous_path.isEmpty() ? browserBridgeRendezvousPath()
                                                          : options_.rendezvous_path;
}

BrowserBridgePipeServer::~BrowserBridgePipeServer() {
    stop();
}

bool BrowserBridgePipeServer::start(QString* error) {
    pipe_name_ = browserBridgePipeName(error);
    if (pipe_name_.isEmpty()) {
        return false;
    }
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!buildBridgePipeSecurity(&attributes, &descriptor, error)) {
        return false;
    }
    const std::wstring wide = pipe_name_.toStdWString();
    pipe_ = CreateNamedPipeW(
        wide.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        64 * 1024,
        64 * 1024,
        0,
        &attributes);
    LocalFree(descriptor);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        if (error) {
            *error = QStringLiteral("CreateNamedPipe failed (err=%1); the name may be squatted")
                         .arg(GetLastError());
        }
        return false;
    }
    shutdown_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    token_ = generateBridgeToken();
    const RendezvousRecord record{
        pipe_name_, token_, options_.protocol, static_cast<qint64>(GetCurrentProcessId())};
    // Fail closed (and leak nothing) if the shutdown event could not be created or the
    // rendezvous record could not be written -- the I/O thread must have a valid abort
    // event, and the relay must be able to discover the pipe.
    if (shutdown_event_ == nullptr || !writeRendezvousRecord(rendezvous_path_, record, error)) {
        if (shutdown_event_ == nullptr && error) {
            *error = QStringLiteral("CreateEvent failed (err=%1)").arg(GetLastError());
        }
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
        if (shutdown_event_ != nullptr) {
            CloseHandle(shutdown_event_);
            shutdown_event_ = nullptr;
        }
        return false;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void BrowserBridgePipeServer::stop() {
    // Single-winner teardown: concurrent stop() calls (and the destructor after an
    // explicit stop()) must not double-join the thread or double-close the handles.
    // std::call_once runs the body exactly once and blocks any concurrent caller
    // until it finishes.
    std::call_once(stop_once_, [this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        if (shutdown_event_ != nullptr) {
            SetEvent(shutdown_event_);
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        if (shutdown_event_ != nullptr) {
            CloseHandle(shutdown_event_);
            shutdown_event_ = nullptr;
        }
        QFile::remove(rendezvous_path_);
    });
}

bool BrowserBridgePipeServer::clientConnected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_;
}

quint64 BrowserBridgePipeServer::connectionGeneration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

void BrowserBridgePipeServer::run() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) {
                return;
            }
        }
        const ConnectResult connect = waitForConnection(pipe_, shutdown_event_);
        if (connect == ConnectResult::Shutdown) {
            return;
        }
        if (connect == ConnectResult::Error) {
            // Transient accept failure: re-arm rather than killing the accept loop for
            // the life of the process. Reset the instance and back off briefly so a
            // persistent error cannot hot-spin.
            DisconnectNamedPipe(pipe_);
            Sleep(50);
            continue;
        }
        if (!handshake()) {
            DisconnectNamedPipe(pipe_);  // bad peer / handshake: drop and re-listen
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = true;
            ++generation_;
            has_request_ = false;  // no request may carry over from a prior connection
            has_response_ = false;
        }
        cv_.notify_all();
        serveConnected();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
        }
        cv_.notify_all();  // wake a waiting sender so it does not hang on a dead peer
        DisconnectNamedPipe(pipe_);
    }
}

bool BrowserBridgePipeServer::handshake() {
    // One absolute budget for the whole handshake, so a dribbling peer cannot hold the
    // sole pipe instance open past the deadline by restarting a per-read timer.
    const IoDeadline deadline{GetTickCount64() + options_.io_timeout_ms, shutdown_event_};
    QJsonObject hello;
    if (!readFrame(pipe_, deadline, &hello)) {
        return false;
    }
    if (hello.value(QStringLiteral("type")).toString() != QLatin1String("hello") ||
        hello.value(QStringLiteral("token")).toString() != token_) {
        return false;
    }
    if (hello.value(QStringLiteral("protocol")).toInt() != options_.protocol) {
        writeFrame(pipe_,
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                               {QStringLiteral("error"), QStringLiteral("protocol_mismatch")}},
                   deadline);
        return false;
    }
    QString why;
    if (!verifyPeer(&why)) {
        writeFrame(pipe_,
                   QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                               {QStringLiteral("error"),
                                QStringLiteral("unauthorized: %1").arg(why)}},
                   deadline);
        return false;
    }
    return writeFrame(pipe_,
                      QJsonObject{{QStringLiteral("type"), QStringLiteral("welcome")},
                                  {QStringLiteral("protocol"), options_.protocol},
                                  {QStringLiteral("pid"),
                                   static_cast<double>(GetCurrentProcessId())}},
                      deadline);
}

bool BrowserBridgePipeServer::verifyPeer(QString* why) const {
    ULONG client_pid = 0;
    if (GetNamedPipeClientProcessId(pipe_, &client_pid) == FALSE) {
        *why = QStringLiteral("cannot identify client");
        return false;
    }
    const QString client_image = clientImagePath(static_cast<DWORD>(client_pid));
    if (client_image.isEmpty() || client_image != ownModulePath()) {
        *why = QStringLiteral("client is not the bridge binary");
        return false;
    }
    if (options_.require_chrome_ancestor &&
        !hasAncestorImage(static_cast<DWORD>(client_pid), QStringLiteral("chrome.exe"), 12)) {
        *why = QStringLiteral("client was not launched by Chrome");
        return false;
    }
    return true;
}

void BrowserBridgePipeServer::serveConnected() {
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return has_request_ || !running_; });
        if (!running_) {
            return;
        }
        QJsonObject request = request_frame_;
        has_request_ = false;
        lock.unlock();

        const IoDeadline deadline{GetTickCount64() + options_.io_timeout_ms, shutdown_event_};
        QJsonObject reply;
        Exchange result;
        if (!writeFrame(pipe_, request, deadline)) {
            result.error = QStringLiteral("browser did not accept the command (timeout or reset)");
        } else if (!readFrame(pipe_, deadline, &reply)) {
            result.error = QStringLiteral("browser did not reply within %1 ms (connection reset)")
                               .arg(options_.io_timeout_ms);
        } else {
            result.ok = true;
            result.reply = reply;
        }

        lock.lock();
        response_ = result;
        has_response_ = true;
        cv_.notify_all();
        lock.unlock();

        if (!result.ok) {
            return;  // I/O failure: tear down the connection and re-listen
        }
    }
}

BrowserBridgePipeServer::Exchange BrowserBridgePipeServer::sendCommandAwaitReply(
    const QJsonObject& frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!connected_ || !running_) {
        return {false, {}, QStringLiteral("Browser not connected.")};
    }
    if (has_request_) {
        return {false, {}, QStringLiteral("A browser action is already in progress.")};
    }
    // Pin the connection identity: if the relay disconnects and a fresh one replaces
    // it while we wait, the generation changes. Waking on a generation change (not on
    // connected_, which oscillates false->true across a reconnect) is what stops the
    // sender from silently sleeping through its own connection's death and hanging for
    // the life of the next session.
    const quint64 my_generation = generation_;
    request_frame_ = frame;
    has_request_ = true;
    has_response_ = false;
    cv_.notify_all();
    cv_.wait(lock, [this, my_generation] {
        return has_response_ || !running_ || generation_ != my_generation;
    });
    // Always clear the slot on the way out: if the connection died before the I/O
    // thread consumed the request, leaving has_request_ set would make the NEXT
    // connection execute a stale command frame.
    const bool got_response = has_response_;
    Exchange result =
        got_response
            ? response_
            : Exchange{false, {}, QStringLiteral("Browser connection closed before it replied.")};
    has_request_ = false;
    has_response_ = false;
    return result;
}

}  // namespace sak::win32mcp
