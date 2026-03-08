// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_report.h"

#include "sak/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>

namespace sak {

QJsonObject TransferReport::toJson() const {
    QJsonObject json;
    json["transfer_id"] = transfer_id;
    json["source_host"] = source_host;
    json["destination_host"] = destination_host;
    json["status"] = status;
    json["started_at"] = started_at.toSecsSinceEpoch();
    json["completed_at"] = completed_at.toSecsSinceEpoch();
    json["total_bytes"] = static_cast<qint64>(total_bytes);
    json["total_files"] = total_files;

    QJsonArray errorsArray;
    for (const auto& error : errors) {
        errorsArray.append(error);
    }
    json["errors"] = errorsArray;

    QJsonArray warningsArray;
    for (const auto& warning : warnings) {
        warningsArray.append(warning);
    }
    json["warnings"] = warningsArray;

    json["manifest"] = manifest.toJson();
    return json;
}

bool TransferReport::saveToFile(const QString& path) const {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logError("Failed to open transfer report for writing: {}",
                 path.toStdString());
        return false;
    }
    QJsonDocument doc(toJson());
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    qint64 written = file.write(data);
    if (written != data.size()) {
        logError("Failed to write transfer report: {}",
                 path.toStdString());
        return false;
    }
    return true;
}

} // namespace sak
