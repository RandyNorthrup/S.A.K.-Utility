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
#include <QStringList>
#include <QVector>

#include <expected>
#include <memory>

class TestMboxWriter;

namespace sak {

/// @brief Writes Unix mbox files from PST item data
class MboxWriter {
    // Test seam, matching the convention used across this tree (DriveScannerTests,
    // FileScannerTests, PackageMatcherTests, ...). sanitizeFolderName is a pure private static
    // whose reserved-device-name arm was PROVEN UNTESTED by a mutation drill: breaking the shared
    // sak::isWindowsReservedName turned every other migrated suite red and left this one green.
    // Granting the test access is behaviour-preserving; widening the public API to reach it would
    // not be.
    friend class ::TestMboxWriter;

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

    /// Flush and close all open file handles. The writer is TERMINAL afterwards: a later write
    /// would reopen (and truncate) a finalized mailbox, so it fails closed instead.
    void finalize();

    /// True once any write, flush or close failed, i.e. the mailbox on disk is not what the run
    /// produced. A caller that finalized must treat the export as failed when this is set.
    [[nodiscard]] bool hadFailure() const { return m_failed; }

    /// Total bytes written across all files
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    [[nodiscard]] QFile* getOrCreateFile(const QString& folder_path);

    /// Hash key identifying the mailbox a folder writes into.
    [[nodiscard]] QString fileKey(const QString& folder_path) const;

    /// Resolve the on-disk .mbox path for a folder, disambiguated against the paths already in use.
    [[nodiscard]] QString resolveFilePath(const QString& folder_path) const;

    /// Open (truncating on this run's first open, appending after a handle-cap eviction) and record
    /// the handle. Null on any failure, which callers must surface.
    [[nodiscard]] QFile* openMboxFile(const QString& key, const QString& file_path);

    /// Flush, close and delete one handle. False when a deferred I/O error means the mailbox on
    /// disk is incomplete.
    bool closeOneFile(QFile* file, const QString& key);

    /// Enforce the open-handle cap by closing least-recently-used mailboxes.
    void evictOldestOpenFiles();

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
    /// @param alt_boundary Delimiter of the nested multipart/alternative that carries the plain and
    ///        HTML alternatives when the message also has attachments; empty when not nested.
    static void appendMultipartBody(QByteArray& message,
                                    const PstItemDetail& item,
                                    const QString& boundary,
                                    const QString& alt_boundary,
                                    const QVector<QPair<QString, QByteArray>>& attachments);

    [[nodiscard]] static QByteArray escapeFromLines(const QByteArray& content);
    [[nodiscard]] static QString sanitizeFolderName(const QString& name);

    QString m_output_dir;
    bool m_one_per_folder;
    QString m_combined_basename;
    QHash<QString, QFile*> m_open_files;
    /// Keys of m_open_files in least-recently-used order, so the handle cap evicts the coldest.
    QStringList m_open_order;
    /// Key -> resolved .mbox path, so a mailbox evicted by the handle cap reopens the same file
    /// instead of resolving to a fresh suffixed name.
    QHash<QString, QString> m_key_paths;
    /// Case-folded .mbox paths already in use, so two distinct folders whose names sanitize to the
    /// same file (or differ only in case, which Windows resolves to ONE file) do not silently merge
    /// into one mailbox or truncate each other.
    QSet<QString> m_used_file_paths;
    /// Case-folded paths already truncated during this run; a reopen after eviction appends.
    QSet<QString> m_truncated_paths;
    /// Keys whose mailbox was left incomplete by a failed write/flush. Writing more into one would
    /// hide a half-written record behind valid-looking mail, so those writes fail closed.
    QSet<QString> m_failed_keys;
    qint64 m_bytes_written = 0;
    bool m_finalized = false;
    bool m_failed = false;
};

}  // namespace sak
