// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_protocol.h"

#include <QtTest/QtTest>

using namespace sak;

class TransferProtocolTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void encodeDecodeRoundtrip();
};

void TransferProtocolTests::encodeDecodeRoundtrip() {
    QJsonObject payload;
    payload["message_type"] = "HELLO";
    payload["protocol_version"] = "1.0";
    payload["hostname"] = "TEST-PC";

    QByteArray framed = TransferProtocol::encodeMessage(payload);
    QByteArray buffer;

    auto messages = TransferProtocol::readMessages(buffer, framed);
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages[0].value("hostname").toString(), QString("TEST-PC"));
    QCOMPARE(messages[0].value("message_type").toString(), QString("HELLO"));
}

QTEST_MAIN(TransferProtocolTests)
#include "test_transfer_protocol.moc"
