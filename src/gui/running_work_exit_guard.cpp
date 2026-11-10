// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file running_work_exit_guard.cpp
/// @brief Asks before closing or relaunching while long work runs, then cancels it cooperatively.

#include "sak/running_work_exit_guard.h"

#include "sak/logger.h"
#include "sak/running_operations.h"

#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopeGuard>
#include <QWidget>

#include <utility>

namespace sak {

namespace {

// How often the progress dialog re-reads what is still stopping. Runs only while that dialog is
// shown, i.e. after the user asked to close; never in the background.
constexpr int kCancellationPollMs = 100;

QString joinedNames(const QStringList& names) {
    if (names.size() == 1) {
        return names.constFirst();
    }
    const QStringList leading = names.mid(0, names.size() - 1);
    return QObject::tr("%1 and %2").arg(leading.join(QStringLiteral(", ")), names.constLast());
}

}  // namespace

RunningWorkExitGuard::RunningWorkExitGuard(QWidget* window) : QObject(window), m_window(window) {
    m_poll.setInterval(kCancellationPollMs);
    connect(&m_poll, &QTimer::timeout, this, &RunningWorkExitGuard::onPoll);
}

bool RunningWorkExitGuard::proceedNowOrAfterCancelling(Intent intent,
                                                       std::function<void()> proceed) {
    if (isWaitingForCancellation()) {
        return false;  // Already cancelling; the deferred action runs once that work stops.
    }
    const QStringList running = RunningOperations::runningNames();
    if (running.isEmpty()) {
        return true;
    }
    logInfo("{} requested while running: {}",
            intent == Intent::Exit ? "Exit" : "Restart as administrator",
            running.join(QStringLiteral(", ")).toStdString());
    if (!askToCancel(intent, running, RunningOperations::cannotStopNotes())) {
        logInfo(m_prompt_outlived ? "The work the exit prompt asked about ended; nothing cancelled"
                                  : "Exit declined; running work continues");
        return false;
    }
    RunningOperations::cancelAllRunning();
    waitForCancellation(RunningOperations::runningNames(), std::move(proceed));
    return false;
}

bool RunningWorkExitGuard::isWaitingForCancellation() const {
    return m_poll.isActive();
}

// "Still running: X." reads right whatever X is: one name or several, and a name that is itself
// a plural ("Partition changes is running" did not). The question after it has no pronoun to
// agree with the names either.
QString RunningWorkExitGuard::stillRunning(const QStringList& names) {
    return tr("Still running: %1.").arg(joinedNames(names));
}

QString RunningWorkExitGuard::question(Intent intent, const QStringList& names) {
    return stillRunning(names) + QLatin1Char(' ') +
           (intent == Intent::RestartElevated ? tr("Cancel and restart as administrator?")
                                              : tr("Cancel and exit?"));
}

QString RunningWorkExitGuard::proceedButtonText(Intent intent) {
    return intent == Intent::RestartElevated ? tr("Restart after cancelling")
                                             : tr("Exit after cancelling");
}

QString RunningWorkExitGuard::questionWhenSomeCannotStop(Intent intent, const QStringList& names) {
    return stillRunning(names) + QLatin1Char(' ') +
           (intent == Intent::RestartElevated ? tr("Restart as administrator anyway?")
                                              : tr("Exit anyway?"));
}

QString RunningWorkExitGuard::proceedAnywayButtonText(Intent intent) {
    return intent == Intent::RestartElevated ? tr("Restart anyway") : tr("Exit anyway");
}

QString RunningWorkExitGuard::informativeText(const QStringList& cannot_stop_notes) {
    if (cannot_stop_notes.isEmpty()) {
        return tr(
            "Each operation is asked to stop at its next safe point, and S.A.K. Utility "
            "waits for it before it closes.");
    }
    return cannot_stop_notes.join(QStringLiteral("\n\n")) + QStringLiteral("\n\n") +
           tr("Everything else S.A.K. Utility can stop is asked to stop at its next safe point, "
              "and S.A.K. Utility waits for that before it closes.");
}

QString RunningWorkExitGuard::stoppingText(const QStringList& names) {
    return tr("Waiting for %1 to stop...").arg(joinedNames(names));
}

bool RunningWorkExitGuard::askToCancel(Intent intent,
                                       const QStringList& names,
                                       const QStringList& cannot_stop_notes) {
    m_prompt_outlived = false;
    QMessageBox box(m_window);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("S.A.K. Utility"));
    QPushButton* const go_ahead = box.addButton(QString(), QMessageBox::AcceptRole);
    QPushButton* const keep_running = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(keep_running);
    box.setEscapeButton(keep_running);
    showPrompt(&box, go_ahead, intent, names, cannot_stop_notes);
    // While the prompt is open it follows the work it asks about: a note that goes is taken out,
    // and work that ends closes it before that work's result appears over it.
    RunningOperations::setChangeListener([this, &box, go_ahead, intent](const QObject* finished) {
        refreshPrompt(&box, go_ahead, intent, finished);
    });
    const auto stop_following = qScopeGuard([]() { RunningOperations::setChangeListener({}); });
    box.exec();
    return !m_prompt_outlived && box.clickedButton() == go_ahead;
}

void RunningWorkExitGuard::showPrompt(QMessageBox* box,
                                      QPushButton* go_ahead,
                                      Intent intent,
                                      const QStringList& names,
                                      const QStringList& cannot_stop_notes) {
    // Work Windows finishes on its own is not promised a stop: the prompt says so, and its button
    // goes ahead "anyway" rather than "after cancelling".
    const bool some_cannot_stop = !cannot_stop_notes.isEmpty();
    box->setText(some_cannot_stop ? questionWhenSomeCannotStop(intent, names)
                                  : question(intent, names));
    box->setInformativeText(informativeText(cannot_stop_notes));
    go_ahead->setText(some_cannot_stop ? proceedAnywayButtonText(intent)
                                       : proceedButtonText(intent));
}

void RunningWorkExitGuard::refreshPrompt(QMessageBox* box,
                                         QPushButton* go_ahead,
                                         Intent intent,
                                         const QObject* finished_owner) {
    const QStringList names = RunningOperations::runningNames(finished_owner);
    if (names.isEmpty()) {
        m_prompt_outlived = true;
        box->reject();
        return;
    }
    showPrompt(box, go_ahead, intent, names, RunningOperations::cannotStopNotes(finished_owner));
}

void RunningWorkExitGuard::waitForCancellation(const QStringList& names,
                                               std::function<void()> proceed) {
    m_proceed = std::move(proceed);
    // Busy indicator with no Cancel button: the cancellation already happened, and there is
    // nothing safe left to offer but waiting for it.
    auto* progress = new QProgressDialog(stoppingText(names), QString(), 0, 0, m_window);
    progress->setWindowTitle(tr("S.A.K. Utility"));
    progress->setAccessibleName(tr("Stopping running work"));
    progress->setWindowFlag(Qt::WindowCloseButtonHint, false);
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    progress->show();
    m_progress = progress;
    m_poll.start();
}

void RunningWorkExitGuard::onPoll() {
    const QStringList still_running = RunningOperations::runningNames();
    if (!still_running.isEmpty()) {
        if (m_progress != nullptr) {
            m_progress->setLabelText(stoppingText(still_running));
        }
        return;
    }
    m_poll.stop();
    if (m_progress != nullptr) {
        m_progress->close();
        m_progress->deleteLater();
    }
    logInfo("Running work stopped; continuing with the requested exit");
    const std::function<void()> proceed = std::exchange(m_proceed, {});
    if (proceed) {
        proceed();
    }
}

}  // namespace sak
