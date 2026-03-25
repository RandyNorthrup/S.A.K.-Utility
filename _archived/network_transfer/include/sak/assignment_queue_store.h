// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/orchestration_types.h"

#include <QQueue>
#include <QString>

namespace sak {

/// @brief Persistent storage for deployment assignment queues
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

}  // namespace sak
