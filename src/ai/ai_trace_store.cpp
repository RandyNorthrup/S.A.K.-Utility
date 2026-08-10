// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_trace_store.h"

#include "sak/ai/ai_credential_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QUuid>

#include <cmath>
#include <utility>

namespace sak::ai {

namespace {

constexpr auto kTraceFile = "trace.jsonl";
constexpr auto kActivityFile = "activity.jsonl";
constexpr auto kReplayFile = "turn_replay.jsonl";
constexpr int kTraceSchemaVersion = 1;
constexpr qint64 kMaxTraceJsonlBytes = 32LL * 1024LL * 1024LL;
constexpr qint64 kMaxActivityJsonlBytes = 32LL * 1024LL * 1024LL;
constexpr qint64 kMaxReplayJsonlBytes = 20LL * 1024LL * 1024LL;
// When a JSONL stream reaches its cap it is rolled (path -> path.1 -> path.2 ...)
// rather than dropped, so the audit trail never silently stops. The ring is
// bounded, so total on-disk size stays at about cap * (kMaxRolledJsonlFiles + 1).
constexpr int kMaxRolledJsonlFiles = 3;
constexpr qsizetype kMaxReplayMetadataStringChars = 4096;
constexpr qint64 kMaxReplayMetadataBytes = 64LL * 1024LL;
constexpr int kMaxRedactedArrayItems = 200;
constexpr int kMaxRedactedObjectMembers = 200;
constexpr int kMaxJsonRedactionDepth = 8;
constexpr qsizetype kTraceIdSuffixChars = 12;
// Bounds for loading a (possibly attacker-tampered) JSONL file: one line can never balloon
// memory past this, and the loader stops and reports once the aggregate exceeds this, so a
// swapped-in giant file cannot force unbounded readLine()/QVector growth.
constexpr qint64 kMaxJsonlLineBytes = 4LL * 1024LL * 1024LL;
constexpr qint64 kMaxJsonlLoadBytes = 128LL * 1024LL * 1024LL;

QString nowIso() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QDateTime parseDate(const QJsonObject& obj, const QString& key) {
    return QDateTime::fromString(obj.value(key).toString(), Qt::ISODateWithMs);
}

// A JSON number can be non-finite or outside qint64 range; casting such a double straight to
// qint64 is undefined behavior. Clamp to a valid, non-negative range and fail safe.
qint64 safeDurationMs(const QJsonValue& value) {
    const double raw = value.toDouble(0.0);
    if (!std::isfinite(raw) || raw <= 0.0) {
        return 0;
    }
    constexpr double kMaxSafeDuration = 9.0e18;  // < INT64_MAX and exactly representable
    return raw >= kMaxSafeDuration ? static_cast<qint64>(kMaxSafeDuration)
                                   : static_cast<qint64>(raw);
}

QJsonObject tokenUsageToJson(const TokenUsage& usage) {
    QJsonObject obj;
    obj[QStringLiteral("input_tokens")] = static_cast<double>(usage.input_tokens);
    obj[QStringLiteral("cached_input_tokens")] = static_cast<double>(usage.cached_input_tokens);
    obj[QStringLiteral("output_tokens")] = static_cast<double>(usage.output_tokens);
    obj[QStringLiteral("reasoning_tokens")] = static_cast<double>(usage.reasoning_tokens);
    obj[QStringLiteral("total_tokens")] = static_cast<double>(usage.total_tokens);
    return obj;
}

QStringList stringListFromJson(const QJsonValue& value) {
    QStringList result;
    const auto array = value.toArray();
    for (const auto& item : array) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) {
            result.append(text);
        }
    }
    return result;
}

QJsonArray stringListToJson(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            array.append(trimmed);
        }
    }
    return array;
}

bool sensitiveKey(const QString& key) {
    const QString name = key.trimmed().toLower();
    static const QStringList markers = {QStringLiteral("password"),
                                        QStringLiteral("passwd"),
                                        QStringLiteral("secret"),
                                        QStringLiteral("token"),
                                        QStringLiteral("api_key"),
                                        QStringLiteral("api-key"),
                                        QStringLiteral("apikey"),
                                        QStringLiteral("authorization"),
                                        QStringLiteral("bearer"),
                                        QStringLiteral("cookie"),
                                        QStringLiteral("credential"),
                                        QStringLiteral("ciphertext"),
                                        QStringLiteral("recovery_password")};
    for (const auto& marker : markers) {
        if (name.contains(marker)) {
            return true;
        }
    }
    return false;
}

QJsonValue redactAndCapJson(const QJsonValue& value, const QString& key = {}, int depth = 0);

QJsonObject redactObject(const QJsonObject& object, int depth) {
    QJsonObject out;
    int members = 0;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (members >= kMaxRedactedObjectMembers) {
            out.insert(QStringLiteral("__truncated__"), QStringLiteral("[truncated-object]"));
            break;
        }
        out.insert(it.key(), redactAndCapJson(it.value(), it.key(), depth + 1));
        ++members;
    }
    return out;
}

QJsonArray redactArray(const QJsonArray& array, const QString& key, int depth) {
    QJsonArray out;
    for (int i = 0; i < array.size() && i < kMaxRedactedArrayItems; ++i) {
        out.append(redactAndCapJson(array.at(i), key, depth + 1));
    }
    if (array.size() > kMaxRedactedArrayItems) {
        out.append(QStringLiteral("[truncated-array]"));
    }
    return out;
}

QString redactString(QString text) {
    text = CredentialStore::redactSecrets(std::move(text));
    if (text.size() > kMaxReplayMetadataStringChars) {
        text = text.left(kMaxReplayMetadataStringChars) + QStringLiteral("...[truncated]");
    }
    return text;
}

QJsonValue redactAndCapJson(const QJsonValue& value, const QString& key, int depth) {
    if (sensitiveKey(key)) {
        return QStringLiteral("[redacted]");
    }
    if (depth > kMaxJsonRedactionDepth) {
        return QStringLiteral("[truncated-depth]");
    }
    if (value.isObject()) {
        return redactObject(value.toObject(), depth);
    }
    if (value.isArray()) {
        return redactArray(value.toArray(), key, depth);
    }
    if (value.isString()) {
        return redactString(value.toString());
    }
    return value;
}

QJsonObject redactedReplayMetadata(const QJsonObject& metadata) {
    QJsonObject redacted = redactAndCapJson(metadata).toObject();
    const QByteArray compact = QJsonDocument(redacted).toJson(QJsonDocument::Compact);
    if (compact.size() <= kMaxReplayMetadataBytes) {
        return redacted;
    }

    QJsonObject capped;
    capped[QStringLiteral("truncated")] = true;
    capped[QStringLiteral("original_size_bytes")] = compact.size();
    capped[QStringLiteral("preview")] =
        CredentialStore::redactSecrets(QString::fromUtf8(compact.left(kMaxReplayMetadataBytes)));
    return capped;
}

qint64 jsonlSizeCapForLabel(const QString& label) {
    if (label == QLatin1String("turn replay")) {
        return kMaxReplayJsonlBytes;
    }
    if (label == QLatin1String("activity")) {
        return kMaxActivityJsonlBytes;
    }
    return kMaxTraceJsonlBytes;
}

bool writeAppendPayload(QFile& file,
                        const QJsonObject& object,
                        const QString& label,
                        QString* error_message) {
    const qint64 start_size = file.size();
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (file.write(payload) == payload.size() && file.flush()) {
        return true;
    }
    // Roll the partial line back so the JSONL stays parseable and report failure.
    file.resize(start_size);
    if (error_message) {
        *error_message = QStringLiteral("Could not append %1: %2").arg(label, file.errorString());
    }
    return false;
}

QString rolledJsonlPath(const QString& path, int index) {
    return QStringLiteral("%1.%2").arg(path).arg(index);
}

// Rolls a full JSONL file out of the way so appends can continue on a fresh file:
// drops the oldest rolled file, shifts path.(n-1) -> path.n down the ring, then
// moves the live file to path.1. Returns false only if a filesystem op fails, in
// which case the caller must NOT write (fail safe -- never clobber the trail).
bool rotateJsonl(const QString& path, const QString& label, QString* error_message) {
    const auto fail = [&](const QString& detail) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not rotate %1 audit file: %2").arg(label, detail);
        }
        return false;
    };
    const QString oldest = rolledJsonlPath(path, kMaxRolledJsonlFiles);
    if (QFile::exists(oldest) && !QFile::remove(oldest)) {
        return fail(oldest);
    }
    for (int i = kMaxRolledJsonlFiles - 1; i >= 1; --i) {
        const QString from = rolledJsonlPath(path, i);
        if (QFile::exists(from) && !QFile::rename(from, rolledJsonlPath(path, i + 1))) {
            return fail(from);
        }
    }
    if (QFile::exists(path) && !QFile::rename(path, rolledJsonlPath(path, 1))) {
        return fail(path);
    }
    return true;
}

bool appendJsonLine(const QString& path,
                    const QJsonObject& object,
                    const QString& label,
                    qint64 cap_override,
                    QString* error_message) {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not create %1 directory: %2").arg(label, info.absolutePath());
        }
        return false;
    }
    const qint64 cap = cap_override > 0 ? cap_override : jsonlSizeCapForLabel(label);
    if (info.exists() && info.size() >= cap && !rotateJsonl(path, label, error_message)) {
        // Rotation failed: fail safe rather than risk clobbering the audit trail.
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not append %1: %2").arg(label, file.errorString());
        }
        return false;
    }
    return writeAppendPayload(file, object, label, error_message);
}

}  // namespace

QJsonObject AiTraceEvent::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("schema_version")] = kTraceSchemaVersion;
    obj[QStringLiteral("timestamp")] = (timestamp_utc.isValid() ? timestamp_utc
                                                                : QDateTime::currentDateTimeUtc())
                                           .toString(Qt::ISODateWithMs);
    obj[QStringLiteral("run_id")] = run_id;
    obj[QStringLiteral("parent_span_id")] = parent_span_id;
    obj[QStringLiteral("span_id")] = span_id;
    obj[QStringLiteral("kind")] = kind;
    obj[QStringLiteral("name")] = name;
    obj[QStringLiteral("status")] = status;
    obj[QStringLiteral("duration_ms")] = static_cast<double>(duration_ms);
    obj[QStringLiteral("token_usage")] = tokenUsageToJson(token_usage);
    // Redact here too: trace.jsonl must share the same secret-redaction guarantee
    // as turn_replay.jsonl, or workflow input values / command previews containing
    // passwords, bearer tokens or API keys are persisted in cleartext.
    obj[QStringLiteral("metadata")] = redactedReplayMetadata(metadata);
    return obj;
}

AiTraceEvent AiTraceEvent::fromJson(const QJsonObject& object) {
    AiTraceEvent event;
    event.timestamp_utc = parseDate(object, QStringLiteral("timestamp"));
    event.run_id = object.value(QStringLiteral("run_id")).toString();
    event.parent_span_id = object.value(QStringLiteral("parent_span_id")).toString();
    event.span_id = object.value(QStringLiteral("span_id")).toString();
    event.kind = object.value(QStringLiteral("kind")).toString();
    event.name = object.value(QStringLiteral("name")).toString();
    event.status = object.value(QStringLiteral("status")).toString();
    event.duration_ms = safeDurationMs(object.value(QStringLiteral("duration_ms")));
    event.token_usage =
        TokenUsageTracker::fromJson(object.value(QStringLiteral("token_usage")).toObject());
    event.metadata = object.value(QStringLiteral("metadata")).toObject();
    return event;
}

QJsonObject AiActivityEvent::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("schema_version")] = kTraceSchemaVersion;
    obj[QStringLiteral("timestamp")] = (timestamp_utc.isValid() ? timestamp_utc
                                                                : QDateTime::currentDateTimeUtc())
                                           .toString(Qt::ISODateWithMs);
    obj[QStringLiteral("session_id")] = session_id;
    obj[QStringLiteral("run_id")] = run_id;
    obj[QStringLiteral("parent_id")] = parent_id;
    obj[QStringLiteral("activity_id")] = activity_id;
    obj[QStringLiteral("kind")] = kind;
    obj[QStringLiteral("workflow_id")] = workflow_id;
    obj[QStringLiteral("phase_id")] = phase_id;
    obj[QStringLiteral("agent_id")] = agent_id;
    obj[QStringLiteral("state")] = state;
    // Free-text fields carry model/tool output where secrets realistically appear, and are
    // otherwise length-unbounded; route them through the same redact+cap as metadata so a
    // token or a huge blob cannot land verbatim in the audit line.
    obj[QStringLiteral("summary")] = redactString(summary);
    obj[QStringLiteral("tool_name")] = tool_name;
    obj[QStringLiteral("token_usage")] = tokenUsageToJson(token_usage);
    obj[QStringLiteral("artifact_refs")] = stringListToJson(artifact_refs);
    obj[QStringLiteral("evidence_refs")] = stringListToJson(evidence_refs);
    obj[QStringLiteral("question_for_human")] = redactString(question_for_human);
    obj[QStringLiteral("recovery_action")] = redactString(recovery_action);
    obj[QStringLiteral("error")] = redactString(error);
    // Same redaction as trace.jsonl / turn_replay.jsonl (see AiTraceEvent::toJson).
    obj[QStringLiteral("metadata")] = redactedReplayMetadata(metadata);
    return obj;
}

AiActivityEvent AiActivityEvent::fromJson(const QJsonObject& object) {
    AiActivityEvent event;
    event.timestamp_utc = parseDate(object, QStringLiteral("timestamp"));
    event.session_id = object.value(QStringLiteral("session_id")).toString();
    event.run_id = object.value(QStringLiteral("run_id")).toString();
    event.parent_id = object.value(QStringLiteral("parent_id")).toString();
    event.activity_id = object.value(QStringLiteral("activity_id")).toString();
    event.kind = object.value(QStringLiteral("kind")).toString();
    event.workflow_id = object.value(QStringLiteral("workflow_id")).toString();
    event.phase_id = object.value(QStringLiteral("phase_id")).toString();
    event.agent_id = object.value(QStringLiteral("agent_id")).toString();
    event.state = object.value(QStringLiteral("state")).toString();
    event.summary = object.value(QStringLiteral("summary")).toString();
    event.tool_name = object.value(QStringLiteral("tool_name")).toString();
    event.token_usage =
        TokenUsageTracker::fromJson(object.value(QStringLiteral("token_usage")).toObject());
    event.artifact_refs = stringListFromJson(object.value(QStringLiteral("artifact_refs")));
    event.evidence_refs = stringListFromJson(object.value(QStringLiteral("evidence_refs")));
    event.question_for_human = object.value(QStringLiteral("question_for_human")).toString();
    event.recovery_action = object.value(QStringLiteral("recovery_action")).toString();
    event.error = object.value(QStringLiteral("error")).toString();
    event.metadata = object.value(QStringLiteral("metadata")).toObject();
    return event;
}

TraceStore::TraceStore(QString session_dir) : m_session_dir(std::move(session_dir)) {}

void TraceStore::setSessionDirectory(const QString& session_dir) {
    // Same mutex the locked append/load hold, so a config change from one thread cannot race
    // (C++ data race UB) a JSONL write/read in flight on an AI worker thread.
    const QMutexLocker locker(&m_io_mutex);
    m_session_dir = session_dir;
}

void TraceStore::setMaxJsonlBytes(qint64 max_bytes) {
    const QMutexLocker locker(&m_io_mutex);
    m_max_jsonl_bytes = max_bytes > 0 ? max_bytes : 0;
}

qint64 TraceStore::maxJsonlBytes() const {
    const QMutexLocker locker(&m_io_mutex);
    return m_max_jsonl_bytes;
}

QString TraceStore::sessionDirectory() const {
    const QMutexLocker locker(&m_io_mutex);
    return m_session_dir;
}

QString TraceStore::tracePath() const {
    return m_session_dir.isEmpty() ? QString()
                                   : QDir(m_session_dir).filePath(QString::fromLatin1(kTraceFile));
}

QString TraceStore::activityPath() const {
    return m_session_dir.isEmpty()
               ? QString()
               : QDir(m_session_dir).filePath(QString::fromLatin1(kActivityFile));
}

QString TraceStore::replayPath() const {
    return m_session_dir.isEmpty() ? QString()
                                   : QDir(m_session_dir).filePath(QString::fromLatin1(kReplayFile));
}

bool TraceStore::appendEvent(AiTraceEvent event, QString* error_message) const {
    const QMutexLocker locker(&m_io_mutex);
    if (m_session_dir.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Trace session directory is empty");
        }
        return false;
    }
    if (!QDir().mkpath(m_session_dir)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not create trace directory: %1").arg(m_session_dir);
        }
        return false;
    }
    if (!event.timestamp_utc.isValid()) {
        event.timestamp_utc = QDateTime::currentDateTimeUtc();
    }
    if (event.span_id.isEmpty()) {
        event.span_id = QStringLiteral("span_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(kTraceIdSuffixChars));
    }

    return appendJsonLine(
        tracePath(), event.toJson(), QStringLiteral("trace"), m_max_jsonl_bytes, error_message);
}

QVector<AiTraceEvent> TraceStore::loadEvents(QString* error_message) const {
    const QMutexLocker locker(&m_io_mutex);
    QVector<AiTraceEvent> events;
    QFile file(tracePath());
    if (!file.exists()) {
        return events;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not read trace: %1").arg(file.errorString());
        }
        return events;
    }

    qint64 loaded_bytes = 0;
    while (!file.atEnd()) {
        const QByteArray raw_line = file.readLine(kMaxJsonlLineBytes);
        loaded_bytes += raw_line.size();
        if (loaded_bytes > kMaxJsonlLoadBytes) {
            if (error_message) {
                *error_message = QStringLiteral("Trace file exceeds the %1-byte load cap")
                                     .arg(kMaxJsonlLoadBytes);
            }
            break;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(raw_line.trimmed(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        events.append(AiTraceEvent::fromJson(doc.object()));
    }
    return events;
}

bool TraceStore::appendActivityEvent(AiActivityEvent event, QString* error_message) const {
    const QMutexLocker locker(&m_io_mutex);
    if (m_session_dir.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Trace session directory is empty");
        }
        return false;
    }
    if (!event.timestamp_utc.isValid()) {
        event.timestamp_utc = QDateTime::currentDateTimeUtc();
    }
    if (event.activity_id.isEmpty()) {
        event.activity_id = QStringLiteral("act_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(kTraceIdSuffixChars));
    }
    return appendJsonLine(activityPath(),
                          event.toJson(),
                          QStringLiteral("activity"),
                          m_max_jsonl_bytes,
                          error_message);
}

QVector<AiActivityEvent> TraceStore::loadActivityEvents(QString* error_message) const {
    const QMutexLocker locker(&m_io_mutex);
    QVector<AiActivityEvent> events;
    QFile file(activityPath());
    if (!file.exists()) {
        return events;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = QStringLiteral("Could not read activity: %1").arg(file.errorString());
        }
        return events;
    }

    qint64 loaded_bytes = 0;
    while (!file.atEnd()) {
        const QByteArray raw_line = file.readLine(kMaxJsonlLineBytes);
        loaded_bytes += raw_line.size();
        if (loaded_bytes > kMaxJsonlLoadBytes) {
            if (error_message) {
                *error_message = QStringLiteral("Activity file exceeds the %1-byte load cap")
                                     .arg(kMaxJsonlLoadBytes);
            }
            break;
        }
        QJsonParseError parse_error;
        const auto doc = QJsonDocument::fromJson(raw_line.trimmed(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        events.append(AiActivityEvent::fromJson(doc.object()));
    }
    return events;
}

bool TraceStore::appendReplayEvent(const QString& run_id,
                                   const QString& event_type,
                                   const QString& status,
                                   QJsonObject metadata,
                                   QString* error_message) const {
    const QMutexLocker locker(&m_io_mutex);
    if (m_session_dir.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Trace session directory is empty");
        }
        return false;
    }
    QJsonObject object;
    object[QStringLiteral("schema_version")] = kTraceSchemaVersion;
    object[QStringLiteral("timestamp")] = nowIso();
    object[QStringLiteral("run_id")] = run_id;
    object[QStringLiteral("event_type")] = event_type;
    object[QStringLiteral("status")] = status;
    const QString prompt_hash = metadata.value(QStringLiteral("prompt_sha256")).toString();
    if (!prompt_hash.isEmpty()) {
        object[QStringLiteral("prompt_sha256")] = prompt_hash;
        metadata.remove(QStringLiteral("prompt_sha256"));
    }
    const QString model = metadata.value(QStringLiteral("model")).toString();
    if (!model.isEmpty()) {
        object[QStringLiteral("model")] = model;
    }
    const QString tool_name = metadata.value(QStringLiteral("tool_name")).toString();
    if (!tool_name.isEmpty()) {
        object[QStringLiteral("tool_name")] = tool_name;
    }
    object[QStringLiteral("metadata")] = redactedReplayMetadata(metadata);
    return appendJsonLine(
        replayPath(), object, QStringLiteral("turn replay"), m_max_jsonl_bytes, error_message);
}

AiTraceEvent TraceStore::event(const AiTraceEventSeed& seed) {
    AiTraceEvent event;
    event.timestamp_utc = QDateTime::fromString(nowIso(), Qt::ISODateWithMs);
    event.run_id = seed.run_id;
    event.parent_span_id = seed.parent_span_id;
    event.kind = seed.kind;
    event.name = seed.name;
    event.status = seed.status;
    event.metadata = seed.metadata;
    return event;
}

AiActivityEvent TraceStore::activityEvent(const AiActivityEventSeed& seed) {
    AiActivityEvent event;
    event.timestamp_utc = QDateTime::fromString(nowIso(), Qt::ISODateWithMs);
    event.session_id = seed.session_id;
    event.run_id = seed.run_id;
    event.kind = seed.kind;
    event.state = seed.state;
    event.summary = seed.summary;
    event.metadata = seed.metadata;
    event.workflow_id = seed.metadata.value(QStringLiteral("workflow_id")).toString();
    event.phase_id = seed.metadata.value(QStringLiteral("phase_id")).toString();
    event.agent_id = seed.metadata.value(QStringLiteral("agent_id")).toString();
    event.tool_name = seed.metadata.value(QStringLiteral("tool_name")).toString();
    event.parent_id = seed.metadata.value(QStringLiteral("parent_id")).toString();
    event.question_for_human = seed.metadata.value(QStringLiteral("question_for_human")).toString();
    event.recovery_action = seed.metadata.value(QStringLiteral("recovery_action")).toString();
    event.error = seed.metadata.value(QStringLiteral("error_message")).toString();
    if (event.error.isEmpty()) {
        event.error = seed.metadata.value(QStringLiteral("error")).toString();
    }
    event.artifact_refs = stringListFromJson(seed.metadata.value(QStringLiteral("artifact_refs")));
    if (event.artifact_refs.isEmpty()) {
        event.artifact_refs = stringListFromJson(seed.metadata.value(QStringLiteral("artifacts")));
    }
    event.evidence_refs = stringListFromJson(seed.metadata.value(QStringLiteral("evidence_refs")));
    return event;
}

}  // namespace sak::ai
