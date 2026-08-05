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
    QVERIFY(exchange.error.contains(QStringLiteral("not connected")));
    server.stop();
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
    QVERIFY(exchange.error.contains(QStringLiteral("reset")));
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
    QVERIFY(exchange.error.contains(QStringLiteral("not connected")));
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
    QString busy;
    QVERIFY(!server.start(&busy));
    QVERIFY(busy.contains(QStringLiteral("already running")));

    server.stop();
    QVERIFY(!QFile::exists(rendezvous));  // cycle 1 really tore down
    // A fresh cycle starts and tears down cleanly (the once_flag would have blocked this stop()).
    QVERIFY2(server.start(&error), qPrintable(error));
    QVERIFY(QFile::exists(rendezvous));  // cycle 2 re-published the record
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
}

QTEST_MAIN(BrowserBridgePipeTests)
#include "test_browser_bridge_pipe.moc"
