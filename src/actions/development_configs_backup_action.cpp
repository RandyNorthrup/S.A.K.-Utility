// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file development_configs_backup_action.cpp
/// @brief Implements backup of development environment configuration files

#include "sak/actions/development_configs_backup_action.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>

namespace sak {

DevelopmentConfigsBackupAction::DevelopmentConfigsBackupAction(const QString& backup_location,
                                                               QObject* parent)
    : QuickAction(parent), m_backup_location(backup_location) {}

void DevelopmentConfigsBackupAction::scanGitConfig() {
    for (const UserProfile& user : m_user_profiles) {
        QString gitconfig = user.profile_path + "/.gitconfig";
        if (QFile::exists(gitconfig)) {
            DevConfig cfg;
            cfg.name = ".gitconfig";
            cfg.path = gitconfig;
            cfg.size = QFileInfo(gitconfig).size();
            cfg.is_sensitive = false;

            m_configs.append(cfg);
            m_total_size += cfg.size;
        }
    }
}

void DevelopmentConfigsBackupAction::scanSSHKeys() {
    Q_ASSERT(!m_configs.empty());
    Q_ASSERT(!m_configs.isEmpty());
    for (const UserProfile& user : m_user_profiles) {
        QString ssh_dir = user.profile_path + "/.ssh";
        QDir dir(ssh_dir);
        if (!dir.exists()) {
            continue;
        }

        for (const QFileInfo& file : dir.entryInfoList(QDir::Files)) {
            DevConfig cfg;
            cfg.name = ".ssh/" + file.fileName();
            cfg.path = file.absoluteFilePath();
            cfg.size = file.size();
            cfg.is_sensitive = true;  // SSH keys are sensitive!

            m_configs.append(cfg);
            m_total_size += cfg.size;
            m_found_sensitive_data = true;
        }
    }
}

void DevelopmentConfigsBackupAction::scanVSCodeSettings() {
    Q_ASSERT(!m_configs.empty());
    Q_ASSERT(!m_configs.isEmpty());
    for (const UserProfile& user : m_user_profiles) {
        QString vscode_dir = user.profile_path + "/AppData/Roaming/Code/User";
        QDir dir(vscode_dir);
        if (!dir.exists()) {
            continue;
        }

        QStringList files = {"settings.json", "keybindings.json", "snippets"};
        for (const QString& file : files) {
            QString path = dir.filePath(file);
            QFileInfo info(path);
            if (!info.exists()) {
                continue;
            }

            DevConfig cfg;
            cfg.name = "VSCode/" + file;
            cfg.path = path;
            cfg.size = info.isDir() ? calculateDirSize(path) : info.size();
            cfg.is_sensitive = false;

            m_configs.append(cfg);
            m_total_size += cfg.size;
        }
    }
}

void DevelopmentConfigsBackupAction::scanVisualStudioSettings() {
    Q_ASSERT(!m_configs.empty());
    Q_ASSERT(!m_configs.isEmpty());
    for (const UserProfile& user : m_user_profiles) {
        QString vs_path = user.profile_path + "/AppData/Local/Microsoft/VisualStudio";
        QDir dir(vs_path);
        if (!dir.exists()) {
            continue;
        }

        QDirIterator it(
            vs_path, QStringList() << "*.vssettings", QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            DevConfig cfg;
            cfg.name = "VisualStudio/" + it.fileName();
            cfg.path = it.filePath();
            cfg.size = it.fileInfo().size();
            cfg.is_sensitive = false;

            m_configs.append(cfg);
            m_total_size += cfg.size;
        }
    }
}

void DevelopmentConfigsBackupAction::scanIntelliJSettings() {
    Q_ASSERT(!m_configs.empty());
    Q_ASSERT(!m_configs.isEmpty());
    for (const UserProfile& user : m_user_profiles) {
        QString intellij_path = user.profile_path + "/AppData/Roaming/JetBrains";
        QDir dir(intellij_path);
        if (!dir.exists()) {
            continue;
        }

        QDirIterator it(intellij_path, QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) {
            it.next();
            QString config = it.filePath() + "/config";
            if (!QDir(config).exists()) {
                continue;
            }

            DevConfig cfg;
            cfg.name = "IntelliJ/" + it.fileName();
            cfg.path = config;
            cfg.is_sensitive = false;
            cfg.size = calculateDirSize(config);

            m_configs.append(cfg);
            m_total_size += cfg.size;
        }
    }
}

void DevelopmentConfigsBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);

    WindowsUserScanner scanner;
    m_user_profiles = scanner.scanUsers();

    m_configs.clear();
    m_total_size = 0;
    m_found_sensitive_data = false;

    scanGitConfig();
    scanSSHKeys();
    scanVSCodeSettings();
    scanVisualStudioSettings();
    scanIntelliJSettings();

    ScanResult result;
    result.applicable = (m_configs.count() > 0);
    result.bytes_affected = m_total_size;
    result.files_count = m_configs.count();
    result.estimated_duration_ms = 5000;

    if (m_configs.count() > 0) {
        result.summary = QString("Found %1 dev config(s) - %2 KB")
                             .arg(m_configs.count())
                             .arg(m_total_size / sak::kBytesPerKB);

        if (m_found_sensitive_data) {
            result.warning = "Includes SSH keys - ensure backup location is secure!";
        }

    } else {
        result.summary = "No development configs found";
    }

    Q_ASSERT(!result.summary.isEmpty());

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void DevelopmentConfigsBackupAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("Development configs backup cancelled");
        return;
    }
    setStatus(ActionStatus::Running);
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

    QDir backup_dir(m_backup_location + "/DevConfigs");
    if (!backup_dir.mkpath(".")) {
        sak::logWarning("Failed to create dev configs backup directory: {}",
                        backup_dir.absolutePath().toStdString());
    }

    int processed = 0;
    qint64 bytes_copied = 0;

    for (const DevConfig& cfg : m_configs) {
        if (isCancelled()) {
            emitCancelledResult("Development config backup cancelled", start_time);
            return;
        }

        const QString dest = backup_dir.filePath(cfg.name);
        QFileInfo src_info(cfg.path);

        if (src_info.isFile()) {
            if (!QDir().mkpath(QFileInfo(dest).absolutePath())) {
                sak::logWarning("Failed to create directory for dev config file");
            }
            bool ok = QFile::copy(cfg.path, dest);
            processed += ok ? 1 : 0;
            bytes_copied += ok ? cfg.size : 0;
        } else if (src_info.isDir()) {
            bytes_copied += copyDirectoryContents(cfg.path, dest);
            processed++;
        }

        Q_EMIT executionProgress(QString("Backing up %1...").arg(cfg.name),
                                 (processed * 100) / m_configs.count());
    }

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.success = processed > 0;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    result.files_processed = processed;
    result.bytes_processed = bytes_copied;
    result.message = processed > 0 ? QString("Backed up %1 dev config(s)").arg(processed)
                                   : "No development configs were backed up";
    result.output_path = backup_dir.absolutePath();

    finishWithResult(result, processed > 0 ? ActionStatus::Success : ActionStatus::Failed);
}

qint64 DevelopmentConfigsBackupAction::calculateDirSize(const QString& path) const {
    qint64 size = 0;
    QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        size += it.fileInfo().size();
    }
    return size;
}

qint64 DevelopmentConfigsBackupAction::copyDirectoryContents(const QString& src_path,
                                                             const QString& dest_path) {
    qint64 bytes_copied = 0;
    QDirIterator it(src_path, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(src_path).relativeFilePath(it.filePath());
        const QString dest_file = dest_path + "/" + rel;
        if (!QDir().mkpath(QFileInfo(dest_file).absolutePath())) {
            sak::logWarning("Failed to create directory for dev config file");
        }
        if (QFile::copy(it.filePath(), dest_file)) {
            bytes_copied += it.fileInfo().size();
        }
    }
    return bytes_copied;
}

}  // namespace sak
