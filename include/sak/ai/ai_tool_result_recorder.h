// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace sak::ai {

class ConversationStore;

using ToolResultTextRedactor = std::function<QString(const QString&)>;

struct AiToolResultRecordRequest {
    QString command_id;
    QString command_preview;
    QJsonObject result_json;
    QJsonObject transcript_metadata;
    bool record_command{true};
    bool record_transcript{true};
};

struct AiToolResultRecordResult {
    QString transcript_text;
    bool command_recorded{false};
    bool transcript_recorded{false};
    QStringList errors;

    [[nodiscard]] bool ok() const noexcept { return errors.isEmpty(); }
    [[nodiscard]] bool wroteStore() const noexcept {
        return command_recorded || transcript_recorded;
    }
};

[[nodiscard]] QString clippedToolResultText(QString text,
                                            int max_chars,
                                            const ToolResultTextRedactor& redactor = {});
[[nodiscard]] QString toolResultChatSummary(const QJsonObject& result,
                                            const ToolResultTextRedactor& redactor = {});

class AiToolResultRecorder {
public:
    [[nodiscard]] static AiToolResultRecordResult record(
        ConversationStore* store,
        const AiToolResultRecordRequest& request,
        const ToolResultTextRedactor& redactor = {});
};

}  // namespace sak::ai
