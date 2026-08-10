// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pdf_email_writer.h
/// @brief Writes emails as PDF files via QTextDocument and QPdfWriter

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <expected>

namespace sak {

/// @brief Renders email items as PDF documents
class PdfEmailWriter {
public:
    explicit PdfEmailWriter(const QString& output_dir,
                            bool prefix_with_date = true,
                            bool preserve_folders = true);

    /// Write a single email as a PDF file
    [[nodiscard]] std::expected<QString, error_code> writeMessage(
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& subfolder_path);

    /// Total bytes written
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    /// Resolve (and create) the directory this message's PDF is written into: the export root,
    /// plus the preserved subfolder when enabled. Fails closed on a blank/relative root, a
    /// subfolder that escapes the root, or a directory that cannot be created.
    [[nodiscard]] std::expected<QString, error_code> resolveTargetDirectory(
        const QString& subfolder_path) const;

    [[nodiscard]] QString buildHtmlForPdf(
        const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const;

    [[nodiscard]] QString sanitizeFilename(const QString& subject, const QDateTime& date) const;

    /// Return a not-yet-existing ".pdf" path under target_dir for the base filename,
    /// suffixing "_N" until free so a distinct message is never overwritten.
    [[nodiscard]] QString resolveCollisionPath(const QString& target_dir, const QString& filename);

    QString m_output_dir;
    bool m_prefix_with_date;
    bool m_preserve_folders;
    QHash<QString, int> m_filename_counters;
    qint64 m_bytes_written = 0;
};

}  // namespace sak
