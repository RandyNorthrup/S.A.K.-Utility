// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge_relay.h"

#include "sak/win32mcp/browser_bridge_security.h"
#include "sak/win32mcp/native_messaging.h"

#include <QByteArray>
#include <QtEndian>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <thread>

namespace sak::win32mcp {

namespace {

// How often the pump looks at the pipe while it is waiting for the extension's reply.
// Short enough that a cancel stops a polling handler promptly, long enough that an idle
// wait costs nothing measurable.
constexpr DWORD kCancelPollMs = 25;

// Room for a long \\?\-prefixed image path: twice MAX_PATH wide characters.
constexpr int kImagePathBufferChars = MAX_PATH * 2;

// A native-messaging frame is prefixed by a 4-byte little-endian uint32 length header.
constexpr int kNativeFrameHeaderBytes = 4;

void setError(QString* error, const QString& message) {
    if (error) {
        *error = message;
    }
}

// Our own executable image path, lowercased for a case-insensitive compare. Used to
// verify the recorded server pid is really the bridge binary (not a foreign process a
// rewritten rendezvous record points at).
QString ownImageLower() {
    wchar_t buffer[kImagePathBufferChars] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, kImagePathBufferChars);
    return QString::fromWCharArray(buffer, static_cast<int>(length)).toLower();
}

QString imageLower(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return {};
    }
    wchar_t buffer[kImagePathBufferChars] = {0};
    DWORD size = kImagePathBufferChars;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buffer, &size);
    CloseHandle(process);
    return ok != FALSE ? QString::fromWCharArray(buffer, static_cast<int>(size)).toLower()
                       : QString();
}

// True iff @p pid resolves to our own executable image (case-insensitive). An empty
// image (pid gone / not queryable) is a failure, not a match.
bool pidIsOwnImage(DWORD pid) {
    const QString image = imageLower(pid);
    return !image.isEmpty() && image == ownImageLower();
}

// Loop a synchronous ReadFile until @p size bytes arrive or the pipe closes/errors.
bool pipeReadExact(HANDLE pipe, char* buffer, DWORD size) {
    DWORD offset = 0;
    while (offset < size) {
        DWORD got = 0;
        if (ReadFile(pipe, buffer + offset, size - offset, &got, nullptr) == FALSE || got == 0) {
            return false;
        }
        offset += got;
    }
    return true;
}

bool pipeWriteAll(HANDLE pipe, const char* buffer, DWORD size) {
    DWORD offset = 0;
    while (offset < size) {
        DWORD put = 0;
        if (WriteFile(pipe, buffer + offset, size - offset, &put, nullptr) == FALSE || put == 0) {
            return false;
        }
        offset += put;
    }
    return true;
}

bool stdinReadExact(char* buffer, int size) {
    int offset = 0;
    while (offset < size) {
        std::cin.read(buffer + offset, size - offset);
        const std::streamsize got = std::cin.gcount();
        if (got <= 0) {
            return false;
        }
        offset += static_cast<int>(got);
    }
    return true;
}

// Decode one length-prefixed native-messaging frame after its 4-byte header has been
// validated and read into @p length. Shared by the pipe and stdin readers.
bool readBodyFrame(const std::function<bool(char*, int)>& read_exact,
                   DWORD length,
                   QJsonObject* out) {
    QByteArray frame(kNativeFrameHeaderBytes, Qt::Uninitialized);
    qToLittleEndian<quint32>(length, reinterpret_cast<uchar*>(frame.data()));
    QByteArray body(static_cast<int>(length), Qt::Uninitialized);
    if (!read_exact(body.data(), static_cast<int>(length))) {
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

bool readStdinFrame(QJsonObject* out) {
    char header[kNativeFrameHeaderBytes];
    if (!stdinReadExact(header, kNativeFrameHeaderBytes)) {
        return false;
    }
    if (parseFrame(QByteArray(header, kNativeFrameHeaderBytes)).status ==
        NativeFrame::Status::Error) {
        return false;
    }
    const DWORD length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header));
    return readBodyFrame([](char* b, int n) { return stdinReadExact(b, n); }, length, out);
}

bool writeStdoutFrame(const QJsonObject& message) {
    const QByteArray frame = encodeFrame(message);
    // Chrome terminates a native host that emits a host->browser frame over 1 MiB.
    // Refuse to write an oversized frame (report failure so relayPumpOnce tears the
    // pipe down) rather than letting Chrome kill the host mid-stream.
    if (frame.size() - kNativeFrameHeaderBytes > kMaxHostToBrowserBytes) {
        return false;
    }
    const size_t wrote =
        std::fwrite(frame.constData(), 1, static_cast<size_t>(frame.size()), stdout);
    // A short write OR a failed flush means the frame did not reach Chrome intact; report it so
    // relayPumpOnce tears the pipe down instead of treating a lost reply as delivered.
    const bool flushed = std::fflush(stdout) == 0;
    return wrote == static_cast<size_t>(frame.size()) && flushed;
}

}  // namespace

bool relayReadPipeFrame(HANDLE pipe, QJsonObject* out) {
    char header[kNativeFrameHeaderBytes];
    if (!pipeReadExact(pipe, header, kNativeFrameHeaderBytes)) {
        return false;
    }
    if (parseFrame(QByteArray(header, kNativeFrameHeaderBytes)).status ==
        NativeFrame::Status::Error) {
        return false;
    }
    const DWORD length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header));
    return readBodyFrame([pipe](char* b,
                                int n) { return pipeReadExact(pipe, b, static_cast<DWORD>(n)); },
                         length,
                         out);
}

bool relayWritePipeFrame(HANDLE pipe, const QJsonObject& message) {
    const QByteArray frame = encodeFrame(message);
    return pipeWriteAll(pipe, frame.constData(), static_cast<DWORD>(frame.size()));
}

HANDLE relayConnect(const QString& rendezvous_path,
                    QString* token_out,
                    int* protocol_out,
                    QString* error) {
    RendezvousRecord record;
    if (!readRendezvousRecord(rendezvous_path, &record, error)) {
        return INVALID_HANDLE_VALUE;
    }
    if (record.pipe_name.isEmpty() || record.app_pid <= 0) {
        setError(error,
                 QStringLiteral("Rendezvous record is missing the pipe name or server pid."));
        return INVALID_HANDLE_VALUE;
    }
    // Early sanity: the recorded server pid should be our own binary (a clear error if
    // the server is gone). The binding check below is what actually matters.
    if (!pidIsOwnImage(static_cast<DWORD>(record.app_pid))) {
        setError(error,
                 QStringLiteral("Bridge server pid %1 is not the S.A.K. binary (or is gone).")
                     .arg(record.app_pid));
        return INVALID_HANDLE_VALUE;
    }
    const std::wstring wide = record.pipe_name.toStdWString();
    // SECURITY_IDENTIFICATION: the server may identify us but never impersonate us.
    const HANDLE pipe = CreateFileW(wide.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                    nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        setError(error,
                 QStringLiteral("Cannot open the bridge pipe (err=%1).").arg(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }
    // Bind identity to the ACTUAL connected pipe, not just the recorded pid: a rewritten
    // record could cite our real pid while naming a foreign pipe. The process serving
    // THIS handle must be our binary, or we refuse and close.
    ULONG server_pid = 0;
    if (GetNamedPipeServerProcessId(pipe, &server_pid) == FALSE ||
        !pidIsOwnImage(static_cast<DWORD>(server_pid))) {
        setError(error, QStringLiteral("The bridge pipe is not served by the S.A.K. binary."));
        CloseHandle(pipe);
        return INVALID_HANDLE_VALUE;
    }
    if (token_out) {
        *token_out = record.token;
    }
    if (protocol_out) {
        *protocol_out = record.protocol;
    }
    return pipe;
}

bool relayHandshake(HANDLE pipe, const QString& token, int protocol, QString* error) {
    const QJsonObject hello{{QStringLiteral("type"), QStringLiteral("hello")},
                            {QStringLiteral("token"), token},
                            {QStringLiteral("protocol"), protocol}};
    if (!relayWritePipeFrame(pipe, hello)) {
        setError(error, QStringLiteral("Failed to send the bridge hello."));
        return false;
    }
    QJsonObject welcome;
    if (!relayReadPipeFrame(pipe, &welcome)) {
        setError(error, QStringLiteral("Bridge server closed before welcoming the relay."));
        return false;
    }
    if (welcome.value(QStringLiteral("type")).toString() != QLatin1String("welcome")) {
        setError(error,
                 QStringLiteral("Bridge server refused the relay: %1")
                     .arg(welcome.value(QStringLiteral("error")).toString()));
        return false;
    }
    // The welcome must confirm the SAME protocol version we offered, or the two sides would pump
    // frames under a version skew. Fail closed on a missing or mismatched protocol value rather
    // than trusting any frame whose type happens to be "welcome".
    if (welcome.value(QStringLiteral("protocol")).toInt(-1) != protocol) {
        setError(error,
                 QStringLiteral("Bridge protocol mismatch: relay offered %1, server welcomed %2")
                     .arg(protocol)
                     .arg(welcome.value(QStringLiteral("protocol")).toInt(-1)));
        return false;
    }
    return true;
}

// Wait for the extension's reply while staying able to see a cancel the server sends
// mid-exchange.
//
// The server abandons an exchange when its own I/O deadline elapses
// (BrowserBridgePipeServer::serveConnected). A handler that POLLS -- browser_wait_for,
// browser_download -- does not stop when that happens: it keeps driving the page for as
// long as ITS timeout allows, because a strictly synchronous pump is parked in the
// extension read and cannot notice anything arriving on the pipe. Forwarding the cancel
// is what stops it.
//
// Ownership is split so no handle has two users: the reply thread is the only code that
// touches stdin, and this thread is the only code that touches the pipe (and the only
// caller of browser_write). That is what makes this safe without CancelIoEx on a
// synchronous handle. The reply thread is ALWAYS joined -- an early return that abandoned
// a thread still reading stdin would leave it writing into a destroyed frame.
bool awaitBrowserReply(HANDLE pipe,
                       const BrowserReadFn& browser_read,
                       const BrowserWriteFn& browser_write,
                       QJsonObject* reply) {
    std::atomic<bool> finished{false};
    bool read_ok = false;
    std::thread reader([&] {
        read_ok = browser_read(reply);
        finished.store(true, std::memory_order_release);
    });

    bool pipe_ok = true;
    while (!finished.load(std::memory_order_acquire)) {
        DWORD available = 0;
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            pipe_ok = false;  // the server went away; stop watching and let the read finish
            break;
        }
        if (available < kNativeFrameHeaderBytes) {
            Sleep(kCancelPollMs);
            continue;
        }
        QJsonObject frame;
        if (!relayReadPipeFrame(pipe, &frame)) {
            pipe_ok = false;
            break;
        }
        // Only a cancel may arrive mid-exchange. Anything else means the server sent a
        // second command while this one is unanswered, which would desync the one-op pump
        // -- fail closed rather than forward it.
        if (frame.value(QStringLiteral("type")).toString() != QLatin1String("cancel")) {
            pipe_ok = false;
            break;
        }
        if (!browser_write(frame)) {
            pipe_ok = false;  // Chrome closed the port
            break;
        }
    }

    reader.join();
    return pipe_ok && read_ok;
}

bool relayPumpOnce(HANDLE pipe,
                   const BrowserReadFn& browser_read,
                   const BrowserWriteFn& browser_write) {
    QJsonObject command;
    if (!relayReadPipeFrame(pipe, &command)) {
        return false;  // the server (MCP process) closed the pipe
    }
    if (!browser_write(command)) {
        return false;  // Chrome closed the extension port
    }
    QJsonObject reply;
    if (!awaitBrowserReply(pipe, browser_read, browser_write, &reply)) {
        return false;  // the extension/Chrome went away, or the server abandoned the exchange
    }
    return relayWritePipeFrame(pipe, reply);  // false if the server reset the connection
}

int runBrowserRelay() {
    QString token;
    int protocol = 0;
    QString error;
    const HANDLE pipe = relayConnect(browserBridgeRendezvousPath(), &token, &protocol, &error);
    if (pipe == INVALID_HANDLE_VALUE || !relayHandshake(pipe, token, protocol, &error)) {
        // Tell the extension the bridge is down so it can surface it, then exit cleanly;
        // Chrome relaunches the host on the next port connection.
        writeStdoutFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("bridge_unavailable")},
                                     {QStringLiteral("error"), error}});
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
        return 0;
    }
    writeStdoutFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("bridge_ready")},
                                 {QStringLiteral("protocol"), protocol}});
    const BrowserReadFn reader = [](QJsonObject* out) {
        return readStdinFrame(out);
    };
    const BrowserWriteFn writer = [](const QJsonObject& m) {
        return writeStdoutFrame(m);
    };
    while (relayPumpOnce(pipe, reader, writer)) {
        // keep pumping until either side closes
    }
    CloseHandle(pipe);
    return 0;
}

}  // namespace sak::win32mcp
