// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// NOT COMPILED INTO THE SHIPPING BINARY. The production Chrome native-messaging host is
// browser_bridge_relay.cpp (win32 MCP entry.cpp routes relay mode to runBrowserRelay());
// this translation unit is built ONLY by tests/CMakeLists.txt as an executable, self-checking
// reference for the native-messaging framing/ping contract (see test_win32_mcp_server). Keep it
// in lockstep with the relay's framing if that contract changes; do not add production callers.

#include "sak/win32mcp/win32_mcp_native_host.h"

#include "sak/win32mcp/native_messaging.h"

#include <QCoreApplication>
#include <QtEndian>

#include <cstdio>
#include <iostream>

namespace sak::win32mcp {

namespace {

// The native-messaging framing prefixes each message with a little-endian uint32 length.
constexpr int kNativeFrameHeaderBytes = 4;

QJsonObject pongReply(const QJsonObject& request,
                      const QString& server_name,
                      const QString& server_version) {
    QJsonObject reply{{QStringLiteral("type"), QStringLiteral("pong")},
                      {QStringLiteral("server"), server_name},
                      {QStringLiteral("version"), server_version},
                      {QStringLiteral("protocol"), kBrowserBridgeProtocol},
                      {QStringLiteral("pid"),
                       static_cast<double>(QCoreApplication::applicationPid())}};
    if (request.contains(QStringLiteral("id"))) {
        reply.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    }
    return reply;
}

QJsonObject typedError(const QJsonObject& request, const QString& message) {
    QJsonObject reply{{QStringLiteral("type"), QStringLiteral("error")},
                      {QStringLiteral("error"), message}};
    if (request.contains(QStringLiteral("id"))) {
        reply.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    }
    return reply;
}

// Read exactly n bytes from stdin, blocking until they arrive or the pipe closes.
// Returns false at end-of-stream (clean shutdown) or on a truncated read.
bool readExact(char* dst, int n) {
    int offset = 0;
    while (offset < n) {
        std::cin.read(dst + offset, n - offset);
        const std::streamsize got = std::cin.gcount();
        if (got <= 0) {
            return false;
        }
        offset += static_cast<int>(got);
    }
    return true;
}

bool writeFrame(const QJsonObject& message) {
    const QByteArray frame = encodeFrame(message);
    const size_t want = static_cast<size_t>(frame.size());
    // A short fwrite or a failed flush means the peer (Chrome) received a partial frame or
    // none at all; the length-prefixed native-messaging stream has no in-band resync, so
    // report the failure and let the loop close the port fail-closed rather than keep
    // serving a desynchronized stream.
    if (std::fwrite(frame.constData(), 1, want, stdout) != want || std::fflush(stdout) != 0) {
        return false;
    }
    return true;
}

}  // namespace

QJsonObject handleNativeMessage(const QJsonObject& request,
                                const QString& server_name,
                                const QString& server_version) {
    const QString type = request.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("ping")) {
        return pongReply(request, server_name, server_version);
    }
    // Later units add snapshot/click/type/... forwarding here. Until then an
    // unknown type is a clean, correlated error rather than a dropped message.
    return typedError(request, QStringLiteral("Unsupported message type: '%1'").arg(type));
}

int runNativeHostLoop(const QString& server_name, const QString& server_version) {
    while (true) {
        char header[kNativeFrameHeaderBytes];
        if (!readExact(header, kNativeFrameHeaderBytes)) {
            return 0;  // clean end-of-stream: the extension closed the port
        }
        // Validate the length prefix through the codec (a 4-byte buffer yields
        // NeedMore for an in-range length, Error for zero / over-cap) before
        // allocating the body, so a corrupt prefix cannot trigger a huge alloc.
        if (parseFrame(QByteArray(header, kNativeFrameHeaderBytes)).status ==
            NativeFrame::Status::Error) {
            return 1;  // out-of-range length: framing is unrecoverable
        }
        const int length =
            static_cast<int>(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header)));
        QByteArray frame(header, kNativeFrameHeaderBytes);
        QByteArray body(length, Qt::Uninitialized);
        if (!readExact(body.data(), length)) {
            return 1;  // truncated body: peer died mid-message
        }
        frame.append(body);
        const NativeFrame parsed = parseFrame(frame);
        if (parsed.status != NativeFrame::Status::Ok) {
            // Framing was valid but the body was not a JSON object; stay connected
            // and report the error so one bad message does not kill the session.
            if (!writeFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                                        {QStringLiteral("error"), parsed.error}})) {
                return 1;  // response write failed: stream desynchronized, close the port
            }
            continue;
        }
        if (!writeFrame(handleNativeMessage(parsed.message, server_name, server_version))) {
            return 1;  // response write failed: stream desynchronized, close the port
        }
    }
}

}  // namespace sak::win32mcp
