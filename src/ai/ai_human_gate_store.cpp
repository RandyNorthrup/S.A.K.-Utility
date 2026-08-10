// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_human_gate_store.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>

#include <utility>

namespace sak::ai {
namespace {

constexpr auto kHumanGatesFile = "human_gates.jsonl";
constexpr qsizetype kGeneratedGateIdChars = 12;
// A human-gate log holds a handful of small JSONL records. A file larger than this is
// corruption or a resource-exhaustion attempt against the elevated app; refuse to read it
// wholesale rather than risk an unbounded allocation.
constexpr qint64 kMaxGateLogBytes = 32LL * 1024 * 1024;

bool appendJsonLine(const QString& path, const QJsonObject& object, QString* error_message) {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error_message) {
            *error_message = QStringLiteral("Could not create human-gate directory: %1")
                                 .arg(info.absolutePath());
        }
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not append human-gate log: %1").arg(file.errorString());
        }
        return false;
    }
    const qint64 original_size = file.size();
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    if (file.write(payload) != payload.size() || !file.flush()) {
        // Roll back a partial record: otherwise loadGates skips the malformed line and an
        // already-approved gate resolves back to its older pending record on next start.
        const QString detail = file.errorString();
        file.resize(original_size);
        if (error_message) {
            *error_message = QStringLiteral("Short write to human-gate log: %1").arg(detail);
        }
        return false;
    }
    return true;
}

// Assign the caller's optional error sink in one statement, so each failure path stays a
// single line instead of a repeated guarded branch that inflates the decision count.
void setGateError(QString* error_message, const QString& message) {
    if (error_message) {
        *error_message = message;
    }
}

// Parse every non-blank JSONL record from an already-open, size-checked log. A corrupt record
// is not silently dropped: it is counted so the caller learns the load was incomplete instead
// of trusting a truncated gate history.
QVector<AiHumanGate> parseGateLog(QFile& file, qint64& malformed_lines) {
    QVector<AiHumanGate> gates;
    while (!file.atEnd()) {
        const QByteArray trimmed = file.readLine().trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        QJsonParseError parse_error{};
        const auto doc = QJsonDocument::fromJson(trimmed, &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            ++malformed_lines;
            continue;
        }
        gates.append(AiHumanGate::fromJson(doc.object()));
    }
    return gates;
}

}  // namespace

AiHumanGateStore::AiHumanGateStore(QString session_dir) : m_session_dir(std::move(session_dir)) {}

void AiHumanGateStore::setSessionDirectory(const QString& session_dir) {
    m_session_dir = session_dir;
}

QString AiHumanGateStore::sessionDirectory() const {
    return m_session_dir;
}

QString AiHumanGateStore::gateLogPath() const {
    return m_session_dir.isEmpty()
               ? QString()
               : QDir(m_session_dir).filePath(QString::fromLatin1(kHumanGatesFile));
}

bool AiHumanGateStore::appendGate(AiHumanGate gate, QString* error_message) const {
    if (m_session_dir.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Human-gate store has no session directory");
        }
        return false;
    }
    if (gate.gate_id.isEmpty()) {
        gate.gate_id = QStringLiteral("gate_%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(kGeneratedGateIdChars));
    }
    if (!gate.created_utc.isValid()) {
        gate.created_utc = QDateTime::currentDateTimeUtc();
    }
    if (!gate.isPending() && !gate.resolved_utc.isValid()) {
        gate.resolved_utc = QDateTime::currentDateTimeUtc();
    }
    return appendJsonLine(gateLogPath(), gate.toJson(), error_message);
}

QVector<AiHumanGate> AiHumanGateStore::loadGates(QString* error_message) const {
    QVector<AiHumanGate> gates;
    if (m_session_dir.isEmpty()) {
        // A store with no session directory cannot be read; surface it rather than returning
        // an empty set that is indistinguishable from a legitimately empty log.
        setGateError(error_message, QStringLiteral("Human-gate store has no session directory"));
        return gates;
    }
    QFile file(gateLogPath());
    if (!file.exists()) {
        return gates;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setGateError(error_message,
                     QStringLiteral("Could not read human-gate log: %1").arg(file.errorString()));
        return gates;
    }
    if (file.size() > kMaxGateLogBytes) {
        setGateError(
            error_message,
            QStringLiteral("Human-gate log is too large to read: %1 bytes").arg(file.size()));
        return gates;
    }
    qint64 malformed_lines = 0;
    gates = parseGateLog(file, malformed_lines);
    if (file.error() != QFileDevice::NoError) {
        // A mid-stream I/O failure means the returned set is incomplete; do not present a
        // truncated read as a complete, authoritative gate history.
        setGateError(error_message,
                     QStringLiteral("Human-gate log read failed: %1").arg(file.errorString()));
        return gates;
    }
    if (malformed_lines > 0) {
        setGateError(
            error_message,
            QStringLiteral("Human-gate log has %1 unreadable record(s)").arg(malformed_lines));
    }
    return gates;
}

AiHumanGate AiHumanGateStore::latestPendingGate(QString* error_message) const {
    const QVector<AiHumanGate> gates = loadGates(error_message);
    QHash<QString, AiHumanGate> latest_by_id;
    for (const auto& gate : gates) {
        // Only a record with a recognized lifecycle status may become the latest state for
        // a gate_id. A record whose status is missing/empty/unknown is malformed and is
        // ignored, so it cannot silently overwrite (resolve) a valid pending gate.
        if (!gate.gate_id.isEmpty() && isKnownHumanGateStatus(gate.status)) {
            latest_by_id.insert(gate.gate_id, gate);
        }
    }
    for (auto it = gates.crbegin(); it != gates.crend(); ++it) {
        if (it->gate_id.isEmpty()) {
            continue;
        }
        const AiHumanGate latest = latest_by_id.value(it->gate_id);
        if (latest.isPending()) {
            return latest;
        }
    }
    return {};
}

}  // namespace sak::ai
