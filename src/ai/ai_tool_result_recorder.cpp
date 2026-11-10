// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_result_recorder.h"

#include "sak/ai/ai_conversation_store.h"
#include "sak/ai/ai_credential_store.h"

#include <QJsonObject>

#include <cmath>
#include <limits>

namespace sak::ai {

namespace {
constexpr int kToolCommandSummaryMaxChars = 700;
constexpr int kToolErrorSummaryMaxChars = 900;
constexpr int kToolStdoutSummaryMaxChars = 2400;
constexpr int kToolStderrSummaryMaxChars = 1600;
constexpr auto kToolTruncationMarker = "\n...[output truncated for chat view]";

QString redact(const QString& text, const ToolResultTextRedactor& redactor) {
    const QString credential_safe = CredentialStore::redactSecrets(text);
    return redactor ? redactor(credential_safe) : credential_safe;
}

void appendToolCommandSummary(QStringList* lines,
                              const QJsonObject& result,
                              const ToolResultTextRedactor& redactor) {
    const QString command = result.value(QStringLiteral("preview"))
                                .toString(result.value(QStringLiteral("command")).toString())
                                .trimmed();
    if (!command.isEmpty()) {
        *lines << QStringLiteral("Command: %1")
                      .arg(clippedToolResultText(command, kToolCommandSummaryMaxChars, redactor));
    }
}

QString exitCodeSummary(const QJsonValue& exit_code) {
    const double raw_exit_code = exit_code.toDouble(std::numeric_limits<double>::quiet_NaN());
    const bool valid_exit_code =
        exit_code.isDouble() && std::isfinite(raw_exit_code) &&
        std::trunc(raw_exit_code) == raw_exit_code &&
        raw_exit_code >= static_cast<double>(std::numeric_limits<int>::min()) &&
        raw_exit_code <= static_cast<double>(std::numeric_limits<int>::max());
    return valid_exit_code ? QStringLiteral("Exit code: %1").arg(static_cast<qint64>(raw_exit_code))
                           : QStringLiteral("Exit code: invalid");
}

QString executionStateSuffix(const QJsonObject& result) {
    if (result.value(QStringLiteral("cancelled")).toBool(false)) {
        return QStringLiteral(" (cancelled)");
    }
    if (result.value(QStringLiteral("timed_out")).toBool(false)) {
        return QStringLiteral(" (timed out)");
    }
    return {};
}

QString successSummary(const QJsonValue& success) {
    if (!success.isBool()) {
        return QStringLiteral("Success: invalid");
    }
    return QStringLiteral("Success: %1")
        .arg(success.toBool() ? QStringLiteral("yes") : QStringLiteral("no"));
}

void appendToolStatusSummary(QStringList* lines, const QJsonObject& result) {
    if (result.contains(QStringLiteral("exit_code"))) {
        *lines << exitCodeSummary(result.value(QStringLiteral("exit_code"))) +
                      executionStateSuffix(result);
        return;
    }
    if (result.contains(QStringLiteral("success"))) {
        *lines << successSummary(result.value(QStringLiteral("success")));
    }
}

struct ToolTextSummaryRequest {
    QString m_field;
    QString m_label;
    int m_max_chars{0};
};

void appendToolTextSummary(QStringList* lines,
                           const QJsonObject& result,
                           const ToolTextSummaryRequest& request,
                           const ToolResultTextRedactor& redactor) {
    const QString text = result.value(request.m_field).toString().trimmed();
    if (!text.isEmpty()) {
        *lines << QStringLiteral("%1: %2").arg(
            request.m_label, clippedToolResultText(text, request.m_max_chars, redactor));
    }
}

QString commandLabel(const AiToolResultRecordRequest& request) {
    const QString preview = request.command_preview.trimmed();
    if (!preview.isEmpty()) {
        return preview;
    }
    const QString command_id = request.command_id.trimmed();
    return command_id.isEmpty() ? QStringLiteral("AI tool command") : command_id;
}

void appendCommandRecord(ConversationStore* store,
                         const AiToolResultRecordRequest& request,
                         AiToolResultRecordResult* result) {
    if (store == nullptr) {
        result->errors << QStringLiteral(
            "Command record failed: AI conversation store is not initialized");
        return;
    }
    QString error;
    if (!store->appendCommand(commandLabel(request), request.result_json, &error)) {
        result->errors << QStringLiteral("Command record failed: %1").arg(error);
        return;
    }
    result->command_recorded = true;
}

void appendTranscriptRecord(ConversationStore* store,
                            const AiToolResultRecordRequest& request,
                            AiToolResultRecordResult* result) {
    // Validate the store BEFORE the empty-text short-circuit: a requested
    // transcript with a null store is a genuine failure that must be surfaced,
    // not silently swallowed just because the summary happened to be empty.
    if (store == nullptr) {
        result->errors << QStringLiteral(
            "Transcript log failed: AI conversation store is not initialized");
        return;
    }
    if (result->transcript_text.trimmed().isEmpty()) {
        return;
    }
    QString error;
    if (!store->appendTranscript(QStringLiteral("Tool Result"),
                                 result->transcript_text,
                                 request.transcript_metadata,
                                 &error)) {
        result->errors << QStringLiteral("Transcript log failed: %1").arg(error);
        return;
    }
    result->transcript_recorded = true;
}

}  // namespace

QString clippedToolResultText(QString text, int max_chars, const ToolResultTextRedactor& redactor) {
    text = redact(text, redactor).trimmed();
    if (max_chars > 0 && text.size() > max_chars) {
        const QString marker = QString::fromLatin1(kToolTruncationMarker);
        if (max_chars <= marker.size()) {
            return marker.left(max_chars);
        }
        text = text.left(max_chars - marker.size()).trimmed() + marker;
    }
    return text;
}

QString toolResultChatSummary(const QJsonObject& result, const ToolResultTextRedactor& redactor) {
    if (result.isEmpty()) {
        return {};
    }
    QStringList lines;
    appendToolCommandSummary(&lines, result, redactor);
    appendToolStatusSummary(&lines, result);
    const QString error = result.value(QStringLiteral("error_message")).toString().trimmed();
    if (!error.isEmpty()) {
        lines << QStringLiteral("Error: %1")
                     .arg(clippedToolResultText(error, kToolErrorSummaryMaxChars, redactor));
    }
    appendToolTextSummary(&lines,
                          result,
                          {.m_field = QStringLiteral("stdout"),
                           .m_label = QStringLiteral("Output"),
                           .m_max_chars = kToolStdoutSummaryMaxChars},
                          redactor);
    appendToolTextSummary(&lines,
                          result,
                          {.m_field = QStringLiteral("stderr"),
                           .m_label = QStringLiteral("Errors"),
                           .m_max_chars = kToolStderrSummaryMaxChars},
                          redactor);
    return lines.join(QStringLiteral("\n"));
}

AiToolResultRecordResult AiToolResultRecorder::record(ConversationStore* store,
                                                      const AiToolResultRecordRequest& request,
                                                      const ToolResultTextRedactor& redactor) {
    AiToolResultRecordResult result;
    result.transcript_text = toolResultChatSummary(request.result_json, redactor);
    if (request.record_command) {
        appendCommandRecord(store, request, &result);
    }
    if (request.record_transcript) {
        appendTranscriptRecord(store, request, &result);
    }
    return result;
}

}  // namespace sak::ai
