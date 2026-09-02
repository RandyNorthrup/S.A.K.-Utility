// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_store.h"

#include "sak/ai/ai_paths.h"
#include "sak/ai/ai_store_file_loader.h"

#include <QDir>
#include <QSet>

#include <functional>
#include <utility>

namespace sak::ai {

namespace {

constexpr qint64 kMaxWorkflowFileBytes = 1LL * 1024 * 1024;
constexpr int kMaxWorkflowFiles = 512;
std::function<void(const QString&)> g_before_file_open_hook;
std::function<void(const QString&)> g_before_file_read_hook;

const detail::StoreFileLoadPolicy& workflowFileLoadPolicy() {
    static const detail::StoreFileLoadPolicy policy{QStringLiteral("Workflow"),
                                                    QStringLiteral(".json"),
                                                    QStringLiteral("JSON"),
                                                    kMaxWorkflowFileBytes,
                                                    kMaxWorkflowFiles};
    return policy;
}

}  // namespace

void WorkflowStore::setBeforeFileOpenHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_open_hook = std::move(hook);
}

void WorkflowStore::setBeforeFileReadHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_read_hook = std::move(hook);
}

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
    const detail::StoreFileLoadHooks hooks{.before_file_open = g_before_file_open_hook,
                                           .before_file_read = g_before_file_read_hook,
                                           .before_child_open = {}};
    const detail::StoreFileConsumer consume = [this, errors](const QString& path,
                                                             const QByteArray& bytes) {
        QStringList parse_errors;
        const WorkflowTemplate workflow =
            WorkflowTemplate::fromJsonBytes(bytes, path, &parse_errors);
        if (!parse_errors.isEmpty()) {
            if (errors != nullptr) {
                errors->append(parse_errors);
            }
            return false;
        }
        return addWorkflow(workflow, errors);
    };
    return detail::loadStoreDirectoryFiles(
        directory, workflowFileLoadPolicy(), hooks, consume, errors);
}

bool WorkflowStore::addWorkflow(const WorkflowTemplate& workflow, QStringList* errors) {
    if (!workflow.isValid(errors)) {
        return false;
    }

    const auto existing = m_index_by_id.constFind(workflow.id);
    if (existing != m_index_by_id.constEnd()) {
        // A user-directory workflow intentionally OVERRIDES a built-in of the same id (a supported
        // customization; see userDirectoryOverridesBuiltInWorkflow). Not a shadowing vulnerability:
        // writing to the user workflow directory already requires local access, and every template
        // -- built-in or user -- is validated at load (including the run_powershell single-quote
        // invariant), so an override cannot smuggle an unescaped placeholder past validation.
        m_workflows[*existing] = workflow;
    } else {
        m_index_by_id.insert(workflow.id, static_cast<int>(m_workflows.size()));
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
    return workflowLibraryDirectory();
}

}  // namespace sak::ai
