// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/running_operations.h"
#include "sak/style_constants.h"
#include "sak/thread_custody.h"
#include "sak/user_profile_progress.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/user_profile_restore_worker.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWizard>

#include <memory>

namespace sak {

namespace {

// How long closing the page waits for a cancelled restore to reach its next file boundary before
// handing the still-copying worker to thread custody.
constexpr int kWorkerReleaseMs = 5000;

}  // namespace

// ============================================================================
// Page 6: Execute Restore
// ============================================================================

UserProfileRestoreExecutePage::UserProfileRestoreExecutePage(QWidget* parent)
    : QWizardPage(parent), m_worker(nullptr), m_restoreComplete(false), m_restoreSuccess(false) {
    setTitle(tr("Restore in Progress"));
    setSubTitle(tr("Restoring user profile data..."));

    setupUi();
    RunningOperations::add(
        this,
        tr("Profile restore"),
        [this]() { return isRestoreRunning(); },
        [this]() { onCancelRestore(); });
}

UserProfileRestoreExecutePage::~UserProfileRestoreExecutePage() {
    // The worker is this page's child, so ~QObject would destroy it mid-restore. A large in-flight
    // copy cannot observe cancellation, so release it instead: joined within the bound, or
    // reported and retained until that copy finishes. Never terminated, never an unbounded wait.
    if (m_worker != nullptr) {
        UserProfileRestoreWorker* const worker = m_worker;
        m_worker = nullptr;
        ThreadCustody::release(
            std::unique_ptr<QThread>(worker), [worker]() { worker->cancel(); }, kWorkerReleaseMs);
    }
}

bool UserProfileRestoreExecutePage::isRestoreRunning() const {
    return m_worker != nullptr && m_worker->isRunning();
}

void UserProfileRestoreExecutePage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Status
    m_statusLabel = new QLabel(tr("Ready to restore..."), this);
    m_statusLabel->setStyleSheet(sak::ui::fontSizeWeightColorStyle(
        sak::ui::kFontSizeStatus, sak::ui::kFontWeightSemibold, sak::ui::kColorTextHeading));
    layout->addWidget(m_statusLabel);

    // Overall progress
    auto* overall_label = new QLabel(tr("Overall Progress:"), this);
    layout->addWidget(overall_label);
    m_overallProgressBar = new QProgressBar(this);
    m_overallProgressBar->setTextVisible(true);
    m_overallProgressBar->setAccessibleName(tr("Restore overall byte progress"));
    overall_label->setBuddy(m_overallProgressBar);
    layout->addWidget(m_overallProgressBar);

    // Current operation progress
    m_currentOperationLabel = new QLabel(tr("Current: -"), this);
    layout->addWidget(m_currentOperationLabel);
    m_currentProgressBar = new QProgressBar(this);
    m_currentProgressBar->setTextVisible(true);
    m_currentProgressBar->setAccessibleName(tr("Restore file progress"));
    m_currentOperationLabel->setBuddy(m_currentProgressBar);
    layout->addWidget(m_currentProgressBar);

    layout->addSpacing(sak::ui::kSpacingDefault);

    // Log viewer
    auto* log_label = new QLabel(tr("Operation Log:"), this);
    layout->addWidget(log_label);
    m_logText = new QTextEdit(this);
    m_logText->setAccessibleName(tr("Restore log"));
    m_logText->setReadOnly(true);
    m_logText->setMaximumHeight(sak::kLogAreaMaxH);
    layout->addWidget(m_logText);

    // Buttons
    auto* button_layout = new QHBoxLayout();
    m_cancelButton = new QPushButton(tr("Cancel Restore"), this);
    m_cancelButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    m_viewLogButton = new QPushButton(tr("View Full Log"), this);
    m_viewLogButton->setEnabled(false);
    m_viewLogButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    button_layout->addWidget(m_cancelButton);
    button_layout->addStretch(1);
    button_layout->addWidget(m_viewLogButton);
    layout->addLayout(button_layout);

    // Connections
    connect(m_cancelButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreExecutePage::onCancelRestore);
    connect(m_viewLogButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreExecutePage::onViewLog,
            Qt::QueuedConnection);

    Q_ASSERT(m_statusLabel);
}

void UserProfileRestoreExecutePage::initializePage() {
    Q_ASSERT(m_overallProgressBar);
    Q_ASSERT(m_currentProgressBar);
    // A finished restore is latched for the life of this wizard page. A profile
    // restore is destructive, so re-entering the page (Back then forward) must
    // neither clear the completed result nor silently run the restore a second
    // time; only a fresh wizard (a new instance) may start another restore.
    if (m_restoreComplete) {
        return;
    }
    // Reset state
    m_restoreSuccess = false;
    m_restoreRanToEnd = false;
    m_filesErrored = 0;
    m_networkNotApplied = 0;
    m_lastProcessedBytes = 0;
    m_overallProgressBar->setRange(0, 0);
    m_overallProgressBar->setFormat(tr("Waiting for restore size..."));
    m_overallProgressBar->setAccessibleDescription(tr("Waiting for restore size."));
    m_currentProgressBar->setValue(0);
    m_logText->clear();
    m_statusLabel->setText(tr("Preparing to restore..."));

    // Start restore after UI initializes
    QTimer::singleShot(sak::kTimerDelayShortMs,
                       this,
                       &UserProfileRestoreExecutePage::onStartRestore);
}

void UserProfileRestoreExecutePage::onStartRestore() {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_logText);
    auto* wiz = qobject_cast<UserProfileRestoreWizard*>(wizard());
    if (wiz == nullptr) {
        m_statusLabel->setText(tr("Error: Could not access wizard data"));
        m_restoreComplete = true;
        Q_EMIT completeChanged();
        return;
    }

    if (m_worker != nullptr) {
        // A restore worker is already running for this page (e.g. the page was
        // re-entered). Do not spawn a second one that would race the same
        // destinations and the shared .sakrestore.tmp/.sakold.tmp names.
        return;
    }

    m_statusLabel->setText(tr("Restore in progress..."));
    m_logText->append(tr("[INFO] Restore started..."));

    const QString backup_path = wiz->backupPath();
    const BackupManifest manifest = wiz->manifest();
    const QVector<UserMapping> mappings = wiz->userMappings();
    const ConflictResolution conflict_mode = wiz->conflictResolution();
    const PermissionMode perm_mode = wiz->permissionMode();
    const bool verify = wiz->verifyFiles();
    const bool create_backup = wiz->createBackup();
    // Asked for and checked on the last step, while the person can still go back.
    const QString password = wiz->password();

    auto* worker = new UserProfileRestoreWorker(this);
    // Track the worker so onCancelRestore() can actually reach it; a bare local
    // left m_worker null, making Cancel a no-op.
    m_worker = worker;
    connectRestoreWorkerSignals(worker);

    // Forward the WiFi/Ethernet/AppData selections persisted by their pages'
    // validatePage() so the worker actually applies them (previously dead state).
    worker->startRestore(
        backup_path,
        manifest,
        mappings,
        {conflict_mode, perm_mode, verify, create_backup, password},
        {wiz->wifiProfiles(), wiz->ethernetConfigs(), wiz->appDataSources(), wiz->folderChoices()});

    m_overallProgressBar->setRange(0, 0);
    m_currentProgressBar->setRange(0, 0);  // Indeterminate
}

void UserProfileRestoreExecutePage::connectRestoreWorkerSignals(UserProfileRestoreWorker* worker) {
    // The page's own slots, once each. Lambdas here did the same jobs a second way and left the
    // slots unused: the summary was logged twice, "Current:" never moved off "-", and the file bar
    // stopped wherever its last update left it.
    connect(worker,
            &UserProfileRestoreWorker::overallProgress,
            this,
            &UserProfileRestoreExecutePage::onOverallProgress);
    connect(worker,
            &UserProfileRestoreWorker::fileProgress,
            this,
            &UserProfileRestoreExecutePage::onFileProgress);
    connect(worker,
            &UserProfileRestoreWorker::statusUpdate,
            this,
            &UserProfileRestoreExecutePage::onStatusUpdate);
    connect(worker,
            &UserProfileRestoreWorker::logMessage,
            this,
            &UserProfileRestoreExecutePage::onLogMessage);
    connect(worker,
            &UserProfileRestoreWorker::restoreRanToEnd,
            this,
            &UserProfileRestoreExecutePage::onRestoreRanToEnd);
    connect(worker,
            &UserProfileRestoreWorker::restoreComplete,
            this,
            [this, worker](bool success, const QString& message) {
                onRestoreComplete(success, message);
                m_worker = nullptr;  // Worker is finished; drop the dangling pointer.
                worker->deleteLater();
            });
}

void UserProfileRestoreExecutePage::onCancelRestore() {
    if ((m_worker != nullptr) && m_worker->isRunning()) {
        m_worker->cancel();
        m_logText->append(tr("[WARNING] Canceling restore..."));
    }
}

void UserProfileRestoreExecutePage::onOverallProgress(int completed_users,
                                                      int total_users,
                                                      qint64 processed_bytes,
                                                      qint64 total_bytes) {
    const UserProfileProgressState progress =
        userProfileProgressState(completed_users, total_users, processed_bytes, total_bytes);
    m_lastProcessedBytes = progress.processed_bytes;
    m_overallProgressBar->setRange(progress.minimum, progress.maximum);
    m_overallProgressBar->setValue(progress.value);
    m_overallProgressBar->setFormat(progress.format);
    m_overallProgressBar->setAccessibleDescription(progress.accessible_description);
}

void UserProfileRestoreExecutePage::onFileProgress(int current, int total) {
    if (total > 0) {
        const int percent = (current * kPercentMax) / total;
        m_currentProgressBar->setRange(0, kPercentMax);
        m_currentProgressBar->setValue(percent);
        m_currentProgressBar->setFormat(
            QString("%1% - %2 / %3 files").arg(percent).arg(current).arg(total));
    }
}

void UserProfileRestoreExecutePage::onStatusUpdate(const QString& username,
                                                   const QString& operation) {
    m_statusLabel->setText(tr("Restoring %1: %2").arg(username, operation));
    m_currentOperationLabel->setText(tr("Current: %1 - %2").arg(username, operation));
}

void UserProfileRestoreExecutePage::onLogMessage(const QString& message, bool is_warning) {
    const QString prefix = is_warning ? "[WARNING]" : "[INFO]";
    m_logText->append(QString("%1 %2").arg(prefix, message));

    // Auto-scroll to bottom
    m_logText->verticalScrollBar()->setValue(m_logText->verticalScrollBar()->maximum());
}

void UserProfileRestoreExecutePage::onRestoreRanToEnd(int files_errored, int network_not_applied) {
    m_restoreRanToEnd = true;
    m_filesErrored = files_errored;
    m_networkNotApplied = network_not_applied;
}

namespace {

/// What a restore that ran to its end could not do, for its status line.
QString restoreProblemsText(int files_errored, int network_not_applied) {
    QStringList parts;
    if (files_errored == 1) {
        parts.append(QObject::tr("1 file or folder could not be restored"));
    } else if (files_errored > 1) {
        parts.append(QObject::tr("%1 files or folders could not be restored").arg(files_errored));
    }
    if (network_not_applied == 1) {
        parts.append(QObject::tr("1 network setting was not applied"));
    } else if (network_not_applied > 1) {
        parts.append(QObject::tr("%1 network settings were not applied").arg(network_not_applied));
    }
    return parts.join(QStringLiteral("; "));
}

}  // namespace

namespace {

/// What the page says for a restore that ran every step, with problems or without.
struct FinishedVerdict {
    QString title;
    QString subtitle;
    QString status;
    QString overall_format;
    QString log_line;
};

FinishedVerdict finishedVerdict(bool success, int files_errored, int network_not_applied) {
    if (success) {
        return {.title = QObject::tr("Restore Complete"),
                .subtitle = QObject::tr("The restore finished."),
                .status = QObject::tr("Restore completed successfully!"),
                .overall_format = QObject::tr("Restore complete - %1 processed"),
                .log_line = QObject::tr("[INFO] Restore completed successfully")};
    }
    // Ran every step. With problems it is still finished, not stopped: the files it restored are
    // there, and the summary above names what was not done.
    return {.title = QObject::tr("Restore Finished with Problems"),
            .subtitle = QObject::tr("The restore finished, but not everything was restored."),
            .status = QObject::tr("Restore finished with problems: %1")
                          .arg(restoreProblemsText(files_errored, network_not_applied)),
            .overall_format = QObject::tr("Finished with problems - %1 processed"),
            .log_line = QObject::tr(
                "[WARNING] Restore finished with problems; the warnings above name each one")};
}

}  // namespace

void UserProfileRestoreExecutePage::onRestoreComplete(bool success, const QString& /*summary*/) {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_logText);
    m_restoreComplete = true;
    m_restoreSuccess = success;

    // Nothing is running now, so no bar may still be moving or stopped part way. The worker has
    // logged its own summary; this adds only the verdict.
    m_currentProgressBar->setRange(0, kPercentMax);
    if (success || m_restoreRanToEnd) {
        const FinishedVerdict verdict =
            finishedVerdict(success, m_filesErrored, m_networkNotApplied);
        setTitle(verdict.title);
        setSubTitle(verdict.subtitle);
        m_statusLabel->setText(verdict.status);
        m_overallProgressBar->setRange(0, kPercentMax);
        m_overallProgressBar->setValue(kPercentMax);
        m_overallProgressBar->setFormat(
            verdict.overall_format.arg(formatBytes(m_lastProcessedBytes)));
        m_overallProgressBar->setAccessibleDescription(m_overallProgressBar->format());
        m_currentProgressBar->setValue(kPercentMax);
        m_currentProgressBar->setFormat(tr("Done"));
        m_currentOperationLabel->setText(tr("Current: done"));
        m_logText->append(verdict.log_line);
    } else {
        setTitle(tr("Restore Stopped"));
        setSubTitle(tr("The restore did not finish."));
        m_statusLabel->setText(tr("Restore failed"));
        m_overallProgressBar->setRange(0, kPercentMax);
        m_overallProgressBar->setFormat(tr("Stopped"));
        m_currentProgressBar->setValue(0);
        m_currentProgressBar->setFormat(tr("Stopped"));
        m_currentOperationLabel->setText(tr("Current: stopped"));
        m_logText->append(tr("[ERROR] Restore failed"));
    }

    // The header and the wizard's own Cancel said the restore was still running after it ended.
    m_cancelButton->setEnabled(false);
    if (wizard() != nullptr && wizard()->button(QWizard::CancelButton) != nullptr) {
        wizard()->button(QWizard::CancelButton)->setEnabled(false);
    }
    m_viewLogButton->setEnabled(true);

    Q_EMIT completeChanged();
}

void UserProfileRestoreExecutePage::onViewLog() {
    Q_ASSERT(m_logText);
    QMessageBox msg_box(this);
    msg_box.setWindowTitle(tr("Restore Log"));
    msg_box.setText(tr("Complete restore operation log:"));
    msg_box.setDetailedText(m_logText->toPlainText());
    msg_box.setIcon(m_restoreSuccess ? QMessageBox::Information : QMessageBox::Warning);
    msg_box.setStandardButtons(QMessageBox::Ok | QMessageBox::Save);

    if (msg_box.exec() != QMessageBox::Save) {
        return;
    }
    const QString file_name = QFileDialog::getSaveFileName(
        this,
        tr("Save Log"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/restore_log.txt",
        tr("Text Files (*.txt);;All Files (*.*)"));
    if (file_name.isEmpty()) {
        return;
    }
    QFile file(file_name);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << m_logText->toPlainText();
        file.close();
    } else {
        sak::logError("Failed to save restore log: {}", file_name.toStdString());
    }
}

bool UserProfileRestoreExecutePage::isComplete() const {
    return m_restoreComplete;
}

}  // namespace sak
