// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/openai_response_types.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sak::ai {

/// @brief Owns active OpenAI function-call turn bookkeeping.
///
/// The UI decides how tools run. This class only tracks ordered calls, completed
/// outputs, resume snapshots, and strict state validation.
class AiToolTurn {
public:
    struct AdvanceResult {
        bool ok{false};
        bool finished{false};
        QString error_message;
    };

    [[nodiscard]] bool begin(QString response_id,
                             QVector<OpenAIFunctionCall> calls,
                             QString* error_message = nullptr);
    void reset();

    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] const QString& responseId() const noexcept { return m_response_id; }
    [[nodiscard]] int callIndex() const noexcept { return m_call_index; }
    [[nodiscard]] int callCount() const noexcept { return m_calls.size(); }
    [[nodiscard]] int completedOutputCount() const noexcept { return m_outputs.size(); }
    [[nodiscard]] int remainingCallCount() const noexcept;
    [[nodiscard]] bool hasCurrentCall() const noexcept;
    [[nodiscard]] const OpenAIFunctionCall* currentCall() const noexcept;
    [[nodiscard]] QString currentCallId() const;
    [[nodiscard]] const QVector<OpenAIFunctionCall>& calls() const noexcept { return m_calls; }
    [[nodiscard]] const QVector<OpenAIFunctionOutput>& outputs() const noexcept {
        return m_outputs;
    }

    [[nodiscard]] AdvanceResult appendOutput(OpenAIFunctionOutput output);
    [[nodiscard]] QVector<OpenAIFunctionOutput> takeOutputs();

    [[nodiscard]] QJsonObject toJson(const QString& run_id = {}) const;
    [[nodiscard]] bool restore(const QJsonObject& state, QString* error_message = nullptr);

    [[nodiscard]] static QJsonObject functionCallToJson(const OpenAIFunctionCall& call);
    [[nodiscard]] static OpenAIFunctionCall functionCallFromJson(const QJsonObject& obj);
    [[nodiscard]] static QJsonObject functionOutputToJson(const OpenAIFunctionOutput& output);
    [[nodiscard]] static OpenAIFunctionOutput functionOutputFromJson(const QJsonObject& obj);

private:
    [[nodiscard]] static bool validateCalls(const QVector<OpenAIFunctionCall>& calls,
                                            QString* error_message);
    [[nodiscard]] static bool decodeCalls(const QJsonArray& calls_json,
                                          QVector<OpenAIFunctionCall>* calls,
                                          QString* error_message);
    [[nodiscard]] static bool decodeOutputs(const QJsonArray& outputs_json,
                                            QVector<OpenAIFunctionOutput>* outputs,
                                            QString* error_message);

    QString m_response_id;
    QVector<OpenAIFunctionCall> m_calls;
    QVector<OpenAIFunctionOutput> m_outputs;
    int m_call_index{0};
    bool m_active{false};
};

}  // namespace sak::ai
