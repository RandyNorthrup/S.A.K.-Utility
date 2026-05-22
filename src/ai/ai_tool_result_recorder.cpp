// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_result_recorder.h"

#include "sak/ai/ai_conversation_store.h"

#include <QJsonObject>

namespace sak::ai {

namespace {

QString redact(const QString& text, const ToolResultTextRedactor& redactor) {
    return redactor ? redactor(text) : text;
}

void appendToolCommandSummary(QStringList* lines,
                              const QJsonObject& result,
                              const ToolResultTextRedactor& redactor) {
    const QString command = result.value(QStringLiteral("preview"))
                                .toString(result.value(QStringLiteral("command")).toString())
                                .trimmed();
    if (!command.isEmpty()) {
        *lines << QStringLiteral("Command: %1").arg(clippedToolResultText(command, 700, redactor));
    }
}

void appendToolStatusSummary(QStringList* lines, const QJsonObject& result) {
    if (result.contains(QStringLiteral("exit_code"))) {
        QString status =
            QStringLiteral("Exit code: %1").arg(result.value(QStringLiteral("exit_code")).toInt());
        if (result.value(QStringLiteral("cancelled")).toBool(false)) {
            status += QStringLiteral(" (cancelled)");
        } else if (result.value(QStringLiteral("timed_out")).toBool(false)) {
            status += QStringLiteral(" (timed out)");
        }
        *lines << status;
    } else if (result.contains(QStringLiteral("success"))) {
        *lines << QStringLiteral("Success: %1")
                      .arg(result.value(QStringLiteral("success")).toBool(false)
                               ? QStringLiteral("yes")
                               : QStringLiteral("no"));
    }
}

struct ToolTextSummaryRequest {
    QString field;
    QString label;
    int max_chars{0};
};

void appendToolTextSummary(QStringList* lines,
                           const QJsonObject& result,
                           const ToolTextSummaryRequest& request,
                           const ToolResultTextRedactor& redactor) {
    const QString text = result.value(request.field).toString().trimmed();
    if (!text.isEmpty()) {
        *lines << QStringLiteral("%1: %2").arg(
            request.label, clippedToolResultText(text, request.max_chars, redactor));
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
    if (!store) {
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
    if (result->transcript_text.trimmed().isEmpty()) {
        return;
    }
    if (!store) {
        result->errors << QStringLiteral(
            "Transcript log failed: AI conversation store is not initialized");
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
        text = text.left(max_chars - 64).trimmed() +
               QStringLiteral("\n...[output truncated for chat view]");
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
        lines << QStringLiteral("Error: %1").arg(clippedToolResultText(error, 900, redactor));
    }
    appendToolTextSummary(
        &lines,
        result,
        {.field = QStringLiteral("stdout"), .label = QStringLiteral("Output"), .max_chars = 2400},
        redactor);
    appendToolTextSummary(
        &lines,
        result,
        {.field = QStringLiteral("stderr"), .label = QStringLiteral("Errors"), .max_chars = 1600},
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
