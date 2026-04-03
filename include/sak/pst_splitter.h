// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_splitter.h
/// @brief Wraps PstWriter to split output across multiple PST volumes

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QHash>
#include <QString>

#include <cstdint>
#include <expected>
#include <memory>

namespace sak {

class PstWriter;

/// @brief Splits PST output across multiple volume files
class PstSplitter {
public:
    PstSplitter(const QString& base_path, qint64 max_size_bytes);
    ~PstSplitter();

    PstSplitter(const PstSplitter&) = delete;
    PstSplitter& operator=(const PstSplitter&) = delete;
    PstSplitter(PstSplitter&&) = delete;
    PstSplitter& operator=(PstSplitter&&) = delete;

    /// Initialize the first volume
    [[nodiscard]] std::expected<void, error_code> create();

    /// Create a folder — recreated in each new volume automatically
    [[nodiscard]] std::expected<uint64_t, error_code> createFolder(uint64_t parent_nid,
                                                                   const QString& name,
                                                                   const QString& container_class);

    /// Write a message — automatically rotates to next volume if needed
    [[nodiscard]] std::expected<void, error_code> writeMessage(
        uint64_t folder_nid,
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data);

    /// Finalize all open volumes
    [[nodiscard]] std::expected<void, error_code> finalizeAll();

    /// Number of volumes created so far
    [[nodiscard]] int volumeCount() const;

    /// Total bytes written across all volumes
    [[nodiscard]] qint64 totalBytesWritten() const;

private:
    [[nodiscard]] std::expected<void, error_code> rotateVolume();
    [[nodiscard]] QString volumePath(int index) const;

    struct FolderInfo {
        uint64_t parent_nid = 0;
        QString name;
        QString container_class;
    };

    QString m_base_path;
    qint64 m_max_size;
    int m_volume_index = 0;
    qint64 m_total_bytes_written = 0;
    std::unique_ptr<PstWriter> m_current_writer;
    QHash<uint64_t, FolderInfo> m_folder_info;
    QHash<uint64_t, uint64_t> m_folder_nid_map;
};

}  // namespace sak
