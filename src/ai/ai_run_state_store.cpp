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

namespace sak::ai {

namespace {

constexpr auto kRunStateFile = "run_state.json";

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
        if (error_message) {
            *error_message = QStringLiteral("Run-state store has no session directory");
        }
        return false;
    }

    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error_message) {
            *error_message = file.errorString();
        }
        return false;
    }

    const QByteArray payload = QJsonDocument(state.toJson()).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        if (error_message) {
            *error_message = file.errorString();
        }
        return false;
    }
    if (!file.commit()) {
        if (error_message) {
            *error_message = file.errorString();
        }
        return false;
    }
    return true;
}

AiRunState AiRunStateStore::loadSnapshot(QString* error_message) const {
    const QString path = runStatePath();
    if (path.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Run-state store has no session directory");
        }
        return {};
    }

    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error_message) {
            *error_message = file.errorString();
        }
        return {};
    }

    const QByteArray bytes = file.readAll();
    QJsonParseError parse_error{};
    const auto doc = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = parse_error.errorString();
        }
        return {};
    }
    return AiRunState::fromJson(doc.object());
}

bool AiRunStateStore::clearSnapshot(QString* error_message) const {
    const QString path = runStatePath();
    if (path.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("Run-state store has no session directory");
        }
        return false;
    }
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.remove()) {
        if (error_message) {
            *error_message = file.errorString();
        }
        return false;
    }
    return true;
}

}  // namespace sak::ai
