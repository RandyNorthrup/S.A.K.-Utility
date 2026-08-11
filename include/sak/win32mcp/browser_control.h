// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/win32mcp/browser_bridge.h"
#include "sak/win32mcp/browser_bridge_pipe.h"
#include "sak/win32mcp/win32_mcp_tools.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include <utility>

/// @file browser_control.h
/// @brief The MCP-process facade that binds the browser-control session brain
/// (BrowserBridgeSession) to the pipe transport (BrowserBridgePipeServer) and exposes
/// them to the JSON-RPC dispatch as a set of `browser_*` tools.
///
/// It lives in the long-lived MCP process and is driven entirely from the single MCP
/// thread. The pipe server runs its own I/O thread and only publishes a connection
/// generation counter + a connected flag; this facade reconciles the single-threaded
/// session with that async connection state right before each tool call (a new
/// generation means a fresh browser, so the session drops any stale ref_index). One
/// browser tool call becomes: sync session <- connection, session.beginCommand (mint
/// id / enforce one-op / resolve refs), pipe.sendCommandAwaitReply (blocking transport),
/// session.onReply (single-winner correlation + snapshot install), then a ToolResult.
namespace sak::win32mcp {

class BrowserControl {
public:
    /// Production uses the default options (real per-user rendezvous path, Chrome
    /// ancestry required). Tests inject a temp rendezvous path and relax the ancestor
    /// check via the pipe server's Options.
    explicit BrowserControl(BrowserBridgePipeServer::Options options = {})
        : pipe_(std::move(options)) {}

    BrowserControl(const BrowserControl&) = delete;
    BrowserControl& operator=(const BrowserControl&) = delete;

    /// Start the pipe server (create the hardened pipe + publish the rendezvous
    /// record). Returns false with @p error set on failure; the MCP server then runs
    /// with browser control degraded (browser_* calls report "unavailable") while its
    /// other tools keep working.
    [[nodiscard]] bool start(QString* error);
    void stop();

    /// The browser tool definitions to merge into `tools/list`.
    [[nodiscard]] static QJsonArray toolCatalog();

    /// True iff @p name is one of the browser_* tools this facade owns.
    [[nodiscard]] static bool handles(const QString& name);

    /// True while a verified relay is connected (a browser is reachable).
    [[nodiscard]] bool clientConnected() const { return pipe_.clientConnected(); }

    /// Execute one browser tool call end-to-end and return the MCP text result. Any
    /// refusal (not connected / busy / stale ref / bad args) or transport failure comes
    /// back as an is_error ToolResult the model can read, never a throw.
    [[nodiscard]] ToolResult invoke(const QString& name, const QJsonObject& arguments);

private:
    void syncSessionToConnection();

    BrowserBridgePipeServer pipe_;
    browser::BrowserBridgeSession session_;
    quint64 observed_generation_{0};
    bool started_{false};
};

}  // namespace sak::win32mcp
