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

// A run-state snapshot is a small JSON object (phase/agent/tool counts plus a pending
// gate). Cap the on-disk size before readAll so a corrupt or hostile oversized
// run_state.json cannot drive an unbounded allocation; fail closed above the cap.
constexpr qint64 kMaxRunStateBytes = 16 * 1024 * 1024;  // 16 MiB

// Record a failure reason only when the caller asked for one. Folding the pending-pointer
// guard in here keeps each failure path a single statement instead of its own branch, so a
// function's control flow reflects the checks it makes and not the optional reporting.
void setError(QString* error_message, const QString& text) {
    if (error_message != nullptr) {
        *error_message = text;
    }
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

    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error_message, file.errorString());
        return false;
    }

    const QByteArray payload = QJsonDocument(state.toJson()).toJson(QJsonDocument::Indented);
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

    if (file.size() > kMaxRunStateBytes) {
        setError(error_message, QStringLiteral("Run-state snapshot exceeds the maximum size"));
        return {};
    }

    const QByteArray bytes = file.readAll();
    QJsonParseError parse_error{};
    const auto doc = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        setError(error_message, parse_error.errorString());
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
