// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_ai_response.cpp
/// @brief Mutation-fuzz of the OpenAI Responses reply parser (G14 parser sweep).
///
/// OpenAIResponsesClient::parseResponseObject turns a raw HTTP response body into an
/// OpenAIResponseResult, and the function_calls it extracts are dispatched to real tools. That body
/// is untrusted: a compromised or buggy endpoint, a proxy, or a truncated stream can hand the
/// parser hostile or partial JSON, so the contract is fail-closed ([[no-fallbacks-fail-closed]]):
///
///   1. A structurally-broken tool call (a function_call item missing its call_id or name) must
///      poison the WHOLE response, never be silently dropped so a well-formed sibling can still be
///      dispatched from a set the model only partially emitted.
///   2. A terminal/non-success status (incomplete, failed, cancelled, in_progress, a non-string
///      status, or any value other than "completed") carries no trustworthy output and must yield
///      an empty result -- no calls, no text, no id.
///   3. An oversized body, non-JSON, a non-object, or an API-error envelope must yield an empty
///      result.
///
/// parseResponseObject is a pure static over raw bytes, so this harness drives it over thousands of
/// mutated response bodies and asserts for EVERY input: no crash/hang; determinism; every returned
/// call is structurally complete (non-empty call_id AND name); and an INDEPENDENT oracle --
/// re-derived from the JSON, not copied from the parser's control flow -- that a must-be-empty
/// response (bad JSON / API error / terminal status) never surfaces any usable field.

#include "sak/ai/openai_responses_client.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

using sak::ai::OpenAIResponseResult;
using sak::ai::OpenAIResponsesClient;

// The parser's own body ceiling (openai_responses_client.cpp kMaxResponseBodyBytes); a body past it
// is rejected before any parse.
constexpr qint64 kMaxResponseBodyBytesForTest = 64LL * 1024 * 1024;

// A compact signature of the result fields the fuzzer reasons about; two parses of identical bytes
// must produce identical signatures (determinism).
struct ResultSignature {
    QString id;
    QString output_text;
    int call_count = 0;
    QString calls_joined;  // call_id|name|arguments per call, concatenated in order

    bool operator==(const ResultSignature&) const = default;
};

ResultSignature signatureOf(const OpenAIResponseResult& r) {
    ResultSignature sig;
    sig.id = r.id;
    sig.output_text = r.output_text;
    sig.call_count = static_cast<int>(r.function_calls.size());
    for (const auto& call : r.function_calls) {
        sig.calls_joined += call.call_id + QLatin1Char('|') + call.name + QLatin1Char('|') +
                            call.arguments_json + QLatin1Char('\n');
    }
    return sig;
}

// A result that surfaces no usable field -- what parseResponseObject returns on every fail-closed
// path (bad JSON, API error, broken call, terminal status).
bool resultIsEmpty(const OpenAIResponseResult& r) {
    return r.id.isEmpty() && r.output_text.isEmpty() && r.function_calls.isEmpty() &&
           r.citations.isEmpty();
}

// INDEPENDENT oracle: a SUFFICIENT (not exhaustive) condition for an empty result, re-derived from
// the bytes rather than copied from the parser. If this is true the parser MUST have failed closed;
// it may also be empty for reasons this oracle does not model (e.g. a broken call), so the
// invariant is one-directional and never asserts non-emptiness.
bool mustBeEmpty(const QByteArray& data) {
    if (data.size() > kMaxResponseBodyBytesForTest) {
        return true;  // oversized body rejected before parse
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return true;  // non-JSON or non-object -> responseRootObject fails
    }
    if (!OpenAIResponsesClient::extractApiError(data).isEmpty()) {
        return true;  // an API-error envelope is never a usable answer
    }
    const QJsonValue status = doc.object().value(QStringLiteral("status"));
    if (status.isUndefined() || status.isNull()) {
        return false;  // no status -> left to the normal output/empty checks
    }
    if (!status.isString()) {
        return true;  // a present, non-string status is terminal
    }
    return status.toString() != QStringLiteral("completed");
}

QString aiResponseInvariant(const QByteArray& input) {
    QString error_message;
    const OpenAIResponseResult result = OpenAIResponsesClient::parseResponseObject(input,
                                                                                   &error_message);

    // Determinism: a second parse of the same bytes must reach the same result.
    const OpenAIResponseResult again = OpenAIResponsesClient::parseResponseObject(input, nullptr);
    if (signatureOf(result) != signatureOf(again)) {
        return QStringLiteral("parseResponseObject is non-deterministic on identical input");
    }

    // Structural integrity: any call that survived into the result is complete. A dispatched call
    // with an empty call_id or name is exactly the half-formed tool call the fail-closed contract
    // must reject, so its presence is a security defect.
    for (const auto& call : result.function_calls) {
        if (call.call_id.isEmpty() || call.name.isEmpty()) {
            return QStringLiteral(
                "a function call with an empty call_id or name reached the result (fail-open)");
        }
    }

    // Independent oracle: a must-be-empty response never surfaces a usable field.
    if (mustBeEmpty(input) && !resultIsEmpty(result)) {
        return QStringLiteral(
            "a bad-JSON / API-error / terminal-status response surfaced a usable field "
            "(fail-open)");
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("ai response fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

// A completed response carrying one well-formed tool call -- the primary accept-path seed the
// mutator degrades from (into missing-field calls, terminal statuses, and malformed JSON).
QByteArray completedFunctionCallSeed() {
    return QByteArrayLiteral(
        "{\"id\":\"resp_1\",\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
        "\"call_id\":\"call_1\",\"name\":\"list_dir\",\"arguments\":\"{\\\"path\\\":\\\".\\\"}\"}],"
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":5,\"total_tokens\":15}}");
}

}  // namespace

class AiResponseFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parserNeverCrashesAndStaysFailClosed() {
        std::vector<QByteArray> corpus{
            completedFunctionCallSeed(),
            // Completed text message.
            QByteArrayLiteral(
                "{\"id\":\"r2\",\"status\":\"completed\",\"output\":[{\"type\":"
                "\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hi\"}]}]}"),
            // A structurally-broken tool call (missing name) -- must poison the whole response.
            QByteArrayLiteral("{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                              "\"call_id\":\"call_1\",\"arguments\":\"{}\"}]}"),
            // Terminal statuses: incomplete, failed, in_progress, and a non-string status.
            QByteArrayLiteral("{\"status\":\"incomplete\",\"output\":[{\"type\":\"function_call\","
                              "\"call_id\":\"c\",\"name\":\"n\",\"arguments\":\"{}\"}]}"),
            QByteArrayLiteral("{\"status\":\"failed\",\"error\":{\"message\":\"boom\"}}"),
            QByteArrayLiteral("{\"status\":\"in_progress\"}"),
            QByteArrayLiteral("{\"status\":123,\"output\":[]}"),
            // An API-error envelope -- never a usable answer.
            QByteArrayLiteral("{\"error\":{\"message\":\"invalid api key\",\"type\":\"auth\"}}"),
            // No status field (unit-fixture style) with usable output.
            QByteArrayLiteral("{\"output\":[{\"type\":\"message\",\"content\":[{\"type\":"
                              "\"output_text\",\"text\":\"ok\"}]}]}"),
            // Degenerate / empty / non-object / truncated bodies.
            QByteArrayLiteral("{}"),
            QByteArrayLiteral("{\"output\":[null,{},123]}"),
            QByteArrayLiteral("[]"),
            QByteArrayLiteral("42"),
            QByteArrayLiteral("not json"),
            QByteArrayLiteral("{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","),
            QByteArray(),
        };
        const sak::fuzz::Target target = [](const QByteArray& input) {
            return aiResponseInvariant(input);
        };
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }

    // Pin the anchors the fuzz oracle is built around: a completed call parses through, a broken
    // call poisons the whole response, and a terminal status yields nothing.
    void knownResponsesClassifyAsExpected() {
        QString err;
        const OpenAIResponseResult good =
            OpenAIResponsesClient::parseResponseObject(completedFunctionCallSeed(), &err);
        QCOMPARE(good.function_calls.size(), 1);
        QCOMPARE(good.function_calls.first().name, QStringLiteral("list_dir"));

        const OpenAIResponseResult broken = OpenAIResponsesClient::parseResponseObject(
            QByteArrayLiteral("{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                              "\"call_id\":\"c\",\"arguments\":\"{}\"}]}"),
            &err);
        QVERIFY(broken.function_calls.isEmpty());  // the broken call poisoned the whole response

        const OpenAIResponseResult terminal = OpenAIResponsesClient::parseResponseObject(
            QByteArrayLiteral("{\"status\":\"incomplete\",\"output\":[{\"type\":\"function_call\","
                              "\"call_id\":\"c\",\"name\":\"n\",\"arguments\":\"{}\"}]}"),
            &err);
        QVERIFY(terminal.function_calls.isEmpty());
        QVERIFY(terminal.output_text.isEmpty());
    }
};

QTEST_GUILESS_MAIN(AiResponseFuzzTests)
#include "test_fuzz_ai_response.moc"
