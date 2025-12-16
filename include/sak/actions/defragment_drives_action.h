// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <QProcess>

namespace sak {

/**
 * @brief Defragment Drives Action
 * 
 * Analyzes and defragments HDDs (skips SSDs automatically).
 * Uses Windows defrag.exe with /O optimization.
 */
class DefragmentDrivesAction : public QuickAction {
    Q_OBJECT

public:
    explicit DefragmentDrivesAction(QObject* parent = nullptr);

    QString name() const override { return "Defragment Drives"; }
    QString description() const override { return "Optimize HDDs (skips SSDs)"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    struct DriveInfo {
        QString letter;
        QString type; // HDD, SSD, Removable
        qint64 total_space;
        qint64 free_space;
        int fragmentation_percent;
        bool needs_defrag;
    };

    QVector<DriveInfo> m_drives;
    int m_hdd_count{0};

    bool isDriveSSD(const QString& drive_letter);
    int analyzeFragmentation(const QString& drive_letter);
};

} // namespace sak

