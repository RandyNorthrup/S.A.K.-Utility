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
#include <utility>

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
    bool catastrophic{false};    // irreversible + high-blast-radius (disk wipe,
                                 // partition apply, image flash); ALWAYS forces an
                                 // explicit human confirm, even in Unattended
};

/// Invoke thunk: validated arguments in, structured result out. The thunk runs the
/// headless service. Any GUI-thread marshalling a particular service needs is the
/// thunk's own responsibility -- the registry never touches the GUI and never holds
/// its lock while a thunk runs.
using AppActionInvoke = std::function<AppActionResult(const QJsonObject& arguments)>;

/// The caller's cancel route for one invocation, polled from the thread running the action:
/// true once the caller wants the action stopped (the assistant's Stop). An empty check never
/// cancels.
using AppActionCancelCheck = std::function<bool()>;

/// Invoke thunk for an action that can be stopped part-way. It polls @p should_cancel, asks the
/// work it drives to stop once that reads true, and returns promptly -- the work's own
/// cancelled result when it confirms the stop, appActionCancelledResult() otherwise.
using AppActionCancellableInvoke = std::function<AppActionResult(
    const QJsonObject& arguments, const AppActionCancelCheck& should_cancel)>;

/// The result an action reports when a Stop ended it before it finished.
[[nodiscard]] AppActionResult appActionCancelledResult();

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

    /// Register an action whose thunk takes the caller's cancel route (same rules as
    /// registerAction).
    bool registerCancellableAction(const AppActionDescriptor& descriptor,
                                   AppActionCancellableInvoke invoke,
                                   QString* error = nullptr);

    /// Whether a Stop reaches this action part-way (registered with registerCancellableAction).
    /// Any other action runs to its own end once started.
    [[nodiscard]] bool isCancellable(const QString& id) const;

    [[nodiscard]] bool contains(const QString& id) const;
    [[nodiscard]] std::optional<AppActionDescriptor> descriptor(const QString& id) const;

    /// All descriptors, sorted by id (the QMap keeps them ordered).
    [[nodiscard]] QVector<AppActionDescriptor> list() const;

    /// The catalog as a JSON array for the `sak_app_actions` tool: one object per
    /// action with id/title/description/category/params + risk flags.
    [[nodiscard]] QJsonArray toJsonCatalog() const;

    /// Invoke an action by id. Unknown id sets @p error and returns a failed result.
    /// The thunk is copied out under the lock and called with the lock released, so a
    /// long-running or GUI-marshalling action never blocks other readers. @p should_cancel
    /// reaches a cancellable action's thunk; any other action ignores it.
    [[nodiscard]] AppActionResult invoke(const QString& id,
                                         const QJsonObject& arguments,
                                         QString* error = nullptr,
                                         const AppActionCancelCheck& should_cancel = {}) const;

    [[nodiscard]] int count() const;

private:
    struct Entry {
        AppActionDescriptor descriptor;
        AppActionCancellableInvoke invoke;
        bool cancellable{false};
    };

    bool storeAction(const AppActionDescriptor& descriptor,
                     AppActionCancellableInvoke invoke,
                     bool cancellable,
                     QString* error);

    mutable QReadWriteLock m_lock;
    QMap<QString, Entry> m_actions;
};

/// The registration callback the action catalogs call: one call shape for both thunk kinds. A
/// thunk that takes the caller's cancel route goes to add_cancellable (a Stop reaches it
/// part-way); any other goes to add_plain and runs to its own end once started.
struct AppActionAdder {
    std::function<void(const AppActionDescriptor&, AppActionInvoke)> add_plain;
    std::function<void(const AppActionDescriptor&, AppActionCancellableInvoke)> add_cancellable;

    void operator()(const AppActionDescriptor& descriptor, AppActionInvoke invoke) const {
        add_plain(descriptor, std::move(invoke));
    }
    void operator()(const AppActionDescriptor& descriptor,
                    AppActionCancellableInvoke invoke) const {
        add_cancellable(descriptor, std::move(invoke));
    }
};

/// Serialize one descriptor to the catalog JSON shape (shared by the registry and
/// tests). Kept free so tests can assert the exact model-facing schema.
[[nodiscard]] QJsonObject appActionDescriptorToJson(const AppActionDescriptor& descriptor);

}  // namespace sak
