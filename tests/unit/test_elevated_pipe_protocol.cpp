// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevated_pipe_protocol.cpp
/// @brief Unit tests for the Named Pipe IPC wire protocol (Phase 2)
///
///  - Message framing (header encoding, payload round-trip)
///  - All builder functions (TaskRequest, ProgressUpdate, etc.)
///  - Payload parsing (valid + invalid JSON)
///  - Pipe name generation (uniqueness, format)

#include "sak/elevated_pipe_protocol.h"

#include <QTest>

class TestElevatedPipeProtocol : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ======================================================================
    // Frame Encoding
    // ======================================================================

    void testFrameEmpty() {
        QByteArray frame = sak::frameMessage(sak::PipeMessageType::Shutdown);
        QCOMPARE(frame.size(), sak::kPipeHeaderSize);

        // First 4 bytes = payload length (0, little-endian)
        QCOMPARE(static_cast<uint8_t>(frame[0]), 0);
        QCOMPARE(static_cast<uint8_t>(frame[1]), 0);
        QCOMPARE(static_cast<uint8_t>(frame[2]), 0);
        QCOMPARE(static_cast<uint8_t>(frame[3]), 0);

        // Byte 4 = message type
        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::Shutdown));
    }

    void testFrameWithPayload() {
        QByteArray payload = R"({"key":"value"})";
        QByteArray frame = sak::frameMessage(sak::PipeMessageType::TaskResult, payload);

        QCOMPARE(frame.size(), sak::kPipeHeaderSize + payload.size());

        // Decode length from first 4 bytes (little-endian)
        uint32_t encoded_len = static_cast<uint8_t>(frame[0]) |
                               (static_cast<uint8_t>(frame[1]) << 8) |
                               (static_cast<uint8_t>(frame[2]) << 16) |
                               (static_cast<uint8_t>(frame[3]) << 24);
        QCOMPARE(encoded_len, static_cast<uint32_t>(payload.size()));

        // Type byte
        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::TaskResult));

        // Payload bytes follow
        QByteArray extracted = frame.mid(sak::kPipeHeaderSize);
        QCOMPARE(extracted, payload);
    }

    void testFrameLengthEncoding() {
        // Test with a payload > 255 bytes to exercise multi-byte length
        QByteArray payload(300, 'A');
        QByteArray frame = sak::frameMessage(sak::PipeMessageType::ProgressUpdate, payload);

        uint32_t encoded_len = static_cast<uint8_t>(frame[0]) |
                               (static_cast<uint8_t>(frame[1]) << 8) |
                               (static_cast<uint8_t>(frame[2]) << 16) |
                               (static_cast<uint8_t>(frame[3]) << 24);
        QCOMPARE(encoded_len, 300u);
    }

    // ======================================================================
    // Builder Functions
    // ======================================================================

    void testBuildTaskRequest() {
        QJsonObject payload;
        payload["drive"] = "C:";
        QByteArray frame = sak::buildTaskRequest("Check Disk Errors", payload);

        // Must start with a valid header
        QVERIFY(frame.size() > sak::kPipeHeaderSize);
        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::TaskRequest));

        // Parse the payload back
        QByteArray json_bytes = frame.mid(sak::kPipeHeaderSize);
        auto doc = QJsonDocument::fromJson(json_bytes);
        QVERIFY(doc.isObject());
        QCOMPARE(doc["task"].toString(), "Check Disk Errors");
        QCOMPARE(doc["payload"].toObject()["drive"].toString(), "C:");
    }

    void testBuildProgressUpdate() {
        QByteArray frame = sak::buildProgressUpdate(42, "Scanning...");

        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::ProgressUpdate));

        QByteArray json_bytes = frame.mid(sak::kPipeHeaderSize);
        auto doc = QJsonDocument::fromJson(json_bytes);
        QCOMPARE(doc["percent"].toInt(), 42);
        QCOMPARE(doc["status"].toString(), "Scanning...");
    }

    void testBuildTaskResult() {
        QJsonObject data;
        data["items_fixed"] = 3;
        QByteArray frame = sak::buildTaskResult(true, data);

        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::TaskResult));

        QByteArray json_bytes = frame.mid(sak::kPipeHeaderSize);
        auto doc = QJsonDocument::fromJson(json_bytes);
        QCOMPARE(doc["success"].toBool(), true);
        QCOMPARE(doc["data"].toObject()["items_fixed"].toInt(), 3);
    }

    void testBuildTaskError() {
        QByteArray frame = sak::buildTaskError(413, "Task not allowed");

        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::TaskError));

        QByteArray json_bytes = frame.mid(sak::kPipeHeaderSize);
        auto doc = QJsonDocument::fromJson(json_bytes);
        QCOMPARE(doc["code"].toInt(), 413);
        QCOMPARE(doc["message"].toString(), "Task not allowed");
    }

    void testBuildCancelRequest() {
        QByteArray frame = sak::buildCancelRequest("Reset Network");

        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::CancelRequest));

        QByteArray json_bytes = frame.mid(sak::kPipeHeaderSize);
        auto doc = QJsonDocument::fromJson(json_bytes);
        QCOMPARE(doc["task"].toString(), "Reset Network");
    }

    void testBuildShutdown() {
        QByteArray frame = sak::buildShutdown();
        QCOMPARE(frame.size(), sak::kPipeHeaderSize);
        QCOMPARE(static_cast<uint8_t>(frame[4]),
                 static_cast<uint8_t>(sak::PipeMessageType::Shutdown));
    }

    void testBuildReady() {
        QByteArray frame = sak::buildReady();
        QCOMPARE(frame.size(), sak::kPipeHeaderSize);
        QCOMPARE(static_cast<uint8_t>(frame[4]), static_cast<uint8_t>(sak::PipeMessageType::Ready));
    }

    // ======================================================================
    // Payload Parsing
    // ======================================================================

    void testParseProgressPayload() {
        QByteArray json = R"({"percent":75,"status":"Almost done"})";
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::ProgressUpdate, json);

        QVERIFY(msg.valid);
        QCOMPARE(msg.type, sak::PipeMessageType::ProgressUpdate);
        QCOMPARE(msg.json["percent"].toInt(), 75);
        QCOMPARE(msg.json["status"].toString(), "Almost done");
    }

    void testParseEmptyPayloadShutdown() {
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::Shutdown, {});
        QVERIFY(msg.valid);
        QCOMPARE(msg.type, sak::PipeMessageType::Shutdown);
    }

    void testParseEmptyPayloadReady() {
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::Ready, {});
        QVERIFY(msg.valid);
        QCOMPARE(msg.type, sak::PipeMessageType::Ready);
    }

    void testParseEmptyPayloadForNonEmptyType() {
        // TaskResult with empty payload should be invalid
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::TaskResult, {});
        QVERIFY(!msg.valid);
    }

    void testParseInvalidJson() {
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::TaskRequest,
                                                 "not valid json!!!");
        QVERIFY(!msg.valid);
    }

    void testParseJsonArray() {
        // Arrays should be rejected — we only accept objects
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::TaskRequest, "[1,2,3]");
        QVERIFY(!msg.valid);
    }

    // ======================================================================
    // Pipe Name Generation
    // ======================================================================

    void testPipeNameFormat() {
        QString name = sak::generatePipeName();
        QVERIFY(name.startsWith(sak::kPipeBasePath));
        QVERIFY(name.length() > static_cast<int>(strlen(sak::kPipeBasePath)));
    }

    void testPipeNameUniqueness() {
        QString name1 = sak::generatePipeName();
        QString name2 = sak::generatePipeName();
        QVERIFY(name1 != name2);
    }

    // ======================================================================
    // Constants
    // ======================================================================

    void testProtocolConstants() {
        QCOMPARE(sak::kPipeHeaderSize, 5);
        QCOMPARE(sak::kPipeMaxPayload, 4u * 1024u * 1024u);
        QVERIFY(sak::kHelperTimeoutMs > 0);
        QVERIFY(sak::kPipeConnectTimeoutMs > 0);
        QVERIFY(sak::kPipeIoTimeoutMs > 0);
    }
};

QTEST_GUILESS_MAIN(TestElevatedPipeProtocol)
#include "test_elevated_pipe_protocol.moc"
