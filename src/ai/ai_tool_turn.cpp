// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_turn.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>

#include <algorithm>

namespace sak::ai {

namespace {

constexpr auto kPendingToolTurnSchema = "sak.ai.pending_tool_turn.v1";

// Bounds on untrusted input (model responses and persisted user-dir snapshots). Generous
// enough to never reject a legitimate turn, but they stop a hostile snapshot from driving
// unbounded reserves/copies or per-call JSON parses into memory/CPU exhaustion.
constexpr int kMaxFunctionCallsPerTurn = 4096;
constexpr int kMaxArgumentsJsonChars = 1'048'576;  // 1 MiB of arguments_json per call

void setError(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

// Bind the snapshot to the run it belongs to. toJson writes a non-empty run_id, so a
// present-but-empty or wrong-typed run_id is tampering, not something a faithful
// snapshot produces -- reject it. When the caller knows which run it is resuming
// (expected_run_id non-empty), a mismatch means this pending turn is from a different
// run and must not be resumed here.
bool validateSnapshotRunId(const QJsonObject& state,
                           const QString& expected_run_id,
                           QString* error_message) {
    const QJsonValue run_id_value = state.value(QStringLiteral("run_id"));
    QString snapshot_run_id;
    if (!run_id_value.isUndefined() && !run_id_value.isNull()) {
        if (!run_id_value.isString() || run_id_value.toString().trimmed().isEmpty()) {
            setError(error_message,
                     QStringLiteral("Pending tool turn snapshot run id is not a valid string"));
            return false;
        }
        snapshot_run_id = run_id_value.toString().trimmed();
    }
    const QString expected = expected_run_id.trimmed();
    if (!expected.isEmpty() && snapshot_run_id != expected) {
        setError(error_message,
                 QStringLiteral(
                     "Pending tool turn snapshot run id does not match the expected run"));
        return false;
    }
    return true;
}

// Validate the snapshot's required scalar identity fields. The values themselves stay in
// restore() (they are needed for the final commit); only the guards move here so restore()
// reads as a flat sequence of fail-closed checks.
bool validateSnapshotScalarFields(const QString& response_id,
                                  const QJsonArray& calls_json,
                                  int call_index,
                                  QString* error_message) {
    if (response_id.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn snapshot missing response id"));
        return false;
    }
    if (calls_json.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn snapshot has no calls"));
        return false;
    }
    if (call_index < 0 || call_index >= calls_json.size()) {
        setError(error_message,
                 QStringLiteral("Pending tool turn snapshot has invalid call index"));
        return false;
    }
    return true;
}

}  // namespace

bool AiToolTurn::begin(QString response_id,
                       QVector<OpenAIFunctionCall> calls,
                       QString* error_message) {
    // Fail CLOSED without destroying existing state: validate everything BEFORE touching any
    // member. begin() previously reset() on failure, so feeding invalid calls (untrusted model
    // output) while a turn was mid-flight silently wiped its pending calls and completed
    // outputs. On failure the current turn is now left untouched; success overwrites cleanly.
    response_id = response_id.trimmed();
    if (response_id.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn missing response id"));
        return false;
    }
    if (calls.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn has no function calls"));
        return false;
    }
    if (!validateCalls(calls, error_message)) {
        return false;
    }

    m_response_id = response_id;
    m_calls = std::move(calls);
    m_outputs.clear();
    m_outputs.reserve(m_calls.size());
    m_call_index = 0;
    m_active = true;
    return true;
}

void AiToolTurn::reset() {
    m_active = false;
    m_response_id.clear();
    m_calls.clear();
    m_outputs.clear();
    m_call_index = 0;
}

int AiToolTurn::remainingCallCount() const noexcept {
    return m_active ? std::max(0, static_cast<int>(m_calls.size()) - m_call_index) : 0;
}

bool AiToolTurn::hasCurrentCall() const noexcept {
    return m_active && m_call_index >= 0 && m_call_index < m_calls.size();
}

const OpenAIFunctionCall* AiToolTurn::currentCall() const noexcept {
    return hasCurrentCall() ? &m_calls.at(m_call_index) : nullptr;
}

QString AiToolTurn::currentCallId() const {
    const auto* call = currentCall();
    return (call != nullptr) ? call->call_id : QString();
}

AiToolTurn::AdvanceResult AiToolTurn::appendOutput(OpenAIFunctionOutput output) {
    AdvanceResult result;
    if (!m_active) {
        result.error_message = QStringLiteral("No active pending tool turn");
        return result;
    }
    const auto* call = currentCall();
    if (call == nullptr) {
        result.error_message = QStringLiteral("Pending tool turn has no current call");
        return result;
    }
    if (output.call_id.trimmed().isEmpty()) {
        result.error_message = QStringLiteral("Tool output missing call id");
        return result;
    }
    if (output.call_id != call->call_id) {
        result.error_message = QStringLiteral("Tool output call id mismatch: expected %1, got %2")
                                   .arg(call->call_id, output.call_id);
        return result;
    }

    m_outputs.append(std::move(output));
    ++m_call_index;
    result.ok = true;
    result.finished = m_call_index >= m_calls.size();
    return result;
}

QVector<OpenAIFunctionOutput> AiToolTurn::takeOutputs() {
    // swap (not move + clear) so m_outputs is left provably empty without a moved-from access.
    QVector<OpenAIFunctionOutput> taken_outputs;
    taken_outputs.swap(m_outputs);
    return taken_outputs;
}

QJsonObject AiToolTurn::toJson(const QString& run_id) const {
    QJsonObject state;
    if (!m_active || m_response_id.trimmed().isEmpty()) {
        return state;
    }

    state[QStringLiteral("schema")] = QString::fromLatin1(kPendingToolTurnSchema);
    state[QStringLiteral("response_id")] = m_response_id;
    state[QStringLiteral("call_index")] = m_call_index;
    if (!run_id.trimmed().isEmpty()) {
        state[QStringLiteral("run_id")] = run_id.trimmed();
    }

    QJsonArray calls_array;
    for (const auto& call : m_calls) {
        calls_array.append(functionCallToJson(call));
    }
    state[QStringLiteral("calls")] = calls_array;

    QJsonArray outputs_array;
    for (const auto& output : m_outputs) {
        outputs_array.append(functionOutputToJson(output));
    }
    state[QStringLiteral("outputs")] = outputs_array;

    if (hasCurrentCall()) {
        state[QStringLiteral("current_call")] = functionCallToJson(m_calls.at(m_call_index));
    }
    return state;
}

bool AiToolTurn::snapshotOutputsIsArray(const QJsonValue& outputs_value, QString* error_message) {
    // A faithful snapshot always serializes an "outputs" array, so a present-but-non-array
    // value is tampered/malformed and must fail closed rather than be coerced to an empty
    // array by toArray().
    if (!outputs_value.isUndefined() && !outputs_value.isNull() && !outputs_value.isArray()) {
        setError(error_message,
                 QStringLiteral("Pending tool turn snapshot outputs is not an array"));
        return false;
    }
    return true;
}

bool AiToolTurn::validateSnapshotOutputsMatchCalls(const QVector<OpenAIFunctionOutput>& outputs,
                                                   const QVector<OpenAIFunctionCall>& calls,
                                                   int call_index,
                                                   QString* error_message) {
    // Each completed call appends exactly one output and advances the index in lockstep, so a
    // faithful snapshot has outputs.size() == call_index. Reject BOTH extra outputs and a short
    // outputs list -- the latter would forge completion of calls that never produced a result
    // and let resume jump past skipped calls.
    if (outputs.size() > call_index) {
        setError(error_message,
                 QStringLiteral("Pending tool turn snapshot has too many completed outputs"));
        return false;
    }
    if (outputs.size() < call_index) {
        setError(error_message,
                 QStringLiteral(
                     "Pending tool turn snapshot is missing outputs for completed calls"));
        return false;
    }
    // Each recorded output must correspond positionally to the call it answers; otherwise a
    // tampered snapshot could pair one call's output with another call's result.
    for (int i = 0; i < outputs.size(); ++i) {
        if (outputs.at(i).call_id != calls.at(i).call_id) {
            setError(error_message,
                     QStringLiteral("Pending tool turn snapshot output %1 does not match its call")
                         .arg(i));
            return false;
        }
    }
    return true;
}

bool AiToolTurn::restore(const QJsonObject& state,
                         QString* error_message,
                         const QString& expected_run_id) {
    // Fail CLOSED without destroying existing state: like begin(), validate the whole
    // (untrusted, user-dir-persisted) snapshot into locals and commit only on full success, so
    // a rejected/tampered snapshot cannot wipe an in-flight turn.
    const QString schema = state.value(QStringLiteral("schema")).toString();
    if (schema != QLatin1String(kPendingToolTurnSchema)) {
        setError(error_message, QStringLiteral("Unsupported pending tool turn schema"));
        return false;
    }

    if (!validateSnapshotRunId(state, expected_run_id, error_message)) {
        return false;
    }

    const QString response_id = state.value(QStringLiteral("response_id")).toString().trimmed();
    const QJsonArray calls_json = state.value(QStringLiteral("calls")).toArray();
    const int call_index = state.value(QStringLiteral("call_index")).toInt(-1);
    if (!validateSnapshotScalarFields(response_id, calls_json, call_index, error_message)) {
        return false;
    }

    const QJsonValue outputs_value = state.value(QStringLiteral("outputs"));
    if (!snapshotOutputsIsArray(outputs_value, error_message)) {
        return false;
    }

    QVector<OpenAIFunctionCall> decoded_calls;
    if (!decodeCalls(calls_json, &decoded_calls, error_message)) {
        return false;
    }

    QVector<OpenAIFunctionOutput> decoded_outputs;
    if (!decodeOutputs(outputs_value.toArray(), &decoded_outputs, error_message)) {
        return false;
    }
    if (!validateSnapshotOutputsMatchCalls(
            decoded_outputs, decoded_calls, call_index, error_message)) {
        return false;
    }

    m_response_id = response_id;
    m_calls = std::move(decoded_calls);
    m_outputs = std::move(decoded_outputs);
    m_call_index = call_index;
    m_active = true;
    return true;
}

QJsonObject AiToolTurn::functionCallToJson(const OpenAIFunctionCall& call) {
    QJsonObject obj;
    obj[QStringLiteral("call_id")] = call.call_id;
    obj[QStringLiteral("name")] = call.name;
    obj[QStringLiteral("arguments_json")] = call.arguments_json;
    return obj;
}

OpenAIFunctionCall AiToolTurn::functionCallFromJson(const QJsonObject& obj) {
    OpenAIFunctionCall call;
    call.call_id = obj.value(QStringLiteral("call_id")).toString();
    call.name = obj.value(QStringLiteral("name")).toString();
    call.arguments_json = obj.value(QStringLiteral("arguments_json")).toString();
    return call;
}

QJsonObject AiToolTurn::functionOutputToJson(const OpenAIFunctionOutput& output) {
    QJsonObject obj;
    obj[QStringLiteral("call_id")] = output.call_id;
    obj[QStringLiteral("output")] = output.output;
    return obj;
}

OpenAIFunctionOutput AiToolTurn::functionOutputFromJson(const QJsonObject& obj) {
    OpenAIFunctionOutput output;
    output.call_id = obj.value(QStringLiteral("call_id")).toString();
    output.output = obj.value(QStringLiteral("output")).toString();
    return output;
}

bool AiToolTurn::validateCall(const OpenAIFunctionCall& call, int index, QString* error_message) {
    if (call.call_id.trimmed().isEmpty()) {
        setError(error_message,
                 QStringLiteral("Function call at index %1 missing call id").arg(index));
        return false;
    }
    if (call.name.trimmed().isEmpty()) {
        setError(error_message, QStringLiteral("Function call %1 missing name").arg(call.call_id));
        return false;
    }
    if (call.arguments_json.size() > kMaxArgumentsJsonChars) {
        setError(error_message,
                 QStringLiteral("Function call %1 arguments_json exceeds the size limit")
                     .arg(call.call_id));
        return false;
    }
    // Validate the arguments up front so the whole batch is rejected atomically before ANY
    // call executes -- otherwise an earlier destructive call could run before a later
    // malformed one is refused. Empty arguments are allowed; a non-empty value must parse
    // to a JSON object (the function-call argument contract).
    const QString args = call.arguments_json.trimmed();
    if (!args.isEmpty()) {
        QJsonParseError parse_error{};
        const auto doc = QJsonDocument::fromJson(args.toUtf8(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            setError(error_message,
                     QStringLiteral("Function call %1 has malformed arguments_json: %2")
                         .arg(call.call_id, parse_error.errorString()));
            return false;
        }
    }
    return true;
}

bool AiToolTurn::validateCalls(const QVector<OpenAIFunctionCall>& calls, QString* error_message) {
    if (calls.size() > kMaxFunctionCallsPerTurn) {
        setError(
            error_message,
            QStringLiteral("Pending tool turn has too many function calls (%1)").arg(calls.size()));
        return false;
    }
    QSet<QString> seen_ids;
    for (int i = 0; i < calls.size(); ++i) {
        const auto& call = calls.at(i);
        if (!validateCall(call, i, error_message)) {
            return false;
        }
        if (seen_ids.contains(call.call_id)) {
            setError(error_message,
                     QStringLiteral("Duplicate function call id %1 in batch").arg(call.call_id));
            return false;
        }
        seen_ids.insert(call.call_id);
    }
    return true;
}

bool AiToolTurn::decodeCalls(const QJsonArray& calls_json,
                             QVector<OpenAIFunctionCall>* calls,
                             QString* error_message) {
    if (calls == nullptr) {
        setError(error_message, QStringLiteral("Pending tool turn call decode target missing"));
        return false;
    }
    if (calls_json.size() > kMaxFunctionCallsPerTurn) {
        setError(error_message, QStringLiteral("Pending tool turn snapshot has too many calls"));
        return false;
    }
    calls->clear();
    calls->reserve(calls_json.size());
    for (const auto& value : calls_json) {
        if (!value.isObject()) {
            setError(error_message, QStringLiteral("Pending tool turn call item is not an object"));
            return false;
        }
        const QJsonObject obj = value.toObject();
        // Fail CLOSED on a wrong-typed arguments_json: functionCallFromJson coerces a non-string
        // (number/object/array) to an empty string, which validateCall would then accept as a
        // legitimate no-argument call. A present arguments_json must be a JSON string.
        const QJsonValue args_value = obj.value(QStringLiteral("arguments_json"));
        if (!args_value.isUndefined() && !args_value.isNull() && !args_value.isString()) {
            setError(error_message,
                     QStringLiteral("Pending tool turn call arguments_json is not a string"));
            return false;
        }
        calls->append(functionCallFromJson(obj));
    }
    return validateCalls(*calls, error_message);
}

bool AiToolTurn::decodeOutputs(const QJsonArray& outputs_json,
                               QVector<OpenAIFunctionOutput>* outputs,
                               QString* error_message) {
    if (outputs == nullptr) {
        setError(error_message, QStringLiteral("Pending tool turn output decode target missing"));
        return false;
    }
    if (outputs_json.size() > kMaxFunctionCallsPerTurn) {
        setError(error_message, QStringLiteral("Pending tool turn snapshot has too many outputs"));
        return false;
    }
    outputs->clear();
    outputs->reserve(outputs_json.size());
    for (const auto& value : outputs_json) {
        if (!value.isObject()) {
            setError(error_message,
                     QStringLiteral("Pending tool turn output item is not an object"));
            return false;
        }
        const QJsonObject obj = value.toObject();
        // Fail CLOSED on a wrong-typed output body: a non-string "output" would be coerced to an
        // empty string by functionOutputFromJson, silently discarding a tampered field.
        const QJsonValue output_value = obj.value(QStringLiteral("output"));
        if (!output_value.isUndefined() && !output_value.isNull() && !output_value.isString()) {
            setError(error_message, QStringLiteral("Pending tool turn output is not a string"));
            return false;
        }
        const OpenAIFunctionOutput output = functionOutputFromJson(obj);
        if (output.call_id.trimmed().isEmpty()) {
            setError(error_message, QStringLiteral("Pending tool turn output missing call id"));
            return false;
        }
        outputs->append(output);
    }
    return true;
}

}  // namespace sak::ai
