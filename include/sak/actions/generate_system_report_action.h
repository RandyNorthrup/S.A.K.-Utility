// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Generate System Report Action
 *
 * Generates comprehensive HTML system report using msinfo32 and PowerShell.
 */
class GenerateSystemReportAction : public QuickAction {
    Q_OBJECT

public:
    explicit GenerateSystemReportAction(const QString& output_location, QObject* parent = nullptr);

    QString name() const override { return "Generate System Report"; }
    QString description() const override { return "Create comprehensive system report"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    QString m_output_location;
    QString m_report_path;

    /// @brief Builds the report header with box-drawing frame and timestamp.
    /// @return Formatted report header string.
    QString buildReportHeader() const;

    /// @brief Gathers OS, hardware, CPU, memory, BIOS,
    /// network, and activation info via Get-ComputerInfo.
    /// @return Report section text; may contain timeout fallback text.
    QString gatherOsAndHardwareInfo();

    /// @brief Builds the PowerShell script for OS and computer system info sections.
    QString buildOsInfoScript() const;
    /// @brief Builds the PowerShell script for hardware, BIOS, network, and activation sections.
    QString buildHardwareInfoScript() const;

    /// @brief Gathers physical disk and SMART info via Get-PhysicalDisk.
    /// @return Report section text.
    QString gatherStorageInfo();

    /// @brief Gathers active network adapters and IP configuration.
    /// @return Report section text.
    QString gatherNetworkInfo();

    /// @brief Gathers Qt system info and mounted volume details.
    /// @return Report section text.
    QString gatherQtAndVolumeInfo() const;

    /// @brief Saves the assembled report to disk with timing footer.
    /// @return True on successful write.
    bool saveReport(const QString& report, const QString& filepath);

    /// @brief Save the report file and emit the final ExecutionResult
    void saveReportAndFinish(const QString& report, const QString& filepath,
        const QDateTime& start_time);
};

} // namespace sak

