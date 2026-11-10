// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>

class QMessageBox;
class QProgressDialog;
class QPushButton;
class QWidget;

/// @file running_work_exit_guard.h
/// @brief Asks before closing or relaunching while long work runs, then cancels it cooperatively.
namespace sak {

/// The one place the main window asks "Still running: X. Cancel and exit?" (owner decision
/// 2026-10-24). It reads sak::RunningOperations, so no panel has its own close prompt.
///
/// When something is running the user chooses Cancel (nothing changes) or to go ahead after
/// cancelling: every running operation is then asked to stop cooperatively, a progress dialog
/// names what is still stopping, and the deferred action runs once all of it has stopped.
/// Threads are never terminated; the wait is the work's own cooperative stop.
///
/// Work a cancel cannot stop -- a full format Windows finishes on its own -- is not promised a
/// stop: while an operation reports such a note, the prompt shows it and asks whether to go ahead
/// anyway, and its button does not say "after cancelling".
class RunningWorkExitGuard final : public QObject {
    Q_OBJECT

public:
    /// What the caller wants to do once nothing is running.
    enum class Intent {
        Exit,             ///< Close the application.
        RestartElevated,  ///< Relaunch as administrator, then close this instance.
    };

    explicit RunningWorkExitGuard(QWidget* window);

    /// Returns true when nothing is running, so the caller may carry out @p intent now.
    /// Otherwise asks the user and returns false; if the user chose to go ahead, the running work
    /// is cancelled and @p proceed runs once it has all stopped.
    [[nodiscard]] bool proceedNowOrAfterCancelling(Intent intent, std::function<void()> proceed);

    /// True between the user's go-ahead and the moment the cancelled work has all stopped.
    [[nodiscard]] bool isWaitingForCancellation() const;

    /// The question asked for @p names, e.g. "Still running: USB write. Cancel and exit?".
    [[nodiscard]] static QString question(Intent intent, const QStringList& names);

    /// The go-ahead button's label for @p intent.
    [[nodiscard]] static QString proceedButtonText(Intent intent);

    /// The question asked for @p names while some of that work cannot be stopped, e.g.
    /// "Still running: Partition changes. Exit anyway?".
    [[nodiscard]] static QString questionWhenSomeCannotStop(Intent intent,
                                                            const QStringList& names);

    /// The go-ahead button's label for @p intent while some of the work cannot be stopped.
    [[nodiscard]] static QString proceedAnywayButtonText(Intent intent);

    /// The prompt's informative text: what happens to the running work, with each of
    /// @p cannot_stop_notes (RunningOperations::cannotStopNotes) first when there are any.
    [[nodiscard]] static QString informativeText(const QStringList& cannot_stop_notes);

private:
    [[nodiscard]] bool askToCancel(Intent intent,
                                   const QStringList& names,
                                   const QStringList& cannot_stop_notes);
    /// Puts the question, the notes and the go-ahead's label for what runs now on the prompt.
    static void showPrompt(QMessageBox* box,
                           QPushButton* go_ahead,
                           Intent intent,
                           const QStringList& names,
                           const QStringList& cannot_stop_notes);
    /// Follows a change RunningOperations::notifyChanged reports while the prompt is open: closes
    /// it when nothing it asked about still runs, and otherwise rewrites it for what does.
    void refreshPrompt(QMessageBox* box,
                       QPushButton* go_ahead,
                       Intent intent,
                       const QObject* finished_owner);
    /// "Still running: <names>."
    [[nodiscard]] static QString stillRunning(const QStringList& names);
    void waitForCancellation(const QStringList& names, std::function<void()> proceed);
    void onPoll();
    [[nodiscard]] static QString stoppingText(const QStringList& names);

    QPointer<QWidget> m_window;
    QTimer m_poll;
    QPointer<QProgressDialog> m_progress;
    std::function<void()> m_proceed;
    /// The prompt closed because the work it asked about ended, not by the user's answer.
    bool m_prompt_outlived{false};
};

}  // namespace sak
