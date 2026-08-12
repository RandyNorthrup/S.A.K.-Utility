// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_mcp_framing.cpp
/// @brief Mutation-fuzz of the two byte-framed transports the control bridge parses
///        (G14-12: browser-extension JSON contract, plus the MCP stdio JSON-RPC line).
///
/// Both decoders sit on an untrusted byte stream: parseFrame() reads Chrome native-
/// messaging frames (a 4-byte little-endian length prefix + UTF-8 JSON body) from the
/// browser extension, and parseJsonLine() reads newline-delimited JSON-RPC from the
/// bundled control engine. Each carries the exact first-party bounds logic a fuzzer
/// should hammer -- a length prefix that must be range-checked before it drives an
/// allocation, a 64 MiB / 16 MiB ceiling, an endian decode, and a short-buffer path --
/// ahead of QJsonDocument::fromJson. The harness feeds thousands of mutated buffers
/// through each and asserts the framing contract holds for EVERY input:
///
///   parseFrame:   consumed always stays within [0, buffer size]; an Ok frame consumes
///                 a positive number of bytes; a NeedMore frame consumes nothing; and
///                 re-parsing the tail after an Ok never faults.
///   parseJsonLine: exactly one of {parsed object, error string} is populated -- a
///                 success yields a non-empty object with the "2.0" version tag and no
///                 error, a rejection yields an empty object and a reason.
///
/// A consumed value that ran past the buffer, or a success that left the version tag
/// unchecked, is the class of off-by-one/overflow defect this exists to catch. No
/// crash / no hang is implicit: a fault never returns the empty invariant string.

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/win32mcp/native_messaging.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

// Seed corpus for parseFrame: valid frames, a truncated frame, a zero-length prefix,
// and an oversized length prefix -- the boundaries the length check guards.
std::vector<QByteArray> nativeFrameCorpus() {
    return {
        QByteArray(),
        sak::win32mcp::encodeFrame(QJsonObject{{QStringLiteral("method"), QStringLiteral("ping")}}),
        sak::win32mcp::encodeFrame(
            QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("result"), QJsonObject{}}}),
        QByteArray::fromHex("04000000"),          // length says 4, body absent -> NeedMore
        QByteArray::fromHex("00000000"),          // zero-length frame
        QByteArray::fromHex("ffffffff41"),        // huge length prefix + one byte
        QByteArray::fromHex("03000000") + "{}x",  // length 3, non-object-ish body
    };
}

// Seed corpus for parseJsonLine: a well-formed line, a version-less object, non-object
// JSON, and raw garbage.
std::vector<QByteArray> jsonRpcCorpus() {
    return {
        QByteArray(),
        sak::ai::mcp::jsonLine(sak::ai::mcp::toolsListPayload(1)),
        QByteArray("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{}}\n"),
        QByteArray("{\"id\":7,\"result\":{}}\n"),  // missing the 2.0 version tag
        QByteArray("[1,2,3]\n"),                   // valid JSON, not an object
        QByteArray("not json at all\n"),
    };
}

QString nativeFrameInvariant(const QByteArray& input) {
    const sak::win32mcp::NativeFrame frame = sak::win32mcp::parseFrame(input);
    if (frame.consumed < 0 || frame.consumed > input.size()) {
        return QStringLiteral("parseFrame consumed %1 outside [0, %2]")
            .arg(frame.consumed)
            .arg(input.size());
    }
    using Status = sak::win32mcp::NativeFrame::Status;
    if (frame.status == Status::Ok && frame.consumed <= 0) {
        return QStringLiteral("parseFrame returned Ok but consumed no bytes");
    }
    if (frame.status == Status::NeedMore && frame.consumed != 0) {
        return QStringLiteral("parseFrame returned NeedMore but consumed %1 bytes")
            .arg(frame.consumed);
    }
    if (frame.status == Status::Ok) {
        // Re-parsing the remainder after a good frame must also be crash-safe.
        static_cast<void>(sak::win32mcp::parseFrame(input.mid(frame.consumed)));
    }
    return {};
}

QString jsonRpcInvariant(const QByteArray& input) {
    QString error;
    const QJsonObject parsed = sak::ai::mcp::parseJsonLine(input, &error);
    // Exactly one of {parsed object, error} is populated: success -> object, no error;
    // rejection -> empty object, a reason.
    if (parsed.isEmpty() == error.isEmpty()) {
        return QStringLiteral("parseJsonLine ambiguous result: object empty=%1, error empty=%2")
            .arg(parsed.isEmpty())
            .arg(error.isEmpty());
    }
    if (!parsed.isEmpty() &&
        parsed.value(QStringLiteral("jsonrpc")).toString() != QLatin1String("2.0")) {
        return QStringLiteral("parseJsonLine accepted a message without the 2.0 version tag");
    }
    return {};
}

QByteArray failureBanner(const QString& label, const sak::fuzz::FuzzOutcome& outcome) {
    const QString message = QStringLiteral("%1 fuzz failed after %2 inputs: %3\n  bytes (hex): %4")
                                .arg(label)
                                .arg(outcome.iterations_run)
                                .arg(outcome.failure_detail,
                                     sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

void runFuzz(const char* label,
             const std::vector<QByteArray>& corpus,
             const sak::fuzz::Target& target) {
    const sak::fuzz::FuzzOutcome outcome =
        sak::fuzz::run(corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
    if (!outcome.ok) {
        const QByteArray banner = failureBanner(QString::fromLatin1(label), outcome);
        QVERIFY2(false, banner.constData());
    }
    QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
}

}  // namespace

class McpFramingFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void nativeMessagingFrameNeverMisframes() {
        runFuzz("parseFrame", nativeFrameCorpus(), nativeFrameInvariant);
    }

    void jsonRpcLineResultIsUnambiguous() {
        runFuzz("parseJsonLine", jsonRpcCorpus(), jsonRpcInvariant);
    }
};

QTEST_APPLESS_MAIN(McpFramingFuzzTests)
#include "test_fuzz_mcp_framing.moc"
