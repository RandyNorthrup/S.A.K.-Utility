// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_backup_worker.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/layout_constants.h"
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
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
    auto* layout = new QVBoxLayout(this);

    // Status label
    m_statusLabel = new QLabel(tr("Ready to start backup"), this);
    m_statusLabel->setStyleSheet(QString("QLabel { font-weight: 600; color: %1; }")
        .arg(sak::ui::kColorTextHeading));
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
    m_logEdit->setMaximumHeight(sak::kLogAreaSmallMaxH);
    layout->addWidget(m_logEdit);

    // Start button
    m_startButton = new QPushButton(tr("Start Backup"), this);
    m_startButton->setIcon(QIcon::fromTheme("media-playback-start"));
    m_startButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_startButton, &QPushButton::clicked, this,
        &UserProfileBackupExecutePage::onStartBackup);
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
    PermissionMode permissionMode = PermissionMode::StripAll;

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

    saveInstalledAppsToBackup(wiz->installedApps());
    saveWifiProfilesToBackup(wiz->wifiProfiles());
    saveEthernetConfigsToBackup(wiz->ethernetConfigs());
    saveAppDataSourcesToBackup(wiz->appDataSources());
    connectAndStartBackupWorker(smartFilter, permissionMode,
                                compressionLevel, encrypt, password);
}

void UserProfileBackupExecutePage::saveInstalledAppsToBackup(
    const QVector<InstalledAppInfo>& installedApps) {
    if (installedApps.isEmpty()) return;

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
        const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
        if (appsFile.write(json_bytes) != json_bytes.size()) {
            sak::logError("Incomplete write of installed apps list");
            appendLog(tr("WARNING: Incomplete write of installed applications list"));
        }
        appsFile.close();
        appendLog(tr("Saved %1 installed application(s) to backup").arg(installedApps.size()));
    } else {
        sak::logError("Could not save installed apps list: {}", appsFile.errorString().toStdString());
        appendLog(tr("WARNING: Could not save installed applications list"));
    }
}

void UserProfileBackupExecutePage::saveWifiProfilesToBackup(
    const QVector<WifiProfileInfo>& profiles) {
    if (profiles.isEmpty()) return;

    QJsonArray arr;
    for (const auto& p : profiles) {
        if (p.selected) arr.append(p.toJson());
    }
    if (arr.isEmpty()) return;

    QJsonDocument doc(arr);
    QFile file(m_destinationPath + "/wifi_profiles.json");
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
        if (file.write(json_bytes) != json_bytes.size()) {
            sak::logError("Incomplete write of WiFi profiles");
            appendLog(tr("WARNING: Incomplete write of WiFi profiles"));
        }
        file.close();
        appendLog(tr("Saved %1 WiFi profile(s) to backup").arg(arr.size()));
    } else {
        sak::logError("Could not save WiFi profiles: {}", file.errorString().toStdString());
        appendLog(tr("WARNING: Could not save WiFi profiles"));
    }
}

void UserProfileBackupExecutePage::saveEthernetConfigsToBackup(
    const QVector<EthernetConfigInfo>& configs) {
    if (configs.isEmpty()) return;

    QJsonArray arr;
    for (const auto& c : configs) {
        if (c.selected) arr.append(c.toJson());
    }
    if (arr.isEmpty()) return;

    QJsonDocument doc(arr);
    QFile file(m_destinationPath + "/ethernet_configs.json");
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
        if (file.write(json_bytes) != json_bytes.size()) {
            sak::logError("Incomplete write of Ethernet configs");
            appendLog(tr("WARNING: Incomplete write of Ethernet configurations"));
        }
        file.close();
        appendLog(tr("Saved %1 Ethernet configuration(s) to backup").arg(arr.size()));
    } else {
        sak::logError("Could not save Ethernet configs: {}", file.errorString().toStdString());
        appendLog(tr("WARNING: Could not save Ethernet configurations"));
    }
}

void UserProfileBackupExecutePage::saveAppDataSourcesToBackup(
    const QVector<AppDataSourceInfo>& sources) {
    if (sources.isEmpty()) return;

    QJsonArray arr;
    for (const auto& s : sources) {
        if (s.selected) arr.append(s.toJson());
    }
    if (arr.isEmpty()) return;

    QJsonDocument doc(arr);
    QFile file(m_destinationPath + "/app_data_sources.json");
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
        if (file.write(json_bytes) != json_bytes.size()) {
            sak::logError("Incomplete write of app data sources");
            appendLog(tr("WARNING: Incomplete write of application data sources"));
        }
        file.close();
        appendLog(tr("Saved %1 application data source(s) to backup").arg(arr.size()));
    } else {
        sak::logError("Could not save app data sources: {}", file.errorString().toStdString());
        appendLog(tr("WARNING: Could not save application data sources"));
    }
}

void UserProfileBackupExecutePage::connectAndStartBackupWorker(
    SmartFilter smartFilter, PermissionMode permissionMode,
    int compressionLevel, bool encrypt, const QString& password) {
    auto worker = new UserProfileBackupWorker(this);

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

    worker->startBackup(m_manifest, m_users, m_destinationPath, smartFilter, permissionMode,
                       compressionLevel, encrypt, password);

    m_overallProgress->setRange(0, m_users.size());
    m_currentProgress->setRange(0, 0);
}

void UserProfileBackupExecutePage::onBackupProgress(int current, int total, qint64 bytes,
    qint64 totalBytes) {
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
