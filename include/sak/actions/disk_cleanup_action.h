// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <QStringList>

namespace sak {

/**
 * @brief Clean temporary files and system junk
 * 
 * Scans and removes:
 * - Windows temp folders
 * - User temp folders
 * - Browser caches
 * - Recycle bin
 * - Windows Update cleanup
 * - Thumbnail cache
 * 
 * Category: System Optimization
 */
class DiskCleanupAction : public QuickAction {
    Q_OBJECT

public:
    DiskCleanupAction();
    ~DiskCleanupAction() override = default;

    QString name() const override { return "Disk Cleanup"; }
    QString description() const override { return "Remove temporary files, caches, and system junk"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    struct CleanupTarget {
        QString path;
        QString description;
        qint64 size{0};
        int file_count{0};
        bool safe_to_delete{true};
    };

    /**
     * @brief Scan Windows temp directory
     */
    void scanWindowsTemp();

    /**
     * @brief Scan user temp directory
     */
    void scanUserTemp();

    /**
     * @brief Scan browser caches
     */
    void scanBrowserCaches();

    /**
     * @brief Scan recycle bin
     */
    void scanRecycleBin();

    /**
     * @brief Scan Windows Update cleanup candidates
     */
    void scanWindowsUpdate();

    /**
     * @brief Scan thumbnail cache
     */
    void scanThumbnailCache();

    /**
     * @brief Calculate directory size recursively
     * @param path Directory path
     * @param file_count Output: number of files
     * @return Total size in bytes
     */
    qint64 calculateDirectorySize(const QString& path, int& file_count);

    /**
     * @brief Delete directory contents recursively
     * @param path Directory path
     * @param deleted_count Output: number of deleted files
     * @return Bytes deleted
     */
    qint64 deleteDirectoryContents(const QString& path, int& deleted_count);

    /**
     * @brief Check if path is safe to delete
     * @param path Path to check
     * @return True if safe
     */
    bool isSafeToDelete(const QString& path) const;

    std::vector<CleanupTarget> m_targets;
    qint64 m_total_bytes{0};
    int m_total_files{0};
};

} // namespace sak
