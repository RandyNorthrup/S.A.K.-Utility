// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_run_state_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sak::ai {

namespace {

constexpr auto kRunStateFile = "run_state.json";
constexpr int kRunStateSchemaVersion = 1;

// A run-state snapshot is a small JSON object (phase/agent/tool counts plus a pending
// gate). Cap the on-disk size before readAll so a corrupt or hostile oversized
// run_state.json cannot drive an unbounded allocation; fail closed above the cap.
constexpr qint64 kMaxRunStateBytes = 16 * 1024 * 1024;  // 16 MiB
constexpr qint64 kRunStateReadChunkBytes = 64 * 1024;

// Record a failure reason only when the caller asked for one. Folding the pending-pointer
// guard in here keeps each failure path a single statement instead of its own branch, so a
// function's control flow reflects the checks it makes and not the optional reporting.
void setError(QString* error_message, const QString& text) {
    if (error_message != nullptr) {
        *error_message = text;
    }
}

QString validateStringFields(const QJsonObject& object) {
    static const QStringList kFields = {
        QStringLiteral("run_id"),
        QStringLiteral("workflow_id"),
        QStringLiteral("status"),
        QStringLiteral("phase_id"),
        QStringLiteral("message"),
    };
    for (const QString& field : kFields) {
        if (!object.contains(field) || !object.value(field).isString()) {
            return QStringLiteral("Run-state snapshot field '%1' must be a string").arg(field);
        }
    }
    return {};
}

QString validateCounterFields(const QJsonObject& object) {
    static const QStringList kFields = {
        QStringLiteral("active_subagents"),
        QStringLiteral("completed_subagents"),
        QStringLiteral("active_tools"),
        QStringLiteral("completed_tools"),
    };
    for (const QString& field : kFields) {
        const QJsonValue value = object.value(field);
        const double number = value.toDouble(-1.0);
        if (!object.contains(field) || !value.isDouble() || !std::isfinite(number) ||
            number < 0.0 || number > static_cast<double>(std::numeric_limits<int>::max()) ||
            std::floor(number) != number) {
            return QStringLiteral("Run-state snapshot field '%1' must be a non-negative integer")
                .arg(field);
        }
    }
    return {};
}

bool knownRunStatus(const QString& status) {
    return status == QLatin1String("idle") || status == QLatin1String("planning") ||
           status == QLatin1String("running") || status == QLatin1String("waiting_for_human") ||
           status == QLatin1String("cancelling") || status == QLatin1String("cancelled") ||
           status == QLatin1String("completed") || status == QLatin1String("failed");
}

QString validatePendingGate(const QJsonObject& object) {
    const QJsonValue pending_value = object.value(QStringLiteral("has_pending_human_gate"));
    if (!object.contains(QStringLiteral("has_pending_human_gate")) || !pending_value.isBool()) {
        return QStringLiteral(
            "Run-state snapshot field 'has_pending_human_gate' must be a boolean");
    }
    if (!pending_value.toBool()) {
        return {};
    }
    const QJsonValue gate_value = object.value(QStringLiteral("pending_human_gate"));
    if (!gate_value.isObject()) {
        return QStringLiteral(
            "Run-state snapshot field 'pending_human_gate' must be an object when pending");
    }
    const QJsonObject gate = gate_value.toObject();
    if (!gate.value(QStringLiteral("gate_id")).isString() ||
        gate.value(QStringLiteral("gate_id")).toString().isEmpty()) {
        return QStringLiteral("Run-state snapshot pending gate must have a non-empty gate_id");
    }
    if (!gate.value(QStringLiteral("status")).isString() ||
        gate.value(QStringLiteral("status")).toString() != humanGateWaitingStatus()) {
        return QStringLiteral("Run-state snapshot pending gate must have waiting status");
    }
    return {};
}

QString validateSnapshotSchema(const QJsonObject& object) {
    const QJsonValue version = object.value(QStringLiteral("schema_version"));
    if (!version.isDouble() || version.toDouble() != kRunStateSchemaVersion) {
        return QStringLiteral("Run-state snapshot has unsupported or missing schema_version");
    }
    QString error = validateStringFields(object);
    if (!error.isEmpty()) {
        return error;
    }
    const QString status = object.value(QStringLiteral("status")).toString();
    if (!knownRunStatus(status)) {
        return QStringLiteral("Run-state snapshot has unknown status: %1").arg(status);
    }
    error = validateCounterFields(object);
    return error.isEmpty() ? validatePendingGate(object) : error;
}

}  // namespace

AiRunStateStore::AiRunStateStore(QString session_dir) : m_session_dir(std::move(session_dir)) {}

void AiRunStateStore::setSessionDirectory(const QString& session_dir) {
    m_session_dir = session_dir;
}

QString AiRunStateStore::sessionDirectory() const {
    return m_session_dir;
}

QString AiRunStateStore::runStatePath() const {
    if (m_session_dir.isEmpty()) {
        return {};
    }
    return QDir(m_session_dir).filePath(QString::fromLatin1(kRunStateFile));
}

bool AiRunStateStore::saveSnapshot(const AiRunState& state, QString* error_message) const {
    const QString path = runStatePath();
    if (path.isEmpty()) {
        setError(error_message, QStringLiteral("Run-state store has no session directory"));
        return false;
    }

    QJsonObject object = state.toJson();
    object[QStringLiteral("schema_version")] = kRunStateSchemaVersion;
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (payload.size() > kMaxRunStateBytes) {
        setError(error_message,
                 QStringLiteral("Cannot save run-state snapshot: encoded payload is too large"));
        return false;
    }

    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        setError(error_message,
                 QStringLiteral("Could not create run-state directory: %1").arg(directory));
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error_message, file.errorString());
        return false;
    }

    if (file.write(payload) != payload.size()) {
        setError(error_message, file.errorString());
        return false;
    }
    if (!file.commit()) {
        setError(error_message, file.errorString());
        return false;
    }
    return true;
}

bool AiRunStateStore::readBoundedSnapshot(QIODevice* device,
                                          QByteArray* bytes,
                                          QString* error_message) {
    bytes->clear();
    while (true) {
        const qint64 remaining = (kMaxRunStateBytes + 1) - bytes->size();
        const qint64 request_size = std::min(kRunStateReadChunkBytes, remaining);
        QByteArray chunk(static_cast<qsizetype>(request_size), '\0');
        const qint64 count = device->read(chunk.data(), request_size);
        if (count < 0) {
            setError(
                error_message,
                QStringLiteral("Run-state snapshot read failed: %1").arg(device->errorString()));
            return false;
        }
        if (count == 0) {
            if (device->atEnd()) {
                return true;
            }
            setError(error_message, QStringLiteral("Run-state snapshot read made no progress"));
            return false;
        }
        chunk.truncate(static_cast<qsizetype>(count));
        bytes->append(chunk);
        if (bytes->size() > kMaxRunStateBytes) {
            setError(error_message,
                     QStringLiteral("Run-state snapshot grew beyond the read size limit"));
            return false;
        }
    }
}

AiRunState AiRunStateStore::loadSnapshot(QString* error_message) const {
    const QString path = runStatePath();
    if (path.isEmpty()) {
        setError(error_message, QStringLiteral("Run-state store has no session directory"));
        return {};
    }

    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error_message, file.errorString());
        return {};
    }

    const qint64 declared_size = file.size();
    if (declared_size < 0) {
        setError(error_message, QStringLiteral("Could not determine run-state snapshot size"));
        return {};
    }
    if (declared_size > kMaxRunStateBytes) {
        setError(error_message,
                 QStringLiteral("Run-state snapshot declared size exceeds the read limit"));
        return {};
    }

    QByteArray bytes;
    if (!readBoundedSnapshot(&file, &bytes, error_message)) {
        return {};
    }
    QJsonParseError parse_error{};
    const auto doc = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        setError(error_message,
                 QStringLiteral("Run-state snapshot JSON parse failed: %1")
                     .arg(parse_error.errorString()));
        return {};
    }
    if (!doc.isObject()) {
        setError(error_message, QStringLiteral("Run-state snapshot root must be a JSON object"));
        return {};
    }
    const QString schema_error = validateSnapshotSchema(doc.object());
    if (!schema_error.isEmpty()) {
        setError(error_message, schema_error);
        return {};
    }
    return AiRunState::fromJson(doc.object());
}

bool AiRunStateStore::clearSnapshot(QString* error_message) const {
    const QString path = runStatePath();
    if (path.isEmpty()) {
        setError(error_message, QStringLiteral("Run-state store has no session directory"));
        return false;
    }
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.remove()) {
        setError(error_message, file.errorString());
        return false;
    }
    return true;
}

}  // namespace sak::ai
