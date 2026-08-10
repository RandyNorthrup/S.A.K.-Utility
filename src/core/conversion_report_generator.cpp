// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file conversion_report_generator.cpp
/// @brief HTML and CSV report generation for OST conversion

#include "sak/conversion_report_generator.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/report_style_constants.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>

#include <limits>

namespace sak {

namespace {
constexpr int kSecondsPerHour = 3600;
constexpr int kSha256PreviewChars = 16;
constexpr qsizetype kReportHtmlReserveChars = 8192;
constexpr int kGigabyteDisplayPrecision = 2;
}  // namespace

// ======================================================================
// Public API
// ======================================================================

QString ConversionReportGenerator::generateHtmlReport(const OstConversionBatchResult& batch,
                                                      const QString& output_directory) {
    if (output_directory.trimmed().isEmpty()) {
        logError("ConversionReport: refusing to write report to a blank output directory");
        return {};
    }
    QString html = buildReportHtml(batch);
    QString report_path = output_directory + QStringLiteral("/conversion_report.html");

    QDir dir(output_directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        logError("ConversionReport: failed to create output directory {}",
                 output_directory.toStdString());
        return {};
    }

    // QSaveFile writes to a temporary sibling and atomically renames on commit(), so a
    // short or failed write can never truncate a previously good report.
    QSaveFile file(report_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("ConversionReport: failed to open report {} for writing: {}",
                 report_path.toStdString(),
                 file.errorString().toStdString());
        return {};
    }

    {
        QTextStream out(&file);
        out << html;
        out.flush();
        if (out.status() != QTextStream::Ok) {
            logError("ConversionReport: stream error writing report {}", report_path.toStdString());
            file.cancelWriting();
            return {};
        }
    }

    if (!file.commit()) {
        logError("ConversionReport: failed to finalize report {}: {}",
                 report_path.toStdString(),
                 file.errorString().toStdString());
        return {};
    }

    logInfo("ConversionReport: report saved to {}", report_path.toStdString());
    return report_path;
}

QString ConversionReportGenerator::csvSafeCell(const QString& value) {
    QString cell = value;

    // Formula-injection guard: email subjects/senders/message-ids are attacker-controlled. A cell
    // a spreadsheet would evaluate as a formula (leading = + - @, or a leading tab/CR that some
    // parsers strip to reveal one) gets a single-quote prefix so Excel/Calc treat it as text.
    static const QString kFormulaLeads = QStringLiteral("=+-@\t\r\n");
    if (!cell.isEmpty() && kFormulaLeads.contains(cell.at(0))) {
        cell.prepend(QLatin1Char('\''));
    }

    // RFC 4180 quoting: wrap and double internal quotes if the cell contains a comma, quote,
    // newline, or carriage return.
    static const QString kMustQuote = QStringLiteral(",\"\n\r");
    const bool needs_quote =
        std::any_of(cell.cbegin(), cell.cend(), [](QChar c) { return kMustQuote.contains(c); });
    if (needs_quote) {
        cell.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QStringLiteral("\"%1\"").arg(cell);
    }
    return cell;
}

int ConversionReportGenerator::writeCsvDataRows(
    QTextStream& out,
    const QVector<PstItemDetail>& items,
    const QVector<QVector<MapiProperty>>& all_properties,
    const QList<QString>& sorted_names) {
    if (items.size() != all_properties.size()) {
        // items[i] and all_properties[i] are parallel; a size mismatch means the manifest
        // request is inconsistent. Surface it rather than silently emitting a truncated
        // manifest for the common prefix.
        logError(
            "ConversionReport: CSV item/property count mismatch ({} items vs {} property sets)",
            std::to_string(items.size()),
            std::to_string(all_properties.size()));
    }
    const qsizetype count = qMin(items.size(), all_properties.size());
    for (qsizetype i = 0; i < count; ++i) {
        const auto& item = items[i];

        out << item.node_id << "," << csvSafeCell(item.subject) << ","
            << csvSafeCell(item.sender_name) << "," << csvSafeCell(item.sender_email) << ","
            << csvSafeCell(item.date.toString(Qt::ISODate)) << "," << csvSafeCell(item.message_id);

        const auto& props = all_properties[i];
        QHash<QString, QString> prop_map;
        for (const auto& prop : props) {
            prop_map[prop.property_name] = prop.display_value;
        }

        for (const auto& name : sorted_names) {
            out << "," << csvSafeCell(prop_map.value(name));
        }

        out << "\n";
    }

    return static_cast<int>(qMin<qsizetype>(count, std::numeric_limits<int>::max()));
}

namespace {
// The sorted, bounded set of property names that become pivot columns for the manifest.
//
// Bound the pivot width: a hostile mail file can declare an unbounded number of distinct
// property names, and each becomes a column emitted for every item (O(items x names)).
// Legitimate manifests use far fewer; cap fail-closed so a crafted store cannot force
// quadratic CPU/disk use.
QList<QString> collectManifestPropertyColumns(
    const QVector<QVector<MapiProperty>>& all_properties) {
    QSet<QString> prop_names;
    for (const auto& props : all_properties) {
        for (const auto& prop : props) {
            prop_names.insert(prop.property_name);
        }
    }
    QList<QString> sorted_names = prop_names.values();
    std::sort(sorted_names.begin(), sorted_names.end());

    constexpr qsizetype kMaxPropertyColumns = 4096;
    if (sorted_names.size() > kMaxPropertyColumns) {
        logWarning(
            "ConversionReport: {} distinct property names exceeds column cap {} -- truncating",
            std::to_string(sorted_names.size()),
            std::to_string(kMaxPropertyColumns));
        sorted_names = sorted_names.mid(0, kMaxPropertyColumns);
    }
    return sorted_names;
}
}  // namespace

QString ConversionReportGenerator::generateCsvManifest(
    const QVector<PstItemDetail>& items,
    const QVector<QVector<MapiProperty>>& all_properties,
    const QString& output_directory) {
    if (output_directory.trimmed().isEmpty()) {
        logError("ConversionReport: refusing to write CSV to a blank output directory");
        return {};
    }
    QString csv_path = output_directory + QStringLiteral("/properties_manifest.csv");

    QDir dir(output_directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        logError("ConversionReport: failed to create output directory {}",
                 output_directory.toStdString());
        return {};
    }

    // Atomic write: a short/failed write never truncates a previously good manifest.
    QSaveFile file(csv_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("ConversionReport: failed to open CSV {} for writing: {}",
                 csv_path.toStdString(),
                 file.errorString().toStdString());
        return {};
    }

    int count = 0;
    {
        QTextStream out(&file);

        // Header row
        out << "NodeId,Subject,SenderName,SenderEmail,Date,MessageClass";

        const QList<QString> sorted_names = collectManifestPropertyColumns(all_properties);
        for (const auto& name : sorted_names) {
            out << "," << csvSafeCell(name);  // property names come from data -> escape them too
        }
        out << "\n";

        count = writeCsvDataRows(out, items, all_properties, sorted_names);

        out.flush();
        if (out.status() != QTextStream::Ok) {
            logError("ConversionReport: stream error writing CSV {}", csv_path.toStdString());
            file.cancelWriting();
            return {};
        }
    }

    if (!file.commit()) {
        logError("ConversionReport: failed to finalize CSV {}: {}",
                 csv_path.toStdString(),
                 file.errorString().toStdString());
        return {};
    }

    logInfo("ConversionReport: CSV manifest saved -- {} items", std::to_string(count));
    return csv_path;
}

// ======================================================================
// Private helpers
// ======================================================================

QString ConversionReportGenerator::buildSummaryStatsHtml(const OstConversionBatchResult& batch,
                                                         qint64 duration_ms) {
    QString html;
    auto addStat = [&](const QString& value, const QString& label) {
        html += QStringLiteral(
                    "<div class='stat'><div class='stat-value'>%1</div>"
                    "<div class='stat-label'>%2</div></div>")
                    .arg(value, label);
    };

    addStat(QString::number(batch.files_total), QStringLiteral("Files"));
    addStat(QString::number(batch.files_succeeded), QStringLiteral("Succeeded"));
    if (batch.files_failed > 0) {
        addStat(QStringLiteral("<span class='error'>%1</span>").arg(batch.files_failed),
                QStringLiteral("Failed"));
    }
    addStat(QString::number(batch.total_items_converted), QStringLiteral("Items Converted"));
    if (batch.total_items_recovered > 0) {
        addStat(QString::number(batch.total_items_recovered), QStringLiteral("Items Recovered"));
    }
    addStat(formatBytes(batch.total_bytes_written), QStringLiteral("Total Output"));
    addStat(formatDuration(duration_ms), QStringLiteral("Duration"));
    return html;
}

namespace {
// The CSS status class for one file-result row.
QString fileResultRowStatusClass(const OstConversionResult& result) {
    if (result.items_failed > 0 && result.items_converted == 0) {
        // A truncated deleted-item scan left items OUT of this output, so the row must not
        // read as a clean run just because nothing that WAS enumerated failed to write.
        return QStringLiteral("error");
    }
    if (result.items_failed > 0 || !result.recovery_complete || !result.errors.isEmpty()) {
        // A file with logged errors is not a clean success even when no item was
        // individually marked failed (e.g. a checksum/open/finalization error).
        return QStringLiteral("warn");
    }
    return QStringLiteral("success");
}
}  // namespace

QString ConversionReportGenerator::buildFileResultsTableHtml(
    const OstConversionBatchResult& batch) {
    QString html;
    html += QStringLiteral(
        "<h2>File Results</h2>"
        "<table><tr>"
        "<th>Source File</th>"
        "<th>Items</th>"
        "<th>Failed</th>"
        "<th>Recovered</th>"
        "<th>Output Size</th>"
        "<th>Duration</th>"
        "<th>SHA-256</th>"
        "</tr>");

    for (const auto& result : batch.file_results) {
        qint64 file_dur = 0;
        if (result.started.isValid() && result.finished.isValid()) {
            file_dur = result.started.msecsTo(result.finished);
        }

        const QString status_class = fileResultRowStatusClass(result);

        html += QStringLiteral(
                    "<tr><td>%1</td>"
                    "<td class='%2'>%3</td>"
                    "<td class='%4'>%5</td>"
                    "<td>%6</td>"
                    "<td>%7</td>"
                    "<td>%8</td>"
                    "<td class='hash-preview'>%9</td>"
                    "</tr>")
                    .arg(result.source_path.toHtmlEscaped())
                    .arg(status_class)
                    .arg(result.items_converted)
                    .arg(result.items_failed > 0 ? QStringLiteral("error") : QString())
                    .arg(result.items_failed)
                    .arg(result.items_recovered)
                    .arg(formatBytes(result.bytes_written))
                    .arg(formatDuration(file_dur))
                    .arg(result.source_sha256.isEmpty()
                             ? QStringLiteral("\u2014")
                             : result.source_sha256.left(kSha256PreviewChars) +
                                   QStringLiteral("\u2026"));
    }

    html += QStringLiteral("</table>");
    return html;
}

QString ConversionReportGenerator::buildErrorLogHtml(const OstConversionBatchResult& batch) {
    bool has_errors = false;
    for (const auto& result : batch.file_results) {
        if (!result.errors.isEmpty()) {
            has_errors = true;
            break;
        }
    }

    if (!has_errors) {
        return {};
    }

    QString html;
    html += QStringLiteral(
        "<h2>Error Log</h2><table>"
        "<tr><th>File</th><th>Error</th></tr>");

    constexpr int kMaxReportErrors = 500;
    int error_count = 0;
    bool truncated = false;
    for (const auto& result : batch.file_results) {
        if (truncated) {
            break;
        }
        for (const auto& err : result.errors) {
            if (error_count >= kMaxReportErrors) {
                // More errors remain than we will render. Flag it so the truncation notice
                // is emitted exactly once below: the previous inner+outer break could exit
                // at an exact-cap boundary WITHOUT emitting it, silently dropping later
                // results' errors.
                truncated = true;
                break;
            }
            html += QStringLiteral("<tr><td>%1</td><td class='error'>%2</td></tr>")
                        .arg(result.source_path.toHtmlEscaped())
                        .arg(err.toHtmlEscaped());
            ++error_count;
        }
    }
    if (truncated) {
        html += QStringLiteral(
            "<tr><td colspan='2' class='warn'>"
            "... truncated (more errors omitted)</td></tr>");
    }

    html += QStringLiteral("</table>");
    return html;
}

QString ConversionReportGenerator::buildReportHtml(const OstConversionBatchResult& batch) {
    qint64 duration_ms = 0;
    if (batch.batch_started.isValid() && batch.batch_finished.isValid()) {
        duration_ms = batch.batch_started.msecsTo(batch.batch_finished);
    }

    QString html;
    html.reserve(kReportHtmlReserveChars);

    html += QString::fromLatin1(report::kEnterpriseReportDocumentOpen)
                .arg(QStringLiteral("S.A.K. Utility - Conversion Report"),
                     report::enterpriseReportStyleSheet());

    html += QStringLiteral("<h1>Conversion Report</h1>");
    html += QStringLiteral("<p>Generated by S.A.K. Utility on %1</p>")
                .arg(batch.batch_finished.toString(Qt::RFC2822Date));

    html += QStringLiteral("<h2>Summary</h2><div>");
    html += buildSummaryStatsHtml(batch, duration_ms);
    html += QStringLiteral("</div>");

    html += buildFileResultsTableHtml(batch);
    html += buildErrorLogHtml(batch);

    html += QStringLiteral(
        "<div class='footer'>"
        "Report generated by <b>S.A.K. Utility</b> "
        "(Swiss Army Knife Utility for PC Technicians)"
        "</div></div></body></html>");

    return html;
}

QString ConversionReportGenerator::formatDuration(qint64 ms) {
    if (ms < kMillisecondsPerSecond) {
        return QStringLiteral("%1 ms").arg(ms);
    }

    qint64 total_sec = ms / kMillisecondsPerSecond;

    if (total_sec < kSecondsPerMinute) {
        return QStringLiteral("%1 s").arg(total_sec);
    }

    qint64 minutes = total_sec / kSecondsPerMinute;
    qint64 seconds = total_sec % kSecondsPerMinute;

    if (minutes < kSecondsPerMinute) {
        return QStringLiteral("%1 min %2 s").arg(minutes).arg(seconds);
    }

    qint64 hours = total_sec / kSecondsPerHour;
    minutes = (total_sec % kSecondsPerHour) / kSecondsPerMinute;
    return QStringLiteral("%1 h %2 min").arg(hours).arg(minutes);
}

QString ConversionReportGenerator::formatBytes(qint64 bytes) {
    if (bytes < kBytesPerKB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (static_cast<double>(bytes) < kBytesPerMBf) {
        return QStringLiteral("%1 KB").arg(bytes / kBytesPerKB);
    }
    if (static_cast<double>(bytes) < kBytesPerGBf) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / kBytesPerMBf, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(
        static_cast<double>(bytes) / kBytesPerGBf, 0, 'f', kGigabyteDisplayPrecision);
}

}  // namespace sak
