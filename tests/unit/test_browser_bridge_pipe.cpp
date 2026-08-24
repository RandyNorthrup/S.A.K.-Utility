// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge_pipe.h"
#include "sak/win32mcp/native_messaging.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest/QtTest>

#include <atomic>
#include <thread>

using sak::win32mcp::BrowserBridgePipeServer;
using sak::win32mcp::encodeFrame;
using sak::win32mcp::NativeFrame;
using sak::win32mcp::parseFrame;

namespace {

// -- Minimal synchronous test client (stands in for the Chrome-spawned relay) ------

HANDLE clientConnect(const QString& name) {
    const std::wstring wide = name.toStdWString();
    for (int attempt = 0; attempt < 200; ++attempt) {
        const HANDLE handle = CreateFileW(wide.c_str(),
                                          GENERIC_READ | GENERIC_WRITE,
                                          0,
                                          nullptr,
                                          OPEN_EXISTING,
                                          SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                          nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            return handle;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(wide.c_str(), 100);
        } else {
            Sleep(10);
        }
    }
    return INVALID_HANDLE_VALUE;
}

bool clientWrite(HANDLE handle, const QJsonObject& object) {
    const QByteArray frame = encodeFrame(object);
    DWORD offset = 0;
    while (offset < static_cast<DWORD>(frame.size())) {
        DWORD wrote = 0;
        if (WriteFile(handle, frame.constData() + offset, frame.size() - offset, &wrote, nullptr) ==
                FALSE ||
            wrote == 0) {
            return false;
        }
        offset += wrote;
    }
    return true;
}

bool clientReadExact(HANDLE handle, char* buffer, DWORD size) {
    DWORD offset = 0;
    while (offset < size) {
        DWORD got = 0;
        if (ReadFile(handle, buffer + offset, size - offset, &got, nullptr) == FALSE || got == 0) {
            return false;
        }
        offset += got;
    }
    return true;
}

bool clientRead(HANDLE handle, QJsonObject* out) {
    char header[4];
    if (!clientReadExact(handle, header, 4)) {
        return false;
    }
    const quint32 length = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(header));
    QByteArray frame(header, 4);
    QByteArray body(static_cast<int>(length), Qt::Uninitialized);
    if (!clientReadExact(handle, body.data(), length)) {
        return false;
    }
    frame.append(body);
    const NativeFrame parsed = parseFrame(frame);
    if (parsed.status != NativeFrame::Status::Ok) {
        return false;
    }
    *out = parsed.message;
    return true;
}

// Connect + complete the hello/welcome handshake. Returns the connected handle, or
// INVALID_HANDLE_VALUE on any failure.
HANDLE connectAndHandshake(const QString& name, const QString& token) {
    const HANDLE handle = clientConnect(name);
    if (handle == INVALID_HANDLE_VALUE) {
        return handle;
    }
    const bool sent = clientWrite(handle,
                                  QJsonObject{{QStringLiteral("type"), QStringLiteral("hello")},
                                              {QStringLiteral("token"), token},
                                              {QStringLiteral("protocol"), 1}});
    QJsonObject welcome;
    if (!sent || !clientRead(handle, &welcome) ||
        welcome.value(QStringLiteral("type")).toString() != QLatin1String("welcome")) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}

// Result of a single handshake attempt with a caller-supplied hello frame. Unlike
// connectAndHandshake this does NOT require a welcome -- it reports exactly what the
// server did, so a test can assert a REFUSAL (no welcome, or an explicit error frame).
struct HandshakeAttempt {
    bool connected{false};  // the pipe opened
    bool got_frame{false};  // the server sent a frame back before dropping
    QJsonObject reply;      // that frame (empty if the server dropped silently)
};

HandshakeAttempt attemptHandshake(const QString& name, const QJsonObject& hello) {
    HandshakeAttempt out;
    const HANDLE handle = clientConnect(name);
    if (handle == INVALID_HANDLE_VALUE) {
        return out;
    }
    out.connected = true;
    if (clientWrite(handle, hello)) {
        QJsonObject frame;
        if (clientRead(handle, &frame)) {
            out.got_frame = true;
            out.reply = frame;
        }
    }
    CloseHandle(handle);
    return out;
}

BrowserBridgePipeServer::Options testOptions(const QString& rendezvous, DWORD timeout_ms) {
    BrowserBridgePipeServer::Options options;
    options.require_chrome_ancestor = false;  // the test client is not a Chrome child
    options.rendezvous_path = rendezvous;
    options.io_timeout_ms = timeout_ms;
    return options;
}

}  // namespace

class BrowserBridgePipeTests : public QObject {
    Q_OBJECT

private slots:
    void send_beforeAnyClientReturnsNotConnected();
    void handshake_refusesForgedTokenAndUnknownType();
    void handshake_refusesProtocolMismatch();
    void ancestorChain_boundsDepthAndMatchesImage();
    void handshake_thenCommandReplyRoundTrips();
    void silentPeer_deadlineResetsConnection();
    void reconnect_secondClientServedWithNewGeneration();
    void doubleStop_isSafe();
    void restartAfterStopWorksAndDoubleStartRefused();
};

void BrowserBridgePipeTests::send_beforeAnyClientReturnsNotConnected() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(testOptions(dir.filePath(QStringLiteral("r.json")), 30'000));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")}});
    QVERIFY(!exchange.ok);
    QCOMPARE(exchange.error, QStringLiteral("Browser not connected."));
    server.stop();
}

namespace {
void verifyGoodHandshakeIsAdmittedExactlyOnce(BrowserBridgePipeServer& server,
                                              const QString& name,
                                              const QString& real_token);
}  // namespace

void BrowserBridgePipeTests::handshake_refusesForgedTokenAndUnknownType() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(testOptions(dir.filePath(QStringLiteral("r.json")), 30'000));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    const QString name = server.pipeName();
    const QString real_token = server.token();

    // (1) Forged token: the token is the shared secret authorizing the pipe peer. A hello
    //     carrying the wrong token (a foreign process that connected to the pipe and
    //     guessed) is dropped with no welcome, and the server never admits the peer.
    HandshakeAttempt forged;
    std::thread forged_client([&] {
        forged = attemptHandshake(name,
                                  QJsonObject{{QStringLiteral("type"), QStringLiteral("hello")},
                                              {QStringLiteral("token"),
                                               real_token + QStringLiteral("_forged")},
                                              {QStringLiteral("protocol"), 1}});
    });
    forged_client.join();
    QVERIFY(forged.connected);  // pipe opened -- the refusal is the token gate, not connect
    // Token gate drops the peer before any frame; !got_frame pins that (!= welcome was vacuous).
    QVERIFY(!forged.got_frame);
    QVERIFY(!server.clientConnected());

    // (2) Unknown first-frame type WITH the correct token: the handshake must open with a
    //     "hello"; anything else ("attach" here) is refused even though the token is right.
    HandshakeAttempt bad_type;
    std::thread type_client([&] {
        bad_type = attemptHandshake(name,
                                    QJsonObject{{QStringLiteral("type"), QStringLiteral("attach")},
                                                {QStringLiteral("token"), real_token},
                                                {QStringLiteral("protocol"), 1}});
    });
    type_client.join();
    // Reached the server at all -- the forged case above pins this and the type case was silent on
    // it. An attempt that never got a pipe (an accept loop slow to re-arm after the first refusal,
    // past the client's retry budget) leaves got_frame false, so this case would pass while the
    // first-frame TYPE gate it is named for never ran.
    QVERIFY(bad_type.connected);
    QVERIFY(!bad_type.got_frame);  // wrong first-frame type is dropped before any welcome frame
    QVERIFY(!server.clientConnected());

    verifyGoodHandshakeIsAdmittedExactlyOnce(server, name, real_token);
    server.stop();
}

namespace {

// Non-vacuity control for the two refusals above: the SAME harness, with a correct hello (right
// type + token + protocol), completes the handshake and the server admits the peer and serves a
// command -- so the refusals are the hello/token gates, not a server that welcomes nobody. The
// re-armed accept loop (proven elsewhere) accepts this connection.
void verifyGoodHandshakeIsAdmittedExactlyOnce(BrowserBridgePipeServer& server,
                                              const QString& name,
                                              const QString& real_token) {
    std::atomic<bool> good_ok{false};
    std::thread good_client([&] {
        const HANDLE handle = connectAndHandshake(name, real_token);
        if (handle == INVALID_HANDLE_VALUE) {
            return;
        }
        QJsonObject command;
        if (clientRead(handle, &command)) {
            clientWrite(handle,
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                                    {QStringLiteral("id"), command.value(QStringLiteral("id"))},
                                    {QStringLiteral("payload"), QJsonObject{}}});
            FlushFileBuffers(handle);
            good_ok = true;
        }
        CloseHandle(handle);
    });
    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);
    const auto served = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("ok-1")},
                    {QStringLiteral("cmd"), QStringLiteral("snapshot")}});
    QVERIFY2(served.ok, qPrintable(served.error));
    // Exactly ONE connection was ever ADMITTED. The generation counts verified peers (it is what
    // browser_control's syncSessionToConnection watches to fire onHostConnected), so the two
    // refused handshakes above must not have advanced it: after them plus this one good peer it
    // is 1, not 3. clientConnected() is only an instantaneous sample and cannot see a peer that
    // was counted and then dropped.
    QCOMPARE(server.connectionGeneration(), quint64{1});
    good_client.join();
    QVERIFY(good_ok.load());
}

}  // namespace

void BrowserBridgePipeTests::handshake_refusesProtocolMismatch() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(testOptions(dir.filePath(QStringLiteral("r.json")), 30'000));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    const QString name = server.pipeName();
    const QString real_token = server.token();

    // A hello with the right type and token but a mismatched protocol version is refused:
    // no welcome, and the server never admits the peer. The server makes a best-effort
    // "protocol_mismatch" diagnostic write before it drops, but its immediate
    // DisconnectNamedPipe can discard that frame before the client reads it (a named pipe
    // drops unread data on disconnect), so the RELIABLE contract asserted here is the
    // refusal itself; if the diagnostic frame does survive the race it must be the error,
    // never a welcome. This stays specific to the protocol gate: were that gate removed,
    // the mismatched hello would be welcomed and the "!= welcome" checks would go red.
    HandshakeAttempt mism;
    std::thread client([&] {
        mism = attemptHandshake(name,
                                QJsonObject{{QStringLiteral("type"), QStringLiteral("hello")},
                                            {QStringLiteral("token"), real_token},
                                            {QStringLiteral("protocol"), 999}});
    });
    client.join();
    // The attempt actually REACHED the protocol gate. Every other assertion here is satisfied by a
    // client that never connected at all (empty reply != welcome, got_frame false so the if()
    // never fires, clientConnected() false because nobody arrived); the sibling forged-token case
    // pins connect explicitly and this one did not.
    QVERIFY(mism.connected);
    QVERIFY(mism.reply.value(QStringLiteral("type")).toString() != QLatin1String("welcome"));
    if (mism.got_frame) {
        QCOMPARE(mism.reply.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        QCOMPARE(mism.reply.value(QStringLiteral("error")).toString(),
                 QStringLiteral("protocol_mismatch"));
    }
    QVERIFY(!server.clientConnected());
    // Never ADMITTED either: the generation counts verified peers, so a hello refused at the
    // protocol gate must leave it at zero. clientConnected() is an instantaneous sample that a
    // peer counted and then dropped would slip past.
    QCOMPARE(server.connectionGeneration(), quint64{0});
    server.stop();
}

void BrowserBridgePipeTests::ancestorChain_boundsDepthAndMatchesImage() {
    using Server = BrowserBridgePipeServer;
    // In production the pipe optionally binds the peer to a Chrome-launched process
    // (require_chrome_ancestor) by walking the process tree for chrome.exe within a depth
    // cap. Model a tree: leaf 100 -> 90 -> 80 -> 70(chrome.exe) -> 0(root), so chrome is
    // three ancestors above the leaf. Images are stored LOWERCASED (as the live walk does).
    const QHash<DWORD, DWORD> parent{{100, 90}, {90, 80}, {80, 70}, {70, 0}};
    const QHash<DWORD, QString> image{{100, QStringLiteral("tab.exe")},
                                      {90, QStringLiteral("renderer.exe")},
                                      {80, QStringLiteral("gpu.exe")},
                                      {70, QStringLiteral("chrome.exe")}};

    // Authorized: a generous depth budget reaches the chrome ancestor.
    QVERIFY(Server::ancestorChainContainsImageForTesting(
        100, parent, image, QStringLiteral("chrome.exe"), 12));

    // Depth cap (the security bound): a budget too small to reach chrome must NOT authorize.
    // The walk inspects one ancestor per iteration, so a budget of 2 sees only the two
    // nearest ancestors (renderer, gpu) and stops before chrome -> refused. This is what
    // stops a peer with a very distant, coincidental chrome.exe ancestor from being bound.
    QVERIFY(!Server::ancestorChainContainsImageForTesting(
        100, parent, image, QStringLiteral("chrome.exe"), 2));
    // Pin the cap EXACTLY, not just "2 is too few / 12 is plenty": chrome is the third ancestor,
    // so 3 is the smallest budget that authorizes. Without this pair (3 -> true, 2 -> false) a
    // walk one ancestor stingier than documented passes both existing cases, and production's
    // kChromeAncestorMaxDepth = 12 would silently mean 11.
    QVERIFY(Server::ancestorChainContainsImageForTesting(
        100, parent, image, QStringLiteral("chrome.exe"), 3));

    // No chrome anywhere in the chain -> refused (a non-Chrome-launched peer).
    const QHash<DWORD, QString> noChrome{{100, QStringLiteral("tab.exe")},
                                         {90, QStringLiteral("renderer.exe")},
                                         {80, QStringLiteral("gpu.exe")},
                                         {70, QStringLiteral("explorer.exe")}};
    QVERIFY(!Server::ancestorChainContainsImageForTesting(
        100, parent, noChrome, QStringLiteral("chrome.exe"), 12));

    // A CYCLE in the parent map (100 -> 90 -> 100) must terminate under the depth bound
    // rather than loop forever, and with no chrome ancestor it fails closed. A stale/reused
    // Toolhelp parent pid can produce exactly such a cycle.
    const QHash<DWORD, DWORD> cyclic{{100, 90}, {90, 100}};
    const QHash<DWORD, QString> cyclicImage{{100, QStringLiteral("tab.exe")},
                                            {90, QStringLiteral("renderer.exe")}};
    QVERIFY(!Server::ancestorChainContainsImageForTesting(
        100, cyclic, cyclicImage, QStringLiteral("chrome.exe"), 12));
}

void BrowserBridgePipeTests::handshake_thenCommandReplyRoundTrips() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(testOptions(dir.filePath(QStringLiteral("r.json")), 30'000));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    const QString name = server.pipeName();
    const QString token = server.token();
    std::atomic<bool> client_ok{false};
    std::thread client([&] {
        const HANDLE handle = connectAndHandshake(name, token);
        if (handle == INVALID_HANDLE_VALUE) {
            return;
        }
        QJsonObject command;
        if (clientRead(handle, &command)) {
            clientWrite(handle,
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                                    {QStringLiteral("id"), command.value(QStringLiteral("id"))},
                                    {QStringLiteral("cmd"), command.value(QStringLiteral("cmd"))},
                                    {QStringLiteral("payload"),
                                     QJsonObject{{QStringLiteral("echoed"), true}}}});
            FlushFileBuffers(handle);  // ensure the server reads the reply before we close
            client_ok = true;
        }
        CloseHandle(handle);
    });

    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);
    QCOMPARE(server.connectionGeneration(), quint64{1});

    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")},
                    {QStringLiteral("cmd"), QStringLiteral("snapshot")}});
    QVERIFY2(exchange.ok, qPrintable(exchange.error));
    QCOMPARE(exchange.reply.value(QStringLiteral("id")).toString(), QStringLiteral("b-1"));
    QCOMPARE(exchange.reply.value(QStringLiteral("payload"))
                 .toObject()
                 .value(QStringLiteral("echoed"))
                 .toBool(),
             true);

    client.join();
    QVERIFY(client_ok.load());
    server.stop();
}

void BrowserBridgePipeTests::silentPeer_deadlineResetsConnection() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(testOptions(dir.filePath(QStringLiteral("r.json")), 300));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    const QString name = server.pipeName();
    const QString token = server.token();
    std::thread client([&] {
        const HANDLE handle = connectAndHandshake(name, token);
        if (handle == INVALID_HANDLE_VALUE) {
            return;
        }
        QJsonObject command;
        clientRead(handle, &command);  // read the command but never reply
        Sleep(600);                    // stay connected past the server's 300 ms deadline
        CloseHandle(handle);
    });

    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);
    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")},
                    {QStringLiteral("cmd"), QStringLiteral("snapshot")}});
    QVERIFY(!exchange.ok);
    QCOMPARE(exchange.error,
             QStringLiteral("browser did not reply within 300 ms (connection reset)"));
    QTRY_VERIFY_WITH_TIMEOUT(!server.clientConnected(), 5000);  // connection torn down

    client.join();
    server.stop();
}

void BrowserBridgePipeTests::reconnect_secondClientServedWithNewGeneration() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(testOptions(dir.filePath(QStringLiteral("r.json")), 500));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    const QString name = server.pipeName();
    const QString token = server.token();

    // Relay A: connect, read the command, then vanish without replying (a reset).
    std::thread client_a([&] {
        const HANDLE handle = connectAndHandshake(name, token);
        if (handle == INVALID_HANDLE_VALUE) {
            return;
        }
        QJsonObject command;
        clientRead(handle, &command);
        CloseHandle(handle);  // die mid-command
    });
    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);
    const auto reset = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")},
                    {QStringLiteral("cmd"), QStringLiteral("snapshot")}});
    QVERIFY(!reset.ok);
    QCOMPARE(reset.error, QStringLiteral("browser did not reply within 500 ms (connection reset)"));
    client_a.join();
    QTRY_VERIFY_WITH_TIMEOUT(!server.clientConnected(), 5000);  // the accept loop re-armed

    // Relay B: a fresh connection must be accepted and served (generation advances).
    std::atomic<bool> client_ok{false};
    std::thread client_b([&] {
        const HANDLE handle = connectAndHandshake(name, token);
        if (handle == INVALID_HANDLE_VALUE) {
            return;
        }
        QJsonObject command;
        if (clientRead(handle, &command)) {
            clientWrite(handle,
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                                    {QStringLiteral("id"), command.value(QStringLiteral("id"))},
                                    {QStringLiteral("payload"), QJsonObject{}}});
            FlushFileBuffers(handle);
            client_ok = true;
        }
        CloseHandle(handle);
    });
    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);
    QCOMPARE(server.connectionGeneration(), quint64{2});
    const auto served = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-2")},
                    {QStringLiteral("cmd"), QStringLiteral("snapshot")}});
    QVERIFY2(served.ok, qPrintable(served.error));
    QCOMPARE(served.reply.value(QStringLiteral("id")).toString(), QStringLiteral("b-2"));
    client_b.join();
    QVERIFY(client_ok.load());
    server.stop();
}

void BrowserBridgePipeTests::doubleStop_isSafe() {
    QTemporaryDir dir;
    const QString rendezvous = dir.filePath(QStringLiteral("r.json"));
    BrowserBridgePipeServer server(testOptions(rendezvous, 30'000));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));
    QVERIFY(QFile::exists(rendezvous));  // start() published the discovery record
    server.stop();
    server.stop();  // second stop (and the destructor's) must be a safe no-op, not a crash

    // Idempotence is OBSERVABLE, not just "it did not crash": the first stop tore the cycle
    // down -- rendezvous record removed, sender fails closed -- and the second left that
    // teardown exactly as it found it rather than re-closing a handle or re-joining a thread.
    QVERIFY(!QFile::exists(rendezvous));
    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")}});
    QVERIFY(!exchange.ok);
    QCOMPARE(exchange.error, QStringLiteral("Browser not connected."));
}

void BrowserBridgePipeTests::restartAfterStopWorksAndDoubleStartRefused() {
    // B13-05: start -> stop -> start must tear down each cycle (the old std::once_flag disabled
    // every stop() after the first), and a second start() while running must be refused rather
    // than move-assign onto a joinable std::thread (which would std::terminate).
    QTemporaryDir dir;
    const QString rendezvous = dir.filePath(QStringLiteral("r.json"));
    BrowserBridgePipeServer server(testOptions(rendezvous, 30'000));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    // A second start while running is refused (not a crash).
    const QString cycle1_pipe = server.pipeName();
    const QString cycle1_token = server.token();
    QString busy;
    QVERIFY(!server.start(&busy));
    QCOMPARE(busy, QStringLiteral("Bridge pipe server is already running"));
    // The refusal must also be INERT: a start() that ran createPipeResources BEFORE testing
    // running_ returns this exact message, so the message alone proves nothing about the live
    // cycle -- it would have drawn a new pipe-name nonce and token, overwritten pipe_ out from
    // under the I/O thread, and rotated the credential a connected relay already holds.
    QCOMPARE(server.pipeName(), cycle1_pipe);
    QCOMPARE(server.token(), cycle1_token);
    QVERIFY(QFile::exists(rendezvous));

    server.stop();
    QVERIFY(!QFile::exists(rendezvous));  // cycle 1 really tore down
    // A fresh cycle starts and tears down cleanly (the once_flag would have blocked this stop()).
    QVERIFY2(server.start(&error), qPrintable(error));
    QVERIFY(QFile::exists(rendezvous));  // cycle 2 re-published the record
    // ...and re-published it with a FRESH identity. Existence alone passes a cycle that reuses
    // cycle 1's pipe name and token, leaving a stopped session's 128-bit credential valid for the
    // new one and making the anti-squat nonce predictable from the old record.
    QVERIFY(server.pipeName() != cycle1_pipe);
    QVERIFY(server.token() != cycle1_token);
    server.stop();
    // The SECOND stop is the whole point: under the retired std::once_flag it silently did
    // nothing, leaving cycle 2's pipe handle, I/O thread and rendezvous record alive. The
    // record being gone -- and the sender failing closed -- is the observable proof that this
    // teardown ran, which the old terminal QVERIFY(true) never established.
    QVERIFY(!QFile::exists(rendezvous));
    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")}});
    QVERIFY(!exchange.ok);
    QCOMPARE(exchange.error, QStringLiteral("Browser not connected."));
}

QTEST_MAIN(BrowserBridgePipeTests)
#include "test_browser_bridge_pipe.moc"
