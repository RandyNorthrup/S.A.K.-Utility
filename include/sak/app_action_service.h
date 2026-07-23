// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/app_action_registry.h"

#include <QObject>
#include <QString>

#include <memory>

/// @file app_action_service.h
/// @brief Bridges the app's headless QuickAction subsystem into the
/// AppActionRegistry so the AI assistant can run those actions headless.
///
/// Two pieces:
///  - runQuickActionSync(): a generic, GUI-free bridge that runs one
///    QuickActionController action to completion SYNCHRONOUSLY (the assistant
///    tool handler already runs on a worker thread and expects a value back),
///    without blocking the controller's own (GUI) thread. It marshals the
///    async executeAction() onto the controller's thread and pumps a local
///    event loop on the caller's thread until the completion/error signal.
///  - AppActionService: constructs an app-scoped QuickActionController with the
///    built-in technician QuickActions and registers each into an
///    AppActionRegistry with a thunk that calls runQuickActionSync().
namespace sak {

class QuickActionController;

/// Generous default: some actions (SFC/DISM, CHKDSK) legitimately run for many
/// minutes. The assistant tool layer applies its own outer timeout too.
inline constexpr int kDefaultQuickActionTimeoutMs = 30 * 60 * 1000;

/// Run @p action_name on @p controller to completion and return its result.
/// Confirmation is suppressed (require_confirmation=false): the assistant applies
/// its own human-gate/restore-point BEFORE invoking, so the controller must not
/// pop a GUI dialog. Returns a failed result on unknown action or timeout.
/// Safe to call from a non-GUI worker thread; does not block the controller's thread.
[[nodiscard]] AppActionResult runQuickActionSync(QuickActionController* controller,
                                                 const QString& action_name,
                                                 int timeout_ms = kDefaultQuickActionTimeoutMs);

/// Register every action currently in @p controller into @p registry as an
/// AppAction. Each descriptor's id is "action.<slug-of-name>", category is mapped
/// from the QuickAction category, mutating is set (these run system changes), and
/// requires_admin is taken from the action. The invoke thunk runs the action headless
/// via runQuickActionSync(). Returns the number registered. Action-agnostic (no
/// dependency on the concrete built-in actions), so it is unit-testable with probes.
[[nodiscard]] int registerQuickActionsInto(QuickActionController& controller,
                                           AppActionRegistry& registry);

class AppActionService : public QObject {
    Q_OBJECT

public:
    /// @param backup_dir output/backup location handed to the actions that write
    /// files (system report, settings screenshots, BitLocker key backup).
    explicit AppActionService(const QString& backup_dir, QObject* parent = nullptr);
    ~AppActionService() override;

    AppActionService(const AppActionService&) = delete;
    AppActionService& operator=(const AppActionService&) = delete;
    AppActionService(AppActionService&&) = delete;
    AppActionService& operator=(AppActionService&&) = delete;

    /// Register every built-in QuickAction into @p registry as an AppAction whose
    /// invoke thunk runs it headless via runQuickActionSync(). Returns the count
    /// registered. Ids are stable ("action.<slug>").
    int registerInto(AppActionRegistry& registry);

    /// The underlying controller (for the GUI panels to share, and for tests).
    [[nodiscard]] QuickActionController* controller() const;

private:
    std::unique_ptr<QuickActionController> m_controller;
    QString m_backup_dir;
};

/// A QuickAction::ExecutionResult flattened to primitives, so the mapping to an
/// AppActionResult can live in the action-agnostic bridge and be unit-tested there.
struct AppActionExecutionFields {
    bool success{false};
    QString message;
    qint64 bytes{0};
    qint64 files{0};
    qint64 duration_ms{0};
    QString output_path;
};

/// Map an execution into the AppAction result (success/message + a data payload).
/// Exposed for tests.
[[nodiscard]] AppActionResult appActionResultFromExecution(const AppActionExecutionFields& fields);

}  // namespace sak
