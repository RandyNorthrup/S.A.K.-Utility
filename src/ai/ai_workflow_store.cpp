// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSet>

namespace sak::ai {

namespace {

bool loadWorkflowFile(const QString& path, WorkflowTemplate* workflow, QStringList* errors) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("%1: %2").arg(path, file.errorString()));
        }
        return false;
    }

    QStringList parse_errors;
    *workflow = WorkflowTemplate::fromJsonBytes(file.readAll(), path, &parse_errors);
    if (!parse_errors.isEmpty()) {
        if (errors != nullptr) {
            errors->append(parse_errors);
        }
        return false;
    }
    return workflow->isValid(errors);
}

QString portableDataRoot() {
    QString app_dir = QCoreApplication::applicationDirPath();
    if (app_dir.trimmed().isEmpty()) {
        app_dir = QDir::currentPath();
    }
    return QDir(app_dir).filePath(QStringLiteral("data"));
}

}  // namespace

bool WorkflowStore::loadDefaults(QStringList* errors) {
    bool ok = loadBuiltIn(errors);
    const QString user_dir = defaultUserWorkflowDirectory();
    if (QDir(user_dir).exists()) {
        ok = loadDirectory(user_dir, errors) && ok;
    }
    return ok;
}

bool WorkflowStore::loadBuiltIn(QStringList* errors) {
    return loadDirectory(builtInResourceRoot(), errors);
}

bool WorkflowStore::loadDirectory(const QString& directory, QStringList* errors) {
    QDir dir(directory);
    if (!dir.exists()) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("Workflow directory not found: %1").arg(directory));
        }
        return false;
    }

    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    bool ok = true;
    for (const auto& file_name : files) {
        WorkflowTemplate workflow;
        const QString path = dir.filePath(file_name);
        if (!loadWorkflowFile(path, &workflow, errors)) {
            ok = false;
            continue;
        }
        ok = addWorkflow(workflow, errors) && ok;
    }
    rebuildIndex();
    return ok;
}

bool WorkflowStore::addWorkflow(const WorkflowTemplate& workflow, QStringList* errors) {
    if (!workflow.isValid(errors)) {
        return false;
    }

    const auto existing = m_index_by_id.constFind(workflow.id);
    if (existing != m_index_by_id.constEnd()) {
        m_workflows[*existing] = workflow;
    } else {
        m_index_by_id.insert(workflow.id, m_workflows.size());
        m_workflows.append(workflow);
    }
    return true;
}

QVector<WorkflowTemplate> WorkflowStore::workflowsForRole(const QString& role) const {
    QVector<WorkflowTemplate> result;
    const QString normalized = role.trimmed();
    for (const auto& workflow : m_workflows) {
        if (normalized.isEmpty() || workflow.role.compare(normalized, Qt::CaseInsensitive) == 0) {
            result.append(workflow);
        }
    }
    return result;
}

QStringList WorkflowStore::roles() const {
    QSet<QString> seen;
    QStringList result;
    for (const auto& workflow : m_workflows) {
        if (workflow.role.isEmpty() || seen.contains(workflow.role)) {
            continue;
        }
        seen.insert(workflow.role);
        result.append(workflow.role);
    }
    result.sort(Qt::CaseInsensitive);
    return result;
}

const WorkflowTemplate* WorkflowStore::workflowById(const QString& id) const {
    const auto existing = m_index_by_id.constFind(id);
    if (existing == m_index_by_id.constEnd()) {
        return nullptr;
    }
    const int index = *existing;
    if (index < 0 || index >= m_workflows.size()) {
        return nullptr;
    }
    return &m_workflows[index];
}

QString WorkflowStore::builtInResourceRoot() {
    return QStringLiteral(":/ai/workflows");
}

QString WorkflowStore::defaultUserWorkflowDirectory() {
    return QDir(portableDataRoot()).filePath(QStringLiteral("ai/workflows"));
}

void WorkflowStore::rebuildIndex() {
    m_index_by_id.clear();
    for (int i = 0; i < m_workflows.size(); ++i) {
        m_index_by_id.insert(m_workflows.at(i).id, i);
    }
}

}  // namespace sak::ai
