// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

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
};

} // namespace sak

