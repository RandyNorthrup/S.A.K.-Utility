// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/style_constants.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_backup_worker.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QVBoxLayout>

namespace sak {

namespace {
constexpr int kCompressionFastMaxLevel = 3;
constexpr int kCompressionBalancedMaxLevel = 6;

// Write JSON to `path` atomically: QSaveFile stages a temp file and only renames
// it into place on commit(), so a short or interrupted write never leaves a
// truncated sidecar behind. `error` is set on failure.
bool writeJsonSidecarAtomic(const QString& path, const QJsonDocument& doc, QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        error = QStringLiteral("incomplete write");
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}
}  // namespace

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
    , m_destinationPath(destinationPath) {
    setTitle(tr("Execute Backup"));
    setSubTitle(tr("Backup in progress..."));

    setupUi();
}

void UserProfileBackupExecutePage::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* layout = new QVBoxLayout(this);

    // Status label
    m_statusLabel = new QLabel(tr("Ready to start backup"), this);
    m_statusLabel->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightSemibold, sak::ui::kColorTextHeading));
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
    connect(
        m_startButton, &QPushButton::clicked, this, &UserProfileBackupExecutePage::onStartBackup);
    layout->addWidget(m_startButton);

    layout->addStretch();

    Q_ASSERT(m_statusLabel);
}

void UserProfileBackupExecutePage::initializePage() {
    m_statusLabel->setText(tr("Ready to start backup. Click Start Backup to begin."));
    m_startButton->setEnabled(true);
}

bool UserProfileBackupExecutePage::isComplete() const {
    return m_completed;
}

void UserProfileBackupExecutePage::onStartBackup() {
    Q_ASSERT(m_startButton);
    Q_ASSERT(m_statusLabel);
    if (m_started) {
        return;
    }

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
    PermissionMode permissionMode = wiz->getPermissionMode();

    int compressionLevel = wiz->getCompressionLevel();
    bool encrypt = wiz->isEncryptionEnabled();
    QString password = wiz->getEncryptionPassword();

    appendLog(tr("Compression: %1")
                  .arg(compressionLevel == 0                              ? tr("None")
                       : compressionLevel <= kCompressionFastMaxLevel     ? tr("Fast")
                       : compressionLevel <= kCompressionBalancedMaxLevel ? tr("Balanced")
                                                                          : tr("Maximum")));

    if (encrypt) {
        appendLog(tr("Encryption: Enabled (AES-256)"));
    }

    // The sidecars are written into the destination directory, so it must exist
    // and be writable before anything is saved (previously they were written
    // first and silently failed when the directory did not yet exist).
    if (!ensureDestinationDirectory()) {
        failBackupPreflight(
            tr("Destination directory could not be created: %1").arg(m_destinationPath));
        return;
    }

    // Save every metadata sidecar atomically; if any fails, do not start the
    // backup -- a missing WiFi/Ethernet/app sidecar must not be reported as a
    // successful backup.
    if (!saveAllSidecars(wiz)) {
        failBackupPreflight(
            tr("Failed to write one or more backup metadata files; backup not started."));
        return;
    }

    connectAndStartBackupWorker(smartFilter, permissionMode, compressionLevel, encrypt, password);
}

bool UserProfileBackupExecutePage::saveAllSidecars(UserProfileBackupWizard* wiz) {
    // Every sidecar is attempted (left operand runs first) so all failures are
    // logged, but any single failure makes the whole set fail.
    bool ok = saveInstalledAppsToBackup(wiz->installedApps());
    ok = saveWifiProfilesToBackup(wiz->wifiProfiles()) && ok;
    ok = saveEthernetConfigsToBackup(wiz->ethernetConfigs()) && ok;
    ok = saveAppDataSourcesToBackup(wiz->appDataSources()) && ok;
    return ok;
}

bool UserProfileBackupExecutePage::ensureDestinationDirectory() {
    QDir dir(m_destinationPath);
    if (dir.exists()) {
        return true;
    }
    if (!QDir().mkpath(m_destinationPath)) {
        sak::logError("Could not create backup destination: {}", m_destinationPath.toStdString());
        return false;
    }
    return true;
}

void UserProfileBackupExecutePage::failBackupPreflight(const QString& message) {
    sak::logError("Backup preflight failed: {}", message.toStdString());
    appendLog(tr("ERROR: %1").arg(message));
    m_statusLabel->setText(tr("Backup failed to start"));
    // Allow the user to fix the problem and retry; do not mark the page complete.
    m_started = false;
    m_startButton->setEnabled(true);
}

bool UserProfileBackupExecutePage::saveInstalledAppsToBackup(
    const QVector<InstalledAppInfo>& installedApps) {
    if (installedApps.isEmpty()) {
        return true;
    }

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

    QString error;
    if (!writeJsonSidecarAtomic(
            m_destinationPath + "/installed_apps.json", QJsonDocument(appsArray), error)) {
        sak::logError("Could not save installed apps list: {}", error.toStdString());
        appendLog(tr("ERROR: Could not save installed applications list: %1").arg(error));
        return false;
    }
    appendLog(tr("Saved %1 installed application(s) to backup").arg(installedApps.size()));
    return true;
}

bool UserProfileBackupExecutePage::saveWifiProfilesToBackup(
    const QVector<WifiProfileInfo>& profiles) {
    if (profiles.isEmpty()) {
        return true;
    }

    QJsonArray arr;
    for (const auto& p : profiles) {
        if (p.selected) {
            arr.append(p.toJson());
        }
    }
    if (arr.isEmpty()) {
        return true;
    }

    QString error;
    if (!writeJsonSidecarAtomic(
            m_destinationPath + "/wifi_profiles.json", QJsonDocument(arr), error)) {
        sak::logError("Could not save WiFi profiles: {}", error.toStdString());
        appendLog(tr("ERROR: Could not save WiFi profiles: %1").arg(error));
        return false;
    }
    appendLog(tr("Saved %1 WiFi profile(s) to backup").arg(arr.size()));
    return true;
}

bool UserProfileBackupExecutePage::saveEthernetConfigsToBackup(
    const QVector<EthernetConfigInfo>& configs) {
    if (configs.isEmpty()) {
        return true;
    }

    QJsonArray arr;
    for (const auto& c : configs) {
        if (c.selected) {
            arr.append(c.toJson());
        }
    }
    if (arr.isEmpty()) {
        return true;
    }

    QString error;
    if (!writeJsonSidecarAtomic(
            m_destinationPath + "/ethernet_configs.json", QJsonDocument(arr), error)) {
        sak::logError("Could not save Ethernet configs: {}", error.toStdString());
        appendLog(tr("ERROR: Could not save Ethernet configurations: %1").arg(error));
        return false;
    }
    appendLog(tr("Saved %1 Ethernet configuration(s) to backup").arg(arr.size()));
    return true;
}

bool UserProfileBackupExecutePage::saveAppDataSourcesToBackup(
    const QVector<AppDataSourceInfo>& sources) {
    if (sources.isEmpty()) {
        return true;
    }

    QJsonArray arr;
    for (const auto& s : sources) {
        if (s.selected) {
            arr.append(s.toJson());
        }
    }
    if (arr.isEmpty()) {
        return true;
    }

    QString error;
    if (!writeJsonSidecarAtomic(
            m_destinationPath + "/app_data_sources.json", QJsonDocument(arr), error)) {
        sak::logError("Could not save app data sources: {}", error.toStdString());
        appendLog(tr("ERROR: Could not save application data sources: %1").arg(error));
        return false;
    }
    appendLog(tr("Saved %1 application data source(s) to backup").arg(arr.size()));
    return true;
}

void UserProfileBackupExecutePage::connectAndStartBackupWorker(SmartFilter smartFilter,
                                                               PermissionMode permissionMode,
                                                               int compressionLevel,
                                                               bool encrypt,
                                                               const QString& password) {
    auto worker = new UserProfileBackupWorker(this);

    connect(worker,
            &UserProfileBackupWorker::overallProgress,
            this,
            &UserProfileBackupExecutePage::onBackupProgress);
    connect(worker,
            &UserProfileBackupWorker::logMessage,
            this,
            [this](const QString& message, bool isWarning) {
                appendLog(isWarning ? QString("[WARNING] %1").arg(message) : message);
            });
    connect(worker,
            &UserProfileBackupWorker::statusUpdate,
            this,
            [this](const QString& username, const QString& operation) {
                m_statusLabel->setText(tr("Backing up %1: %2").arg(username, operation));
            });
    connect(worker,
            &UserProfileBackupWorker::backupComplete,
            this,
            [this, worker](bool success, const QString& message, const BackupManifest&) {
                onBackupComplete(success, message);
                worker->deleteLater();
            });

    const UserProfileBackupWorker::BackupOptions backup_options{
        permissionMode, compressionLevel, encrypt, password};
    worker->startBackup(m_manifest, m_users, m_destinationPath, smartFilter, backup_options);

    m_overallProgress->setRange(0, m_users.size());
    m_currentProgress->setRange(0, 0);
}

void UserProfileBackupExecutePage::onBackupProgress(int current,
                                                    int total,
                                                    qint64 bytes,
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

}  // namespace sak
