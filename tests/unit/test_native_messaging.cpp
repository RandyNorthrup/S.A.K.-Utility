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
    // Fewer than 4 bytes: cannot even read the length.
    QCOMPARE(parseFrame(frame.left(2)).status, NativeFrame::Status::NeedMore);
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
    QCOMPARE(reply.value(QStringLiteral("protocol")).toInt(), kBrowserBridgeProtocol);
    QCOMPARE(reply.value(QStringLiteral("id")).toInt(), 42);
    QVERIFY(reply.value(QStringLiteral("pid")).toDouble() > 0);
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
    QCOMPARE(reply.value(QStringLiteral("id")).toInt(), 9);  // id echoed even on error
}

QTEST_MAIN(NativeMessagingTests)
#include "test_native_messaging.moc"
