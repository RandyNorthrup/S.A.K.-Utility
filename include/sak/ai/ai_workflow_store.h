// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_workflow_template.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace sak::ai {

class WorkflowStore {
public:
    WorkflowStore() = default;

    [[nodiscard]] bool loadDefaults(QStringList* errors = nullptr);
    [[nodiscard]] bool loadBuiltIn(QStringList* errors = nullptr);
    [[nodiscard]] bool loadDirectory(const QString& directory, QStringList* errors = nullptr);
    [[nodiscard]] bool addWorkflow(const WorkflowTemplate& workflow, QStringList* errors = nullptr);

    [[nodiscard]] QVector<WorkflowTemplate> workflows() const { return m_workflows; }
    [[nodiscard]] QVector<WorkflowTemplate> workflowsForRole(const QString& role) const;
    [[nodiscard]] QStringList roles() const;
    [[nodiscard]] const WorkflowTemplate* workflowById(const QString& id) const;

    [[nodiscard]] static QString builtInResourceRoot();
    [[nodiscard]] static QString defaultUserWorkflowDirectory();

private:
    void rebuildIndex();

    QVector<WorkflowTemplate> m_workflows;
    QHash<QString, int> m_index_by_id;
};

}  // namespace sak::ai
