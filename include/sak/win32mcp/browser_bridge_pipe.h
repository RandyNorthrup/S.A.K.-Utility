// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

#include <condition_variable>
#include <mutex>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/// @file browser_bridge_pipe.h
/// @brief The browser-control bridge pipe SERVER, hosted in the long-lived MCP
/// process (the authority end).
///
/// It owns the hardened named pipe (see browser_bridge_security), publishes the
/// rendezvous record, and runs one dedicated I/O thread that: waits for the
/// Chrome-spawned relay to connect, verifies the peer by code identity (the client
/// must be our own executable and, in production, launched by Chrome), completes a
/// token/protocol handshake, then serves exactly one browser command at a time from
/// the MCP thread over overlapped, deadline-bounded reads/writes. All pipe-handle
/// I/O lives on that single thread, so there are no cross-thread handle races; the
/// MCP thread hands a command frame across and waits for the reply.
///
/// The server is deliberately decoupled from BrowserBridgeSession: it exposes a
/// monotonically increasing connection generation instead of calling the (single-
/// threaded) session directly, so the MCP dispatch can observe a fresh relay and
/// drive onHostConnected/onHostDisconnected on its own thread.
namespace sak::win32mcp {

class BrowserBridgePipeServer {
public:
    struct Options {
        int protocol{1};                     ///< kBrowserBridgeProtocol.
        bool require_chrome_ancestor{true};  ///< Production; tests relax this.
        DWORD io_timeout_ms{30'000};         ///< Per read/write deadline.
        QString rendezvous_path;             ///< Empty -> the real per-user location.
    };

    /// The result of one command/reply exchange.
    struct Exchange {
        bool ok{false};
        QJsonObject reply;
        QString error;
    };

    explicit BrowserBridgePipeServer(Options options = {});
    ~BrowserBridgePipeServer();

    BrowserBridgePipeServer(const BrowserBridgePipeServer&) = delete;
    BrowserBridgePipeServer& operator=(const BrowserBridgePipeServer&) = delete;

    /// Create the hardened pipe, write the rendezvous record, and start the I/O
    /// thread. Returns false (fail-closed) with @p error set if the pipe cannot be
    /// created -- notably if the name already exists (squatting), since it uses
    /// FILE_FLAG_FIRST_PIPE_INSTANCE.
    [[nodiscard]] bool start(QString* error);

    /// Signal shutdown, abort any in-flight I/O, join the thread, close handles, and
    /// remove the rendezvous record. Safe to call more than once.
    void stop();

    /// True while a verified relay is connected.
    [[nodiscard]] bool clientConnected() const;

    /// Increments on every newly verified relay connection. The MCP dispatch compares
    /// it against the last value it saw to detect a fresh browser session.
    [[nodiscard]] quint64 connectionGeneration() const;

    /// Send one command frame and block for the correlated reply. Call from a single
    /// thread only (the MCP thread); the server serves one op at a time. Returns
    /// ok == false with a reason when no relay is connected, on a deadline (the
    /// connection is reset), or on a framing error.
    [[nodiscard]] Exchange sendCommandAwaitReply(const QJsonObject& frame);

    [[nodiscard]] QString pipeName() const { return pipe_name_; }
    [[nodiscard]] QString token() const { return token_; }

private:
    void run();
    bool handshake();
    void serveConnected();
    bool verifyPeer(QString* why) const;

    Options options_;
    QString pipe_name_;
    QString token_;
    QString rendezvous_path_;

    HANDLE pipe_{INVALID_HANDLE_VALUE};
    HANDLE shutdown_event_{nullptr};
    std::thread thread_;
    std::once_flag stop_once_;  // single-winner teardown, even under concurrent stop()

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool running_{false};
    bool connected_{false};
    quint64 generation_{0};

    // Single in-flight command slot handed from the MCP thread to the I/O thread.
    bool has_request_{false};
    bool has_response_{false};
    QJsonObject request_frame_;
    Exchange response_;
};

}  // namespace sak::win32mcp
