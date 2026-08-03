// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_writer.h
/// @brief Writes Unix mbox format files (one per folder or combined)

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include <expected>
#include <memory>

namespace sak {

/// @brief Writes Unix mbox files from PST item data
class MboxWriter {
public:
    /// @param combined_basename When not writing one-mbox-per-folder, names the single output file
    ///        (<combined_basename>.mbox) so distinct source files/jobs sharing one output directory
    ///        do not truncate each other's mailbox. Empty falls back to the legacy "mailbox.mbox".
    explicit MboxWriter(const QString& output_dir,
                        bool one_per_folder,
                        const QString& combined_basename = QString());
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

    [[nodiscard]] std::expected<QByteArray, error_code> buildMimeMessage(
        const PstItemDetail& item,
        const QString& sender,
        const QDateTime& date,
        const QVector<QPair<QString, QByteArray>>& attachments);

    [[nodiscard]] std::expected<QByteArray, error_code> formatMboxEntry(
        const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments);

    /// Generate a high-entropy multipart boundary verified not to occur in the message body, so no
    /// body content can be mistaken for a boundary delimiter. Empty on the (astronomically
    /// unlikely) failure to find a free value, which the caller must treat as a hard failure.
    [[nodiscard]] static QString makeUniqueBoundary(const PstItemDetail& item);

    static void appendMessageHeaders(QByteArray& message,
                                     const PstItemDetail& item,
                                     const QString& sender,
                                     const QDateTime& date);
    static void appendMultipartBody(QByteArray& message,
                                    const PstItemDetail& item,
                                    const QString& boundary,
                                    const QVector<QPair<QString, QByteArray>>& attachments);

    [[nodiscard]] static QByteArray escapeFromLines(const QByteArray& content);
    [[nodiscard]] static QString sanitizeFolderName(const QString& name);

    QString m_output_dir;
    bool m_one_per_folder;
    QString m_combined_basename;
    QHash<QString, QFile*> m_open_files;
    /// Resolved .mbox paths already in use, so two distinct folders whose names
    /// sanitize to the same file do not silently merge into one mailbox.
    QSet<QString> m_used_file_paths;
    qint64 m_bytes_written = 0;
};

}  // namespace sak
