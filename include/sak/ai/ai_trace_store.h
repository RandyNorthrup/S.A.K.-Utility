// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_token_usage_tracker.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak::ai {

struct AiTraceEvent {
    QDateTime timestamp_utc;
    QString run_id;
    QString parent_span_id;
    QString span_id;
    QString kind;
    QString name;
    QString status;
    qint64 duration_ms{0};
    TokenUsage token_usage;
    QJsonObject metadata;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiTraceEvent fromJson(const QJsonObject& object);
};

struct AiActivityEvent {
    QDateTime timestamp_utc;
    QString session_id;
    QString run_id;
    QString parent_id;
    QString activity_id;
    QString kind;
    QString workflow_id;
    QString phase_id;
    QString agent_id;
    QString state;
    QString summary;
    QString tool_name;
    TokenUsage token_usage;
    QStringList artifact_refs;
    QStringList evidence_refs;
    QString question_for_human;
    QString recovery_action;
    QString error;
    QJsonObject metadata;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiActivityEvent fromJson(const QJsonObject& object);
};

struct AiTraceEventSeed {
    QString run_id;
    QString kind;
    QString name;
    QString status;
    QJsonObject metadata;
    QString parent_span_id;
};

struct AiActivityEventSeed {
    QString session_id;
    QString run_id;
    QString kind;
    QString state;
    QString summary;
    QJsonObject metadata;
};

/// @brief Persists AI run trace events as JSONL inside an AI session folder.
class TraceStore {
public:
    explicit TraceStore(QString session_dir = {});

    void setSessionDirectory(const QString& session_dir);
    [[nodiscard]] QString sessionDirectory() const;
    [[nodiscard]] QString tracePath() const;
    [[nodiscard]] QString activityPath() const;

    [[nodiscard]] bool appendEvent(AiTraceEvent event, QString* error_message = nullptr) const;
    [[nodiscard]] QVector<AiTraceEvent> loadEvents(QString* error_message = nullptr) const;
    [[nodiscard]] bool appendActivityEvent(AiActivityEvent event,
                                           QString* error_message = nullptr) const;
    [[nodiscard]] QVector<AiActivityEvent> loadActivityEvents(
        QString* error_message = nullptr) const;

    [[nodiscard]] static AiTraceEvent event(const AiTraceEventSeed& seed);
    [[nodiscard]] static AiActivityEvent activityEvent(const AiActivityEventSeed& seed);

private:
    QString m_session_dir;
};

}  // namespace sak::ai
