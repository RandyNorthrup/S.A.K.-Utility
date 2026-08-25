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
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QtEndian>
#include <QtTest/QtTest>

#include <vector>

namespace {

// Width of the native-messaging length prefix; parseFrame consumes this plus the body
// length it decodes out of those same bytes.
constexpr int kFramePrefixBytes = 4;

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
    if (frame.status == Status::Ok) {
        // Ok proves the 4-byte prefix was present and drove the decode, so the exact consumed
        // count is derivable from the input this harness already holds: prefix + the
        // little-endian body length. Pinning the VALUE, not just "> 0", is what catches a
        // header-only or off-by-one consume -- the desync every caller that erases `consumed`
        // and re-parses would suffer, silently, on every one of these mutated inputs.
        const auto declared = static_cast<qint64>(
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(input.constData())));
        const qint64 expected = kFramePrefixBytes + declared;
        if (frame.consumed != expected) {
            return QStringLiteral("parseFrame Ok consumed %1, expected %2 + %3 = %4")
                .arg(frame.consumed)
                .arg(kFramePrefixBytes)
                .arg(declared)
                .arg(expected);
        }
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
    if (!parsed.isEmpty()) {
        // Expectation derived from the INPUT bytes, not from the object the parser handed back:
        // parseJsonLine returns doc.object() intact, so an accepted message must be exactly the
        // document that arrived on the wire. A version that MANUFACTURES the "2.0" tag instead
        // of refusing -- the fail-open normalization -- adds a key the input never carried and
        // trips here on every mutant that reaches it.
        const QJsonObject source = QJsonDocument::fromJson(input.trimmed()).object();
        if (parsed != source) {
            return QStringLiteral(
                "parseJsonLine returned an object the input bytes did not contain");
        }
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
    // On the all-pass path (guaranteed here: any failure QVERIFY2(false)-returns above),
    // run() increments iterations_run once per seed (checkSeeds) plus once per mutation
    // iteration, so the exact count is corpus.size() + the iteration budget. The old >=
    // bound would still pass if the mutation loop ran ZERO iterations -- the whole
    // campaign silently evaporating while the seed rounds alone satisfied it.
    QCOMPARE(outcome.iterations_run,
             static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
}

}  // namespace

class McpFramingFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void nativeMessagingFrameNeverMisframes() {
        // The fuzz invariant is one-directional (it never asserts a specific status), so pin the
        // deterministic seed table by name: each corpus entry's exact status and consumed count.
        using Status = sak::win32mcp::NativeFrame::Status;
        const std::vector<QByteArray> corpus = nativeFrameCorpus();
        // Empty buffer: cannot even read the length prefix.
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(0)).status, Status::NeedMore);
        // Two well-formed frames decode and consume the whole buffer (non-vacuity: proves the
        // table is not satisfiable by a decoder that rejects everything).
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(1)).status, Status::Ok);
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(1)).consumed,
                 static_cast<int>(corpus.at(1).size()));
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(2)).status, Status::Ok);
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(2)).consumed,
                 static_cast<int>(corpus.at(2).size()));
        // Declared length 4, body absent: a SHORT READ, not a framing failure. This is the
        // smallest in-range length, so it is the one the short-buffer guard's condition can be
        // narrowed past (e.g. adding `&& length > kLengthPrefixBytes`) while every other test
        // in the suite stays green -- the host would then kill the extension port on an
        // ordinary partial read of a minimal `{}` message.
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(3)).status, Status::NeedMore);
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(3)).consumed, 0);
        // Zero length, over-cap length, and a body that is not a JSON object are all
        // unrecoverable framing errors, and none of them consumes bytes.
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(4)).status, Status::Error);
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(5)).status, Status::Error);
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(6)).status, Status::Error);
        QCOMPARE(sak::win32mcp::parseFrame(corpus.at(6)).consumed, 0);

        runFuzz("parseFrame", nativeFrameCorpus(), nativeFrameInvariant);
    }

    void jsonRpcLineResultIsUnambiguous() {
        runFuzz("parseJsonLine", jsonRpcCorpus(), jsonRpcInvariant);
    }

    // R5-G10-9: the fuzz corpus tops out at a few dozen bytes and mutants grow by at most a
    // few bytes per round, so the pre-parse size ceiling (kMaxJsonRpcMessageBytes, the guard
    // that stops a hostile MCP server from flooding QJsonDocument::fromJson with one giant line)
    // is never reached by fuzzing. Drive it directly.
    void jsonRpcLineRefusesOverCeiling() {
        QString error;
        // One byte over the 16 MiB ceiling of pure 'a' bytes: rejected for SIZE before any DOM
        // is allocated. The "ceiling" reason (not the "Invalid ... JSON" parse reason) proves
        // the size branch fired first -- if the guard were removed, fromJson would run on the
        // garbage and the reason would be a parse error instead, failing this assertion.
        const QByteArray over(sak::ai::mcp::kMaxJsonRpcMessageBytes + 1, 'a');
        const QJsonObject rejected = sak::ai::mcp::parseJsonLine(over, &error);
        QVERIFY(rejected.isEmpty());
        // Byte-exact: the reason names the ceiling VALUE, which separates the size branch from
        // the parse-error and version-tag branches word for word.
        QCOMPARE(error, QStringLiteral("MCP message exceeds the 16777216-byte ceiling"));

        // Boundary: the guard is strictly greater-than, so a line of EXACTLY the ceiling is
        // legal and must still parse. A >= mutation would wrongly refuse a maximum-sized
        // message, and nothing else in the suite reaches this size.
        QByteArray at_ceiling = QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":1,\"pad\":\"");
        const QByteArray at_tail = QByteArrayLiteral("\"}");
        at_ceiling.append(QByteArray(
            sak::ai::mcp::kMaxJsonRpcMessageBytes - at_ceiling.size() - at_tail.size(), 'a'));
        at_ceiling.append(at_tail);
        QCOMPARE(at_ceiling.size(), sak::ai::mcp::kMaxJsonRpcMessageBytes);
        QString at_error;
        const QJsonObject at_parsed = sak::ai::mcp::parseJsonLine(at_ceiling, &at_error);
        QVERIFY2(at_error.isEmpty(), at_error.toUtf8().constData());
        QCOMPARE(at_parsed.value(QStringLiteral("id")).toInt(), 1);

        // Non-vacuity: a well-formed line under the ceiling still parses to a JSON-RPC object.
        const QByteArray ok("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n");
        const QJsonObject parsed = sak::ai::mcp::parseJsonLine(ok, &error);
        QVERIFY(!parsed.isEmpty());
        QVERIFY(error.isEmpty());
        // The WHOLE object, not one field: parseJsonLine returns doc.object() intact and its
        // callers correlate on "id" and dispatch on "result"/"error", so a version that kept
        // only the fields this test looked at would break every one of them.
        QCOMPARE(parsed,
                 QJsonObject({{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                              {QStringLiteral("id"), 1},
                              {QStringLiteral("result"), QJsonObject{}}}));
    }

    // R5-G18-4: jsonRpcLineResultIsUnambiguous only proves SOME reason was set -- the corpus
    // carries one seed per refusal branch (jsonRpcCorpus, lines 63-66) but the invariant cannot
    // tell which branch fired. The ceiling branch is pinned byte-exact above; pin the other two
    // the same way, because the reason is what callers surface (ai_mcp_stdio_client.cpp:244
    // hands it straight to fail()).
    void jsonRpcLineRefusalReasonNamesTheBranch() {
        // Valid JSON, but an ARRAY: refused by the !doc.isObject() arm of the two-arm guard
        // (ai_mcp_jsonrpc.h:91), which reports the PARSE reason. Dropping that arm is otherwise
        // silent: QJsonDocument::object() on an array document yields {}, so the message would
        // fall through to the version-tag guard and be refused with the WRONG reason while the
        // fuzz invariant (empty object + non-empty error) still held. Qt rejects every other
        // non-object top-level value as a parse error, so an array is the only input that
        // reaches this arm.
        QString array_error;
        const QJsonObject array_parsed = sak::ai::mcp::parseJsonLine(QByteArray("[1,2,3]\n"),
                                                                     &array_error);
        QVERIFY(array_parsed.isEmpty());
        QVERIFY2(array_error.startsWith(QStringLiteral("Invalid MCP stdio JSON response:")),
                 array_error.toUtf8().constData());

        // Raw garbage: same arm, reached through its parse-error half.
        QString garbage_error;
        const QJsonObject garbage_parsed =
            sak::ai::mcp::parseJsonLine(QByteArray("not json at all\n"), &garbage_error);
        QVERIFY(garbage_parsed.isEmpty());
        QVERIFY2(garbage_error.startsWith(QStringLiteral("Invalid MCP stdio JSON response:")),
                 garbage_error.toUtf8().constData());

        // A syntactically valid object that omits the version tag: the THIRD branch. Byte-exact
        // on this first-party string, so no other branch can masquerade as it.
        QString version_error;
        const QJsonObject version_parsed =
            sak::ai::mcp::parseJsonLine(QByteArray("{\"id\":7,\"result\":{}}\n"), &version_error);
        QVERIFY(version_parsed.isEmpty());
        QCOMPARE(version_error,
                 QStringLiteral("MCP message is missing the JSON-RPC 2.0 version tag"));
        // The three reasons are pairwise distinct, which is what makes the branch identifiable.
        QVERIFY(array_error != version_error);
    }
};

QTEST_APPLESS_MAIN(McpFramingFuzzTests)
#include "test_fuzz_mcp_framing.moc"
