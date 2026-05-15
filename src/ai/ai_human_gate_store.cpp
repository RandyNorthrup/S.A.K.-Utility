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
    file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    file.write("\n");
    return true;
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
            QUuid::createUuid().toString(QUuid::WithoutBraces).left(12));
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
    QFile file(gateLogPath());
    if (!file.exists()) {
        return gates;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not read human-gate log: %1").arg(file.errorString());
        }
        return gates;
    }
    while (!file.atEnd()) {
        QJsonParseError parse_error{};
        const auto doc = QJsonDocument::fromJson(file.readLine().trimmed(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            continue;
        }
        gates.append(AiHumanGate::fromJson(doc.object()));
    }
    return gates;
}

AiHumanGate AiHumanGateStore::latestPendingGate(QString* error_message) const {
    const QVector<AiHumanGate> gates = loadGates(error_message);
    QHash<QString, AiHumanGate> latest_by_id;
    for (const auto& gate : gates) {
        if (!gate.gate_id.isEmpty()) {
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
