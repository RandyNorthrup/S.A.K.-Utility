// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <QStringList>

#include <vector>

class QDir;

namespace sak {

/**
 * @brief Backup QuickBooks company files
 *
 * Scans for QuickBooks data files:
 * - .QBW (company files)
 * - .QBB (backup files)
 * - .TLG (transaction logs)
 * - .ND (network data files)
 *
 * Searches common locations:
 * - C:\Users\Public\Documents\Intuit\QuickBooks
 * - Documents\QuickBooks
 * - Network shares
 *
 * Category: Quick Backups
 */
class QuickBooksBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit QuickBooksBackupAction(const QString& backup_location);
    ~QuickBooksBackupAction() override = default;

    QString name() const override { return "QuickBooks Backup"; }
    QString description() const override { return "Backup QuickBooks company files and data"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

    /**
     * @brief Set backup destination
     * @param location Backup folder path
     */
    void setBackupLocation(const QString& location) { m_backup_location = location; }

private:
    /// @brief Represents a discovered QuickBooks data file (QBW, QBB, TLG, or ND)
    struct QuickBooksFile {
        QString path;
        QString filename;
        QString type;
        qint64 size{0};
        QDateTime modified;
        bool is_open{false};
    };

    /**
     * @brief Scan common QuickBooks locations
     */
    void scanCommonLocations();

    /**
     * @brief Scan specific directory for QuickBooks files
     * @param dir_path Directory to scan
     */
    void scanDirectory(const QString& dir_path);

    /**
     * @brief Check if file is currently open
     * @param file_path File to check
     * @return True if file is open
     */
    bool isFileOpen(const QString& file_path) const;

    /**
     * @brief Copy file with progress
     * @param source Source file
     * @param destination Destination path
     * @return True if successful
     */
    bool copyFileWithProgress(const QString& source, const QString& destination);

    /**
     * @brief Get file type description
     * @param extension File extension
     * @return Human-readable type
     */
    QString getFileTypeDescription(const QString& extension) const;

    /// @brief Check if QuickBooks process is currently running
    bool isQuickBooksRunning();

    /// @brief Copy discovered QuickBooks files to backup directory
    void executeCopyFiles(const QDir& backup_dir,
                          const QDateTime& start_time,
                          int& files_copied,
                          int& files_skipped_open,
                          qint64& bytes_copied,
                          QStringList& copied_files);

    /// @brief Build and emit the final execution result
    void executeBuildResult(const QDateTime& start_time,
                            const QDir& backup_dir,
                            int files_copied,
                            int files_skipped_open,
                            qint64 bytes_copied,
                            const QStringList& copied_files);

    QString m_backup_location;
    std::vector<QuickBooksFile> m_found_files;
    qint64 m_total_bytes{0};
};

}  // namespace sak
