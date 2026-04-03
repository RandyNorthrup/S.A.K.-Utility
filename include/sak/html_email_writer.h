// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file html_email_writer.h
/// @brief Writes emails as styled HTML pages with embedded images

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QHash>
#include <QString>
#include <QVector>

class QTextStream;

#include <expected>

namespace sak {

/// @brief Writes email items as standalone HTML pages
class HtmlEmailWriter {
public:
    explicit HtmlEmailWriter(const QString& output_dir,
                             bool prefix_with_date = true,
                             bool preserve_folders = true);

    /// Write a single email as an HTML file with embedded images
    [[nodiscard]] std::expected<QString, error_code> writeMessage(
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& subfolder_path);

    /// Total bytes written
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    [[nodiscard]] QString buildHtmlPage(
        const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const;

    void buildHtmlHeaderFields(QTextStream& ts, const PstItemDetail& item) const;

    [[nodiscard]] QString saveFileAttachments(
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& dir_path,
        const QString& filename);

    [[nodiscard]] QString sanitizeFilename(const QString& subject, const QDateTime& date) const;

    QString m_output_dir;
    bool m_prefix_with_date;
    bool m_preserve_folders;
    QHash<QString, int> m_filename_counters;
    qint64 m_bytes_written = 0;
};

}  // namespace sak
