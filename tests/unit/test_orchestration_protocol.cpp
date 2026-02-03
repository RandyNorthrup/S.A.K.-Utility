#include <QtTest/QtTest>

#include "sak/orchestration_protocol.h"

using namespace sak;

class OrchestrationProtocolTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void encodeDecodeRoundtrip();
};

void OrchestrationProtocolTests::encodeDecodeRoundtrip() {
    QJsonObject payload;
    payload["message_type"] = "DESTINATION_REGISTER";
    payload["protocol_version"] = "1.0";
    payload["destination_info"] = QJsonObject{{"hostname", "TEST-PC"}};

    QByteArray framed = OrchestrationProtocol::encodeMessage(payload);
    QByteArray buffer;

    auto messages = OrchestrationProtocol::readMessages(buffer, framed);
    QCOMPARE(messages.size(), 1);
    QCOMPARE(messages[0].value("message_type").toString(), QString("DESTINATION_REGISTER"));
    QCOMPARE(messages[0].value("destination_info").toObject().value("hostname").toString(), QString("TEST-PC"));
}

QTEST_MAIN(OrchestrationProtocolTests)
#include "test_orchestration_protocol.moc"
