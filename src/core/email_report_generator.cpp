// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_report_generator.cpp
/// @brief Generates professional HTML/JSON/CSV reports

#include "sak/email_report_generator.h"

#include "sak/email_constants.h"
#include "sak/layout_constants.h"
#include "sak/report_style_constants.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTextStream>

namespace {

constexpr int kByteSizePrecisionSmall = 1;
constexpr int kByteSizePrecisionLarge = 2;
constexpr int kFolderTreeIndentSpacesPerLevel = 2;

/// Format bytes into a human-readable string
QString formatBytes(qint64 bytes) {
    if (bytes < sak::kBytesPerKB) {
        return QString::number(bytes) + QStringLiteral(" B");
    }
    if (bytes < sak::kBytesPerMB) {
        return QString::number(bytes / sak::kBytesPerKBf, 'f', kByteSizePrecisionSmall) +
               QStringLiteral(" KB");
    }
    if (bytes < sak::kBytesPerGB) {
        return QString::number(bytes / sak::kBytesPerMBf, 'f', kByteSizePrecisionSmall) +
               QStringLiteral(" MB");
    }
    return QString::number(bytes / sak::kBytesPerGBf, 'f', kByteSizePrecisionLarge) +
           QStringLiteral(" GB");
}

/// Build an HTML table row
QString tableRow(const QString& label, const QString& value) {
    return QStringLiteral(
               "<tr><td style=\"padding:4px 12px 4px 0;"
               "font-weight:bold;\">%1</td>"
               "<td style=\"padding:4px 0;\">%2</td></tr>\n")
        .arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}

/// Recursively render folder tree as HTML
void renderFolderTree(const QVector<sak::PstFolder>& folders, QString& html, int indent_level) {
    constexpr int kMaxIndent = 20;
    if (indent_level > kMaxIndent) {
        return;
    }

    for (const auto& folder : folders) {
        QString indent(indent_level * kFolderTreeIndentSpacesPerLevel, QLatin1Char(' '));
        html += QStringLiteral("<li>%1 <b>%2</b> (%3 items)")
                    .arg(indent,
                         folder.display_name.toHtmlEscaped(),
                         QString::number(folder.content_count));
        if (!folder.children.isEmpty()) {
            html += QStringLiteral("<ul>\n");
            renderFolderTree(folder.children, html, indent_level + 1);
            html += QStringLiteral("</ul>\n");
        }
        html += QStringLiteral("</li>\n");
    }
}

/// Render folder tree as JSON
QJsonArray folderTreeToJson(const QVector<sak::PstFolder>& folders) {
    QJsonArray arr;
    for (const auto& folder : folders) {
        QJsonObject obj;
        obj[QStringLiteral("name")] = folder.display_name;
        obj[QStringLiteral("item_count")] = folder.content_count;
        obj[QStringLiteral("unread_count")] = folder.unread_count;
        if (!folder.children.isEmpty()) {
            obj[QStringLiteral("children")] = folderTreeToJson(folder.children);
        }
        arr.append(obj);
    }
    return arr;
}

QJsonObject reportMetadataToJson(const EmailReportGenerator::ReportData& data) {
    QJsonObject metadata;
    metadata[QStringLiteral("technician")] = data.technician_name;
    metadata[QStringLiteral("customer")] = data.customer_name;
    metadata[QStringLiteral("ticket_number")] = data.ticket_number;
    metadata[QStringLiteral("report_date")] =
        data.report_date.isValid() ? data.report_date.toString(Qt::ISODate)
                                   : QDateTime::currentDateTime().toString(Qt::ISODate);
    metadata[QStringLiteral("tool")] = QStringLiteral("SAK Utility");
    return metadata;
}

QJsonObject reportFileInfoToJson(const sak::PstFileInfo& file_info) {
    QJsonObject file_obj;
    file_obj[QStringLiteral("path")] = file_info.file_path;
    file_obj[QStringLiteral("display_name")] = file_info.display_name;
    file_obj[QStringLiteral("size_bytes")] = file_info.file_size_bytes;
    file_obj[QStringLiteral("is_unicode")] = file_info.is_unicode;
    file_obj[QStringLiteral("is_ost")] = file_info.is_ost;
    file_obj[QStringLiteral("encryption_type")] = file_info.encryption_type;
    file_obj[QStringLiteral("total_folders")] = file_info.total_folders;
    file_obj[QStringLiteral("total_items")] = file_info.total_items;
    return file_obj;
}

QJsonObject reportStatisticsToJson(const EmailReportGenerator::ReportData& data) {
    QJsonObject stats;
    stats[QStringLiteral("emails")] = data.total_emails;
    stats[QStringLiteral("contacts")] = data.total_contacts;
    stats[QStringLiteral("calendar_items")] = data.total_calendar_items;
    stats[QStringLiteral("tasks")] = data.total_tasks;
    stats[QStringLiteral("notes")] = data.total_notes;
    stats[QStringLiteral("attachments")] = data.total_attachments;
    stats[QStringLiteral("attachment_bytes")] = data.total_attachment_bytes;
    stats[QStringLiteral("searches_performed")] = data.searches_performed;
    stats[QStringLiteral("total_search_hits")] = data.total_search_hits;
    return stats;
}

QJsonArray exportResultsToJson(const QVector<sak::EmailExportResult>& export_results) {
    QJsonArray exports;
    for (const auto& result : export_results) {
        QJsonObject export_obj;
        export_obj[QStringLiteral("format")] = result.export_format;
        export_obj[QStringLiteral("path")] = result.export_path;
        export_obj[QStringLiteral("items_exported")] = result.items_exported;
        export_obj[QStringLiteral("items_failed")] = result.items_failed;
        export_obj[QStringLiteral("total_bytes")] = result.total_bytes;
        if (result.started.isValid()) {
            export_obj[QStringLiteral("started")] = result.started.toString(Qt::ISODate);
        }
        if (result.finished.isValid()) {
            export_obj[QStringLiteral("finished")] = result.finished.toString(Qt::ISODate);
        }
        exports.append(export_obj);
    }
    return exports;
}

QJsonArray discoveredProfilesToJson(const QVector<sak::EmailClientProfile>& profiles) {
    QJsonArray profile_array;
    for (const auto& profile : profiles) {
        QJsonObject profile_obj;
        profile_obj[QStringLiteral("client_name")] = profile.client_name;
        profile_obj[QStringLiteral("profile_name")] = profile.profile_name;
        profile_obj[QStringLiteral("data_file_count")] = profile.data_files.size();
        profile_obj[QStringLiteral("total_size_bytes")] = profile.total_size_bytes;
        profile_array.append(profile_obj);
    }
    return profile_array;
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

EmailReportGenerator::EmailReportGenerator(QObject* parent) : QObject(parent) {}

// ============================================================================
// HTML Report
// ============================================================================

QString EmailReportGenerator::generateHtml(const ReportData& data) {
    QString html;
    QTextStream stream(&html);

    stream << QStringLiteral(
        "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n"
        "<meta charset=\"UTF-8\">\n"
        "<title>Email Inspection Report</title>\n"
        "<style>\n");
    stream << sak::report::enterpriseReportStyleSheet();
    stream << QStringLiteral("</style>\n</head>\n<body>\n");

    stream << QStringLiteral("<h1>Email Inspection Report</h1>\n");

    renderMetadataSection(stream, data);
    renderFileInfoSection(stream, data);
    renderStatisticsSection(stream, data);

    // Folder Tree
    if (!data.folder_tree.isEmpty()) {
        stream << QStringLiteral("<h2>Folder Structure</h2>\n<ul>\n");
        renderFolderTree(data.folder_tree, html, 0);
        stream << QStringLiteral("</ul>\n");
    }

    // Export Results
    if (!data.export_results.isEmpty()) {
        stream << QStringLiteral("<h2>Export Operations</h2>\n");
        for (const auto& export_res : data.export_results) {
            stream << QStringLiteral("<table>\n");
            stream << tableRow(QStringLiteral("Format"), export_res.export_format);
            stream << tableRow(QStringLiteral("Path"), export_res.export_path);
            stream << tableRow(QStringLiteral("Items Exported"),
                               QString::number(export_res.items_exported));
            stream << tableRow(QStringLiteral("Items Failed"),
                               QString::number(export_res.items_failed));
            stream << tableRow(QStringLiteral("Total Size"), formatBytes(export_res.total_bytes));
            stream << QStringLiteral("</table><br>\n");
        }
    }

    // Discovery profiles
    if (!data.discovered_profiles.isEmpty()) {
        stream << QStringLiteral("<h2>Discovered Email Profiles</h2>\n");
        for (const auto& profile : data.discovered_profiles) {
            stream << QStringLiteral("<table>\n");
            stream << tableRow(QStringLiteral("Client"), profile.client_name);
            stream << tableRow(QStringLiteral("Profile"), profile.profile_name);
            stream << tableRow(QStringLiteral("Data Files"),
                               QString::number(profile.data_files.size()));
            stream << tableRow(QStringLiteral("Total Size"), formatBytes(profile.total_size_bytes));
            stream << QStringLiteral("</table><br>\n");
        }
    }

    // Footer
    stream << QStringLiteral(
        "<div class=\"footer\">"
        "Generated by S.A.K. Utility &mdash; "
        "Email &amp; PST/OST Inspector"
        "</div>\n</body>\n</html>\n");

    return html;
}

void EmailReportGenerator::renderMetadataSection(QTextStream& stream, const ReportData& data) {
    stream << QStringLiteral("<h2>Report Details</h2>\n<table>\n");
    if (!data.technician_name.isEmpty()) {
        stream << tableRow(QStringLiteral("Technician"), data.technician_name);
    }
    if (!data.customer_name.isEmpty()) {
        stream << tableRow(QStringLiteral("Customer"), data.customer_name);
    }
    if (!data.ticket_number.isEmpty()) {
        stream << tableRow(QStringLiteral("Ticket #"), data.ticket_number);
    }
    QLocale locale;
    stream << tableRow(QStringLiteral("Report Date"),
                       data.report_date.isValid()
                           ? locale.toString(data.report_date, QLocale::LongFormat)
                           : locale.toString(QDateTime::currentDateTime(), QLocale::LongFormat));
    stream << QStringLiteral("</table>\n");
}

void EmailReportGenerator::renderFileInfoSection(QTextStream& stream, const ReportData& data) {
    if (data.file_info.file_path.isEmpty()) {
        return;
    }
    stream << QStringLiteral("<h2>File Information</h2>\n<table>\n");
    stream << tableRow(QStringLiteral("File"), data.file_info.file_path);
    stream << tableRow(QStringLiteral("Display Name"), data.file_info.display_name);
    stream << tableRow(QStringLiteral("Size"), formatBytes(data.file_info.file_size_bytes));
    stream << tableRow(QStringLiteral("Format"),
                       data.file_info.is_unicode ? QStringLiteral("Unicode (modern)")
                                                 : QStringLiteral("ANSI (legacy)"));
    stream << tableRow(QStringLiteral("Type"),
                       data.file_info.is_ost ? QStringLiteral("OST (Offline Storage)")
                                             : QStringLiteral("PST (Personal Folders)"));
    QString enc_text;
    switch (data.file_info.encryption_type) {
    case 0:
        enc_text = QStringLiteral("None");
        break;
    case 1:
        enc_text = QStringLiteral("Compressible");
        break;
    case sak::email::kEncryptHigh:
        enc_text = QStringLiteral("High");
        break;
    default:
        enc_text = QStringLiteral("Unknown");
        break;
    }
    stream << tableRow(QStringLiteral("Encryption"), enc_text);
    stream << tableRow(QStringLiteral("Total Folders"),
                       QString::number(data.file_info.total_folders));
    stream << tableRow(QStringLiteral("Total Items"), QString::number(data.file_info.total_items));
    stream << QStringLiteral("</table>\n");
}

void EmailReportGenerator::renderStatisticsSection(QTextStream& stream, const ReportData& data) {
    int total_items = data.total_emails + data.total_contacts + data.total_calendar_items +
                      data.total_tasks + data.total_notes;
    if (total_items <= 0) {
        return;
    }
    stream << QStringLiteral(
        "<h2>Item Statistics</h2>\n"
        "<div class=\"stats-grid\">\n");

    auto statCard = [&stream](const QString& value, const QString& label) {
        stream << QStringLiteral(
                      "<div class=\"stat-card\">"
                      "<div class=\"stat-value\">%1</div>"
                      "<div class=\"stat-label\">%2</div>"
                      "</div>\n")
                      .arg(value, label);
    };

    statCard(QString::number(data.total_emails), QStringLiteral("Emails"));
    statCard(QString::number(data.total_contacts), QStringLiteral("Contacts"));
    statCard(QString::number(data.total_calendar_items), QStringLiteral("Calendar Items"));
    statCard(QString::number(data.total_tasks), QStringLiteral("Tasks"));
    statCard(QString::number(data.total_notes), QStringLiteral("Notes"));
    statCard(QString::number(data.total_attachments), QStringLiteral("Attachments"));

    stream << QStringLiteral("</div>\n");
}

// ============================================================================
// JSON Report
// ============================================================================

QByteArray EmailReportGenerator::generateJson(const ReportData& data) {
    QJsonObject root;
    root[QStringLiteral("metadata")] = reportMetadataToJson(data);

    if (!data.file_info.file_path.isEmpty()) {
        root[QStringLiteral("file_info")] = reportFileInfoToJson(data.file_info);
    }

    root[QStringLiteral("statistics")] = reportStatisticsToJson(data);
    if (!data.folder_tree.isEmpty()) {
        root[QStringLiteral("folder_tree")] = folderTreeToJson(data.folder_tree);
    }
    if (!data.export_results.isEmpty()) {
        root[QStringLiteral("export_results")] = exportResultsToJson(data.export_results);
    }
    if (!data.discovered_profiles.isEmpty()) {
        root[QStringLiteral("discovered_profiles")] =
            discoveredProfilesToJson(data.discovered_profiles);
    }

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ============================================================================
// CSV Report (Summary)
// ============================================================================

QString EmailReportGenerator::generateCsv(const ReportData& data) {
    QString csv;
    QTextStream stream(&csv);

    // Summary section
    stream << QStringLiteral("\"Email Inspection Report Summary\"\r\n\r\n");
    stream << QStringLiteral("\"Metric\",\"Value\"\r\n");
    stream << QStringLiteral("\"File\",\"%1\"\r\n").arg(data.file_info.file_path);
    stream
        << QStringLiteral("\"Size\",\"%1\"\r\n").arg(formatBytes(data.file_info.file_size_bytes));
    stream << QStringLiteral("\"Total Emails\",%1\r\n").arg(data.total_emails);
    stream << QStringLiteral("\"Total Contacts\",%1\r\n").arg(data.total_contacts);
    stream << QStringLiteral("\"Total Calendar Items\",%1\r\n").arg(data.total_calendar_items);
    stream << QStringLiteral("\"Total Tasks\",%1\r\n").arg(data.total_tasks);
    stream << QStringLiteral("\"Total Notes\",%1\r\n").arg(data.total_notes);
    stream << QStringLiteral("\"Total Attachments\",%1\r\n").arg(data.total_attachments);
    stream << QStringLiteral("\"Searches Performed\",%1\r\n").arg(data.searches_performed);
    stream << QStringLiteral("\"Search Hits\",%1\r\n").arg(data.total_search_hits);

    return csv;
}
