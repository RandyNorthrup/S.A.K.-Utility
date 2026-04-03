// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file conversion_report_generator.h
/// @brief Generates HTML and CSV reports for OST conversion batches

#pragma once

#include "sak/email_types.h"
#include "sak/ost_converter_types.h"

#include <QList>
#include <QString>
#include <QVector>

class QTextStream;

namespace sak {

/// @brief Generates professional HTML/CSV conversion reports
class ConversionReportGenerator {
public:
    /// Generate HTML report for a conversion batch
    [[nodiscard]] static QString generateHtmlReport(const OstConversionBatchResult& batch,
                                                    const QString& output_directory);

    /// Generate CSV properties manifest for MSG exports
    [[nodiscard]] static QString generateCsvManifest(
        const QVector<PstItemDetail>& items,
        const QVector<QVector<MapiProperty>>& all_properties,
        const QString& output_directory);

private:
    [[nodiscard]] static QString buildReportHtml(const OstConversionBatchResult& batch);

    [[nodiscard]] static QString buildSummaryStatsHtml(const OstConversionBatchResult& batch,
                                                       qint64 duration_ms);

    [[nodiscard]] static QString buildFileResultsTableHtml(const OstConversionBatchResult& batch);

    [[nodiscard]] static QString buildErrorLogHtml(const OstConversionBatchResult& batch);

    [[nodiscard]] static int writeCsvDataRows(QTextStream& out,
                                              const QVector<PstItemDetail>& items,
                                              const QVector<QVector<MapiProperty>>& all_properties,
                                              const QList<QString>& sorted_names);

    [[nodiscard]] static QString formatDuration(qint64 ms);
    [[nodiscard]] static QString formatBytes(qint64 bytes);
};

}  // namespace sak
