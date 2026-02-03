#include "sak/deployment_history.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>

namespace sak {

QJsonObject DeploymentHistoryEntry::toJson() const {
    QJsonObject json;
    json["deployment_id"] = deployment_id;
    json["started_at"] = started_at.toString(Qt::ISODate);
    json["completed_at"] = completed_at.toString(Qt::ISODate);
    json["total_jobs"] = total_jobs;
    json["completed_jobs"] = completed_jobs;
    json["failed_jobs"] = failed_jobs;
    json["status"] = status;
    json["template_path"] = template_path;
    return json;
}

DeploymentHistoryEntry DeploymentHistoryEntry::fromJson(const QJsonObject& json) {
    DeploymentHistoryEntry entry;
    entry.deployment_id = json.value("deployment_id").toString();
    entry.started_at = QDateTime::fromString(json.value("started_at").toString(), Qt::ISODate);
    entry.completed_at = QDateTime::fromString(json.value("completed_at").toString(), Qt::ISODate);
    entry.total_jobs = json.value("total_jobs").toInt();
    entry.completed_jobs = json.value("completed_jobs").toInt();
    entry.failed_jobs = json.value("failed_jobs").toInt();
    entry.status = json.value("status").toString();
    entry.template_path = json.value("template_path").toString();
    return entry;
}

DeploymentHistoryManager::DeploymentHistoryManager(const QString& historyPath)
    : m_historyPath(historyPath) {
}

QString DeploymentHistoryManager::historyPath() const {
    return m_historyPath;
}

QVector<DeploymentHistoryEntry> DeploymentHistoryManager::loadEntries() const {
    QFile file(m_historyPath);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    const auto data = file.readAll();
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        return {};
    }

    QVector<DeploymentHistoryEntry> entries;
    const auto array = doc.array();
    entries.reserve(array.size());
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        entries.push_back(DeploymentHistoryEntry::fromJson(value.toObject()));
    }

    return entries;
}

bool DeploymentHistoryManager::appendEntry(const DeploymentHistoryEntry& entry) {
    auto entries = loadEntries();
    entries.push_back(entry);

    QSaveFile file(m_historyPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QJsonArray array;
    for (const auto& item : entries) {
        array.append(item.toJson());
    }

    QJsonDocument doc(array);
    file.write(doc.toJson(QJsonDocument::Indented));
    return file.commit();
}

bool DeploymentHistoryManager::exportCsv(const QString& filePath) const {
    const auto entries = loadEntries();
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QTextStream stream(&file);
    stream << "deployment_id,started_at,completed_at,total_jobs,completed_jobs,failed_jobs,status,template_path\n";
    for (const auto& entry : entries) {
        stream << '"' << entry.deployment_id << "\",";
        stream << '"' << entry.started_at.toString(Qt::ISODate) << "\",";
        stream << '"' << entry.completed_at.toString(Qt::ISODate) << "\",";
        stream << entry.total_jobs << ',' << entry.completed_jobs << ',' << entry.failed_jobs << ',';
        stream << '"' << entry.status << "\",";
        stream << '"' << entry.template_path << "\"\n";
    }

    stream.flush();
    return file.commit();
}

} // namespace sak
