// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_report_generator.cpp
/// @brief Generates professional HTML/JSON/CSV reports

#include "sak/email_report_generator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QTextStream>

namespace {

/// Format bytes into a human-readable string
QString formatBytes(qint64 bytes) {
    if (bytes < 1024) {
        return QString::number(bytes) + QStringLiteral(" B");
    }
    if (bytes < 1024 * 1024) {
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    }
    if (bytes < 1024LL * 1024 * 1024) {
        return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
    }
    return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + QStringLiteral(" GB");
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
        QString indent(indent_level * 2, QLatin1Char(' '));
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
        "<style>\n"
        "body { font-family: 'Segoe UI', Arial, sans-serif; "
        "margin: 40px; color: #333; }\n"
        "h1 { color: #1a5276; border-bottom: 3px solid #1a5276; "
        "padding-bottom: 8px; }\n"
        "h2 { color: #2e86c1; margin-top: 30px; }\n"
        "table { border-collapse: collapse; margin: 10px 0; }\n"
        ".stats-grid { display: grid; grid-template-columns: "
        "repeat(auto-fit, minmax(200px, 1fr)); gap: 16px; "
        "margin: 16px 0; }\n"
        ".stat-card { background: #f8f9fa; border: 1px solid #dee2e6; "
        "border-radius: 8px; padding: 16px; text-align: center; }\n"
        ".stat-value { font-size: 28px; font-weight: bold; "
        "color: #1a5276; }\n"
        ".stat-label { color: #6c757d; margin-top: 4px; }\n"
        ".footer { margin-top: 40px; padding-top: 16px; "
        "border-top: 1px solid #dee2e6; color: #6c757d; "
        "font-size: 12px; }\n"
        "</style>\n</head>\n<body>\n");

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
    case 2:
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

    // Metadata
    QJsonObject metadata;
    metadata[QStringLiteral("technician")] = data.technician_name;
    metadata[QStringLiteral("customer")] = data.customer_name;
    metadata[QStringLiteral("ticket_number")] = data.ticket_number;
    metadata[QStringLiteral("report_date")] =
        data.report_date.isValid() ? data.report_date.toString(Qt::ISODate)
                                   : QDateTime::currentDateTime().toString(Qt::ISODate);
    metadata[QStringLiteral("tool")] = QStringLiteral("SAK Utility");
    root[QStringLiteral("metadata")] = metadata;

    // File info
    if (!data.file_info.file_path.isEmpty()) {
        QJsonObject file_obj;
        file_obj[QStringLiteral("path")] = data.file_info.file_path;
        file_obj[QStringLiteral("display_name")] = data.file_info.display_name;
        file_obj[QStringLiteral("size_bytes")] = data.file_info.file_size_bytes;
        file_obj[QStringLiteral("is_unicode")] = data.file_info.is_unicode;
        file_obj[QStringLiteral("is_ost")] = data.file_info.is_ost;
        file_obj[QStringLiteral("encryption_type")] = data.file_info.encryption_type;
        file_obj[QStringLiteral("total_folders")] = data.file_info.total_folders;
        file_obj[QStringLiteral("total_items")] = data.file_info.total_items;
        root[QStringLiteral("file_info")] = file_obj;
    }

    // Statistics
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
    root[QStringLiteral("statistics")] = stats;

    // Folder tree
    if (!data.folder_tree.isEmpty()) {
        root[QStringLiteral("folder_tree")] = folderTreeToJson(data.folder_tree);
    }

    // Export results
    if (!data.export_results.isEmpty()) {
        QJsonArray exports;
        for (const auto& ex : data.export_results) {
            QJsonObject ex_obj;
            ex_obj[QStringLiteral("format")] = ex.export_format;
            ex_obj[QStringLiteral("path")] = ex.export_path;
            ex_obj[QStringLiteral("items_exported")] = ex.items_exported;
            ex_obj[QStringLiteral("items_failed")] = ex.items_failed;
            ex_obj[QStringLiteral("total_bytes")] = ex.total_bytes;
            if (ex.started.isValid()) {
                ex_obj[QStringLiteral("started")] = ex.started.toString(Qt::ISODate);
            }
            if (ex.finished.isValid()) {
                ex_obj[QStringLiteral("finished")] = ex.finished.toString(Qt::ISODate);
            }
            exports.append(ex_obj);
        }
        root[QStringLiteral("export_results")] = exports;
    }

    // Profiles
    if (!data.discovered_profiles.isEmpty()) {
        QJsonArray profiles;
        for (const auto& prof : data.discovered_profiles) {
            QJsonObject prof_obj;
            prof_obj[QStringLiteral("client_name")] = prof.client_name;
            prof_obj[QStringLiteral("profile_name")] = prof.profile_name;
            prof_obj[QStringLiteral("data_file_count")] = prof.data_files.size();
            prof_obj[QStringLiteral("total_size_bytes")] = prof.total_size_bytes;
            profiles.append(prof_obj);
        }
        root[QStringLiteral("discovered_profiles")] = profiles;
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
