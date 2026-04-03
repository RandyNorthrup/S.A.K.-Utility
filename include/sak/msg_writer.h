// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file msg_writer.h
/// @brief Writes Microsoft MSG files (OLE2 compound files with MAPI properties)

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <expected>

namespace sak {

/// @brief Writes MS-OXMSG compound files preserving all MAPI properties
class MsgWriter {
public:
    explicit MsgWriter(const QString& output_dir,
                       bool prefix_with_date = true,
                       bool preserve_folders = true);

    /// Write a single message as a .msg compound file
    [[nodiscard]] std::expected<QString, error_code> writeMessage(
        const PstItemDetail& item,
        const QVector<MapiProperty>& all_properties,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& subfolder_path);

    /// Total bytes written across all files
    [[nodiscard]] qint64 totalBytesWritten() const { return m_bytes_written; }

private:
    struct DirEntryLinks {
        int child_id = -1;
        int left_id = -1;
        int right_id = -1;
    };

    void collectMessageStreams(const PstItemDetail& item,
                               const QVector<MapiProperty>& properties,
                               const QVector<QPair<QString, QByteArray>>& attachments,
                               QVector<QByteArray>& streams,
                               QStringList& stream_names);

    [[nodiscard]] QByteArray buildDirectoryData(const QVector<QByteArray>& streams,
                                                const QStringList& stream_names,
                                                const QVector<int>& stream_start_sectors);

    [[nodiscard]] std::expected<void, error_code> createCompoundFile(
        const QString& path,
        const PstItemDetail& item,
        const QVector<MapiProperty>& properties,
        const QVector<QPair<QString, QByteArray>>& attachments);

    [[nodiscard]] QByteArray buildCompoundFileHeader(int total_sectors) const;
    [[nodiscard]] QByteArray buildFatSector(const QVector<int32_t>& fat_entries) const;
    [[nodiscard]] QByteArray buildDirectoryEntry(const QString& name,
                                                 uint8_t type,
                                                 uint32_t start_sector,
                                                 uint32_t size,
                                                 const DirEntryLinks& links) const;
    [[nodiscard]] QByteArray buildPropertyStream(const PstItemDetail& item,
                                                 const QVector<MapiProperty>& properties) const;

    [[nodiscard]] QString sanitizeFilename(const QString& subject, const QDateTime& date) const;

    QString m_output_dir;
    bool m_prefix_with_date;
    bool m_preserve_folders;
    QHash<QString, int> m_filename_counters;
    qint64 m_bytes_written = 0;

    static constexpr int kSectorSize = 512;
    static constexpr int kMiniSectorSize = 64;
    static constexpr uint32_t kEndOfChain = 0xFF'FF'FF'FE;
    static constexpr uint32_t kFreeSector = 0xFF'FF'FF'FF;
    static constexpr uint32_t kFatSector = 0xFF'FF'FF'FD;
};

}  // namespace sak
