// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_control.h"

#include "sak/win32mcp/browser_contract.h"

namespace sak::win32mcp {

bool BrowserControl::start(QString* error) {
    started_ = pipe_.start(error);
    return started_;
}

void BrowserControl::stop() {
    pipe_.stop();
    started_ = false;
}

QJsonArray BrowserControl::toolCatalog() const {
    return browser::browserToolCatalog();
}

bool BrowserControl::handles(const QString& name) const {
    return name.startsWith(QStringLiteral("browser_"));
}

// Reconcile the single-threaded session with the pipe server's async connection state.
// The server bumps its generation on every newly verified relay; observing a new
// generation means a fresh browser session (the old ref_index is dead), and losing the
// connection means no browser is reachable. Called on the MCP thread immediately before
// each tool call, so the session never acts on a connection it has not accounted for.
void BrowserControl::syncSessionToConnection() {
    const bool connected = pipe_.clientConnected();
    const quint64 generation = pipe_.connectionGeneration();
    if (connected && generation != observed_generation_) {
        session_.onHostConnected();
        observed_generation_ = generation;
    } else if (!connected && session_.connected()) {
        session_.onHostDisconnected();
    }
}

ToolResult BrowserControl::invoke(const QString& name, const QJsonObject& arguments) {
    if (!started_) {
        return {QStringLiteral("Browser control is unavailable: the bridge pipe failed to start."),
                true};
    }
    syncSessionToConnection();
    const browser::BrowserBridgeSession::Outgoing outgoing = session_.beginCommand(name, arguments);
    if (!outgoing.ok) {
        return {outgoing.error, true};
    }
    const BrowserBridgePipeServer::Exchange exchange = pipe_.sendCommandAwaitReply(outgoing.frame);
    if (!exchange.ok) {
        // The transport failed (no relay, or a deadline/reset). Retire the outstanding
        // op so a late-arriving reply for it can never be mis-paired to the next call.
        session_.retireOutstanding();
        return {exchange.error, true};
    }
    const browser::BrowserBridgeSession::Incoming incoming = session_.onReply(exchange.reply);
    if (!incoming.matched) {
        return {QStringLiteral("The browser reply did not correlate to the request."), true};
    }
    if (incoming.is_error) {
        return {incoming.error, true};
    }
    return {incoming.text, false};
}

}  // namespace sak::win32mcp
