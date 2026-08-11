// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/user_profile_restore_worker.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace sak {

namespace {
constexpr int kRestoreProgressDisplayPrecision = 2;
}  // namespace

// ============================================================================
// Page 6: Execute Restore
// ============================================================================

UserProfileRestoreExecutePage::UserProfileRestoreExecutePage(QWidget* parent)
    : QWizardPage(parent), m_worker(nullptr), m_restoreComplete(false), m_restoreSuccess(false) {
    setTitle(tr("Restore in Progress"));
    setSubTitle(tr("Restoring user profile data..."));

    setupUi();
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
    auto* overallLabel = new QLabel(tr("Overall Progress:"), this);
    layout->addWidget(overallLabel);
    m_overallProgressBar = new QProgressBar(this);
    m_overallProgressBar->setTextVisible(true);
    layout->addWidget(m_overallProgressBar);

    // Current operation progress
    m_currentOperationLabel = new QLabel(tr("Current: -"), this);
    layout->addWidget(m_currentOperationLabel);
    m_currentProgressBar = new QProgressBar(this);
    m_currentProgressBar->setTextVisible(true);
    layout->addWidget(m_currentProgressBar);

    layout->addSpacing(sak::ui::kSpacingDefault);

    // Log viewer
    auto* logLabel = new QLabel(tr("Operation Log:"), this);
    layout->addWidget(logLabel);
    m_logText = new QTextEdit(this);
    m_logText->setReadOnly(true);
    m_logText->setMaximumHeight(sak::kLogAreaMaxH);
    layout->addWidget(m_logText);

    // Buttons
    auto* buttonLayout = new QHBoxLayout();
    m_cancelButton = new QPushButton(tr("Cancel Restore"), this);
    m_cancelButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    m_viewLogButton = new QPushButton(tr("View Full Log"), this);
    m_viewLogButton->setEnabled(false);
    m_viewLogButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    buttonLayout->addWidget(m_cancelButton);
    buttonLayout->addStretch(1);
    buttonLayout->addWidget(m_viewLogButton);
    layout->addLayout(buttonLayout);

    // Connections
    connect(m_cancelButton,
            &QPushButton::clicked,
            this,
            &UserProfileRestoreExecutePage::onCancelRestore);
    connect(
        m_viewLogButton, &QPushButton::clicked, this, &UserProfileRestoreExecutePage::onViewLog);

    Q_ASSERT(m_statusLabel);
}

void UserProfileRestoreExecutePage::initializePage() {
    Q_ASSERT(m_overallProgressBar);
    Q_ASSERT(m_currentProgressBar);
    // Reset state
    m_restoreComplete = false;
    m_restoreSuccess = false;
    m_overallProgressBar->setValue(0);
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
    if (!wiz) {
        m_statusLabel->setText(tr("Error: Could not access wizard data"));
        m_restoreComplete = true;
        Q_EMIT completeChanged();
        return;
    }

    if (m_worker) {
        // A restore worker is already running for this page (e.g. the page was
        // re-entered). Do not spawn a second one that would race the same
        // destinations and the shared .sakrestore.tmp/.sakold.tmp names.
        return;
    }

    m_statusLabel->setText(tr("Restore in progress..."));
    m_logText->append(tr("[INFO] Restore started..."));

    const QString backupPath = wiz->backupPath();
    const BackupManifest manifest = wiz->manifest();
    const QVector<UserMapping> mappings = wiz->userMappings();
    const ConflictResolution conflictMode = wiz->conflictResolution();
    const PermissionMode permMode = wiz->permissionMode();
    const bool verify = wiz->verifyFiles();
    const bool createBackup = wiz->createBackup();

    // An encrypted backup cannot be read without the password, and the worker refuses to
    // start without one. Ask here, where the manifest is already known, rather than
    // letting every file fail to decode.
    QString password;
    if (manifest.encrypted) {
        bool accepted = false;
        password = QInputDialog::getText(
            this,
            tr("Encrypted Backup"),
            tr("This backup is encrypted.\nEnter the password used when it was created:"),
            QLineEdit::Password,
            QString(),
            &accepted);
        if (!accepted || password.isEmpty()) {
            m_statusLabel->setText(tr("Restore not started"));
            m_logText->append(tr("[ERROR] A password is required to restore this backup."));
            return;
        }
    }

    auto* worker = new UserProfileRestoreWorker(this);
    // Track the worker so onCancelRestore() can actually reach it; a bare local
    // left m_worker null, making Cancel a no-op.
    m_worker = worker;
    connectRestoreWorkerSignals(worker);

    // Forward the WiFi/Ethernet/AppData selections persisted by their pages'
    // validatePage() so the worker actually applies them (previously dead state).
    worker->startRestore(backupPath,
                         manifest,
                         mappings,
                         {conflictMode, permMode, verify, createBackup, password},
                         {wiz->wifiProfiles(), wiz->ethernetConfigs(), wiz->appDataSources()});

    m_overallProgressBar->setRange(0, mappings.size());
    m_currentProgressBar->setRange(0, 0);  // Indeterminate
}

void UserProfileRestoreExecutePage::connectRestoreWorkerSignals(UserProfileRestoreWorker* worker) {
    connect(worker,
            &UserProfileRestoreWorker::overallProgress,
            this,
            [this](int current, int total, qint64 bytes, qint64 totalBytes) {
                m_overallProgressBar->setMaximum(total);
                m_overallProgressBar->setValue(current);
                (void)bytes;
                (void)totalBytes;  // Currently unused
            });
    connect(worker, &UserProfileRestoreWorker::fileProgress, this, [this](int current, int total) {
        m_currentProgressBar->setMaximum(total);
        m_currentProgressBar->setValue(current);
    });
    connect(worker,
            &UserProfileRestoreWorker::statusUpdate,
            this,
            [this](const QString& username, const QString& operation) {
                m_statusLabel->setText(tr("Restoring %1: %2").arg(username, operation));
            });
    connect(worker,
            &UserProfileRestoreWorker::logMessage,
            this,
            [this](const QString& message, bool isWarning) {
                const QString prefix = isWarning ? "[WARNING]" : "[INFO]";
                m_logText->append(QString("%1 %2").arg(prefix, message));
            });
    connect(worker,
            &UserProfileRestoreWorker::restoreComplete,
            this,
            [this, worker](bool success, const QString& message) {
                m_statusLabel->setText(success ? tr("Restore complete!") : tr("Restore failed"));
                m_logText->append(success ? tr("[INFO] Restore completed successfully")
                                          : tr("[ERROR] Restore failed"));
                m_logText->append(QString("[INFO] %1").arg(message));
                m_restoreComplete = true;
                m_restoreSuccess = success;
                m_cancelButton->setEnabled(false);
                m_viewLogButton->setEnabled(true);
                Q_EMIT completeChanged();
                m_worker = nullptr;  // Worker is finished; drop the dangling pointer.
                worker->deleteLater();
            });
}

void UserProfileRestoreExecutePage::onCancelRestore() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
        m_logText->append(tr("[WARNING] Canceling restore..."));
    }
}

void UserProfileRestoreExecutePage::onOverallProgress(int current,
                                                      int total,
                                                      qint64 bytes,
                                                      qint64 totalBytes) {
    if (total > 0) {
        const int percent = (current * kPercentMax) / total;
        m_overallProgressBar->setValue(percent);

        const double gbCopied = bytes / sak::kBytesPerGBf;
        const double gbTotal = totalBytes / sak::kBytesPerGBf;
        m_overallProgressBar->setFormat(
            QString("%1% - %2 / %3 GB")
                .arg(percent)
                .arg(gbCopied, 0, 'f', kRestoreProgressDisplayPrecision)
                .arg(gbTotal, 0, 'f', kRestoreProgressDisplayPrecision));
    }
}

void UserProfileRestoreExecutePage::onFileProgress(int current, int total) {
    if (total > 0) {
        const int percent = (current * kPercentMax) / total;
        m_currentProgressBar->setValue(percent);
        m_currentProgressBar->setFormat(
            QString("%1% - %2 / %3 files").arg(percent).arg(current).arg(total));
    }
}

void UserProfileRestoreExecutePage::onStatusUpdate(const QString& username,
                                                   const QString& operation) {
    m_currentOperationLabel->setText(tr("Current: %1 - %2").arg(username, operation));
}

void UserProfileRestoreExecutePage::onLogMessage(const QString& message, bool isWarning) {
    const QString prefix = isWarning ? "[WARNING]" : "[INFO]";
    m_logText->append(QString("%1 %2").arg(prefix, message));

    // Auto-scroll to bottom
    m_logText->verticalScrollBar()->setValue(m_logText->verticalScrollBar()->maximum());
}

void UserProfileRestoreExecutePage::onRestoreComplete(bool success, const QString& message) {
    Q_ASSERT(m_statusLabel);
    Q_ASSERT(m_logText);
    m_restoreComplete = true;
    m_restoreSuccess = success;

    if (success) {
        m_statusLabel->setText(tr("[OK] Restore completed successfully!"));
    } else {
        m_statusLabel->setText(tr("[X] Restore failed"));
    }

    m_logText->append(QString("\n=== RESTORE COMPLETE ===\n%1").arg(message));
    m_cancelButton->setEnabled(false);
    m_viewLogButton->setEnabled(true);

    Q_EMIT completeChanged();
}

void UserProfileRestoreExecutePage::onViewLog() {
    Q_ASSERT(m_logText);
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Restore Log"));
    msgBox.setText(tr("Complete restore operation log:"));
    msgBox.setDetailedText(m_logText->toPlainText());
    msgBox.setIcon(m_restoreSuccess ? QMessageBox::Information : QMessageBox::Warning);
    msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Save);

    if (msgBox.exec() != QMessageBox::Save) {
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(
        this,
        tr("Save Log"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/restore_log.txt",
        tr("Text Files (*.txt);;All Files (*.*)"));
    if (fileName.isEmpty()) {
        return;
    }
    QFile file(fileName);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << m_logText->toPlainText();
        file.close();
    } else {
        sak::logError("Failed to save restore log: {}", fileName.toStdString());
    }
}

bool UserProfileRestoreExecutePage::isComplete() const {
    return m_restoreComplete;
}

}  // namespace sak
