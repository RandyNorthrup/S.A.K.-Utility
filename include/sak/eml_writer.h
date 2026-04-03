// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file eml_writer.h
/// @brief RFC 5322 MIME .eml file writer for OST/PST conversion

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QHash>
#include <QString>

#include <expected>

namespace sak {

/// @brief Writes RFC 5322 MIME .eml files from PstItemDetail data
class EmlWriter {
public:
    explicit EmlWriter(const QString& output_dir,
                       bool prefix_with_date = true,
                       bool preserve_folders = true);

    /// Write a single message as an EML file
    [[nodiscard]] std::expected<QString, error_code> writeMessage(
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& subfolder_path);

    /// Total bytes written across all files
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    [[nodiscard]] QByteArray buildEmlContent(
        const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const;

    [[nodiscard]] QString sanitizeFilename(const QString& subject, const QDateTime& date) const;

    QString m_output_dir;
    bool m_prefix_with_date;
    bool m_preserve_folders;
    QHash<QString, int> m_filename_counters;
    qint64 m_bytes_written = 0;
};

}  // namespace sak
