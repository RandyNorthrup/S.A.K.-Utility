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

void writeFrame(const QJsonObject& message) {
    const QByteArray frame = encodeFrame(message);
    std::fwrite(frame.constData(), 1, static_cast<size_t>(frame.size()), stdout);
    std::fflush(stdout);
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
        char header[4];
        if (!readExact(header, 4)) {
            return 0;  // clean end-of-stream: the extension closed the port
        }
        // Validate the length prefix through the codec (a 4-byte buffer yields
        // NeedMore for an in-range length, Error for zero / over-cap) before
        // allocating the body, so a corrupt prefix cannot trigger a huge alloc.
        if (parseFrame(QByteArray(header, 4)).status == NativeFrame::Status::Error) {
            return 1;  // out-of-range length: framing is unrecoverable
        }
        const int length =
            static_cast<int>(qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header)));
        QByteArray frame(header, 4);
        QByteArray body(length, Qt::Uninitialized);
        if (!readExact(body.data(), length)) {
            return 1;  // truncated body: peer died mid-message
        }
        frame.append(body);
        const NativeFrame parsed = parseFrame(frame);
        if (parsed.status != NativeFrame::Status::Ok) {
            // Framing was valid but the body was not a JSON object; stay connected
            // and report the error so one bad message does not kill the session.
            writeFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                                   {QStringLiteral("error"), parsed.error}});
            continue;
        }
        writeFrame(handleNativeMessage(parsed.message, server_name, server_version));
    }
}

}  // namespace sak::win32mcp
