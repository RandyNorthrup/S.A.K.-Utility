// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest/QtTest>

using sak::win32mcp::browser::BrowserBridgeSession;

namespace {

// A minimal raw snapshot capture payload (the shape the extension returns and
// renderSnapshot consumes) with one interactable button.
QJsonObject snapshotPayload(int backendNodeId, const QString& name) {
    return QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.com/")},
                       {QStringLiteral("title"), QStringLiteral("Example")},
                       {QStringLiteral("nodes"),
                        QJsonArray{QJsonObject{{QStringLiteral("backendNodeId"), backendNodeId},
                                               {QStringLiteral("role"), QStringLiteral("button")},
                                               {QStringLiteral("name"), name},
                                               {QStringLiteral("interactable"), true},
                                               {QStringLiteral("visible"), true},
                                               {QStringLiteral("depth"), 0},
                                               {QStringLiteral("bounds"),
                                                QJsonObject{{QStringLiteral("x"), 0},
                                                            {QStringLiteral("y"), 0},
                                                            {QStringLiteral("width"), 80},
                                                            {QStringLiteral("height"), 20}}}}}}};
}

QJsonObject resultFrame(const QString& id, const QString& cmd, const QJsonObject& payload) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("cmd"), cmd},
                       {QStringLiteral("payload"), payload}};
}

}  // namespace

class BrowserBridgeTests : public QObject {
    Q_OBJECT

private slots:
    void beginCommand_refusedWhenNotConnected();
    void beginCommand_mintsCommandFrameWithTypeAndId();
    void beginCommand_refusesSecondWhileOutstanding();
    void onReply_nonMatchingIdIsDroppedKeepingOutstanding();
    void onReply_lateReplyAfterTimeoutIsDropped();
    void snapshot_populatesRefIndexAndClickResolvesBackendNodeId();
    void onReply_errorFrameSurfacesMessageAndRetires();
    void hostReconnect_clearsRefIndexSoStaleRefFails();
    void onDetached_refusesRefActionButAllowsSnapshot();
    void onReply_unexpectedTypeIsError();
    void reply_forgedSnapshotCmdCannotInstallRefIndex();
    void detachDuringSnapshot_replyDoesNotInstallRefIndex();
    void navigationInvalidatesRefsFromPriorSnapshot();
};

void BrowserBridgeTests::beginCommand_refusedWhenNotConnected() {
    BrowserBridgeSession session;
    const auto out = session.beginCommand(QStringLiteral("browser_navigate"),
                                          QJsonObject{{QStringLiteral("url"),
                                                       QStringLiteral("https://example.com/")}});
    QVERIFY(!out.ok);
    QVERIFY(out.error.contains(QStringLiteral("not connected")));
    QVERIFY(!session.hasOutstanding());
}

void BrowserBridgeTests::beginCommand_mintsCommandFrameWithTypeAndId() {
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto out = session.beginCommand(QStringLiteral("browser_navigate"),
                                          QJsonObject{{QStringLiteral("url"),
                                                       QStringLiteral("https://example.com/")}});
    QVERIFY(out.ok);
    QCOMPARE(out.frame.value(QStringLiteral("type")).toString(), QStringLiteral("command"));
    QCOMPARE(out.frame.value(QStringLiteral("cmd")).toString(), QStringLiteral("navigate"));
    QCOMPARE(out.frame.value(QStringLiteral("url")).toString(),
             QStringLiteral("https://example.com/"));
    QCOMPARE(out.frame.value(QStringLiteral("id")).toString(), QStringLiteral("b-1"));
    QCOMPARE(session.outstandingId(), QStringLiteral("b-1"));
}

void BrowserBridgeTests::beginCommand_refusesSecondWhileOutstanding() {
    BrowserBridgeSession session;
    session.onHostConnected();
    QVERIFY(session.beginCommand(QStringLiteral("browser_snapshot"), {}).ok);
    const auto second = session.beginCommand(QStringLiteral("browser_reload"), {});
    QVERIFY(!second.ok);
    QVERIFY(second.error.contains(QStringLiteral("already in progress")));
}

void BrowserBridgeTests::onReply_nonMatchingIdIsDroppedKeepingOutstanding() {
    BrowserBridgeSession session;
    session.onHostConnected();
    (void)session.beginCommand(QStringLiteral("browser_reload"), {});  // b-1 outstanding
    const auto in = session.onReply(
        resultFrame(QStringLiteral("b-999"), QStringLiteral("reload"), QJsonObject{}));
    QVERIFY(!in.matched);
    QVERIFY(session.hasOutstanding());  // still waiting for b-1
    QCOMPARE(session.outstandingId(), QStringLiteral("b-1"));
}

void BrowserBridgeTests::onReply_lateReplyAfterTimeoutIsDropped() {
    BrowserBridgeSession session;
    session.onHostConnected();
    (void)session.beginCommand(QStringLiteral("browser_reload"), {});  // b-1
    session.retireOutstanding();                                       // timeout fired
    QVERIFY(!session.hasOutstanding());
    // A late reply for the retired b-1 must not match.
    const auto late = session.onReply(
        resultFrame(QStringLiteral("b-1"), QStringLiteral("reload"), QJsonObject{}));
    QVERIFY(!late.matched);
}

void BrowserBridgeTests::snapshot_populatesRefIndexAndClickResolvesBackendNodeId() {
    BrowserBridgeSession session;
    session.onHostConnected();

    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    QVERIFY(snap.ok);
    const auto snapReply =
        session.onReply(resultFrame(snap.frame.value(QStringLiteral("id")).toString(),
                                    QStringLiteral("snapshot"),
                                    snapshotPayload(4242, QStringLiteral("Sign in"))));
    QVERIFY(snapReply.matched);
    QVERIFY(!snapReply.is_error);
    QVERIFY(snapReply.text.contains(QStringLiteral("[ref=e1]")));
    QVERIFY(snapReply.text.contains(QStringLiteral("Sign in")));
    QVERIFY(session.refIndex().contains(QStringLiteral("e1")));

    // Now a click by that ref resolves to the stored backendNodeId.
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(click.ok);
    QCOMPARE(click.frame.value(QStringLiteral("cmd")).toString(), QStringLiteral("click"));
    QCOMPARE(click.frame.value(QStringLiteral("backendNodeId")).toInt(), 4242);
    QCOMPARE(click.frame.value(QStringLiteral("id")).toString(), QStringLiteral("b-2"));
}

void BrowserBridgeTests::onReply_errorFrameSurfacesMessageAndRetires() {
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto out = session.beginCommand(QStringLiteral("browser_reload"), {});
    const auto in = session.onReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                    {QStringLiteral("id"), out.frame.value(QStringLiteral("id"))},
                    {QStringLiteral("message"), QStringLiteral("cdp_attach_failed")}});
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QCOMPARE(in.error, QStringLiteral("cdp_attach_failed"));
    QVERIFY(!session.hasOutstanding());  // retired on match
}

void BrowserBridgeTests::hostReconnect_clearsRefIndexSoStaleRefFails() {
    BrowserBridgeSession session;
    session.onHostConnected();
    (void)session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrame(QStringLiteral("b-1"),
                                      QStringLiteral("snapshot"),
                                      snapshotPayload(7, QStringLiteral("Go"))));
    QVERIFY(session.refIndex().contains(QStringLiteral("e1")));

    // A fresh host connect is a new CDP session: the old refs must be gone.
    session.onHostConnected();
    QVERIFY(session.refIndex().isEmpty());
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(!click.ok);
    QVERIFY(
        click.error.contains(QStringLiteral("snapshot")));  // "call browser_snapshot to refresh"
}

void BrowserBridgeTests::onDetached_refusesRefActionButAllowsSnapshot() {
    BrowserBridgeSession session;
    session.onHostConnected();
    (void)session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrame(QStringLiteral("b-1"),
                                      QStringLiteral("snapshot"),
                                      snapshotPayload(7, QStringLiteral("Go"))));

    session.onDetached();
    QVERIFY(session.refIndexStale());
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(!click.ok);
    QVERIFY(click.error.contains(QStringLiteral("page changed")));

    // A fresh snapshot is still allowed and clears the stale flag.
    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    QVERIFY(snap.ok);
    (void)session.onReply(resultFrame(snap.frame.value(QStringLiteral("id")).toString(),
                                      QStringLiteral("snapshot"),
                                      snapshotPayload(9, QStringLiteral("Next"))));
    QVERIFY(!session.refIndexStale());
}

void BrowserBridgeTests::onReply_unexpectedTypeIsError() {
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto out = session.beginCommand(QStringLiteral("browser_reload"), {});
    const auto in =
        session.onReply(QJsonObject{{QStringLiteral("type"), QStringLiteral("welcome")},
                                    {QStringLiteral("id"), out.frame.value(QStringLiteral("id"))}});
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY(in.error.contains(QStringLiteral("Unexpected reply type")));
}

void BrowserBridgeTests::reply_forgedSnapshotCmdCannotInstallRefIndex() {
    // A reply that self-declares cmd:"snapshot" for a command the session sent as a
    // NON-snapshot must not install ref_index -- state keys off the sent cmd, not the
    // untrusted reply frame. (Prompt-injection guard.)
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto nav = session.beginCommand(QStringLiteral("browser_navigate"),
                                          QJsonObject{{QStringLiteral("url"),
                                                       QStringLiteral("https://example.com/")}});
    QVERIFY(nav.ok);
    const auto in = session.onReply(resultFrame(nav.frame.value(QStringLiteral("id")).toString(),
                                                QStringLiteral("snapshot"),
                                                snapshotPayload(9999, QStringLiteral("Cancel"))));
    QVERIFY(in.matched);
    QVERIFY(session.refIndex().isEmpty());  // forged snapshot payload NOT installed
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(!click.ok);  // no such ref exists
}

void BrowserBridgeTests::detachDuringSnapshot_replyDoesNotInstallRefIndex() {
    // A detach while a snapshot is in flight must invalidate that reply: it captured a
    // now-dead DOM, so its ref_index must not be installed and refs stay refused.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    session.onDetached();  // DOM changed while the snapshot was being taken
    const auto in = session.onReply(resultFrame(snap.frame.value(QStringLiteral("id")).toString(),
                                                QStringLiteral("snapshot"),
                                                snapshotPayload(7, QStringLiteral("Go"))));
    QVERIFY(in.matched);
    QVERIFY(session.refIndex().isEmpty());  // dead-DOM snapshot not trusted
    QVERIFY(session.refIndexStale());
    QVERIFY(in.text.contains(QStringLiteral("changed")));
}

void BrowserBridgeTests::navigationInvalidatesRefsFromPriorSnapshot() {
    // Programmatic navigation keeps CDP attached (no onDetached), but refs from the
    // prior page must still be refused until the next snapshot.
    BrowserBridgeSession session;
    session.onHostConnected();
    (void)session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrame(QStringLiteral("b-1"),
                                      QStringLiteral("snapshot"),
                                      snapshotPayload(100, QStringLiteral("Old"))));
    QVERIFY(session.refIndex().contains(QStringLiteral("e1")));
    QVERIFY(!session.refIndexStale());

    const auto nav = session.beginCommand(QStringLiteral("browser_navigate"),
                                          QJsonObject{{QStringLiteral("url"),
                                                       QStringLiteral("https://other.example/")}});
    (void)session.onReply(resultFrame(nav.frame.value(QStringLiteral("id")).toString(),
                                      QStringLiteral("navigate"),
                                      QJsonObject{{QStringLiteral("url"),
                                                   QStringLiteral("https://other.example/")}}));
    QVERIFY(session.refIndexStale());  // navigation invalidated the old refs
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(!click.ok);
    QVERIFY(click.error.contains(QStringLiteral("page changed")));
}

QTEST_MAIN(BrowserBridgeTests)
#include "test_browser_bridge.moc"
