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
    if (!askToCancel(intent, running)) {
        logInfo("Exit declined; running work continues");
        return false;
    }
    RunningOperations::cancelAllRunning();
    waitForCancellation(RunningOperations::runningNames(), std::move(proceed));
    return false;
}

bool RunningWorkExitGuard::isWaitingForCancellation() const {
    return m_poll.isActive();
}

QString RunningWorkExitGuard::question(Intent intent, const QStringList& names) {
    const bool one = names.size() == 1;
    const QString running = one ? tr("%1 is running.").arg(joinedNames(names))
                                : tr("%1 are running.").arg(joinedNames(names));
    if (intent == Intent::RestartElevated) {
        return running + QLatin1Char(' ') +
               (one ? tr("Cancel it and restart as administrator?")
                    : tr("Cancel them and restart as administrator?"));
    }
    return running + QLatin1Char(' ') +
           (one ? tr("Cancel it and exit?") : tr("Cancel them and exit?"));
}

QString RunningWorkExitGuard::proceedButtonText(Intent intent) {
    return intent == Intent::RestartElevated ? tr("Restart after cancelling")
                                             : tr("Exit after cancelling");
}

QString RunningWorkExitGuard::stoppingText(const QStringList& names) {
    return tr("Waiting for %1 to stop...").arg(joinedNames(names));
}

bool RunningWorkExitGuard::askToCancel(Intent intent, const QStringList& names) {
    QMessageBox box(m_window);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("S.A.K. Utility"));
    box.setText(question(intent, names));
    box.setInformativeText(
        tr("Each operation is asked to stop at its next safe point, and S.A.K. Utility waits for "
           "it before it closes."));
    const QPushButton* const go_ahead = box.addButton(proceedButtonText(intent),
                                                      QMessageBox::AcceptRole);
    QPushButton* const keep_running = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(keep_running);
    box.setEscapeButton(keep_running);
    box.exec();
    return box.clickedButton() == go_ahead;
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
