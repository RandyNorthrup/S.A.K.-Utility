// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file loopback_http_server.h
/// @brief A one-response HTTP/1.1 server on 127.0.0.1, for tests of real downloads.
///
/// runNetworkTransfer blocks the thread that calls it, so a QTcpServer on that thread could never
/// answer. This one is plain Winsock on its own thread. Its body is generated a block at a time as
/// blocking send() drains, so serving 64 MiB holds one 64 KiB block and allocates nothing through
/// Qt: a LargeAllocationCounter around a download sees only the client's allocations.

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

namespace sak::test {

class LoopbackHttpServer {
public:
    /// The byte at @p offset of every body this server sends: a fixed pattern, so a test can hash
    /// the expected content without holding it.
    [[nodiscard]] static char byteAt(qint64 offset) {
        constexpr qint64 kStride = 31;
        constexpr qint64 kOffset = 7;
        constexpr qint64 kByteMask = 0xFF;
        return static_cast<char>((offset * kStride + kOffset) & kByteMask);
    }

    /// SHA-256 of the first @p bytes of the pattern, computed a block at a time.
    [[nodiscard]] static QByteArray patternSha256(qint64 bytes) {
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        QByteArray block;
        for (qint64 done = 0; done < bytes; done += block.size()) {
            block.resize(static_cast<qsizetype>(std::min<qint64>(kBlockBytes, bytes - done)));
            for (qsizetype index = 0; index < block.size(); ++index) {
                block[index] = byteAt(done + index);
            }
            hasher.addData(block);
        }
        return hasher.result();
    }

    explicit LoopbackHttpServer(qint64 body_bytes) {
        WSADATA data{};
        m_started = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        m_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(address);
        if (m_listen == INVALID_SOCKET ||
            bind(m_listen, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
            listen(m_listen, 1) != 0 ||
            getsockname(m_listen, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            return;
        }
        m_port = ntohs(address.sin_port);
        m_thread = std::thread([this, body_bytes]() { serveOne(body_bytes); });
    }

    ~LoopbackHttpServer() {
        if (m_listen != INVALID_SOCKET) {
            closesocket(m_listen);  // ends an accept() still waiting
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_started) {
            WSACleanup();
        }
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer(LoopbackHttpServer&&) = delete;
    LoopbackHttpServer& operator=(LoopbackHttpServer&&) = delete;

    [[nodiscard]] bool listening() const { return m_port != 0; }

    [[nodiscard]] QUrl url(const QString& name) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/%2").arg(m_port).arg(name));
    }

private:
    static constexpr qsizetype kBlockBytes = 64 * 1024;
    static constexpr size_t kMaxRequestBytes = 64 * 1024;

    static bool sendAll(SOCKET client, const char* data, size_t size) {
        while (size > 0) {
            const int sent = send(client, data, static_cast<int>(size), 0);
            if (sent <= 0) {
                return false;  // the client hung up, as one that hit its cap does
            }
            data += sent;
            size -= static_cast<size_t>(sent);
        }
        return true;
    }

    static bool readRequest(SOCKET client) {
        std::string request;
        char buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < kMaxRequestBytes) {
            const int got = recv(client, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                return false;
            }
            request.append(buffer, static_cast<size_t>(got));
        }
        return true;
    }

    void serveOne(qint64 body_bytes) {
        const SOCKET client = accept(m_listen, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            return;
        }
        if (readRequest(client)) {
            const std::string header =
                "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
                std::to_string(body_bytes) + "\r\nConnection: close\r\n\r\n";
            std::vector<char> block(static_cast<size_t>(kBlockBytes));
            bool open = sendAll(client, header.data(), header.size());
            for (qint64 sent = 0; open && sent < body_bytes;) {
                const auto count =
                    static_cast<size_t>(std::min<qint64>(kBlockBytes, body_bytes - sent));
                for (size_t index = 0; index < count; ++index) {
                    block[index] = byteAt(sent + static_cast<qint64>(index));
                }
                open = sendAll(client, block.data(), count);
                sent += static_cast<qint64>(count);
            }
        }
        shutdown(client, SD_SEND);
        closesocket(client);
    }

    bool m_started{false};
    SOCKET m_listen{INVALID_SOCKET};
    quint16 m_port{0};
    std::thread m_thread;
};

}  // namespace sak::test
