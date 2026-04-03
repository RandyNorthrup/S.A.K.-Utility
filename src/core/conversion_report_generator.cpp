// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file conversion_report_generator.cpp
/// @brief HTML and CSV report generation for OST conversion

#include "sak/conversion_report_generator.h"

#include "sak/logger.h"

#include <QDir>
#include <QFile>
#include <QTextStream>

namespace sak {

namespace {
constexpr double kBytesPerMBf = 1024.0 * 1024.0;
constexpr double kBytesPerGBf = 1024.0 * 1024.0 * 1024.0;
constexpr int kMsPerSecond = 1000;
constexpr int kSecondsPerMinute = 60;
constexpr int kSecondsPerHour = 3600;
}  // namespace

// ======================================================================
// Public API
// ======================================================================

QString ConversionReportGenerator::generateHtmlReport(const OstConversionBatchResult& batch,
                                                      const QString& output_directory) {
    QString html = buildReportHtml(batch);
    QString report_path = output_directory + QStringLiteral("/conversion_report.html");

    QDir dir(output_directory);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    QFile file(report_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("ConversionReport: failed to write report to {}", report_path.toStdString());
        return {};
    }

    QTextStream out(&file);
    out << html;
    file.close();

    logInfo("ConversionReport: report saved to {}", report_path.toStdString());
    return report_path;
}

int ConversionReportGenerator::writeCsvDataRows(
    QTextStream& out,
    const QVector<PstItemDetail>& items,
    const QVector<QVector<MapiProperty>>& all_properties,
    const QList<QString>& sorted_names) {
    auto csvEscape = [](const QString& str) -> QString {
        if (str.contains(QLatin1Char(',')) || str.contains(QLatin1Char('"')) ||
            str.contains(QLatin1Char('\n'))) {
            QString escaped = str;
            escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(escaped);
        }
        return str;
    };

    int count = qMin(items.size(), all_properties.size());
    for (int i = 0; i < count; ++i) {
        const auto& item = items[i];

        out << item.node_id << "," << csvEscape(item.subject) << "," << csvEscape(item.sender_name)
            << "," << csvEscape(item.sender_email) << ","
            << csvEscape(item.date.toString(Qt::ISODate)) << "," << csvEscape(item.message_id);

        const auto& props = all_properties[i];
        QHash<QString, QString> prop_map;
        for (const auto& prop : props) {
            prop_map[prop.property_name] = prop.display_value;
        }

        for (const auto& name : sorted_names) {
            out << "," << csvEscape(prop_map.value(name));
        }

        out << "\n";
    }

    return count;
}

QString ConversionReportGenerator::generateCsvManifest(
    const QVector<PstItemDetail>& items,
    const QVector<QVector<MapiProperty>>& all_properties,
    const QString& output_directory) {
    QString csv_path = output_directory + QStringLiteral("/properties_manifest.csv");

    QDir dir(output_directory);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    QFile file(csv_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("ConversionReport: failed to write CSV to {}", csv_path.toStdString());
        return {};
    }

    QTextStream out(&file);

    // Header row
    out << "NodeId,Subject,SenderName,SenderEmail,Date,MessageClass";

    // Collect all unique property names across all items
    QSet<QString> prop_names;
    for (const auto& props : all_properties) {
        for (const auto& prop : props) {
            prop_names.insert(prop.property_name);
        }
    }
    QList<QString> sorted_names = prop_names.values();
    std::sort(sorted_names.begin(), sorted_names.end());

    for (const auto& name : sorted_names) {
        out << "," << name;
    }
    out << "\n";

    int count = writeCsvDataRows(out, items, all_properties, sorted_names);

    file.close();

    logInfo("ConversionReport: CSV manifest saved — {} items", std::to_string(count));
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

        QString status_class;
        if (result.items_failed > 0 && result.items_converted == 0) {
            status_class = QStringLiteral("error");
        } else if (result.items_failed > 0) {
            status_class = QStringLiteral("warn");
        } else {
            status_class = QStringLiteral("success");
        }

        html += QStringLiteral(
                    "<tr><td>%1</td>"
                    "<td class='%2'>%3</td>"
                    "<td class='%4'>%5</td>"
                    "<td>%6</td>"
                    "<td>%7</td>"
                    "<td>%8</td>"
                    "<td style='font-family:monospace;font-size:10px;'>%9</td>"
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
                             ? QStringLiteral("—")
                             : result.source_sha256.left(16) + QStringLiteral("…"));
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
    for (const auto& result : batch.file_results) {
        for (const auto& err : result.errors) {
            if (error_count >= kMaxReportErrors) {
                html += QStringLiteral(
                    "<tr><td colspan='2' class='warn'>"
                    "... truncated (more errors omitted)</td></tr>");
                break;
            }
            html += QStringLiteral("<tr><td>%1</td><td class='error'>%2</td></tr>")
                        .arg(result.source_path.toHtmlEscaped())
                        .arg(err.toHtmlEscaped());
            ++error_count;
        }
        if (error_count >= kMaxReportErrors) {
            break;
        }
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
    html.reserve(8192);

    html += QStringLiteral(
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>S.A.K. Utility — Conversion Report</title>"
        "<style>"
        "body { font-family: 'Segoe UI', Arial, sans-serif; "
        "       margin: 24px; background: #f5f5f5; }"
        ".container { max-width: 900px; margin: 0 auto; "
        "             background: white; padding: 24px; "
        "             border-radius: 8px; box-shadow: 0 1px 4px #0002; }"
        "h1 { color: #0078D4; margin-bottom: 4px; }"
        "h2 { color: #333; border-bottom: 2px solid #0078D4; "
        "     padding-bottom: 4px; }"
        "table { border-collapse: collapse; width: 100%; "
        "        margin: 12px 0; }"
        "th { background: #0078D4; color: white; padding: 8px 12px; "
        "     text-align: left; }"
        "td { padding: 6px 12px; border-bottom: 1px solid #e0e0e0; }"
        "tr:hover { background: #f0f8ff; }"
        ".stat { display: inline-block; margin: 8px 16px 8px 0; }"
        ".stat-value { font-size: 24px; font-weight: bold; "
        "              color: #0078D4; }"
        ".stat-label { font-size: 12px; color: #666; }"
        ".success { color: #107C10; }"
        ".error { color: #D13438; }"
        ".warn { color: #CA5010; }"
        ".footer { margin-top: 24px; padding-top: 12px; "
        "          border-top: 1px solid #e0e0e0; color: #999; "
        "          font-size: 12px; }"
        "</style></head><body><div class='container'>");

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
    if (ms < kMsPerSecond) {
        return QStringLiteral("%1 ms").arg(ms);
    }

    qint64 total_sec = ms / kMsPerSecond;

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
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (static_cast<double>(bytes) < kBytesPerMBf) {
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    }
    if (static_cast<double>(bytes) < kBytesPerGBf) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / kBytesPerMBf, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / kBytesPerGBf, 0, 'f', 2);
}

}  // namespace sak
