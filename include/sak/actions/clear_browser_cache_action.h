// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"

#include <QString>
#include <vector>

namespace sak {

/**
 * @brief Clear browser caches
 * 
 * Supports:
 * - Google Chrome
 * - Mozilla Firefox
 * - Microsoft Edge
 * - Internet Explorer
 * 
 * Clears:
 * - Cache files
 * - Cookies (optional)
 * - History (optional)
 * 
 * Category: System Optimization
 */
class ClearBrowserCacheAction : public QuickAction {
    Q_OBJECT

public:
    ClearBrowserCacheAction();
    ~ClearBrowserCacheAction() override = default;

    QString name() const override { return "Clear Browser Cache"; }
    QString description() const override { return "Clear cache from Chrome, Firefox, Edge, and IE"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    enum class BrowserType {
        Chrome,
        Firefox,
        Edge,
        InternetExplorer
    };

    struct BrowserCache {
        BrowserType browser;
        QString browser_name;
        QString cache_path;
        qint64 size{0};
        int file_count{0};
        bool is_running{false};
    };

    /**
     * @brief Detect installed browsers
     */
    void detectBrowsers();

    /**
     * @brief Scan Chrome cache
     */
    void scanChrome();

    /**
     * @brief Scan Firefox cache
     */
    void scanFirefox();

    /**
     * @brief Scan Edge cache
     */
    void scanEdge();

    /**
     * @brief Scan Internet Explorer cache
     */
    void scanInternetExplorer();

    /**
     * @brief Check if browser is running
     * @param process_name Process name (e.g., "chrome.exe")
     * @return True if running
     */
    bool isBrowserRunning(const QString& process_name) const;

    /**
     * @brief Calculate cache size
     * @param cache_path Cache directory
     * @param file_count Output: number of files
     * @return Total size in bytes
     */
    qint64 calculateCacheSize(const QString& cache_path, int& file_count);

    /**
     * @brief Delete cache directory
     * @param cache_path Cache directory
     * @return Bytes deleted
     */
    qint64 deleteCacheDirectory(const QString& cache_path);

    std::vector<BrowserCache> m_caches;
    qint64 m_total_bytes{0};
    int m_total_files{0};
};

} // namespace sak
