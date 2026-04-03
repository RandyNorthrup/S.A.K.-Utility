// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_writer.h
/// @brief Writes Unix mbox format files (one per folder or combined)

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QFile>
#include <QHash>
#include <QString>
#include <QVector>

#include <expected>
#include <memory>

namespace sak {

/// @brief Writes Unix mbox files from PST item data
class MboxWriter {
public:
    explicit MboxWriter(const QString& output_dir, bool one_per_folder);
    ~MboxWriter();

    MboxWriter(const MboxWriter&) = delete;
    MboxWriter& operator=(const MboxWriter&) = delete;
    MboxWriter(MboxWriter&&) = delete;
    MboxWriter& operator=(MboxWriter&&) = delete;

    /// Write a message into the appropriate mbox file
    [[nodiscard]] std::expected<void, error_code> writeMessage(
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& folder_path);

    /// Close all open file handles
    void finalize();

    /// Total bytes written across all files
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    [[nodiscard]] QFile* getOrCreateFile(const QString& folder_path);

    [[nodiscard]] QByteArray buildMimeMessage(
        const PstItemDetail& item,
        const QString& sender,
        const QDateTime& date,
        const QVector<QPair<QString, QByteArray>>& attachments);

    [[nodiscard]] QByteArray formatMboxEntry(
        const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments);

    [[nodiscard]] static QByteArray escapeFromLines(const QByteArray& content);
    [[nodiscard]] static QString sanitizeFolderName(const QString& name);

    QString m_output_dir;
    bool m_one_per_folder;
    QHash<QString, QFile*> m_open_files;
    qint64 m_bytes_written = 0;
};

}  // namespace sak
