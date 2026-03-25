// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sak {

/// @brief Record of a completed deployment operation
struct DeploymentHistoryEntry {
    QString deployment_id;
    QDateTime started_at;
    QDateTime completed_at;
    int total_jobs{0};
    int completed_jobs{0};
    int failed_jobs{0};
    QString status;
    QString template_path;

    QJsonObject toJson() const;
    static DeploymentHistoryEntry fromJson(const QJsonObject& json);
};

/// @brief Manages persistent deployment history log
class DeploymentHistoryManager {
public:
    explicit DeploymentHistoryManager(const QString& historyPath);

    bool appendEntry(const DeploymentHistoryEntry& entry);
    QVector<DeploymentHistoryEntry> loadEntries() const;
    bool exportCsv(const QString& filePath) const;

    QString historyPath() const;

private:
    QString m_historyPath;
};

}  // namespace sak
