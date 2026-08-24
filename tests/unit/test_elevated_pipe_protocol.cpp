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
#include "sak/elevated_pipe_server.h"

#include <QTest>

#include <array>

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
        // Round-trip anchor: the body this builder emits must be exactly what the READER's
        // TaskError schema gate demands, and that gate must refuse both malformed arms.
        // Nothing in the suite reaches taskErrorBodyIsValid today, so `case TaskError:
        // return false;` (every real helper error frame dropped -> helper_crashed instead
        // of the actual message) and `return true;` (a forged empty body accepted as an
        // error report) BOTH ship green.
        using T = sak::PipeMessageType;
        QVERIFY(sak::parsePayload(T::TaskError, json_bytes).valid);
        QVERIFY(!sak::parsePayload(T::TaskError, R"({"message":"Task not allowed"})").valid);
        QVERIFY(!sak::parsePayload(T::TaskError, R"({"code":413})").valid);
        QVERIFY(!sak::parsePayload(T::TaskError, R"({"code":"413","message":"x"})").valid);
        QVERIFY(!sak::parsePayload(T::TaskError, R"({"code":413,"message":{}})").valid);
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
        // That refusal is decided by the payload.isEmpty() branch and never reaches the
        // TaskResult field schema. Add the non-vacuity anchor (a well-formed body IS
        // accepted) plus both refusal arms of taskResultBodyIsValid, which nothing else in
        // the suite exercises: with no accept anchor a blanket `case TaskResult: return
        // false;` would drop every real helper result (each task -> helper_crashed) and
        // still ship green.
        using T = sak::PipeMessageType;
        QVERIFY(sak::parsePayload(T::TaskResult, R"({"success":true,"data":{}})").valid);
        QVERIFY(!sak::parsePayload(T::TaskResult, R"({"success":1,"data":{}})").valid);
        QVERIFY(!sak::parsePayload(T::TaskResult, R"({"data":{}})").valid);
        QVERIFY(!sak::parsePayload(T::TaskResult, R"({"success":true})").valid);
        QVERIFY(!sak::parsePayload(T::TaskResult, R"({"success":true,"data":[]})").valid);
    }

    void testParseInvalidJson() {
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::TaskRequest,
                                                 "not valid json!!!");
        QVERIFY(!msg.valid);
    }

    void testParseJsonArray() {
        // Arrays should be rejected -- we only accept objects
        sak::PipeMessage msg = sak::parsePayload(sak::PipeMessageType::TaskRequest, "[1,2,3]");
        QVERIFY(!msg.valid);
    }

    // ======================================================================
    // Pipe Name Generation
    // ======================================================================

    void testPipeNameFormat() {
        QString name = sak::generatePipeName();
        QVERIFY(name.startsWith(sak::kPipeBasePath));
        // Pin the exact generated shape instead of a length floor: base + decimal pid + '_' +
        // exactly kPipeNonceHexWidth lowercase hex digits. "longer than the base path" is already
        // satisfied by the pid digits alone, so a nonce truncated to a couple of hex digits ships
        // green (testPipeNameUniqueness only flakes, it does not fail) -- and a guessable name is
        // exactly what the 64-bit CSPRNG nonce exists to prevent.
        const QString suffix = name.mid(static_cast<int>(strlen(sak::kPipeBasePath)));
        const int sep = static_cast<int>(suffix.indexOf(QLatin1Char('_')));
        QVERIFY2(sep > 0, qPrintable(name));
        QCOMPARE(suffix.left(sep),
                 QString::number(static_cast<quint32>(QCoreApplication::applicationPid())));
        const QString nonce = suffix.mid(sep + 1);
        QCOMPARE(static_cast<int>(nonce.size()), sak::kPipeNonceHexWidth);
        for (const QChar ch : nonce) {
            const char16_t code = ch.unicode();
            QVERIFY2((code >= u'0' && code <= u'9') || (code >= u'a' && code <= u'f'),
                     qPrintable(name));
        }
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
        QCOMPARE(sak::kHelperTimeoutMs, 300'000);  // 5 min inactivity timeout
        QCOMPARE(sak::kPipeConnectTimeoutMs, 10'000);
        QCOMPARE(sak::kPipeIoTimeoutMs, 30'000);
    }

    // ======================================================================
    // B5-04: client PID gate fails closed
    // ======================================================================

    void testClientPidGateFailsClosed() {
        using S = sak::ElevatedPipeServer;
        // The matching case: a valid parent PID and an equal client PID.
        QVERIFY(S::clientPidMatchesParent(4321, 4321));
        // A different client PID is rejected.
        QVERIFY(!S::clientPidMatchesParent(4321, 9999));
        // Fail closed: a missing/invalid expected PID authorizes nobody, even if
        // the client reports the same (invalid) value. Previously <=0 SKIPPED the
        // check and accepted any client.
        QVERIFY(!S::clientPidMatchesParent(0, 0));
        QVERIFY(!S::clientPidMatchesParent(-1, -1));
        QVERIFY(!S::clientPidMatchesParent(0, 1234));
    }

    // ======================================================================
    // CODEX_REVIEW_4 M-B3-5: a broken pipe must be distinguishable from no-data so a
    // dead client cancels the active privileged task instead of looping forever.
    // ======================================================================

    void testClassifyPeekDistinguishesBrokenFromNoData() {
        using S = sak::ElevatedPipeServer;
        QCOMPARE(S::classifyPeek(false, 0), S::PipePoll::Broken);  // failed peek -> broken
        QCOMPARE(S::classifyPeek(false, 8), S::PipePoll::Broken);  // failed peek wins over bytes
        QCOMPARE(S::classifyPeek(true, 0), S::PipePoll::NoData);   // ok, nothing pending
        QCOMPARE(S::classifyPeek(true, 5), S::PipePoll::MessageReady);
    }

    // ======================================================================
    // R5-G10-9: the elevated helper's wire protocol must fail closed on a
    // hostile body from the (Builtin-Users-admitted) non-elevated client. The
    // prior parse tests covered malformed JSON, arrays, and empty payloads, but
    // not a structurally-valid object that omits/blanks the per-type required
    // fields -- the exact case where a missing "task" becomes a blank/default
    // DESTRUCTIVE target downstream.
    // ======================================================================

    void testParseTaskRequestRejectsMissingOrBlankFields() {
        using T = sak::PipeMessageType;
        // An empty object omits both fields -> refused (would dispatch a nameless task).
        QVERIFY(!sak::parsePayload(T::TaskRequest, "{}").valid);
        // A blank task id is refused (jsonHasNonEmptyString), not treated as a task named "".
        QVERIFY(!sak::parsePayload(T::TaskRequest, R"({"task":"","payload":{}})").valid);
        // A named task with no object payload is refused.
        QVERIFY(!sak::parsePayload(T::TaskRequest, R"({"task":"Check Disk Errors"})").valid);
        // A payload with no task id is refused.
        QVERIFY(!sak::parsePayload(T::TaskRequest, R"({"payload":{}})").valid);
        // Non-vacuity anchor: the fully-formed request IS accepted, so the negatives
        // above prove the schema gate, not a blanket reject.
        QVERIFY(sak::parsePayload(T::TaskRequest, R"({"task":"Check Disk Errors","payload":{}})")
                    .valid);
        // A CancelRequest must likewise name a non-empty task.
        QVERIFY(!sak::parsePayload(T::CancelRequest, "{}").valid);
        QVERIFY(!sak::parsePayload(T::CancelRequest, R"({"task":""})").valid);
        QVERIFY(sak::parsePayload(T::CancelRequest, R"({"task":"Reset Network"})").valid);
    }

    void testParseRejectsUnknownTypeAndPayloadlessTypeWithBody() {
        using T = sak::PipeMessageType;
        // A spoofed / out-of-range type byte from the untrusted client matches no
        // switch case and must be refused, never silently mapped to a handler.
        QVERIFY(!sak::parsePayload(static_cast<T>(0x99), R"({"task":"x","payload":{}})").valid);
        QVERIFY(!sak::parsePayload(static_cast<T>(0x00), R"({"any":"body"})").valid);
        // Shutdown/Ready carry no payload at all; an accompanying JSON body is malformed
        // (only the empty-payload form, tested above, is valid for these).
        QVERIFY(!sak::parsePayload(T::Shutdown, R"({"x":1})").valid);
        QVERIFY(!sak::parsePayload(T::Ready, R"({"x":1})").valid);
    }

    void testFrameMessageRefusesOverCapPayload() {
        using T = sak::PipeMessageType;
        // Over the 4 MiB ceiling frameMessage returns {} -- a larger payload would
        // narrow through uint32_t, truncate the framed length, and desync the stream.
        const QByteArray over(static_cast<int>(sak::kPipeMaxPayload) + 1, 'a');
        QVERIFY(sak::frameMessage(T::TaskResult, over).isEmpty());
        // Non-vacuity: a payload at exactly the cap still frames (header + cap bytes),
        // so the refusal is the size gate, not an always-empty result.
        const QByteArray at_cap(static_cast<int>(sak::kPipeMaxPayload), 'a');
        const QByteArray framed = sak::frameMessage(T::TaskResult, at_cap);
        QCOMPARE(framed.size(), sak::kPipeHeaderSize + at_cap.size());
    }

    void testDecodeFrameHeaderRejectsOversizedDeclaredLength() {
        using S = sak::ElevatedPipeServer;

        // Build a 5-byte frame header the way the wire encodes it: 4-byte little-endian
        // length prefix + 1 type byte. This prefix is fully attacker-controlled -- it
        // arrives off the untrusted pipe BEFORE any payload -- so an oversized declared
        // length must be refused so readMessage() fails closed BEFORE resizing a buffer
        // and reading that many bytes.
        auto headerFor = [](uint32_t len, sak::PipeMessageType type) {
            std::array<char, sak::kPipeHeaderSize> h{};
            h[sak::kPipeFrameLengthByte0] = static_cast<char>(len & sak::kPipeFrameByteMask);
            h[sak::kPipeFrameLengthByte1] =
                static_cast<char>((len >> sak::kPipeFrameByteShift1) & sak::kPipeFrameByteMask);
            h[sak::kPipeFrameLengthByte2] =
                static_cast<char>((len >> sak::kPipeFrameByteShift2) & sak::kPipeFrameByteMask);
            h[sak::kPipeFrameLengthByte3] =
                static_cast<char>((len >> sak::kPipeFrameByteShift3) & sak::kPipeFrameByteMask);
            h[sak::kPipeFrameTypeByte] = static_cast<char>(type);
            return h;
        };

        // One byte over the 4 MiB ceiling -> refused, and the length decodes exactly.
        const auto over = headerFor(sak::kPipeMaxPayload + 1, sak::PipeMessageType::TaskRequest);
        const auto over_decoded = S::decodeFrameHeader(over.data());
        QVERIFY(!over_decoded.length_within_cap);
        QCOMPARE(over_decoded.payload_len, sak::kPipeMaxPayload + 1);

        // Max uint32 (all length bytes 0xFF) -> refused, and still decodes exactly:
        // proves the top-byte <<24 shift is well defined, not sign-extended garbage that
        // could wrap back under the cap.
        const auto maxlen = headerFor(0xFF'FF'FF'FFu, sak::PipeMessageType::TaskResult);
        const auto max_decoded = S::decodeFrameHeader(maxlen.data());
        QVERIFY(!max_decoded.length_within_cap);
        QCOMPARE(max_decoded.payload_len, 0xFF'FF'FF'FFu);

        // Non-vacuity control: a length exactly AT the cap is accepted, and both the
        // length and the type round-trip -- so the refusals above are the ceiling gate,
        // not an always-false result.
        const auto at_cap = headerFor(sak::kPipeMaxPayload, sak::PipeMessageType::TaskRequest);
        const auto at_decoded = S::decodeFrameHeader(at_cap.data());
        QVERIFY(at_decoded.length_within_cap);
        QCOMPARE(at_decoded.payload_len, sak::kPipeMaxPayload);
        QCOMPARE(static_cast<uint8_t>(at_decoded.type),
                 static_cast<uint8_t>(sak::PipeMessageType::TaskRequest));

        // A small, ordinary length is accepted too (the common case).
        const auto small = headerFor(16U, sak::PipeMessageType::ProgressUpdate);
        QVERIFY(S::decodeFrameHeader(small.data()).length_within_cap);
    }
};

QTEST_GUILESS_MAIN(TestElevatedPipeProtocol)
#include "test_elevated_pipe_protocol.moc"
