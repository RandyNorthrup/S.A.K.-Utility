#include "sak/assignment_queue_store.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace sak {

AssignmentQueueStore::AssignmentQueueStore(const QString& filePath)
    : m_filePath(filePath) {
}

QString AssignmentQueueStore::filePath() const {
    return m_filePath;
}

bool AssignmentQueueStore::save(const DeploymentAssignment& active,
                                const QQueue<DeploymentAssignment>& queue,
                                const QMap<QString, QString>& statusByJob,
                                const QMap<QString, QString>& eventByJob) const {
    QJsonObject root;
    root["active"] = active.toJson();

    QJsonArray queued;
    for (const auto& item : queue) {
        queued.append(item.toJson());
    }
    root["queue"] = queued;

    QJsonObject statusObj;
    for (auto it = statusByJob.constBegin(); it != statusByJob.constEnd(); ++it) {
        statusObj[it.key()] = it.value();
    }
    root["status_by_job"] = statusObj;

    QJsonObject eventObj;
    for (auto it = eventByJob.constBegin(); it != eventByJob.constEnd(); ++it) {
        eventObj[it.key()] = it.value();
    }
    root["event_by_job"] = eventObj;

    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    return file.commit();
}

bool AssignmentQueueStore::load(DeploymentAssignment& active,
                                QQueue<DeploymentAssignment>& queue,
                                QMap<QString, QString>& statusByJob,
                                QMap<QString, QString>& eventByJob) const {
    QFile file(m_filePath);
    if (!file.exists()) {
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const auto data = file.readAll();
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    const auto root = doc.object();
    if (root.contains("active") && root.value("active").isObject()) {
        active = DeploymentAssignment::fromJson(root.value("active").toObject());
    }

    queue.clear();
    const auto queued = root.value("queue").toArray();
    for (const auto& value : queued) {
        if (value.isObject()) {
            queue.enqueue(DeploymentAssignment::fromJson(value.toObject()));
        }
    }

    statusByJob.clear();
    const auto statusObj = root.value("status_by_job").toObject();
    for (auto it = statusObj.begin(); it != statusObj.end(); ++it) {
        statusByJob.insert(it.key(), it.value().toString());
    }

    eventByJob.clear();
    const auto eventObj = root.value("event_by_job").toObject();
    for (auto it = eventObj.begin(); it != eventObj.end(); ++it) {
        eventByJob.insert(it.key(), it.value().toString());
    }

    return true;
}

} // namespace sak
