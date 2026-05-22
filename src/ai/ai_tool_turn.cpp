// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_turn.h"

#include <QJsonArray>

#include <algorithm>

namespace sak::ai {

namespace {

constexpr auto kPendingToolTurnSchema = "sak.ai.pending_tool_turn.v1";

void setError(QString* error_message, const QString& message) {
    if (error_message) {
        *error_message = message;
    }
}

}  // namespace

bool AiToolTurn::begin(QString response_id,
                       QVector<OpenAIFunctionCall> calls,
                       QString* error_message) {
    response_id = response_id.trimmed();
    if (response_id.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn missing response id"));
        reset();
        return false;
    }
    if (calls.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn has no function calls"));
        reset();
        return false;
    }
    if (!validateCalls(calls, error_message)) {
        reset();
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
    return call ? call->call_id : QString();
}

AiToolTurn::AdvanceResult AiToolTurn::appendOutput(OpenAIFunctionOutput output) {
    AdvanceResult result;
    if (!m_active) {
        result.error_message = QStringLiteral("No active pending tool turn");
        return result;
    }
    const auto* call = currentCall();
    if (!call) {
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
    QVector<OpenAIFunctionOutput> outputs = std::move(m_outputs);
    m_outputs.clear();
    return outputs;
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

    QJsonArray calls;
    for (const auto& call : m_calls) {
        calls.append(functionCallToJson(call));
    }
    state[QStringLiteral("calls")] = calls;

    QJsonArray outputs;
    for (const auto& output : m_outputs) {
        outputs.append(functionOutputToJson(output));
    }
    state[QStringLiteral("outputs")] = outputs;

    if (hasCurrentCall()) {
        state[QStringLiteral("current_call")] = functionCallToJson(m_calls.at(m_call_index));
    }
    return state;
}

bool AiToolTurn::restore(const QJsonObject& state, QString* error_message) {
    const QString schema = state.value(QStringLiteral("schema")).toString();
    if (schema != QLatin1String(kPendingToolTurnSchema)) {
        setError(error_message, QStringLiteral("Unsupported pending tool turn schema"));
        reset();
        return false;
    }

    const QString response_id = state.value(QStringLiteral("response_id")).toString().trimmed();
    const QJsonArray calls_json = state.value(QStringLiteral("calls")).toArray();
    const int call_index = state.value(QStringLiteral("call_index")).toInt(-1);
    if (response_id.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn snapshot missing response id"));
        reset();
        return false;
    }
    if (calls_json.isEmpty()) {
        setError(error_message, QStringLiteral("Pending tool turn snapshot has no calls"));
        reset();
        return false;
    }
    if (call_index < 0 || call_index >= calls_json.size()) {
        setError(error_message,
                 QStringLiteral("Pending tool turn snapshot has invalid call index"));
        reset();
        return false;
    }

    QVector<OpenAIFunctionCall> calls;
    if (!decodeCalls(calls_json, &calls, error_message)) {
        reset();
        return false;
    }

    QVector<OpenAIFunctionOutput> outputs;
    if (!decodeOutputs(state.value(QStringLiteral("outputs")).toArray(), &outputs, error_message)) {
        reset();
        return false;
    }
    if (outputs.size() > call_index) {
        setError(error_message,
                 QStringLiteral("Pending tool turn snapshot has too many completed outputs"));
        reset();
        return false;
    }

    m_response_id = response_id;
    m_calls = std::move(calls);
    m_outputs = std::move(outputs);
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

bool AiToolTurn::validateCalls(const QVector<OpenAIFunctionCall>& calls, QString* error_message) {
    for (int i = 0; i < calls.size(); ++i) {
        const auto& call = calls.at(i);
        if (call.call_id.trimmed().isEmpty()) {
            setError(error_message,
                     QStringLiteral("Function call at index %1 missing call id").arg(i));
            return false;
        }
        if (call.name.trimmed().isEmpty()) {
            setError(error_message,
                     QStringLiteral("Function call %1 missing name").arg(call.call_id));
            return false;
        }
    }
    return true;
}

bool AiToolTurn::decodeCalls(const QJsonArray& calls_json,
                             QVector<OpenAIFunctionCall>* calls,
                             QString* error_message) {
    if (!calls) {
        setError(error_message, QStringLiteral("Pending tool turn call decode target missing"));
        return false;
    }
    calls->clear();
    calls->reserve(calls_json.size());
    for (const auto& value : calls_json) {
        if (!value.isObject()) {
            setError(error_message, QStringLiteral("Pending tool turn call item is not an object"));
            return false;
        }
        calls->append(functionCallFromJson(value.toObject()));
    }
    return validateCalls(*calls, error_message);
}

bool AiToolTurn::decodeOutputs(const QJsonArray& outputs_json,
                               QVector<OpenAIFunctionOutput>* outputs,
                               QString* error_message) {
    if (!outputs) {
        setError(error_message, QStringLiteral("Pending tool turn output decode target missing"));
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
        const OpenAIFunctionOutput output = functionOutputFromJson(value.toObject());
        if (output.call_id.trimmed().isEmpty()) {
            setError(error_message, QStringLiteral("Pending tool turn output missing call id"));
            return false;
        }
        outputs->append(output);
    }
    return true;
}

}  // namespace sak::ai
