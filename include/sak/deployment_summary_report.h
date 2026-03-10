// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace sak {

/// @brief Summary of a single deployment job's outcome
struct DeploymentJobSummary {
    QString job_id;
    QString source_user;
    QString destination_id;
    QString status;
    qint64 bytes_transferred{0};
    qint64 total_bytes{0};
    QString error_message;
};

/// @brief Summary of a deployment destination's status and events
struct DeploymentDestinationSummary {
    QString destination_id;
    QString hostname;
    QString ip_address;
    QString status;
    int progress_percent{0};
    QDateTime last_seen;
    QStringList status_events;
};

/// @brief Exports deployment summary reports in CSV and PDF formats
class DeploymentSummaryReport {
public:
    static bool exportCsv(const QString& filePath,
                          const QString& deploymentId,
                          const QDateTime& startedAt,
                          const QDateTime& completedAt,
                          const QVector<DeploymentJobSummary>& jobs,
                          const QVector<DeploymentDestinationSummary>& destinations);

    static bool exportPdf(const QString& filePath,
                          const QString& deploymentId,
                          const QDateTime& startedAt,
                          const QDateTime& completedAt,
                          const QVector<DeploymentJobSummary>& jobs,
                          const QVector<DeploymentDestinationSummary>& destinations);
};

}  // namespace sak
