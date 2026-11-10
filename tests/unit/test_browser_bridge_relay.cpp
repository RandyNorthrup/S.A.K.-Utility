// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge_pipe.h"
#include "sak/win32mcp/browser_bridge_relay.h"
#include "sak/win32mcp/browser_bridge_security.h"
#include "sak/win32mcp/browser_control.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <atomic>
#include <chrono>
#include <thread>

using sak::win32mcp::BrowserBridgePipeServer;
using sak::win32mcp::BrowserControl;
using sak::win32mcp::relayConnect;
using sak::win32mcp::relayHandshake;
using sak::win32mcp::relayPumpOnce;
using sak::win32mcp::RendezvousRecord;
using sak::win32mcp::ToolResult;
using sak::win32mcp::writeRendezvousRecord;

namespace {

BrowserBridgePipeServer::Options serverOptions(const QString& rendezvous) {
    BrowserBridgePipeServer::Options options;
    options.require_chrome_ancestor = false;  // the in-process relay is not a Chrome child
    options.rendezvous_path = rendezvous;
    options.io_timeout_ms = 30'000;
    return options;
}

// A fake extension for relayPumpOnce: capture the forwarded command, then answer it by
// echoing the correlation id with a canned payload. browser_write runs before
// browser_read in the pump, so the id is captured before the reply is built.
struct FakeExtension {
    QJsonObject last_command;
    QJsonObject reply_payload;

    sak::win32mcp::BrowserWriteFn writer() {
        return [this](const QJsonObject& command) {
            last_command = command;
            return true;
        };
    }
    sak::win32mcp::BrowserReadFn reader() {
        return [this](QJsonObject* out) {
            *out = QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                               {QStringLiteral("id"), last_command.value(QStringLiteral("id"))},
                               {QStringLiteral("payload"), reply_payload}};
            return true;
        };
    }
};

// A fake extension that behaves like a POLLING handler: it does not answer until the test
// releases it. That is the only shape in which the abandoned-exchange bug is observable --
// a handler that replies promptly never outlives its command.
struct SlowFakeExtension {
    QJsonObject last_command;
    std::atomic<bool> release{false};
    std::atomic<int> cancels_seen{0};

    sak::win32mcp::BrowserWriteFn writer() {
        return [this](const QJsonObject& frame) {
            if (frame.value(QStringLiteral("type")).toString() == QLatin1String("cancel")) {
                cancels_seen.fetch_add(1);
                release.store(true);  // the poll loop stops when its command is retired
                return true;
            }
            last_command = frame;
            return true;
        };
    }
    sak::win32mcp::BrowserReadFn reader() {
        return [this](QJsonObject* out) {
            // Bounded, so that a regression which stops the cancel from arriving FAILS this
            // test instead of hanging it. A hang reads as a stuck machine and gets retried;
            // a failure names the defect.
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!release.load() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            *out = QJsonObject{{QStringLiteral("type"), QStringLiteral("error")},
                               {QStringLiteral("id"), last_command.value(QStringLiteral("id"))},
                               {QStringLiteral("error"), QStringLiteral("command superseded")}};
            return true;
        };
    }
};

}  // namespace

class BrowserBridgeRelayTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void relayConnect_rejectsForeignServerPid();
    void relayHandshake_rejectsBadToken();
    void relayHandshake_rejectsProtocolMismatch();
    void relay_forwardsCommandAndReplyOverRealPipe();
    void relay_forwardsCancelToAPollingExtensionWhenTheServerAbandons();
    void control_notConnectedReportsError();
    void control_snapshotRoundTripsThroughRelay();
};

void BrowserBridgeRelayTests::relayConnect_rejectsForeignServerPid() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("r.json"));
    // A record pointing at a pid that is not our binary (almost certainly nonexistent)
    // must be refused: a rewritten record cannot redirect the relay to a foreign server.
    const RendezvousRecord record{QStringLiteral("\\\\.\\pipe\\SAK_BrowserBridge_bogus"),
                                  QStringLiteral("deadbeef"),
                                  1,
                                  999'999};
    QString error;
    QVERIFY(writeRendezvousRecord(path, record, &error));

    QString token;
    int protocol = 0;
    const HANDLE pipe = relayConnect(path, &token, &protocol, &error);
    QCOMPARE(pipe, INVALID_HANDLE_VALUE);
    // pid 999999 -> the single pidIsOwnImage-false message, which contains BOTH substrings, so
    // the || distinguished nothing. Pin the exact deterministic message.
    QCOMPARE(error,
             QStringLiteral("Bridge server pid 999999 is not the S.A.K. binary (or is gone)."));
}

void BrowserBridgeRelayTests::relayHandshake_rejectsBadToken() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(serverOptions(dir.filePath(QStringLiteral("r.json"))));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    QString token;
    int protocol = 0;
    const HANDLE pipe =
        relayConnect(dir.filePath(QStringLiteral("r.json")), &token, &protocol, &error);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);
    // The server drops a wrong-token handshake, so the relay's welcome read fails.
    QVERIFY(!relayHandshake(pipe, QStringLiteral("wrong-token"), protocol, &error));
    QCOMPARE(error, QStringLiteral("Bridge server closed before welcoming the relay."));
    CloseHandle(pipe);
    server.stop();
}

void BrowserBridgeRelayTests::relayHandshake_rejectsProtocolMismatch() {
    // B13-08: the handshake must enforce the protocol version. Offering a protocol the server
    // does not speak must fail rather than pump frames under a version skew (the server rejects
    // the hello, and the relay additionally verifies the welcome's protocol as defense in depth).
    QTemporaryDir dir;
    BrowserBridgePipeServer server(serverOptions(dir.filePath(QStringLiteral("r.json"))));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    QString token;
    int protocol = 0;
    const HANDLE pipe =
        relayConnect(dir.filePath(QStringLiteral("r.json")), &token, &protocol, &error);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);
    QVERIFY(!relayHandshake(pipe, token, protocol + 1, &error));  // wrong protocol -> refused
    // ...and the refusal is the SERVER's protocol gate, with the peer never admitted. Without
    // these two pins the QVERIFY above is satisfied by ANY failure path -- including a relay
    // that sent the wrong protocol in its own hello (browser_bridge_relay.cpp:234), got
    // WELCOMED, and only then noticed the skew in its defense-in-depth check: the server would
    // be pumping frames under a version skew, which is exactly what B13-08 forbids.
    // Two accepted messages, not one: the server's best-effort "protocol_mismatch" frame can be
    // discarded by its immediate DisconnectNamedPipe (browser_bridge_pipe.cpp:406) before the
    // relay reads it -- the same race test_browser_bridge_pipe.cpp:285-296 documents.
    QVERIFY2(error == QStringLiteral("Bridge server refused the relay: protocol_mismatch") ||
                 error == QStringLiteral("Bridge server closed before welcoming the relay."),
             qPrintable(error));
    QCOMPARE(server.connectionGeneration(), quint64{0});
    CloseHandle(pipe);
    server.stop();
}

void BrowserBridgeRelayTests::relay_forwardsCommandAndReplyOverRealPipe() {
    QTemporaryDir dir;
    BrowserBridgePipeServer server(serverOptions(dir.filePath(QStringLiteral("r.json"))));
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    QString token;
    int protocol = 0;
    const HANDLE pipe =
        relayConnect(dir.filePath(QStringLiteral("r.json")), &token, &protocol, &error);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);
    QVERIFY2(relayHandshake(pipe, token, protocol, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);

    FakeExtension extension;
    extension.reply_payload = QJsonObject{{QStringLiteral("echoed"), true}};
    std::atomic<bool> pumped{false};
    std::thread relay(
        [&] { pumped = relayPumpOnce(pipe, extension.reader(), extension.writer()); });

    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-1")},
                    {QStringLiteral("cmd"), QStringLiteral("snapshot")}});
    relay.join();
    QVERIFY(pumped.load());
    QVERIFY2(exchange.ok, qPrintable(exchange.error));
    QCOMPARE(exchange.reply.value(QStringLiteral("id")).toString(), QStringLiteral("b-1"));
    QVERIFY(exchange.reply.value(QStringLiteral("payload"))
                .toObject()
                .value(QStringLiteral("echoed"))
                .toBool());
    // The relay forwarded exactly the server's command -- EVERY key, id-transparent. Pinning
    // only "cmd" leaves a relay that REBUILDS the frame green: dropping "type" (what the
    // extension dispatches on) and every command argument (browser_contract.cpp:1513-1531 puts
    // url/backendNodeId/text beside "cmd") still satisfies a one-key check.
    QCOMPARE(extension.last_command,
             (QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                          {QStringLiteral("id"), QStringLiteral("b-1")},
                          {QStringLiteral("cmd"), QStringLiteral("snapshot")}}));
    CloseHandle(pipe);
    server.stop();
}

void BrowserBridgeRelayTests::relay_forwardsCancelToAPollingExtensionWhenTheServerAbandons() {
    QTemporaryDir dir;
    auto options = serverOptions(dir.filePath(QStringLiteral("r.json")));
    // Short enough that the exchange is abandoned while the fake extension is still polling,
    // which is the whole condition under test.
    options.io_timeout_ms = 400;
    BrowserBridgePipeServer server(options);
    QString error;
    QVERIFY2(server.start(&error), qPrintable(error));

    QString token;
    int protocol = 0;
    const HANDLE pipe =
        relayConnect(dir.filePath(QStringLiteral("r.json")), &token, &protocol, &error);
    QVERIFY(pipe != INVALID_HANDLE_VALUE);
    QVERIFY2(relayHandshake(pipe, token, protocol, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(server.clientConnected(), 5000);

    SlowFakeExtension extension;
    std::thread relay([&] { (void)relayPumpOnce(pipe, extension.reader(), extension.writer()); });

    const auto exchange = server.sendCommandAwaitReply(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                    {QStringLiteral("id"), QStringLiteral("b-slow")},
                    {QStringLiteral("cmd"), QStringLiteral("download")}});
    relay.join();

    // The server gave up on the exchange...
    QVERIFY(!exchange.ok);
    QCOMPARE(exchange.error,
             QStringLiteral("browser did not reply within 400 ms (connection reset)"));
    // ...and the extension was TOLD, rather than being left polling until its own ceiling.
    // Without the cancel this count is 0 and the reader never releases, so the join above
    // would hang -- the assertion and the test's ability to finish are the same thing.
    QCOMPARE(extension.cancels_seen.load(), 1);
    // relayPumpOnce's own return is deliberately NOT asserted: whether the late reply lands
    // in the server's drain read or after the teardown is a genuine race, and pinning it
    // would make this test flaky rather than more truthful. What matters is that the
    // extension was told, and the exchange still failed for the caller.

    CloseHandle(pipe);
    server.stop();
}

void BrowserBridgeRelayTests::control_notConnectedReportsError() {
    QTemporaryDir dir;
    BrowserControl control(serverOptions(dir.filePath(QStringLiteral("r.json"))));
    QString error;
    QVERIFY2(control.start(&error), qPrintable(error));

    // No relay has connected: every browser tool call is a clean, model-readable error.
    const ToolResult result = control.invoke(QStringLiteral("browser_snapshot"), {});
    QVERIFY(result.is_error);
    QCOMPARE(result.text,
             QStringLiteral("Browser not connected: the S.A.K. browser-control extension is "
                            "not attached."));
    control.stop();
}

void BrowserBridgeRelayTests::control_snapshotRoundTripsThroughRelay() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("r.json"));
    BrowserControl control(serverOptions(path));
    QString error;
    QVERIFY2(control.start(&error), qPrintable(error));

    // Bring up the relay end (client) against the control's server, then keep pumping.
    QString token;
    int protocol = 0;
    const HANDLE pipe = relayConnect(path, &token, &protocol, &error);
    QVERIFY2(pipe != INVALID_HANDLE_VALUE, qPrintable(error));
    QVERIFY2(relayHandshake(pipe, token, protocol, &error), qPrintable(error));

    FakeExtension extension;
    // A minimal DOM capture: one interactable node the snapshot renderer will keep.
    extension.reply_payload =
        QJsonObject{{QStringLiteral("url"), QStringLiteral("https://example.test/")},
                    {QStringLiteral("title"), QStringLiteral("Example")},
                    {QStringLiteral("nodes"),
                     QJsonArray{QJsonObject{{QStringLiteral("backendNodeId"), 42},
                                            {QStringLiteral("role"), QStringLiteral("button")},
                                            {QStringLiteral("name"), QStringLiteral("Sign in")},
                                            {QStringLiteral("tag"), QStringLiteral("button")},
                                            {QStringLiteral("depth"), 1},
                                            {QStringLiteral("interactable"), true},
                                            {QStringLiteral("visible"), true},
                                            {QStringLiteral("bounds"),
                                             QJsonObject{{QStringLiteral("x"), 10},
                                                         {QStringLiteral("y"), 20},
                                                         {QStringLiteral("width"), 80},
                                                         {QStringLiteral("height"), 30}}}}}}};
    std::atomic<bool> running{true};
    std::thread relay([&] {
        while (running.load() && relayPumpOnce(pipe, extension.reader(), extension.writer())) {}
    });

    QTRY_VERIFY_WITH_TIMEOUT(control.clientConnected(), 5000);
    const ToolResult result = control.invoke(QStringLiteral("browser_snapshot"), {});
    QVERIFY2(!result.is_error, qPrintable(result.text));
    QCOMPARE(result.text,
             QStringLiteral("url: https://example.test/\ntitle: Example\nelements: 1\n"
                            "  - button \"Sign in\" [ref=e1]\n"));

    running = false;
    control.stop();  // tears down the pipe; the relay's next pipe read fails and it exits
    relay.join();
    CloseHandle(pipe);
}

QTEST_MAIN(BrowserBridgeRelayTests)
#include "test_browser_bridge_relay.moc"
