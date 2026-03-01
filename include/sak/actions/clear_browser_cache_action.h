// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <vector>

namespace sak {

/**
 * @brief Clear browser caches
 * 
 * ENTERPRISE FEATURES:
 * - Supports 6 major browsers: Chrome, Edge, Firefox, Brave, Opera, Vivaldi
 * - Get-Process verification before clearing
 * - Before/after size calculation and reporting
 * - Handles Code Cache + regular Cache for Chromium browsers
 * - Firefox multi-profile support
 * - Detailed per-browser clearing results
 * - Structured output with formatted reporting
 * 
 * PROCESS MANAGEMENT:
 * - Detects running browser processes
 * - Skips browsers currently in use
 * - Provides clear guidance on blocked browsers
 * 
 * SIZE TRACKING:
 * - Calculates cache size before clearing
 * - Reports freed space per browser
 * - Total cleared space summary
 * - Smart size formatting (GB/MB/KB/bytes)
 * 
 * Category: System Optimization
 */
class ClearBrowserCacheAction : public QuickAction {
    Q_OBJECT

public:
    ClearBrowserCacheAction();
    ~ClearBrowserCacheAction() override = default;

    QString name() const override { return "Clear Browser Cache"; }
    QString description() const override { return "Clear cache from Chrome, Edge, Firefox, Brave, Opera, and Vivaldi"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    /// @brief Parsed output from the browser cache clearing PowerShell script.
    struct BrowserCacheResult {
        int cleared_count{0};
        int blocked_count{0};
        qint64 size_before{0};
        qint64 size_cleared{0};
        QStringList cleared_browsers;
        QStringList blocked_browsers;
        QStringList details;
    };

    /// @brief Builds the PowerShell script for enterprise browser cache clearing.
    /// @return Complete PowerShell script string.
    QString buildCacheClearingScript() const;

    /// @brief PowerShell helper function definitions and browser config array.
    QString buildScriptPreamble() const;
    /// @brief PowerShell foreach loop clearing Chromium-based browser caches.
    QString buildScriptChromiumLoop() const;
    /// @brief PowerShell Firefox cache clearing and output section.
    QString buildScriptFirefoxAndOutput() const;

    /// @brief Parses structured output from the cache clearing script.
    /// @return Populated BrowserCacheResult.
    BrowserCacheResult parseCacheOutput(const QString& output) const;

    /// @brief Scan all browser cache locations and accumulate totals.
    void scanAllBrowserCaches(qint64& total_bytes, qint64& total_files, int& locations);

    /// @brief Builds the box-drawing success log.
    /// @return Formatted success log string.
    QString buildSuccessLog(const BrowserCacheResult& parsed, const QString& stderr_output, qint64 duration_ms) const;

    /// @brief Builds the box-drawing failure/blocked log.
    /// @return Formatted failure log string.
    QString buildFailureLog(const BrowserCacheResult& parsed) const;
};

} // namespace sak

