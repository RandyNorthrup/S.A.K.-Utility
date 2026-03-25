// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/network_transfer_types.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace sak {

/// @brief Summary report of a completed network transfer
struct TransferReport {
    QString transfer_id;
    QString source_host;
    QString destination_host;
    QString status;  // success/failed/cancelled
    QDateTime started_at;
    QDateTime completed_at;
    qint64 total_bytes{0};
    int total_files{0};
    QStringList errors;
    QStringList warnings;
    TransferManifest manifest;

    QJsonObject toJson() const;
    bool saveToFile(const QString& path) const;
};

}  // namespace sak
