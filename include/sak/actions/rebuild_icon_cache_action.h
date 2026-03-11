// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>

namespace sak {

/**
 * @brief Rebuild Icon Cache Action
 *
 * Deletes and rebuilds Windows icon cache to fix missing/corrupted icons.
 */
class RebuildIconCacheAction : public QuickAction {
    Q_OBJECT

public:
    explicit RebuildIconCacheAction(QObject* parent = nullptr);

    QString name() const override { return "Rebuild Icon Cache"; }
    QString description() const override { return "Fix missing or corrupted icons"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    /// @brief Information about a Windows icon cache file on disk
    struct CacheFileInfo {
        QString file_name;
        qint64 size_bytes;
        bool exists;
    };

    QVector<CacheFileInfo> enumerateCacheFiles();
    bool stopExplorer();
    int deleteCacheFiles(const QVector<CacheFileInfo>& files);
    bool startExplorer();
    bool refreshIconCache();

    /// @brief Build the header portion of the icon cache report
    QString buildIconCacheReportHeader(const QVector<CacheFileInfo>& cache_files,
                                       qint64 total_size) const;
    /// @brief Aggregated icon cache rebuild result data
    struct IconCacheReport {
        int deleted_count = 0;
        qint64 total_size = 0;
        bool explorer_stopped = false;
        bool explorer_started = false;
    };

    /// @brief Build the execution result and finish the action
    void buildAndFinishIconCacheResult(const IconCacheReport& cache_report,
                                       const QString& report,
                                       qint64 duration_ms);
};

}  // namespace sak
