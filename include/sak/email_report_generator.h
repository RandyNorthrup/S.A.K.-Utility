// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_report_generator.h
/// @brief Generates HTML/JSON/CSV reports of email inspection results

#pragma once

#include "sak/email_types.h"

#include <QObject>

class QTextStream;

class EmailReportGenerator : public QObject {
    Q_OBJECT

public:
    /// Aggregate data for report generation
    struct ReportData {
        // Metadata
        QString technician_name;
        QString ticket_number;
        QString customer_name;
        QDateTime report_date;

        // File info
        sak::PstFileInfo file_info;
        sak::PstFolderTree folder_tree;

        // Statistics
        int total_emails = 0;
        int total_contacts = 0;
        int total_calendar_items = 0;
        int total_tasks = 0;
        int total_notes = 0;
        int total_attachments = 0;
        qint64 total_attachment_bytes = 0;

        // Export log
        QVector<sak::EmailExportResult> export_results;

        // Search log
        int searches_performed = 0;
        int total_search_hits = 0;

        // Profile info
        QVector<sak::EmailClientProfile> discovered_profiles;
    };

    explicit EmailReportGenerator(QObject* parent = nullptr);

    /// Generate a professional HTML report
    [[nodiscard]] QString generateHtml(const ReportData& data);

    /// Generate a machine-readable JSON report
    [[nodiscard]] QByteArray generateJson(const ReportData& data);

    /// Generate a summary CSV report
    [[nodiscard]] QString generateCsv(const ReportData& data);

Q_SIGNALS:
    void reportGenerated(QString output_path);
    void errorOccurred(QString error);

private:
    void renderMetadataSection(QTextStream& stream, const ReportData& data);
    void renderFileInfoSection(QTextStream& stream, const ReportData& data);
    void renderStatisticsSection(QTextStream& stream, const ReportData& data);
};
