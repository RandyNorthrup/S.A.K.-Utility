// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dbx_writer.h
/// @brief Writes Outlook Express DBX files (legacy format)

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QFile>
#include <QHash>
#include <QString>
#include <QVector>

#include <expected>

namespace sak {

/// @brief Writes Outlook Express DBX files for legacy migration
class DbxWriter {
public:
    explicit DbxWriter(const QString& output_dir);
    ~DbxWriter();

    DbxWriter(const DbxWriter&) = delete;
    DbxWriter& operator=(const DbxWriter&) = delete;
    DbxWriter(DbxWriter&&) = delete;
    DbxWriter& operator=(DbxWriter&&) = delete;

    /// Write a message into the appropriate DBX file
    [[nodiscard]] std::expected<void, error_code> writeMessage(
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& folder_path);

    /// Close all open file handles
    void finalize();

    /// Total bytes written across all files
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    void writeDbxHeader(QFile& file, const QString& folder_name);
    [[nodiscard]] QByteArray buildDbxMessageEntry(
        const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments);

    QString m_output_dir;
    QHash<QString, QFile*> m_open_files;
    QHash<QString, int> m_message_counts;
    qint64 m_bytes_written = 0;

public:
    static constexpr uint32_t kDbxFolderMagic = 0xFE'12'AD'CF;
    static constexpr int kDbxHeaderSize = 0x24BC;
};

}  // namespace sak
