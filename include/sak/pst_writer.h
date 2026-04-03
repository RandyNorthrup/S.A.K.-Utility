// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_writer.h
/// @brief Creates new PST files from extracted mailbox data (MS-PST format)

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"

#include <QFile>
#include <QHash>
#include <QString>
#include <QVector>

#include <cstdint>
#include <expected>
#include <memory>

namespace sak {

/// @brief Creates a new Unicode PST file with NDB/LTP/Messaging layers
class PstWriter {
public:
    explicit PstWriter(const QString& output_path);
    ~PstWriter();

    PstWriter(const PstWriter&) = delete;
    PstWriter& operator=(const PstWriter&) = delete;
    PstWriter(PstWriter&&) = delete;
    PstWriter& operator=(PstWriter&&) = delete;

    /// Initialize a new Unicode PST file with empty B-Trees
    [[nodiscard]] std::expected<void, error_code> create();

    /// Set the message store display name
    void setDisplayName(const QString& name);

    /// Create a folder in the hierarchy
    [[nodiscard]] std::expected<uint64_t, error_code> createFolder(uint64_t parent_nid,
                                                                   const QString& name,
                                                                   const QString& container_class);

    /// Write a message into a folder
    [[nodiscard]] std::expected<void, error_code> writeMessage(
        uint64_t folder_nid,
        const PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data);

    /// Finalize and close the PST file (write final B-Trees, header CRC)
    [[nodiscard]] std::expected<void, error_code> finalize();

    /// Current file size in bytes
    [[nodiscard]] qint64 currentSize() const;

    /// Whether the writer is open and ready
    [[nodiscard]] bool isOpen() const;

    // Well-known NIDs
    static constexpr uint64_t kNidMessageStore = 0x21;
    static constexpr uint64_t kNidRootFolder = 0x122;
    static constexpr uint64_t kNidNameToIdMap = 0x61;

    // PST header constants
    static constexpr uint32_t kPstMagic = 0x4E'44'42'21;  // !BDN
    static constexpr uint32_t kPstClientMagic = 0x4D53;   // SM (PST)
    static constexpr uint16_t kVersionUnicode = 23;
    static constexpr uint16_t kPageSize = 512;

private:
    // NDB layer helpers
    void writeHeader();
    void writeEmptyBTrees();
    uint64_t allocateBlock(const QByteArray& data);
    void updateNodeBTree();
    void updateBlockBTree();

    // LTP layer helpers
    QByteArray buildPropertyContext(const QVector<QPair<uint16_t, QByteArray>>& properties);
    QByteArray buildHeapOnNode(const QByteArray& data);

    // Messaging layer helpers
    void writeMessageStore();
    void writeFolderNode(uint64_t nid, const QString& name, const QString& container_class);
    void writeMessageNode(uint64_t folder_nid,
                          const PstItemDetail& item,
                          const QVector<QPair<QString, QByteArray>>& attachments);

    QVector<QPair<uint16_t, QByteArray>> buildMessageProperties(const PstItemDetail& item);

    // CRC calculation (MS-PST CRC-32)
    [[nodiscard]] static uint32_t computeCrc32(const QByteArray& data);

    struct NbtEntry {
        uint64_t nid = 0;
        uint64_t bid_data = 0;
        uint64_t bid_sub = 0;
        uint64_t parent_nid = 0;
    };

    struct BbtEntry {
        uint64_t bid = 0;
        uint64_t offset = 0;
        uint32_t size = 0;
        uint32_t crc = 0;
    };

    QFile m_file;
    QString m_output_path;
    QString m_display_name;
    bool m_is_open = false;

    uint64_t m_next_nid = 0x100;
    uint64_t m_next_bid = 0x100;
    qint64 m_write_offset = 0;

    QVector<NbtEntry> m_nbt_entries;
    QVector<BbtEntry> m_bbt_entries;
    QHash<uint64_t, uint64_t> m_folder_nids;


    static constexpr uint16_t kBlockAlignment = 64;
};

}  // namespace sak
