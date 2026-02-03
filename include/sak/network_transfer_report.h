#pragma once

#include <QString>
#include <QDateTime>
#include <QStringList>
#include <QJsonObject>

#include "sak/network_transfer_types.h"

namespace sak {

struct TransferReport {
    QString transfer_id;
    QString source_host;
    QString destination_host;
    QString status; // success/failed/cancelled
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

} // namespace sak
