// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file diagnostic_report_generator.h
/// @brief Diagnostic report generation in HTML, JSON, and CSV formats

#pragma once

#include "sak/diagnostic_types.h"

#include <QObject>
#include <QString>

namespace sak {

/// @brief Generates professional diagnostic reports from collected data
///
/// Produces reports in three formats:
/// - **HTML**: Styled, print-ready report with color-coded health status
/// - **JSON**: Machine-readable structured data
/// - **CSV**: Spreadsheet-compatible tabular export
///
/// Includes a recommendations engine that produces actionable advice
/// based on all diagnostic findings.
///
/// Usage:
/// @code
///   DiagnosticReportGenerator gen;
///   gen.setReportData(data);
///   gen.generateHtml("C:/Reports/diag_report.html");
///   gen.generateJson("C:/Reports/diag_report.json");
///   gen.generateCsv("C:/Reports/diag_report.csv");
/// @endcode
class DiagnosticReportGenerator : public QObject {
    Q_OBJECT

public:
    /// @brief Construct a DiagnosticReportGenerator
    /// @param parent Parent QObject
    explicit DiagnosticReportGenerator(QObject* parent = nullptr);
    ~DiagnosticReportGenerator() override = default;

    // Non-copyable, non-movable
    DiagnosticReportGenerator(const DiagnosticReportGenerator&) = delete;
    DiagnosticReportGenerator& operator=(const DiagnosticReportGenerator&) = delete;
    DiagnosticReportGenerator(DiagnosticReportGenerator&&) = delete;
    DiagnosticReportGenerator& operator=(DiagnosticReportGenerator&&) = delete;

    /// @brief Set the report data to generate from
    /// @param data Aggregated diagnostic data
    void setReportData(const DiagnosticReportData& data) { m_data = data; }

    /// @brief Generate an HTML report
    /// @param output_path File path for the HTML output
    /// @return true on success
    [[nodiscard]] bool generateHtml(const QString& output_path);

    /// @brief Generate a JSON report
    /// @param output_path File path for the JSON output
    /// @return true on success
    [[nodiscard]] bool generateJson(const QString& output_path);

    /// @brief Generate a CSV report
    /// @param output_path File path for the CSV output
    /// @return true on success
    [[nodiscard]] bool generateCsv(const QString& output_path);

    /// @brief Get the full HTML content as a string (for preview)
    /// @return HTML content
    [[nodiscard]] QString renderHtml() const;

Q_SIGNALS:
    /// @brief Emitted when report generation completes
    /// @param format "HTML" / "JSON" / "CSV"
    /// @param path Output file path
    void reportGenerated(const QString& format, const QString& path);

    /// @brief Emitted on generation error
    /// @param message Error description
    void errorOccurred(const QString& message);

private:
    /// @brief Build the HTML header with embedded CSS
    [[nodiscard]] QString buildHtmlHeader() const;

    /// @brief Build the hardware inventory HTML section
    [[nodiscard]] QString buildHardwareSection() const;

    /// @brief Build the SMART health HTML section
    [[nodiscard]] QString buildSmartSection() const;

    /// @brief Build the benchmark results HTML section
    [[nodiscard]] QString buildBenchmarkSection() const;

    /// @brief Build the stress test results HTML section
    [[nodiscard]] QString buildStressTestSection() const;

    /// @brief Build the recommendations HTML section
    [[nodiscard]] QString buildRecommendationsSection() const;

    /// @brief Map a SmartHealthStatus to a CSS class name
    [[nodiscard]] static QString healthStatusCssClass(SmartHealthStatus status);

    /// @brief Map a SmartHealthStatus to a display string
    [[nodiscard]] static QString healthStatusText(SmartHealthStatus status);

    /// @brief Format bytes as a human-readable string (e.g., "16.0 GB")
    [[nodiscard]] static QString formatBytes(uint64_t bytes);

    DiagnosticReportData m_data;
};

} // namespace sak
