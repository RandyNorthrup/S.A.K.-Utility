// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>

namespace sak {

/**
 * @brief Clear Windows Update Cache Action
 *
 * Stops Windows Update service, clears SoftwareDistribution folder,
 * and restarts the service to free up disk space.
 */
class ClearWindowsUpdateCacheAction : public QuickAction {
    Q_OBJECT

public:
    explicit ClearWindowsUpdateCacheAction(QObject* parent = nullptr);

    QString name() const override { return "Clear Windows Update Cache"; }
    QString description() const override { return "Free space from old Windows Update files"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    qint64 m_cache_size{0};
    int m_file_count{0};

    /// @brief Build the enterprise PowerShell script for cache cleanup.
    /// @return Complete PS script string.
    QString buildCacheCleanupScript() const;
    QString buildServiceStopScript() const;
    QString buildCachePurgeScript() const;
    QString buildServiceStartScript() const;

    /// Results parsed from the cache cleanup PS script output.
    struct CacheCleanupResult {
        qint64 total_before{0};
        qint64 total_cleared{0};
        int paths_cleared{0};
        int services_stopped{0};
        int services_started{0};
        QStringList service_details;
        QStringList path_details;
        QStringList errors;
    };

    /// @brief Parse structured output from the cache cleanup script.
    /// @param output Stdout from the PS script.
    /// @param std_err Stderr from the PS script.
    /// @return Parsed CacheCleanupResult.
    CacheCleanupResult parseCacheCleanupOutput(const QString& output, const QString& std_err) const;
    void parseCacheCleanupLine(const QString& trimmed, CacheCleanupResult& parsed) const;

    /// @brief Build the success log with box-drawing formatting.
    /// @return Formatted log string.
    QString buildSuccessLog(const CacheCleanupResult& parsed, qint64 duration_ms) const;

    /// @brief Build the failure log with box-drawing formatting.
    /// @return Formatted log string.
    QString buildFailureLog(const CacheCleanupResult& parsed) const;

    bool stopWindowsUpdateService();
    bool startWindowsUpdateService();
    qint64 calculateDirectorySize(const QString& path, int& file_count);
};

}  // namespace sak
