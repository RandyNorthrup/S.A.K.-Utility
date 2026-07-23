// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QReadWriteLock>
#include <QString>
#include <QVector>

#include <functional>
#include <optional>

/// @file app_action_registry.h
/// @brief App-scoped registry of headless technician actions.
///
/// This is the single enumerable surface that lets the AI assistant command the
/// app's OWN functionality without any GUI in the loop: each registered action is
/// backed by a headless src/core service (not a QWidget), described by metadata the
/// model can read, and invoked by id with validated arguments. The GUI panels and
/// the assistant call the same underlying services; this registry is only the
/// naming/description/dispatch layer over them.
namespace sak {

/// Result of invoking an app action. `data` carries the structured payload the
/// backing service produced; `message` is a short human/model-facing summary.
struct AppActionResult {
    bool success{false};
    QString message;
    QJsonObject data;
};

/// Static metadata describing one invocable app action. The risk flags drive the
/// assistant's guardrails: read_only actions never gate; mutating/destructive/
/// requires_admin actions must pass the human-gate + restore-point path.
struct AppActionDescriptor {
    QString id;                  // stable dotted id, e.g. "system.verify_files"
    QString title;               // short human label
    QString description;         // one line, model-facing
    QString category;            // diagnostics | maintenance | disk | network | ...
    QJsonObject params_schema;   // JSON-Schema for arguments; empty = takes none
    bool read_only{false};       // performs no system/file change
    bool mutating{false};        // changes system state or files
    bool destructive{false};     // potential data loss / irreversible
    bool requires_admin{false};  // needs elevation
};

/// Invoke thunk: validated arguments in, structured result out. The thunk runs the
/// headless service. Any GUI-thread marshalling a particular service needs is the
/// thunk's own responsibility -- the registry never touches the GUI and never holds
/// its lock while a thunk runs.
using AppActionInvoke = std::function<AppActionResult(const QJsonObject& arguments)>;

/// Thread-safe registry. Registration happens once at app startup (GUI thread);
/// listing and invocation happen from the AI worker thread. A read/write lock lets
/// many readers enumerate concurrently while registration is exclusive.
class AppActionRegistry {
public:
    AppActionRegistry() = default;

    AppActionRegistry(const AppActionRegistry&) = delete;
    AppActionRegistry& operator=(const AppActionRegistry&) = delete;
    AppActionRegistry(AppActionRegistry&&) = delete;
    AppActionRegistry& operator=(AppActionRegistry&&) = delete;

    /// Register an action. Rejects an empty id or a duplicate id (sets @p error).
    /// A null invoke is rejected too -- a listed action must be runnable.
    bool registerAction(const AppActionDescriptor& descriptor,
                        AppActionInvoke invoke,
                        QString* error = nullptr);

    [[nodiscard]] bool contains(const QString& id) const;
    [[nodiscard]] std::optional<AppActionDescriptor> descriptor(const QString& id) const;

    /// All descriptors, sorted by id (the QMap keeps them ordered).
    [[nodiscard]] QVector<AppActionDescriptor> list() const;

    /// The catalog as a JSON array for the `sak_app_actions` tool: one object per
    /// action with id/title/description/category/params + risk flags.
    [[nodiscard]] QJsonArray toJsonCatalog() const;

    /// Invoke an action by id. Unknown id sets @p error and returns a failed result.
    /// The thunk is copied out under the lock and called with the lock released, so a
    /// long-running or GUI-marshalling action never blocks other readers.
    [[nodiscard]] AppActionResult invoke(const QString& id,
                                         const QJsonObject& arguments,
                                         QString* error = nullptr) const;

    [[nodiscard]] int count() const;

private:
    struct Entry {
        AppActionDescriptor descriptor;
        AppActionInvoke invoke;
    };

    mutable QReadWriteLock m_lock;
    QMap<QString, Entry> m_actions;
};

/// Serialize one descriptor to the catalog JSON shape (shared by the registry and
/// tests). Kept free so tests can assert the exact model-facing schema.
[[nodiscard]] QJsonObject appActionDescriptorToJson(const AppActionDescriptor& descriptor);

}  // namespace sak
