// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QProcess>
#include <QString>

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
    /// @brief Information about a drive including type, capacity, and fragmentation level
    struct DriveInfo {
        QString letter;
        QString type;  // HDD, SSD, Removable
        qint64 total_space;
        qint64 free_space;
        int fragmentation_percent;
        bool needs_defrag;
    };

    QVector<DriveInfo> m_drives;
    int m_hdd_count{0};

    /// @brief Parsed optimization result counters
    struct OptimizationSummary {
        int total_drives{0};
        QStringList drive_types;
        int optimized{0};
        int total_optimized{0};
        int total_skipped{0};
    };

    static QString executeEnumerateVolumes();
    void executeDefrag(const QString& ps_script, const QDateTime& start_time);
    void executeBuildReport(const QString& accumulated_output,
                            const QString& std_err,
                            const QDateTime& start_time);

    /// @brief Parse OPTIMIZING/DRIVE_TYPE/SUCCESS/TOTAL tags from PowerShell output
    OptimizationSummary parseOptimizationOutput(const QString& output) const;

    bool isDriveSSD(const QString& drive_letter);
    int analyzeFragmentation(const QString& drive_letter);
};

}  // namespace sak
