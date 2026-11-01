// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QJsonObject>
#include <QMetaType>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QThread>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sak {

class ElevationBroker;

[[nodiscard]] QString elevatedTaskIdForQuickAction(const QString& action_name);
[[nodiscard]] QJsonObject buildElevatedActionPayload(const QString& task_id,
                                                     const QString& output_folder);

/**
 * @brief One request to run a quick action, and everything that belongs to that run alone.
 *
 * It travels with the run from the request to its completion: a run queued behind another
 * caller's keeps the output folder its own caller chose, and every signal about the run names
 * the caller that asked for it.
 */
struct QuickActionRun {
    /// The registered action's name.
    QString action_name;
    /// Who asked: a fixed token per panel, a fresh one per assistant call. A caller reacts only
    /// to the runs that carry its own token.
    QString owner;
    /// Where this run writes its files (report, screenshots, recovery keys). Empty for an action
    /// that writes none; an action that needs one refuses to run without it.
    QString output_folder;
};

/**
 * @brief The application's one quick-action runner.
 *
 * The Backup and Restore, Diagnostics and Network panels and the assistant share it (MainWindow
 * creates it with the built-in actions, createAppQuickActionController). It runs one action at a
 * time; a request that arrives while another runs is queued, and its caller is told so.
 *
 * Elevated helper: the controller is the one owner of the broker its administrator actions use.
 * The broker is created on the first administrator run and kept for the controller's lifetime, so
 * runs in one session share one helper (one UAC prompt) while the helper stays inside its idle
 * timeout; ElevationBroker retires a helper near that timeout and starts a new one. A cancel of
 * the run using it (a panel's Stop on exit, the assistant's Stop or timeout) reaches the helper's
 * task. Destroying the controller -- at application exit -- cancels the running task, joins the
 * worker within its bound and shuts the helper down; a worker that overruns is handed, with the
 * action and the broker it uses, to ThreadCustody. No other feature uses this broker.
 *
 * Thread-Safety: NOT internally synchronized. Call requestRun, cancelRunsOf and hasRunsOf on the
 * controller's thread (another thread uses a queued invocation, as runQuickActionSync does). The
 * workers only run QuickAction or broker code that was handed to them.
 */
class QuickActionController : public QObject {
    Q_OBJECT

public:
    explicit QuickActionController(QObject* parent = nullptr);
    ~QuickActionController() override;

    QuickActionController(const QuickActionController&) = delete;
    QuickActionController& operator=(const QuickActionController&) = delete;
    QuickActionController(QuickActionController&&) = delete;
    QuickActionController& operator=(QuickActionController&&) = delete;

    /**
     * @brief Register an action
     * @param action Action to register (ownership transferred)
     * @return The action's name, or empty when it was refused (null, or a duplicate name)
     */
    QString registerAction(std::unique_ptr<QuickAction> action);

    /// @brief The registered action called @p action_name, or nullptr.
    QuickAction* getAction(const QString& action_name) const;

    /// @brief Every registered action, in registration order.
    std::vector<QuickAction*> getAllActions() const;

    /**
     * @brief Run an action for a caller: now, or after the run in progress.
     *
     * The caller confirms with the user first (a panel's question, the assistant's human gate);
     * the controller has no confirmation channel of its own. Every request ends in exactly one
     * runFinished(): a refused one (unknown action, no owner) at once, a queued one after the
     * runs ahead of it.
     */
    void requestRun(const QuickActionRun& run);

    /**
     * @brief Cancel every run @p owner asked for.
     *
     * A queued run never starts (it finishes as cancelled); a running one is asked to stop --
     * for an administrator action, through the elevated helper. Runs of other callers are not
     * touched.
     */
    void cancelRunsOf(const QString& owner);

    /// @brief Whether a run @p owner asked for is running or queued.
    [[nodiscard]] bool hasRunsOf(const QString& owner) const;

#ifdef SAK_CORE_TEST_HOOKS
    /// Test-only: what the controller builds its broker with (an empty function restores the
    /// real ElevationBroker). Compiled only into the tests that drive the elevated route
    /// (tests/CMakeLists.txt, SAK_CORE_QUICK_ACTION_TEST_HOOK_SOURCES).
    static void setElevationBrokerFactoryForTesting(
        std::function<std::unique_ptr<ElevationBroker>()> factory);
#endif

Q_SIGNALS:
    /// @p run waits behind @p running_action, which another request started first.
    void runQueued(const sak::QuickActionRun& run, const QString& running_action);
    void runStarted(const sak::QuickActionRun& run);
    void runProgress(const sak::QuickActionRun& run, const QString& message, int percent);
    /// A line for @p run's caller's log (the same line goes to quick_actions.log).
    void runLog(const sak::QuickActionRun& run, const QString& line);
    /// @p run is over: completed, failed, refused or cancelled. @p result is this run's own,
    /// carried by value, so a slot never reads an action object a later run may be writing.
    void runFinished(const sak::QuickActionRun& run,
                     const sak::QuickAction::ExecutionResult& result,
                     sak::QuickAction::ActionStatus status);

private:
    void startRun(const QuickActionRun& run);
    void startInProcess(QuickAction* action);
    void startElevated(QuickAction* action);
    [[nodiscard]] QString elevatedRunRefusal(const QString& task_id) const;
    ElevationBroker& elevationBroker();
    void finishCurrent(const QuickAction::ExecutionResult& result,
                       QuickAction::ActionStatus status);
    void finishUnstarted(const QuickActionRun& run,
                         const QString& message,
                         QuickAction::ActionStatus status);
    void startNextQueued();
    void joinCurrentThread();

    /// Destruction-only: joins the running worker within its bound, or reports the overrun and
    /// hands the thread, the action it runs and (for an elevated run) the broker to ThreadCustody.
    void retireThreadOnDestroy();

    void logRun(const QuickActionRun& run, const QString& message);
    void writeLogLine(const QString& line) const;
    [[nodiscard]] static QString logLine(const QString& action_name, const QString& message);

    std::vector<std::unique_ptr<QuickAction>> m_actions;
    QHash<QString, QuickAction*> m_action_map;

    QQueue<QuickActionRun> m_queue;
    std::optional<QuickActionRun> m_current;
    QuickAction* m_current_action{nullptr};
    bool m_current_elevated{false};
    QThread* m_thread{nullptr};

    std::unique_ptr<ElevationBroker> m_broker;
    QString m_log_file_path;
};

/// What a caller does with the runs it asked for. Any handler may be empty.
struct QuickActionRunHandlers {
    std::function<void(const QuickActionRun& run, const QString& running_action)> queued;
    std::function<void(const QuickActionRun& run)> started;
    std::function<void(const QuickActionRun& run, const QString& message, int percent)> progress;
    std::function<void(const QuickActionRun& run, const QString& line)> log;
    std::function<void(const QuickActionRun& run,
                       const QuickAction::ExecutionResult& result,
                       QuickAction::ActionStatus status)>
        finished;
};

/**
 * @brief Deliver @p controller's signals for the runs @p owner asked for -- and no others -- to
 *        @p handlers, on @p receiver's thread.
 *
 * The connections are queued, so a run refused inside requestRun() reaches its caller after the
 * request returns, and they end with @p receiver.
 */
void connectRunsOf(QuickActionController& controller,
                   const QString& owner,
                   QObject* receiver,
                   QuickActionRunHandlers handlers);

/// "Completed: <action>", "Cancelled: <action>" or "Failed: <action> - <message>", followed by
/// " - saved to <path>" whenever the run left a file or folder behind (even a failed one).
[[nodiscard]] QString quickActionOutcomeText(const QuickActionRun& run,
                                             const QuickAction::ExecutionResult& result,
                                             QuickAction::ActionStatus status);

/// "Queued: <action> - waits for <running action> to finish".
[[nodiscard]] QString quickActionQueuedText(const QuickActionRun& run,
                                            const QString& running_action);

}  // namespace sak

Q_DECLARE_METATYPE(sak::QuickActionRun)
