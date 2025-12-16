// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

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

    bool stopWindowsUpdateService();
    bool startWindowsUpdateService();
    qint64 calculateDirectorySize(const QString& path, int& file_count);
};

} // namespace sak

