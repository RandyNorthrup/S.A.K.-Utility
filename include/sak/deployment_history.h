#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QVector>

namespace sak {

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

} // namespace sak
