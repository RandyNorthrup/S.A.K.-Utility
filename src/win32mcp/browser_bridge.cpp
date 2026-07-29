// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge.h"

#include "sak/win32mcp/browser_contract.h"

#include <QJsonDocument>

namespace sak::win32mcp::browser {

namespace {

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString formatSnapshot(const SnapshotView& view) {
    const QString body = view.outline.isEmpty() ? QStringLiteral("(no interactable elements found)")
                                                : view.outline;
    return QStringLiteral("url: %1\ntitle: %2\nelements: %3\n%4")
        .arg(view.url, view.title)
        .arg(view.element_count)
        .arg(body);
}

// Commands that land the browser on a new document or a different tab, after which
// any ref from a prior snapshot is meaningless. Programmatic navigation keeps CDP
// attached (no onDetached fires), so the bridge must invalidate refs itself.
bool isNavigationCmd(const QString& cmd) {
    return cmd == QLatin1String("navigate") || cmd == QLatin1String("back") ||
           cmd == QLatin1String("forward") || cmd == QLatin1String("reload") ||
           cmd == QLatin1String("selectTab") || cmd == QLatin1String("newTab") ||
           cmd == QLatin1String("closeTab");
}

}  // namespace

void BrowserBridgeSession::onHostConnected() {
    connected_ = true;
    ref_index_ = QJsonObject{};
    ref_index_stale_ = false;
    outstanding_id_.clear();
    outstanding_cmd_.clear();
    ++session_epoch_;  // new browser session: any in-flight reply is for a dead one
}

void BrowserBridgeSession::onHostDisconnected() {
    connected_ = false;
    ref_index_ = QJsonObject{};
    ref_index_stale_ = false;
    outstanding_id_.clear();
    outstanding_cmd_.clear();
    ++session_epoch_;
}

void BrowserBridgeSession::onDetached() {
    ref_index_stale_ = true;
    // A snapshot already in flight was captured against the now-detached DOM; move the
    // epoch so its reply cannot install ref_index or clear the stale flag.
    ++session_epoch_;
}

BrowserBridgeSession::Outgoing BrowserBridgeSession::refuse(const QString& reason) const {
    return {QJsonObject{}, false, reason};
}

BrowserBridgeSession::Outgoing BrowserBridgeSession::beginCommand(const QString& tool,
                                                                  const QJsonObject& arguments) {
    if (!connected_) {
        return refuse(QStringLiteral(
            "Browser not connected: the S.A.K. browser-control extension is not attached."));
    }
    if (!outstanding_id_.isEmpty()) {
        return refuse(QStringLiteral("A browser action is already in progress."));
    }
    if (ref_index_stale_ && arguments.contains(QStringLiteral("ref"))) {
        return refuse(
            QStringLiteral("The page changed since the last snapshot; call browser_snapshot "
                           "again before acting on an element."));
    }
    const ExtensionCommand command = buildExtensionCommand(tool, arguments, ref_index_);
    if (!command.ok) {
        return refuse(command.error);
    }
    const QString id = QStringLiteral("b-%1").arg(++counter_);
    Outgoing out;
    out.ok = true;
    out.frame = command.command;
    out.frame.insert(QStringLiteral("type"), QStringLiteral("command"));
    out.frame.insert(QStringLiteral("id"), id);
    outstanding_id_ = id;
    // Record the cmd WE sent and the epoch it was issued under; state mutations on the
    // reply are keyed to these, never to the untrusted reply frame.
    outstanding_cmd_ = command.command.value(QStringLiteral("cmd")).toString();
    outstanding_epoch_ = session_epoch_;
    return out;
}

void BrowserBridgeSession::fillResult(const QString& sent_cmd,
                                      quint64 sent_epoch,
                                      const QJsonObject& frame,
                                      Incoming& incoming) {
    const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
    // Only a reply to a snapshot WE issued may install ref_index -- and only if the
    // browser session has not changed underneath it (no detach/reconnect since we
    // asked), so a snapshot of a now-dead DOM is never trusted for act-by-ref.
    if (sent_cmd == QLatin1String("snapshot")) {
        if (sent_epoch != session_epoch_) {
            incoming.text = QStringLiteral(
                "The page changed while the snapshot was being taken; call browser_snapshot "
                "again.");
            return;
        }
        const SnapshotView view = renderSnapshot(payload);
        ref_index_ = view.ref_index;
        ref_index_stale_ = false;
        incoming.text = formatSnapshot(view);
        return;
    }
    // After a navigation/tab-change lands, refs from a prior snapshot no longer map to
    // live nodes; refuse act-by-ref until the next snapshot.
    if (isNavigationCmd(sent_cmd)) {
        ref_index_stale_ = true;
    }
    incoming.text = compactJson(payload);
}

BrowserBridgeSession::Incoming BrowserBridgeSession::onReply(const QJsonObject& frame) {
    Incoming incoming;
    const QString id = frame.value(QStringLiteral("id")).toString();
    // A frame that does not carry the outstanding id is a late reply from a retired
    // op, an unsolicited event, or a stray -- drop it, never mis-pair it.
    if (outstanding_id_.isEmpty() || id != outstanding_id_) {
        return incoming;  // matched == false
    }
    const QString sent_cmd = outstanding_cmd_;
    const quint64 sent_epoch = outstanding_epoch_;
    outstanding_id_.clear();  // single-winner retire
    outstanding_cmd_.clear();
    incoming.matched = true;

    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("result")) {
        fillResult(sent_cmd, sent_epoch, frame, incoming);
        return incoming;
    }
    incoming.is_error = true;
    if (type == QLatin1String("error")) {
        QString message = frame.value(QStringLiteral("message")).toString();
        if (message.isEmpty()) {
            message = frame.value(QStringLiteral("error")).toString();
        }
        incoming.error = message.isEmpty() ? QStringLiteral("Browser command failed.") : message;
    } else {
        incoming.error = QStringLiteral("Unexpected reply type '%1'.").arg(type);
    }
    return incoming;
}

}  // namespace sak::win32mcp::browser
