#pragma once

#include <QQueue>
#include <QString>

#include "sak/orchestration_types.h"

namespace sak {

class AssignmentQueueStore {
public:
    explicit AssignmentQueueStore(const QString& filePath);

    bool save(const DeploymentAssignment& active,
              const QQueue<DeploymentAssignment>& queue,
              const QMap<QString, QString>& statusByJob,
              const QMap<QString, QString>& eventByJob) const;

    bool load(DeploymentAssignment& active,
              QQueue<DeploymentAssignment>& queue,
              QMap<QString, QString>& statusByJob,
              QMap<QString, QString>& eventByJob) const;

    QString filePath() const;

private:
    QString m_filePath;
};

} // namespace sak
