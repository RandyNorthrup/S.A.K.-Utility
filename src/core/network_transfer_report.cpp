#include "sak/network_transfer_report.h"

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
        return false;
    }
    QJsonDocument doc(toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

} // namespace sak
