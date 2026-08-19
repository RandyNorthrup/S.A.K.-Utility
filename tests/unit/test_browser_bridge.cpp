// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge.h"

#include <QByteArray>
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

// A screenshot reply payload: base64 image bytes (the bridge reads dimensions from the
// PNG header itself, not from any payload-declared size).
QJsonObject screenshotPayload(const QString& data) {
    return QJsonObject{{QStringLiteral("data"), data},
                       {QStringLiteral("mimeType"), QStringLiteral("image/png")}};
}

// Base64 of a valid PNG header (8-byte signature + IHDR chunk with the given dimensions),
// enough for the bridge to read width/height from the image.
QString pngHeaderBase64(int width, int height) {
    QByteArray bytes;
    const unsigned char signature[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    bytes.append(reinterpret_cast<const char*>(signature), 8);
    const unsigned char ihdr_len[4] = {0x00, 0x00, 0x00, 0x0D};
    bytes.append(reinterpret_cast<const char*>(ihdr_len), 4);
    bytes.append("IHDR", 4);
    const auto append_be32 = [&bytes](int value) {
        bytes.append(static_cast<char>((value >> 24) & 0xFF));
        bytes.append(static_cast<char>((value >> 16) & 0xFF));
        bytes.append(static_cast<char>((value >> 8) & 0xFF));
        bytes.append(static_cast<char>(value & 0xFF));
    };
    append_be32(width);
    append_be32(height);
    bytes.append(8, '\0');  // bit depth / color type / etc. -- unread, just pads the prefix
    return QString::fromLatin1(bytes.toBase64());
}

QJsonObject resultFrame(const QString& id, const QString& cmd, const QJsonObject& payload) {
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                       {QStringLiteral("id"), id},
                       {QStringLiteral("cmd"), cmd},
                       {QStringLiteral("payload"), payload}};
}

// A result frame carrying the extension's per-reply DOM-generation marker (domEpoch).
QJsonObject resultFrameEpoch(const QString& id,
                             const QString& cmd,
                             const QJsonObject& payload,
                             int domEpoch) {
    QJsonObject frame = resultFrame(id, cmd, payload);
    frame.insert(QStringLiteral("domEpoch"), domEpoch);
    return frame;
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
    void onDetached_refusesDragByToRefEndpoint();
    void externalNavigationViaDomEpochInvalidatesRefs();
    void onReply_unexpectedTypeIsError();
    void reply_forgedSnapshotCmdCannotInstallRefIndex();
    void detachDuringSnapshot_replyDoesNotInstallRefIndex();
    void navigationInvalidatesRefsFromPriorSnapshot();
    void screenshotReply_populatesImageChannelWithDimsFromPngHeader();
    void screenshotReply_nonPngFallsBackToPlainSummary();
    void screenshotReply_emptyDataIsHonestError();
    void screenshotReply_oversizeBase64IsRejected();
    void onReply_resultWithOkFalseIsError();
    void onReply_okFalseSnapshotDoesNotInstallRefIndex();
    void screenshotReply_malformedBase64IsError();
    void reconcileDomEpoch_malformedEpochMarksRefsStale();
    void reconcileDomEpoch_omittedEpochAfterBaselineMarksRefsStale();  // R5-G10-9
    void printReply_nonPdfDataIsRefused();                             // R5-G10-9
    void screenshotReply_hostileMimeTypeCoercedToPng();
    void genericReply_oversizeTextIsRejected();
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

void BrowserBridgeTests::externalNavigationViaDomEpochInvalidatesRefs() {
    // B13-04: the extension stamps a domEpoch on every reply; a later reply whose epoch differs
    // from the snapshot's baseline means the DOM navigated externally (no navigation command from
    // us), so ref_index must be invalidated -- the production wiring of onDetached().
    BrowserBridgeSession session;
    session.onHostConnected();

    // Snapshot at DOM epoch 1 -> ref_index installed, baseline epoch 1, not stale.
    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrameEpoch(snap.frame.value(QStringLiteral("id")).toString(),
                                           QStringLiteral("snapshot"),
                                           snapshotPayload(4242, QStringLiteral("Sign in")),
                                           1));
    QVERIFY(!session.refIndexStale());

    // A later NON-snapshot reply reports a HIGHER epoch: the page navigated out from under us.
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    (void)session.onReply(resultFrameEpoch(shot.frame.value(QStringLiteral("id")).toString(),
                                           QStringLiteral("screenshot"),
                                           screenshotPayload(pngHeaderBase64(10, 10)),
                                           2));
    QVERIFY(session.refIndexStale());

    // A ref-based action is now refused until a fresh snapshot re-baselines the DOM.
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(!click.ok);
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

void BrowserBridgeTests::onDetached_refusesDragByToRefEndpoint() {
    // browser_drag targets a second element via to_ref. onDetached() marks the snapshot stale
    // but leaves ref_index populated, so a drag carrying ONLY to_ref would resolve against a
    // now-dead DOM. The stale gate must refuse an action carrying EITHER ref, not just ref.
    BrowserBridgeSession session;
    session.onHostConnected();
    (void)session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrame(QStringLiteral("b-1"),
                                      QStringLiteral("snapshot"),
                                      snapshotPayload(7, QStringLiteral("Go"))));
    QVERIFY(session.refIndex().contains(QStringLiteral("e1")));

    session.onDetached();
    QVERIFY(session.refIndexStale());
    // to_ref=e1 is still present in ref_index, but the page changed: refuse it.
    const auto drag =
        session.beginCommand(QStringLiteral("browser_drag"),
                             QJsonObject{{QStringLiteral("to_ref"), QStringLiteral("e1")}});
    QVERIFY(!drag.ok);
    QVERIFY(drag.error.contains(QStringLiteral("page changed")));
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

void BrowserBridgeTests::screenshotReply_populatesImageChannelWithDimsFromPngHeader() {
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    QVERIFY(shot.ok);
    QCOMPARE(shot.frame.value(QStringLiteral("cmd")).toString(), QStringLiteral("screenshot"));

    const QString png = pngHeaderBase64(1280, 720);
    const auto in = session.onReply(resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                                QStringLiteral("screenshot"),
                                                screenshotPayload(png)));
    QVERIFY(in.matched);
    QVERIFY(!in.is_error);
    // The image rides the dedicated channel, NOT the text field (which is only a summary).
    QCOMPARE(in.image_base64, png);
    QCOMPARE(in.image_mime, QStringLiteral("image/png"));
    // Dimensions are read from the PNG's own IHDR, so they are the true device pixels.
    QVERIFY(in.text.contains(QStringLiteral("1280x720")));
    QVERIFY(!in.text.contains(png));  // the base64 is never dumped into text
}

void BrowserBridgeTests::screenshotReply_nonPngFallsBackToPlainSummary() {
    // A payload whose header is not a readable PNG still returns the image, but the summary
    // omits dimensions instead of inventing them.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    const QString data = QString::fromLatin1(QByteArray("not-a-real-png-payload").toBase64());
    const auto in = session.onReply(resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                                QStringLiteral("screenshot"),
                                                screenshotPayload(data)));
    QVERIFY(in.matched);
    QVERIFY(!in.is_error);
    QCOMPARE(in.image_base64, data);
    QCOMPARE(in.text, QStringLiteral("Captured a PNG screenshot of the active tab."));
}

void BrowserBridgeTests::screenshotReply_emptyDataIsHonestError() {
    // A capture that yields no bytes is a FAILED read, not an empty-success -- it must
    // surface as an error, never as a blank image the model would treat as real.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    const auto in = session.onReply(resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                                QStringLiteral("screenshot"),
                                                screenshotPayload(QString())));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY(in.image_base64.isEmpty());
    QVERIFY(in.error.contains(QStringLiteral("empty")));
}

void BrowserBridgeTests::screenshotReply_oversizeBase64IsRejected() {
    // A full-page capture is page-influenced in size; a payload over the cap is refused
    // with an honest error instead of flooding the transport / model context.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    const QString huge(16 * 1024 * 1024 + 1, QLatin1Char('A'));
    const auto in = session.onReply(resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                                QStringLiteral("screenshot"),
                                                screenshotPayload(huge)));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY(in.image_base64.isEmpty());
    QVERIFY(in.error.contains(QStringLiteral("too large")));
}

void BrowserBridgeTests::screenshotReply_hostileMimeTypeCoercedToPng() {
    // R5-G10-9: the extension-supplied mimeType is untrusted (a page-influenced reply could
    // carry "text/html" or a script type). sanitizedImageMime must coerce anything that is not
    // one of the image types WE emit down to image/png, so the returned image block can never be
    // turned into an arbitrary content type. The image itself still rides the dedicated channel.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    const QString png = pngHeaderBase64(10, 10);
    const QJsonObject hostile{{QStringLiteral("data"), png},
                              {QStringLiteral("mimeType"), QStringLiteral("text/html")}};
    const auto in = session.onReply(resultFrame(
        shot.frame.value(QStringLiteral("id")).toString(), QStringLiteral("screenshot"), hostile));
    QVERIFY(in.matched);
    QVERIFY(!in.is_error);
    QCOMPARE(in.image_base64, png);
    QCOMPARE(in.image_mime, QStringLiteral("image/png"));  // coerced, not "text/html"

    // Non-vacuity: an image type we DO emit is preserved, so this is a targeted coercion of the
    // disallowed set, not a blanket rewrite-everything-to-png.
    const auto shot2 = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    const QJsonObject webp{{QStringLiteral("data"), png},
                           {QStringLiteral("mimeType"), QStringLiteral("image/webp")}};
    const auto in2 = session.onReply(resultFrame(
        shot2.frame.value(QStringLiteral("id")).toString(), QStringLiteral("screenshot"), webp));
    QVERIFY(!in2.is_error);
    QCOMPARE(in2.image_mime, QStringLiteral("image/webp"));
}

void BrowserBridgeTests::genericReply_oversizeTextIsRejected() {
    // R5-G10-9: a generic (non-image/PDF) reply is echoed verbatim into the model context. A
    // hostile/pathological page can inflate it up to the 64 MiB decoder limit, so the 8 MiB text
    // cap must refuse an oversized reply with an honest error rather than flood the transport.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto out = session.beginCommand(QStringLiteral("browser_reload"), {});
    const QString flood(8 * 1024 * 1024 + 1, QLatin1Char('A'));
    const QJsonObject huge{{QStringLiteral("content"), flood}};
    const auto in = session.onReply(resultFrame(
        out.frame.value(QStringLiteral("id")).toString(), QStringLiteral("reload"), huge));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY(in.text.isEmpty());
    QVERIFY(in.error.contains(QStringLiteral("too large")));

    // Non-vacuity: a small generic reply passes through to the text channel, so the cap is a
    // size gate rather than an always-error.
    const auto out2 = session.beginCommand(QStringLiteral("browser_reload"), {});
    const QJsonObject small{{QStringLiteral("content"), QStringLiteral("hi")}};
    const auto in2 = session.onReply(resultFrame(
        out2.frame.value(QStringLiteral("id")).toString(), QStringLiteral("reload"), small));
    QVERIFY(!in2.is_error);
    QVERIFY(in2.text.contains(QStringLiteral("hi")));
}

void BrowserBridgeTests::onReply_resultWithOkFalseIsError() {
    // A result-typed frame is NOT implicitly success: a payload {ok:false, error:...} is an
    // operation-level failure and must surface as an error, never pass through as a successful
    // text result the model would trust.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto out = session.beginCommand(QStringLiteral("browser_reload"), {});
    const QJsonObject payload{{QStringLiteral("ok"), false},
                              {QStringLiteral("error"), QStringLiteral("navigation_blocked")}};
    const auto in = session.onReply(resultFrame(
        out.frame.value(QStringLiteral("id")).toString(), QStringLiteral("reload"), payload));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QCOMPARE(in.error, QStringLiteral("navigation_blocked"));
    QVERIFY(!session.hasOutstanding());
}

void BrowserBridgeTests::onReply_okFalseSnapshotDoesNotInstallRefIndex() {
    // A snapshot reply that reports {ok:false} failed to capture: it must NOT install a
    // ref_index (which would let the model act by ref against a DOM we never snapshotted).
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    QJsonObject failed = snapshotPayload(4242, QStringLiteral("Sign in"));
    failed.insert(QStringLiteral("ok"), false);
    const auto in = session.onReply(resultFrame(
        snap.frame.value(QStringLiteral("id")).toString(), QStringLiteral("snapshot"), failed));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY(session.refIndex().isEmpty());
}

void BrowserBridgeTests::screenshotReply_malformedBase64IsError() {
    // A screenshot whose "data" is not valid base64 is refused with an honest error rather than
    // forwarded to the client as an opaque, unrenderable image block.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    const auto in =
        session.onReply(resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                    QStringLiteral("screenshot"),
                                    screenshotPayload(QStringLiteral("!!!not-base64!!!"))));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY(in.image_base64.isEmpty());
    QVERIFY(in.error.contains(QStringLiteral("malformed")));
}

void BrowserBridgeTests::reconcileDomEpoch_omittedEpochAfterBaselineMarksRefsStale() {
    // Once a snapshot has established a domEpoch baseline, a later reply that OMITS the domEpoch
    // field entirely is treated as an attempt to hide a navigation (a relayed, page-influenced
    // frame stripping the field to keep stale refs live): refs must be invalidated so act-by-ref is
    // refused until a fresh snapshot re-baselines the DOM. The existing epoch tests always send a
    // later reply that STILL carries domEpoch, so the have_dom_epoch_ downgrade branch was never
    // driven.
    BrowserBridgeSession session;
    session.onHostConnected();

    // Snapshot at DOM epoch 1 -> ref_index installed, baseline epoch 1, not stale.
    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrameEpoch(snap.frame.value(QStringLiteral("id")).toString(),
                                           QStringLiteral("snapshot"),
                                           snapshotPayload(4242, QStringLiteral("Sign in")),
                                           1));
    QVERIFY(!session.refIndexStale());

    // A later NON-snapshot reply that OMITS domEpoch (resultFrame, not resultFrameEpoch): the
    // baseline exists, so dropping the field must invalidate refs.
    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    (void)session.onReply(resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                      QStringLiteral("screenshot"),
                                      screenshotPayload(pngHeaderBase64(10, 10))));
    QVERIFY2(session.refIndexStale(),
             "a reply that drops domEpoch after a baseline exists must invalidate refs");

    // A ref-based action is now refused until a fresh snapshot re-baselines the DOM.
    const auto click =
        session.beginCommand(QStringLiteral("browser_click"),
                             QJsonObject{{QStringLiteral("ref"), QStringLiteral("e1")}});
    QVERIFY(!click.ok);
}

void BrowserBridgeTests::printReply_nonPdfDataIsRefused() {
    // A browser_print reply's payload.data is untrusted, page-influenced base64 that the bridge
    // writes to the user's output folder as a .pdf. A payload that decodes to something that is not
    // a "%PDF-" document (e.g. an executable or HTML) must be refused BEFORE any filesystem write,
    // never saved under a .pdf name. No existing test drove a print reply at all.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto print = session.beginCommand(QStringLiteral("browser_print"), {});
    const QByteArray notPdf = QByteArray("MZ\x90\x00 this is a PE header, not a pdf").toBase64();
    const auto in = session.onReply(
        resultFrame(print.frame.value(QStringLiteral("id")).toString(),
                    QStringLiteral("print"),
                    QJsonObject{{QStringLiteral("data"), QString::fromLatin1(notPdf)}}));
    QVERIFY(in.matched);
    QVERIFY(in.is_error);
    QVERIFY2(in.error.contains(QStringLiteral("not a valid PDF")), qPrintable(in.error));

    // Guard-isolation control: an EMPTY payload yields the DIFFERENT "empty PDF" error, so the
    // %PDF- magic guard is scoped to non-empty non-PDF content rather than firing on every reply.
    // (A real %PDF- payload is deliberately not asserted here -- accepting it writes a file to the
    // user's output folder, an unwanted unit-test side effect.)
    const auto empty = session.beginCommand(QStringLiteral("browser_print"), {});
    const auto emptyIn =
        session.onReply(resultFrame(empty.frame.value(QStringLiteral("id")).toString(),
                                    QStringLiteral("print"),
                                    QJsonObject{{QStringLiteral("data"), QString()}}));
    QVERIFY(emptyIn.is_error);
    QVERIFY(emptyIn.error.contains(QStringLiteral("empty PDF")));
    QVERIFY(!emptyIn.error.contains(QStringLiteral("not a valid PDF")));
}

void BrowserBridgeTests::reconcileDomEpoch_malformedEpochMarksRefsStale() {
    // A present-but-malformed domEpoch (non-integer / out of the exact-integer double range)
    // cannot be trusted as a baseline; the bridge fails closed by marking refs stale instead of
    // casting a garbage double to quint64 (UB) and silently re-baselining.
    BrowserBridgeSession session;
    session.onHostConnected();
    const auto snap = session.beginCommand(QStringLiteral("browser_snapshot"), {});
    (void)session.onReply(resultFrameEpoch(snap.frame.value(QStringLiteral("id")).toString(),
                                           QStringLiteral("snapshot"),
                                           snapshotPayload(4242, QStringLiteral("Sign in")),
                                           1));
    QVERIFY(!session.refIndexStale());

    const auto shot = session.beginCommand(QStringLiteral("browser_screenshot"), {});
    QJsonObject frame = resultFrame(shot.frame.value(QStringLiteral("id")).toString(),
                                    QStringLiteral("screenshot"),
                                    screenshotPayload(pngHeaderBase64(10, 10)));
    frame.insert(QStringLiteral("domEpoch"), 1e18);  // beyond the exact-integer double range
    (void)session.onReply(frame);
    QVERIFY(session.refIndexStale());
}

QTEST_MAIN(BrowserBridgeTests)
#include "test_browser_bridge.moc"
