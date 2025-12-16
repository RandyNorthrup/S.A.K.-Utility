// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <QStringList>
#include <vector>

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

    QString m_backup_location;
    std::vector<QuickBooksFile> m_found_files;
    qint64 m_total_bytes{0};
};

} // namespace sak

