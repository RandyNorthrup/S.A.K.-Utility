// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file smart_disk_analyzer.h
/// @brief SMART disk health analysis via bundled smartctl

#pragma once

#include "sak/diagnostic_types.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

namespace sak {

/// @brief Analyzes storage device SMART data using bundled smartctl.exe
///
/// Parses smartctl JSON output for both SATA and NVMe drives, performs
/// health assessment with configurable thresholds, and generates actionable
/// recommendations. Requires administrator privileges for raw disk access.
///
/// Usage:
/// @code
///   SmartDiskAnalyzer analyzer;
///   connect(&analyzer, &SmartDiskAnalyzer::analysisComplete, this, &Receiver::onSmartDone);
///   analyzer.analyzeAll();    // all drives
///   analyzer.analyzeDrive(0); // specific drive
/// @endcode
class SmartDiskAnalyzer : public QObject {
    Q_OBJECT

public:
    /// @brief Construct a SmartDiskAnalyzer
    /// @param parent Parent QObject for ownership
    explicit SmartDiskAnalyzer(QObject* parent = nullptr);
    ~SmartDiskAnalyzer() override = default;

    // Non-copyable, non-movable
    SmartDiskAnalyzer(const SmartDiskAnalyzer&) = delete;
    SmartDiskAnalyzer& operator=(const SmartDiskAnalyzer&) = delete;
    SmartDiskAnalyzer(SmartDiskAnalyzer&&) = delete;
    SmartDiskAnalyzer& operator=(SmartDiskAnalyzer&&) = delete;

    /// @brief Analyze SMART data for all detected drives
    void analyzeAll();

    /// @brief Analyze SMART data for a specific drive
    /// @param disk_number Windows disk number (e.g., 0 for PhysicalDrive0)
    void analyzeDrive(uint32_t disk_number);

    /// @brief Cancel an in-progress analysis
    void cancel();

    /// @brief Get the cached reports from the last analysis
    /// @return Vector of SMART reports
    [[nodiscard]] const QVector<SmartReport>& reports() const { return m_reports; }

    /// @brief Check if smartctl is available (bundled and accessible)
    /// @return true if smartctl.exe can be found and executed
    [[nodiscard]] bool isSmartctlAvailable() const;

Q_SIGNALS:
    /// @brief Emitted when analysis begins
    void analysisStarted();

    /// @brief Emitted when overall progress updates
    /// @param percent Completion percentage (0-100)
    /// @param message Status description
    void analysisProgress(int percent, const QString& message);

    /// @brief Emitted when a single drive analysis completes
    /// @param report The SMART report for the drive
    void driveAnalyzed(const sak::SmartReport& report);

    /// @brief Emitted when all requested drives are analyzed
    /// @param reports All reports from this analysis run
    void analysisComplete(const QVector<sak::SmartReport>& reports);

    /// @brief Emitted on error (non-fatal; analysis continues for other drives)
    /// @param message Description of the error
    void errorOccurred(const QString& message);

private:
    /// @brief Resolve the path to the bundled smartctl executable
    /// @return Absolute path to smartctl.exe, or empty string if not found
    [[nodiscard]] QString resolveSmartctlPath() const;

    /// @brief Run smartctl and capture JSON output for a specific drive
    /// @param disk_number Physical drive number
    /// @return Raw JSON output from smartctl, or empty QByteArray on failure
    [[nodiscard]] QByteArray runSmartctl(uint32_t disk_number);

    /// @brief Parse smartctl JSON output into a SmartReport
    /// @param json_data Raw JSON bytes
    /// @param disk_number The drive number being parsed
    /// @return Populated SmartReport
    [[nodiscard]] SmartReport parseSmartctlOutput(const QByteArray& json_data,
                                                   uint32_t disk_number);

    /// @brief Parse SATA SMART attributes from the JSON object
    /// @param ata_smart_obj JSON object containing ata_smart_attributes
    /// @param report Target report to populate
    void parseSataAttributes(const QJsonObject& ata_smart_obj, SmartReport& report);

    /// @brief Parse NVMe health log from the JSON object
    /// @param nvme_obj JSON object containing nvme_smart_health_information_log
    /// @param report Target report to populate
    void parseNvmeHealth(const QJsonObject& nvme_obj, SmartReport& report);

    /// @brief Assess overall health based on parsed data
    /// @param report Report to assess (modified in-place)
    void assessHealth(SmartReport& report);

    /// @brief Generate warnings and recommendations for a report
    /// @param report Report to enrich (modified in-place)
    void generateRecommendations(SmartReport& report);

    /// @brief Enumerate available physical drive numbers
    /// @return Sorted list of drive numbers present on the system
    [[nodiscard]] QVector<uint32_t> enumerateDrives();

    QVector<SmartReport> m_reports;
    std::atomic<bool> m_cancelled{false};
};

} // namespace sak
