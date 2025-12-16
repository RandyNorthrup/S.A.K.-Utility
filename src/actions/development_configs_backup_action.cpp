// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/development_configs_backup_action.h"
#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>

namespace sak {

DevelopmentConfigsBackupAction::DevelopmentConfigsBackupAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

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
    for (const UserProfile& user : m_user_profiles) {
        QString ssh_dir = user.profile_path + "/.ssh";
        QDir dir(ssh_dir);
        
        if (dir.exists()) {
            for (const QFileInfo& file : dir.entryInfoList(QDir::Files)) {
                DevConfig cfg;
                cfg.name = ".ssh/" + file.fileName();
                cfg.path = file.absoluteFilePath();
                cfg.size = file.size();
                cfg.is_sensitive = true; // SSH keys are sensitive!
                
                m_configs.append(cfg);
                m_total_size += cfg.size;
                m_found_sensitive_data = true;
            }
        }
    }
}

void DevelopmentConfigsBackupAction::scanVSCodeSettings() {
    for (const UserProfile& user : m_user_profiles) {
        QString vscode_dir = user.profile_path + "/AppData/Roaming/Code/User";
        QDir dir(vscode_dir);
        
        if (dir.exists()) {
            QStringList files = {"settings.json", "keybindings.json", "snippets"};
            
            for (const QString& file : files) {
                QString path = dir.filePath(file);
                QFileInfo info(path);
                
                if (info.exists()) {
                    DevConfig cfg;
                    cfg.name = "VSCode/" + file;
                    cfg.path = path;
                    cfg.size = info.isDir() ? 0 : info.size();
                    cfg.is_sensitive = false;
                    
                    if (info.isDir()) {
                        QDirIterator it(path, QDir::Files, QDirIterator::Subdirectories);
                        while (it.hasNext()) {
                            it.next();
                            cfg.size += it.fileInfo().size();
                        }
                    }
                    
                    m_configs.append(cfg);
                    m_total_size += cfg.size;
                }
            }
        }
    }
}

void DevelopmentConfigsBackupAction::scanVisualStudioSettings() {
    for (const UserProfile& user : m_user_profiles) {
        QString vs_path = user.profile_path + "/AppData/Local/Microsoft/VisualStudio";
        QDir dir(vs_path);
        
        if (dir.exists()) {
            QDirIterator it(vs_path, QStringList() << "*.vssettings", 
                          QDir::Files, QDirIterator::Subdirectories);
            
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
}

void DevelopmentConfigsBackupAction::scanIntelliJSettings() {
    for (const UserProfile& user : m_user_profiles) {
        QString intellij_path = user.profile_path + "/AppData/Roaming/JetBrains";
        QDir dir(intellij_path);
        
        if (dir.exists()) {
            QDirIterator it(intellij_path, QDir::Dirs | QDir::NoDotAndDotDot);
            
            while (it.hasNext()) {
                it.next();
                QString config = it.filePath() + "/config";
                
                if (QDir(config).exists()) {
                    DevConfig cfg;
                    cfg.name = "IntelliJ/" + it.fileName();
                    cfg.path = config;
                    cfg.is_sensitive = false;
                    
                    qint64 size = 0;
                    QDirIterator files(config, QDir::Files, QDirIterator::Subdirectories);
                    while (files.hasNext()) {
                        files.next();
                        size += files.fileInfo().size();
                    }
                    
                    cfg.size = size;
                    m_configs.append(cfg);
                    m_total_size += size;
                }
            }
        }
    }
}

void DevelopmentConfigsBackupAction::scan() {
    setStatus(ActionStatus::Scanning);
    
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
            .arg(m_total_size / 1024);
        
        if (m_found_sensitive_data) {
            result.warning = "Includes SSH keys - ensure backup location is secure!";
        }
        
        setStatus(ActionStatus::Ready);
    } else {
        result.summary = "No development configs found";
        setStatus(ActionStatus::Idle);
    }
    
    Q_EMIT scanComplete(result);
}

void DevelopmentConfigsBackupAction::execute() {
    setStatus(ActionStatus::Running);
    
    QDir backup_dir(m_backup_location + "/DevConfigs");
    backup_dir.mkpath(".");
    
    int processed = 0;
    qint64 bytes_copied = 0;
    
    for (const DevConfig& cfg : m_configs) {
        QString dest = backup_dir.filePath(cfg.name);
        QDir().mkpath(QFileInfo(dest).absolutePath());
        
        QFileInfo src_info(cfg.path);
        
        if (src_info.isFile()) {
            if (QFile::copy(cfg.path, dest)) {
                processed++;
                bytes_copied += cfg.size;
            }
        } else if (src_info.isDir()) {
            QDirIterator it(cfg.path, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                QString rel = QDir(cfg.path).relativeFilePath(it.filePath());
                QString dest_file = dest + "/" + rel;
                
                QDir().mkpath(QFileInfo(dest_file).absolutePath());
                if (QFile::copy(it.filePath(), dest_file)) {
                    bytes_copied += it.fileInfo().size();
                }
            }
            processed++;
        }
        
        Q_EMIT executionProgress(QString("Backing up %1...").arg(cfg.name),
                             (processed * 100) / m_configs.count());
    }
    
    ExecutionResult result;
    result.files_processed = processed;
    result.bytes_processed = bytes_copied;
    result.message = QString("Backed up %1 dev config(s)").arg(processed);
    result.output_path = backup_dir.absolutePath();
    
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

} // namespace sak
