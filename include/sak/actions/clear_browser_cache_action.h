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
};

} // namespace sak

