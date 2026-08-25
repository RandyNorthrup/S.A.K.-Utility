// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/native_messaging.h"
#include "sak/win32mcp/win32_mcp_native_host.h"

#include <QJsonObject>
#include <QtEndian>
#include <QtTest/QtTest>

using sak::win32mcp::encodeFrame;
using sak::win32mcp::handleNativeMessage;
using sak::win32mcp::kBrowserBridgeProtocol;
using sak::win32mcp::kMaxNativeMessageBytes;
using sak::win32mcp::NativeFrame;
using sak::win32mcp::parseFrame;

class NativeMessagingTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void encode_prefixesLittleEndianLength();
    void roundTrip_encodeThenParseRecoversObject();
    void parse_shortBufferNeedsMore();
    void parse_zeroLengthIsError();
    void parse_oversizedLengthIsError();
    void parse_exactlyCapSizedLengthIsAccepted();
    void parse_nonObjectBodyIsError();
    void parse_multipleFramesConsumedIndividually();
    void handle_pingReturnsPongWithIdentityAndEchoedId();
    void handle_echoesIdWithItsJsonTypeAndOmitsItWhenAbsent();
    void handle_unknownTypeReturnsError();
};

void NativeMessagingTests::encode_prefixesLittleEndianLength() {
    const QByteArray frame =
        encodeFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("x")}});
    QCOMPARE(frame.size(), 16);  // 4-byte length prefix + 12-byte compact JSON {"type":"x"}
    const quint32 length =
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(frame.constData()));
    QCOMPARE(static_cast<int>(length), frame.size() - 4);
    // Body is compact JSON, so the fifth byte is the opening brace.
    QCOMPARE(frame.at(4), '{');
}

void NativeMessagingTests::roundTrip_encodeThenParseRecoversObject() {
    const QJsonObject original{{QStringLiteral("type"), QStringLiteral("ping")},
                               {QStringLiteral("id"), 7},
                               {QStringLiteral("note"), QStringLiteral("hello")}};
    const NativeFrame parsed = parseFrame(encodeFrame(original));
    QCOMPARE(parsed.status, NativeFrame::Status::Ok);
    QCOMPARE(parsed.message, original);
    QCOMPARE(parsed.consumed, encodeFrame(original).size());
}

void NativeMessagingTests::parse_shortBufferNeedsMore() {
    QByteArray frame = encodeFrame(QJsonObject{{QStringLiteral("type"), QStringLiteral("ping")}});
    // Fewer than 4 bytes: cannot even read the length. Probed at EVERY size below the boundary,
    // not just at 2. The guard is `buffer.size() < kLengthPrefixBytes`, and the only value that
    // distinguishes `< 4` from `< 3` is size 3 -- which nothing in the tree ever fed parseFrame,
    // since every real caller reads the header with readExact and so always presents exactly 4.
    // The file already pins the OTHER length boundary from both sides; this one was one-sided.
    for (int prefix = 0; prefix < 4; ++prefix) {
        QVERIFY2(parseFrame(frame.left(prefix)).status == NativeFrame::Status::NeedMore,
                 qPrintable(QStringLiteral("a %1-byte buffer must report NeedMore").arg(prefix)));
    }
    // ... and exactly 4 bytes IS enough to read the length, so the boundary is pinned from both
    // sides: this one reports NeedMore for the missing BODY, not for a short header.
    QCOMPARE(parseFrame(frame.left(4)).status, NativeFrame::Status::NeedMore);
    // Full header but a partial body.
    QCOMPARE(parseFrame(frame.left(frame.size() - 1)).status, NativeFrame::Status::NeedMore);
}

void NativeMessagingTests::parse_zeroLengthIsError() {
    QByteArray frame(4, '\0');  // length prefix of 0
    const NativeFrame parsed = parseFrame(frame);
    QCOMPARE(parsed.status, NativeFrame::Status::Error);
    QCOMPARE(parsed.error, QStringLiteral("Native message length is zero"));
}

void NativeMessagingTests::parse_oversizedLengthIsError() {
    QByteArray frame;
    const quint32 tooBig =
        qToLittleEndian<quint32>(static_cast<quint32>(kMaxNativeMessageBytes) + 1);
    frame.append(reinterpret_cast<const char*>(&tooBig), 4);
    const NativeFrame parsed = parseFrame(frame);
    QCOMPARE(parsed.status, NativeFrame::Status::Error);
    QCOMPARE(parsed.error,
             QStringLiteral("Native message length 67108865 exceeds the 67108864-byte cap"));
}

void NativeMessagingTests::parse_exactlyCapSizedLengthIsAccepted() {
    // The oversize guard is strictly greater-than, so a frame whose declared body length is
    // EXACTLY the cap is legal and must be accepted. This pins that boundary: a >= mutation
    // would wrongly reject a maximum-sized message. Body is a valid JSON object of exactly
    // kMaxNativeMessageBytes bytes ({"p":"aa...a"} -> 8 bytes of fixed overhead).
    const int cap = kMaxNativeMessageBytes;
    QByteArray body;
    body.reserve(cap);
    body.append(QByteArrayLiteral("{\"p\":\""));
    body.append(QByteArray(cap - 8, 'a'));
    body.append(QByteArrayLiteral("\"}"));
    QCOMPARE(body.size(), cap);
    QByteArray frame;
    const quint32 length = qToLittleEndian<quint32>(static_cast<quint32>(cap));
    frame.append(reinterpret_cast<const char*>(&length), 4);
    frame.append(body);
    const NativeFrame parsed = parseFrame(frame);
    QCOMPARE(parsed.status, NativeFrame::Status::Ok);
    QCOMPARE(parsed.consumed, frame.size());
}

void NativeMessagingTests::parse_nonObjectBodyIsError() {
    const QByteArray body = QByteArrayLiteral("[1,2,3]");  // valid JSON, but an array
    QByteArray frame;
    const quint32 length = qToLittleEndian<quint32>(static_cast<quint32>(body.size()));
    frame.append(reinterpret_cast<const char*>(&length), 4);
    frame.append(body);
    const NativeFrame parsed = parseFrame(frame);
    QCOMPARE(parsed.status, NativeFrame::Status::Error);
    // The errorString() tail is Qt-version-variant; pin the deterministic prefix.
    QVERIFY(parsed.error.startsWith(QStringLiteral("Native message is not a JSON object: ")));
}

void NativeMessagingTests::parse_multipleFramesConsumedIndividually() {
    const QJsonObject a{{QStringLiteral("type"), QStringLiteral("a")}};
    const QJsonObject b{{QStringLiteral("type"), QStringLiteral("b")}};
    QByteArray buffer = encodeFrame(a) + encodeFrame(b);

    const NativeFrame first = parseFrame(buffer);
    QCOMPARE(first.status, NativeFrame::Status::Ok);
    QCOMPARE(first.message, a);

    buffer.remove(0, first.consumed);
    const NativeFrame second = parseFrame(buffer);
    QCOMPARE(second.status, NativeFrame::Status::Ok);
    QCOMPARE(second.message, b);
    QCOMPARE(second.consumed, buffer.size());  // exactly the remaining frame
}

void NativeMessagingTests::handle_pingReturnsPongWithIdentityAndEchoedId() {
    const QJsonObject reply = handleNativeMessage(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("ping")}, {QStringLiteral("id"), 42}},
        QStringLiteral("sak-win32-mcp"),
        QStringLiteral("1.2.3"));
    QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("pong"));
    QCOMPARE(reply.value(QStringLiteral("server")).toString(), QStringLiteral("sak-win32-mcp"));
    QCOMPARE(reply.value(QStringLiteral("version")).toString(), QStringLiteral("1.2.3"));
    // The protocol number is pinned to its LITERAL, not to the constant that produced it:
    // pongReply inserts kBrowserBridgeProtocol verbatim and comparing the reply back against the
    // same constant moves both sides together, so it cannot observe the value at all. The value
    // has two other independent hardcoded copies that must agree with it -- BRIDGE_PROTOCOL in
    // browser/extension/background.js and BrowserBridgeHello::protocol -- and nothing in tests/
    // pinned the literal, so a skew was invisible. The header even invites the bump ("Bump when
    // the message shapes change"), which is exactly when the other two copies need visiting.
    QCOMPARE(kBrowserBridgeProtocol, 1);
    QCOMPARE(reply.value(QStringLiteral("protocol")).toInt(), kBrowserBridgeProtocol);
    // The echo must hand the id back with its JSON TYPE intact. .toInt() normalizes -- it
    // collapses a string id to 0 and truncates a double -- so an echo that coerced the value
    // still read 42 here. The extension correlates on ids it sends as STRINGS, so an
    // integer-only assertion is the wrong shape entirely.
    QCOMPARE(reply.value(QStringLiteral("id")), QJsonValue(42));
    // The pid is exactly knowable: pongReply reads QCoreApplication::applicationPid() and this
    // test runs IN that same process. "> 0" was satisfied by a hardcoded 1, a thread id, or a
    // counter -- and this is the only assertion in the suite that touches pid.
    QCOMPARE(static_cast<qint64>(reply.value(QStringLiteral("pid")).toDouble()),
             QCoreApplication::applicationPid());
}

void NativeMessagingTests::handle_echoesIdWithItsJsonTypeAndOmitsItWhenAbsent() {
    // The echo is `if (request.contains("id")) reply.insert("id", request.value("id"))`, and both
    // pongReply and typedError carry their own copy of it. Every fixture supplied an INTEGER id,
    // so neither the string case nor the absent case was ever reached: the guard's false arm
    // could be deleted, fabricating an id the caller never sent, and a coercing echo would look
    // identical under .toInt().
    const auto ping = [](const QJsonObject& request) {
        return handleNativeMessage(request,
                                   QStringLiteral("sak-win32-mcp"),
                                   QStringLiteral("1.2.3"));
    };

    // A STRING id -- the shape the browser extension actually sends -- must come back a string.
    const QJsonObject string_id = ping({{QStringLiteral("type"), QStringLiteral("ping")},
                                        {QStringLiteral("id"), QStringLiteral("cmd-7")}});
    QCOMPARE(string_id.value(QStringLiteral("id")), QJsonValue(QStringLiteral("cmd-7")));
    QCOMPARE(string_id.value(QStringLiteral("type")).toString(), QStringLiteral("pong"));

    // No id at all: the reply must not FABRICATE one.
    const QJsonObject no_id = ping({{QStringLiteral("type"), QStringLiteral("ping")}});
    QVERIFY2(!no_id.contains(QStringLiteral("id")),
             "a request without an id must not get one back");
    QCOMPARE(no_id.value(QStringLiteral("type")).toString(), QStringLiteral("pong"));

    // Both halves again on the ERROR path, which has its own separate copy of the guard -- and is
    // precisely the reply the extension must correlate, since a mis-typed or fabricated id there
    // reports a failure against the wrong outstanding request.
    const QJsonObject error_string_id = ping({{QStringLiteral("type"), QStringLiteral("nope")},
                                              {QStringLiteral("id"), QStringLiteral("cmd-9")}});
    QCOMPARE(error_string_id.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(error_string_id.value(QStringLiteral("id")), QJsonValue(QStringLiteral("cmd-9")));

    const QJsonObject error_no_id = ping({{QStringLiteral("type"), QStringLiteral("nope")}});
    QCOMPARE(error_no_id.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QVERIFY2(!error_no_id.contains(QStringLiteral("id")),
             "an error reply must not fabricate an id either");
}

void NativeMessagingTests::handle_unknownTypeReturnsError() {
    const QJsonObject reply =
        handleNativeMessage(QJsonObject{{QStringLiteral("type"), QStringLiteral("launch_missiles")},
                                        {QStringLiteral("id"), 9}},
                            QStringLiteral("sak-win32-mcp"),
                            QStringLiteral("1.0.0"));
    QCOMPARE(reply.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    QCOMPARE(reply.value(QStringLiteral("error")).toString(),
             QStringLiteral("Unsupported message type: 'launch_missiles'"));
    QCOMPARE(reply.value(QStringLiteral("id")), QJsonValue(9));  // id echoed, type intact

    // NEAR MISSES. The dispatch accepts EXACTLY one type, and "launch_missiles" shares no
    // prefix, suffix or substring with "ping", so every way of loosening that compare still
    // refused it. handleNativeMessage has no other caller in the tree, so nothing else
    // constrained the dispatch either.
    for (const QString& near_miss : {QStringLiteral("pingx"),
                                     QStringLiteral("xping"),
                                     QStringLiteral("ping "),
                                     QStringLiteral(" ping"),
                                     QStringLiteral("pin"),
                                     QStringLiteral("PING")}) {
        const QJsonObject refused = handleNativeMessage(
            QJsonObject{{QStringLiteral("type"), near_miss}, {QStringLiteral("id"), 9}},
            QStringLiteral("sak-win32-mcp"),
            QStringLiteral("1.0.0"));
        QCOMPARE(refused.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        QCOMPARE(refused.value(QStringLiteral("error")).toString(),
                 QStringLiteral("Unsupported message type: '%1'").arg(near_miss));
        // A pong carries the host identity; a refusal must not.
        QVERIFY2(!refused.contains(QStringLiteral("server")), qPrintable(near_miss));
    }
}

QTEST_MAIN(NativeMessagingTests)
#include "test_native_messaging.moc"
