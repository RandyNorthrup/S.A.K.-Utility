// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_backup_worker.h"
#include <QVBoxLayout>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>

namespace sak {

// ============================================================================
// UserProfileBackupExecutePage
// ============================================================================

UserProfileBackupExecutePage::UserProfileBackupExecutePage(BackupManifest& manifest,
                                     const QVector<UserProfile>& users,
                                     const QString& destinationPath,
                                     QWidget* parent)
    : QWizardPage(parent)
    , m_manifest(manifest)
    , m_users(users)
    , m_destinationPath(destinationPath)
{
    setTitle(tr("Execute Backup"));
    setSubTitle(tr("Backup in progress..."));
    
    setupUi();
}

void UserProfileBackupExecutePage::setupUi() {
    auto* layout = new QVBoxLayout(this);
    
    // Status label
    m_statusLabel = new QLabel(tr("Ready to start backup"), this);
    m_statusLabel->setStyleSheet("QLabel { font-weight: 600; color: #1e293b; }");
    layout->addWidget(m_statusLabel);
    
    // Current user being backed up
    m_currentUserLabel = new QLabel(this);
    layout->addWidget(m_currentUserLabel);
    
    // Overall progress
    layout->addWidget(new QLabel(tr("Overall Progress:"), this));
    m_overallProgress = new QProgressBar(this);
    m_overallProgress->setFormat("%v / %m (%p%)");
    layout->addWidget(m_overallProgress);
    
    // Current file/folder progress
    layout->addWidget(new QLabel(tr("Current Operation:"), this));
    m_currentProgress = new QProgressBar(this);
    m_currentProgress->setFormat("%v / %m (%p%)");
    layout->addWidget(m_currentProgress);
    
    // Log output
    layout->addWidget(new QLabel(tr("Log:"), this));
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(150);
    layout->addWidget(m_logEdit);
    
    // Start button
    m_startButton = new QPushButton(tr("Start Backup"), this);
    m_startButton->setIcon(QIcon::fromTheme("media-playback-start"));
    connect(m_startButton, &QPushButton::clicked, this, &UserProfileBackupExecutePage::onStartBackup);
    layout->addWidget(m_startButton);
    
    layout->addStretch();
}

void UserProfileBackupExecutePage::initializePage() {
    m_statusLabel->setText(tr("Ready to start backup. Click Start Backup to begin."));
    m_startButton->setEnabled(true);
}

bool UserProfileBackupExecutePage::isComplete() const {
    return m_completed;
}

void UserProfileBackupExecutePage::onStartBackup() {
    if (m_started) return;
    
    m_started = true;
    m_startButton->setEnabled(false);
    m_statusLabel->setText(tr("Backup in progress..."));
    
    appendLog(tr("=== Backup Started ==="));
    appendLog(tr("Destination: %1").arg(m_destinationPath));
    appendLog(tr("Users to backup: %1").arg(m_users.size()));
    
    // Get smart filter and permission mode from wizard
    auto* wiz = qobject_cast<UserProfileBackupWizard*>(wizard());
    if (!wiz) {
        appendLog(tr("ERROR: Could not access wizard"));
        return;
    }
    
    SmartFilter smartFilter = wiz->getSmartFilter();
    
    // Get permission mode from settings page (through wizard field)
    // Default to StripAll if not found
    PermissionMode permissionMode = PermissionMode::StripAll;
    
    // Get compression and encryption settings
    int compressionLevel = wiz->getCompressionLevel();
    bool encrypt = wiz->isEncryptionEnabled();
    QString password = wiz->getEncryptionPassword();
    
    appendLog(tr("Compression: %1").arg(
        compressionLevel == 0 ? tr("None") :
        compressionLevel <= 3 ? tr("Fast") :
        compressionLevel <= 6 ? tr("Balanced") : tr("Maximum")));
    
    if (encrypt) {
        appendLog(tr("Encryption: Enabled (AES-256)"));
    }
    
    // Save installed apps list to backup directory
    QVector<InstalledAppInfo> installedApps = wiz->installedApps();
    if (!installedApps.isEmpty()) {
        QJsonArray appsArray;
        for (const auto& app : installedApps) {
            QJsonObject appObj;
            appObj["name"] = app.name;
            appObj["version"] = app.version;
            appObj["publisher"] = app.publisher;
            appObj["choco_package"] = app.choco_package;
            appObj["category"] = app.category;
            appsArray.append(appObj);
        }
        
        QJsonDocument doc(appsArray);
        QFile appsFile(m_destinationPath + "/installed_apps.json");
        if (appsFile.open(QIODevice::WriteOnly)) {
            appsFile.write(doc.toJson(QJsonDocument::Indented));
            appsFile.close();
            appendLog(tr("Saved %1 installed application(s) to backup").arg(installedApps.size()));
        } else {
            appendLog(tr("WARNING: Could not save installed applications list"));
        }
    }
    
    // Create and configure backup worker
    auto worker = new UserProfileBackupWorker(this);
    
    // Connect signals for progress tracking
    connect(worker, &UserProfileBackupWorker::overallProgress, 
            this, &UserProfileBackupExecutePage::onBackupProgress);
    connect(worker, &UserProfileBackupWorker::logMessage,
            this, [this](const QString& message, bool isWarning) {
                appendLog(isWarning ? QString("[WARNING] %1").arg(message) : message);
            });
    connect(worker, &UserProfileBackupWorker::statusUpdate,
            this, [this](const QString& username, const QString& operation) {
                m_statusLabel->setText(tr("Backing up %1: %2").arg(username, operation));
            });
    connect(worker, &UserProfileBackupWorker::backupComplete,
            this, [this, worker](bool success, const QString& message, const BackupManifest&) {
                onBackupComplete(success, message);
                worker->deleteLater();
            });
    
    // Start backup operation with compression and encryption
    worker->startBackup(m_manifest, m_users, m_destinationPath, smartFilter, permissionMode,
                       compressionLevel, encrypt, password);
    
    // Configure progress bars
    m_overallProgress->setRange(0, m_users.size());
    m_currentProgress->setRange(0, 0); // Indeterminate for file operations
}

void UserProfileBackupExecutePage::onBackupProgress(int current, int total, qint64 bytes, qint64 totalBytes) {
    (void)bytes;
    (void)totalBytes;
    m_overallProgress->setMaximum(total);
    m_overallProgress->setValue(current);
}

void UserProfileBackupExecutePage::onBackupComplete(bool success, const QString& message) {
    m_completed = true;
    m_statusLabel->setText(success ? tr("Backup completed successfully!") : tr("Backup failed!"));
    appendLog(success ? tr("=== Backup Complete ===") : tr("=== Backup Failed ==="));
    appendLog(message);
    Q_EMIT completeChanged();
}

void UserProfileBackupExecutePage::onLogMessage(const QString& message) {
    appendLog(message);
}

void UserProfileBackupExecutePage::appendLog(const QString& message) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logEdit->append(QString("[%1] %2").arg(timestamp, message));
}

} // namespace sak
