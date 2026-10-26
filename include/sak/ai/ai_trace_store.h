// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_token_usage_tracker.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak::ai {

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

struct AiActivityEventSeed {
    QString session_id;
    QString run_id;
    QString kind;
    QString state;
    QString summary;
    QJsonObject metadata;
};

/// @brief Persists the AI activity log (activity.jsonl) inside an AI session folder. It is the
/// one AI audit stream: run details, the session report and the timeline read it back.
class TraceStore {
public:
    explicit TraceStore(QString session_dir = {});

    void setSessionDirectory(const QString& session_dir);
    [[nodiscard]] QString sessionDirectory() const;

    // Overrides the per-stream size cap at which a JSONL file is rolled. Pass 0 to
    // restore the built-in defaults. Primarily an operator/retention tuning knob;
    // also lets tests exercise rotation without writing tens of megabytes.
    void setMaxJsonlBytes(qint64 max_bytes);
    [[nodiscard]] qint64 maxJsonlBytes() const;
    [[nodiscard]] QString activityPath() const;

    /// Rolls before the serialized record would cross either the stream or line-size cap.
    [[nodiscard]] bool appendActivityEvent(AiActivityEvent event,
                                           QString* error_message = nullptr) const;
    /// Returns no events and sets error_message when any record is incomplete or invalid.
    [[nodiscard]] QVector<AiActivityEvent> loadActivityEvents(
        QString* error_message = nullptr) const;

    [[nodiscard]] static AiActivityEvent activityEvent(const AiActivityEventSeed& seed);

private:
    QString m_session_dir;
    // 0 => use the per-stream built-in cap; > 0 => operator/test override.
    qint64 m_max_jsonl_bytes{0};
    // Serializes JSONL append/rotation and the loaders so concurrent AI worker threads
    // cannot interleave partial audit records or race a rotation rename. Mutable because
    // the append/load methods are logically const (they do not change the store's config).
    mutable QMutex m_io_mutex;
};

}  // namespace sak::ai
