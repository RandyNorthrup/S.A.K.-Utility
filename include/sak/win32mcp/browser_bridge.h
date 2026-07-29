// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

/// @file browser_bridge.h
/// @brief The browser-control session state machine that lives in the long-lived
/// MCP process (the pipe-server / authority side of the bridge).
///
/// Topology (settled by the design panel): the long-lived MCP process owns the
/// browser state and drives commands; the Chrome-spawned native host is a thin,
/// id-transparent relay; the extension holds the live CDP session. This class is
/// the pure "brain" on the MCP side -- it mints correlation ids, enforces one
/// outstanding op, matches replies single-winner (so a late reply from a timed-out
/// op can never be mis-paired to the next one), and owns the ref_index produced by
/// renderSnapshot so a later act-by-ref resolves in the process that outlives the
/// ephemeral relay. It performs no I/O and knows nothing about named pipes or
/// Chrome, so the whole protocol is unit-testable with JSON frames.
namespace sak::win32mcp::browser {

class BrowserBridgeSession {
public:
    /// A command frame to send to the extension (via the relay), or a refusal.
    struct Outgoing {
        QJsonObject frame;  ///< The `{type:"command", id, cmd, ...}` frame (valid iff ok).
        bool ok{false};
        QString error;      ///< Why the call was refused (not connected / busy / bad ref).
    };

    /// The outcome of feeding a reply frame back in.
    struct Incoming {
        bool matched{false};   ///< True iff this frame answered the outstanding op.
        bool is_error{false};  ///< True iff the matched reply was an error.
        QString text;          ///< Tool-facing result text (snapshot outline or payload JSON).
        QString error;         ///< Error message when is_error.
        QString image_base64;  ///< Base64 image for a screenshot reply (empty otherwise).
        QString image_mime;    ///< MIME type for image_base64 (e.g. "image/png").
    };

    /// A new relay has connected: this is a fresh browser/CDP session, so any stored
    /// ref_index is from a dead session and is cleared, and any outstanding op is
    /// abandoned.
    void onHostConnected();

    /// The relay disconnected (Chrome closed, extension unloaded): no browser is
    /// reachable until a new host connects.
    void onHostDisconnected();

    /// CDP detached from the tab (user opened DevTools, or the tab navigated/closed):
    /// the current ref_index no longer maps to live nodes, so act-by-ref is refused
    /// until the next snapshot.
    void onDetached();

    [[nodiscard]] bool connected() const { return connected_; }

    /// Build the command frame for a browser_* tool call, minting and recording the
    /// outstanding correlation id. Refuses (ok == false) when no browser is
    /// connected, an op is already in flight, the snapshot is stale for a ref-based
    /// action, or buildExtensionCommand rejects the arguments.
    [[nodiscard]] Outgoing beginCommand(const QString& tool, const QJsonObject& arguments);

    /// Feed a reply frame from the extension. A frame whose id does not match the
    /// outstanding op is dropped (matched == false) -- this is how a late reply from
    /// a previously timed-out op is discarded rather than mis-paired. On a match the
    /// op is retired and, for a snapshot result, the ref_index is refreshed.
    [[nodiscard]] Incoming onReply(const QJsonObject& frame);

    [[nodiscard]] bool hasOutstanding() const { return !outstanding_id_.isEmpty(); }
    [[nodiscard]] QString outstandingId() const { return outstanding_id_; }

    /// Abandon the outstanding op (called by the I/O layer on a read timeout). A
    /// subsequent reply carrying the retired id will no longer match.
    void retireOutstanding() { outstanding_id_.clear(); }

    [[nodiscard]] const QJsonObject& refIndex() const { return ref_index_; }
    [[nodiscard]] bool refIndexStale() const { return ref_index_stale_; }

private:
    Outgoing refuse(const QString& reason) const;
    void fillResult(const QString& sent_cmd,
                    quint64 sent_epoch,
                    const QJsonObject& frame,
                    Incoming& incoming);
    static void fillScreenshotResult(const QJsonObject& payload, Incoming& incoming);

    bool connected_{false};
    bool ref_index_stale_{false};
    quint64 counter_{0};
    // Bumped whenever the browser/DOM identity changes underneath an in-flight op
    // (host connect, host disconnect, CDP detach). A command records the epoch it was
    // issued under; a reply that arrives after the epoch moved cannot install snapshot
    // state, so a snapshot captured against a now-dead DOM is not trusted.
    quint64 session_epoch_{0};
    quint64 outstanding_epoch_{0};
    QString outstanding_id_;
    // The cmd the session authoritatively sent for the outstanding op. State mutations
    // (snapshot install, navigation invalidation) branch on THIS, never on the reply
    // frame's self-declared cmd, which is untrusted (web-page-derived).
    QString outstanding_cmd_;
    QJsonObject ref_index_;
};

}  // namespace sak::win32mcp::browser
