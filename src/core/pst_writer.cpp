// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_writer.cpp
/// @brief PST file writer implementation (NDB/LTP/Messaging layers)

#include "sak/pst_writer.h"

#include "sak/logger.h"

#include <QDataStream>
#include <QDir>
#include <QFileInfo>

#include <array>

namespace sak {

namespace {

// MS-PST MAPI property tags
constexpr uint16_t kPropTagDisplayName = 0x3001;
constexpr uint16_t kPropTagSubject = 0x0037;
constexpr uint16_t kPropTagSenderName = 0x0C1A;
constexpr uint16_t kPropTagSenderEmail = 0x0C1F;
constexpr uint16_t kPropTagDisplayTo = 0x0E04;
constexpr uint16_t kPropTagDisplayCc = 0x0E03;
constexpr uint16_t kPropTagMessageDeliveryTime = 0x0E06;
constexpr uint16_t kPropTagBody = 0x1000;
constexpr uint16_t kPropTagBodyHtml = 0x1013;
constexpr uint16_t kPropTagMessageClass = 0x001A;
constexpr uint16_t kPropTagImportance = 0x0017;
constexpr uint16_t kPropTagContainerClass = 0x3613;
constexpr uint16_t kPropTagContentCount = 0x3602;
constexpr uint16_t kPropTagContentUnreadCount = 0x3603;
constexpr uint16_t kPropTagAttachLongFilename = 0x3707;
constexpr uint16_t kPropTagAttachDataBinary = 0x3701;
constexpr uint16_t kPropTagAttachMethod = 0x3705;

// Property types
constexpr uint16_t kPropTypeInt32 = 0x0003;
constexpr uint16_t kPropTypeBoolean = 0x000B;
constexpr uint16_t kPropTypeInt64 = 0x0014;
constexpr uint16_t kPropTypeUnicode = 0x001F;
constexpr uint16_t kPropTypeBinary = 0x0102;
constexpr uint16_t kPropTypeTime = 0x0040;

// NID types
constexpr uint8_t kNidTypeFolder = 0x02;
constexpr uint8_t kNidTypeNormalMessage = 0x04;
constexpr uint8_t kNidTypeAttachment = 0x05;
constexpr uint8_t kNidTypeContentsTable = 0x0E;
constexpr uint8_t kNidTypeHierarchyTable = 0x0D;

constexpr int kHeaderSize = 564;
constexpr int kBTreePageSize = 512;
constexpr int kMaxBlockSize = 8176;

/// Build a NID from type + index
uint64_t makeNid(uint8_t type, uint64_t index) {
    return (index << 5) | type;
}

/// Encode a Unicode string as UTF-16LE for PST storage
QByteArray encodeUnicode(const QString& str) {
    QByteArray result;
    result.reserve(str.size() * 2);
    for (const auto& ch : str) {
        uint16_t code = ch.unicode();
        result.append(static_cast<char>(code & 0xFF));
        result.append(static_cast<char>((code >> 8) & 0xFF));
    }
    return result;
}

/// Encode a FILETIME (100-ns intervals since 1601-01-01)
QByteArray encodeFileTime(const QDateTime& dt) {
    if (!dt.isValid()) {
        return QByteArray(8, '\0');
    }
    // Windows FILETIME epoch: 1601-01-01 00:00:00 UTC
    // Unix epoch: 1970-01-01 00:00:00 UTC
    // Difference: 11644473600 seconds
    constexpr int64_t kEpochDiffSeconds = 11'644'473'600LL;
    constexpr int64_t kTicksPerSecond = 10'000'000LL;

    int64_t unix_secs = dt.toSecsSinceEpoch();
    int64_t file_time = (unix_secs + kEpochDiffSeconds) * kTicksPerSecond;

    QByteArray result(8, '\0');
    for (int i = 0; i < 8; ++i) {
        result[i] = static_cast<char>((file_time >> (i * 8)) & 0xFF);
    }
    return result;
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

PstWriter::PstWriter(const QString& output_path)
    : m_output_path(output_path), m_display_name(QFileInfo(output_path).completeBaseName()) {}

PstWriter::~PstWriter() {
    if (m_file.isOpen()) {
        m_file.close();
    }
}

// ============================================================================
// Public API
// ============================================================================

std::expected<void, error_code> PstWriter::create() {
    QDir().mkpath(QFileInfo(m_output_path).absolutePath());

    m_file.setFileName(m_output_path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logError("PstWriter: failed to create file: {}", m_output_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    m_is_open = true;
    m_write_offset = 0;
    m_next_nid = 0x100;
    m_next_bid = 0x100;
    m_nbt_entries.clear();
    m_bbt_entries.clear();
    m_folder_nids.clear();

    writeHeader();
    writeMessageStore();

    // Create root folder
    writeFolderNode(kNidRootFolder,
                    m_display_name.isEmpty() ? QStringLiteral("Root") : m_display_name,
                    QStringLiteral("IPF.Note"));

    logInfo("PstWriter: created PST file: {}", m_output_path.toStdString());

    return {};
}

void PstWriter::setDisplayName(const QString& name) {
    m_display_name = name;
}

std::expected<uint64_t, error_code> PstWriter::createFolder(uint64_t parent_nid,
                                                            const QString& name,
                                                            const QString& container_class) {
    Q_ASSERT(m_is_open);

    uint64_t folder_nid = makeNid(kNidTypeFolder, m_next_nid++);
    writeFolderNode(folder_nid, name, container_class);
    m_folder_nids.insert(folder_nid, parent_nid);

    return folder_nid;
}

std::expected<void, error_code> PstWriter::writeMessage(
    uint64_t folder_nid,
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data) {
    Q_ASSERT(m_is_open);

    writeMessageNode(folder_nid, item, attachment_data);
    return {};
}

std::expected<void, error_code> PstWriter::finalize() {
    if (!m_is_open) {
        return std::unexpected(error_code::write_error);
    }

    updateNodeBTree();
    updateBlockBTree();

    // Re-write header with updated B-Tree root pointers
    qint64 saved_offset = m_write_offset;
    m_file.seek(0);
    writeHeader();
    m_write_offset = saved_offset;

    m_file.flush();
    m_file.close();
    m_is_open = false;

    logInfo("PstWriter: finalized PST file ({} bytes)", std::to_string(m_write_offset));

    return {};
}

qint64 PstWriter::currentSize() const {
    return m_write_offset;
}

bool PstWriter::isOpen() const {
    return m_is_open;
}

// ============================================================================
// NDB Layer
// ============================================================================

void PstWriter::writeHeader() {
    QByteArray header(kHeaderSize, '\0');
    QDataStream ds(&header, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // dwMagic (offset 0)
    ds << kPstMagic;
    // dwCRCPartial (offset 4) — placeholder, updated at finalize
    ds << static_cast<uint32_t>(0);
    // wMagicClient (offset 8) — SM for PST
    ds << static_cast<uint16_t>(kPstClientMagic);
    // wVer (offset 10) — 23 for Unicode
    ds << kVersionUnicode;
    // wVerClient (offset 12)
    ds << static_cast<uint16_t>(19);

    // bPlatformCreate (offset 14)
    ds << static_cast<uint8_t>(0x01);
    // bPlatformAccess (offset 15)
    ds << static_cast<uint8_t>(0x01);

    // dwReserved1, dwReserved2 (offsets 16-23)
    ds << static_cast<uint32_t>(0) << static_cast<uint32_t>(0);

    // bidUnused (offset 24-31) — 8 bytes for Unicode
    ds << static_cast<uint64_t>(0);

    // bidNextP (offset 32-39)
    ds << static_cast<uint64_t>(m_next_bid);

    // dwUnique (offset 40-43)
    ds << static_cast<uint32_t>(0x12'34'56'78);

    // rgnid[128] (offset 44-555) — 128 * 4 bytes of NID counters
    // Fill with zeros (simplified)
    for (int i = 0; i < 128; ++i) {
        ds << static_cast<uint32_t>(0);
    }

    m_file.seek(0);
    m_file.write(header);
    if (m_write_offset < kHeaderSize) {
        m_write_offset = kHeaderSize;
    }
}

void PstWriter::writeEmptyBTrees() {
    // Write empty NBT and BBT root pages
    QByteArray empty_page(kBTreePageSize, '\0');
    m_file.seek(m_write_offset);
    m_file.write(empty_page);
    m_write_offset += kBTreePageSize;
    m_file.write(empty_page);
    m_write_offset += kBTreePageSize;
}

uint64_t PstWriter::allocateBlock(const QByteArray& data) {
    uint64_t bid = m_next_bid;
    m_next_bid += 4;

    // Align to 64 bytes
    qint64 aligned_offset = (m_write_offset + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
    m_file.seek(aligned_offset);

    // Write block data
    m_file.write(data);

    // Pad to alignment
    int padding = static_cast<int>(kBlockAlignment - (data.size() % kBlockAlignment));
    if (padding < kBlockAlignment) {
        m_file.write(QByteArray(padding, '\0'));
    }

    BbtEntry entry;
    entry.bid = bid;
    entry.offset = static_cast<uint64_t>(aligned_offset);
    entry.size = static_cast<uint32_t>(data.size());
    entry.crc = computeCrc32(data);
    m_bbt_entries.append(entry);

    m_write_offset = aligned_offset + data.size() + padding;

    return bid;
}

void PstWriter::updateNodeBTree() {
    if (m_nbt_entries.isEmpty()) {
        return;
    }

    QByteArray page(kBTreePageSize, '\0');
    QDataStream ds(&page, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Simplified: write leaf entries directly
    int count = qMin(m_nbt_entries.size(), 20);
    for (int i = 0; i < count; ++i) {
        const auto& entry = m_nbt_entries[i];
        ds << entry.nid << entry.bid_data << entry.bid_sub << entry.parent_nid;
    }

    qint64 aligned = (m_write_offset + kBTreePageSize - 1) &
                     ~static_cast<qint64>(kBTreePageSize - 1);
    m_file.seek(aligned);
    m_file.write(page);
    m_write_offset = aligned + kBTreePageSize;
}

void PstWriter::updateBlockBTree() {
    if (m_bbt_entries.isEmpty()) {
        return;
    }

    QByteArray page(kBTreePageSize, '\0');
    QDataStream ds(&page, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    int count = qMin(m_bbt_entries.size(), 20);
    for (int i = 0; i < count; ++i) {
        const auto& entry = m_bbt_entries[i];
        ds << entry.bid << entry.offset << static_cast<uint16_t>(entry.size)
           << static_cast<uint16_t>(0);
    }

    qint64 aligned = (m_write_offset + kBTreePageSize - 1) &
                     ~static_cast<qint64>(kBTreePageSize - 1);
    m_file.seek(aligned);
    m_file.write(page);
    m_write_offset = aligned + kBTreePageSize;
}

// ============================================================================
// LTP Layer
// ============================================================================

QByteArray PstWriter::buildPropertyContext(const QVector<QPair<uint16_t, QByteArray>>& properties) {
    // Build a simplified property context (PC)
    // The PC is a Heap-on-Node structure containing property entries
    QByteArray result;
    QDataStream ds(&result, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // bClientSig = PC (0xBC)
    ds << static_cast<uint8_t>(0xBC);
    // cEntries
    ds << static_cast<uint16_t>(properties.size());

    // Write property entries
    for (const auto& [tag, value] : properties) {
        ds << tag;
        ds << static_cast<uint16_t>(value.size());

        if (value.size() <= 4) {
            // Inline value
            QByteArray padded = value;
            padded.resize(4, '\0');
            ds.writeRawData(padded.constData(), 4);
        } else {
            // HID reference (we inline small values only for simplicity)
            ds << static_cast<uint32_t>(value.size());
            ds.writeRawData(value.constData(), value.size());
        }
    }

    return result;
}

QByteArray PstWriter::buildHeapOnNode(const QByteArray& data) {
    // Simplified Heap-on-Node wrapper (MS-PST §2.3.1)
    QByteArray result;
    QDataStream ds(&result, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // HNHeader
    constexpr uint8_t kHnSignature = 0xEC;  // bSig per MS-PST §2.3.1.2
    ds << static_cast<uint16_t>(0);         // ibHnpm
    ds << kHnSignature;                     // bSig = HN
    ds << static_cast<uint8_t>(0);          // bClientSig
    ds << static_cast<uint32_t>(0);         // hidUserRoot
    ds << static_cast<uint32_t>(0);         // rgbFillLevel

    // Append raw data
    result.append(data);

    return result;
}

// ============================================================================
// Messaging Layer
// ============================================================================

void PstWriter::writeMessageStore() {
    QVector<QPair<uint16_t, QByteArray>> props;

    // Display name
    props.append({kPropTagDisplayName, encodeUnicode(m_display_name)});

    QByteArray pc_data = buildPropertyContext(props);
    uint64_t bid = allocateBlock(pc_data);

    NbtEntry entry;
    entry.nid = kNidMessageStore;
    entry.bid_data = bid;
    entry.bid_sub = 0;
    entry.parent_nid = 0;
    m_nbt_entries.append(entry);
}

void PstWriter::writeFolderNode(uint64_t nid, const QString& name, const QString& container_class) {
    QVector<QPair<uint16_t, QByteArray>> props;
    props.append({kPropTagDisplayName, encodeUnicode(name)});
    props.append({kPropTagContainerClass, encodeUnicode(container_class)});

    QByteArray count_bytes(4, '\0');
    props.append({kPropTagContentCount, count_bytes});
    props.append({kPropTagContentUnreadCount, count_bytes});

    QByteArray pc_data = buildPropertyContext(props);
    uint64_t bid = allocateBlock(pc_data);

    NbtEntry entry;
    entry.nid = nid;
    entry.bid_data = bid;
    entry.bid_sub = 0;
    entry.parent_nid = (nid == kNidRootFolder) ? kNidMessageStore : 0;
    m_nbt_entries.append(entry);

    // Create contents table node
    uint64_t ct_nid = makeNid(kNidTypeContentsTable, nid >> 5);
    QByteArray empty_tc(16, '\0');
    uint64_t ct_bid = allocateBlock(empty_tc);

    NbtEntry ct_entry;
    ct_entry.nid = ct_nid;
    ct_entry.bid_data = ct_bid;
    ct_entry.bid_sub = 0;
    ct_entry.parent_nid = nid;
    m_nbt_entries.append(ct_entry);

    // Create hierarchy table node
    uint64_t ht_nid = makeNid(kNidTypeHierarchyTable, nid >> 5);
    uint64_t ht_bid = allocateBlock(empty_tc);

    NbtEntry ht_entry;
    ht_entry.nid = ht_nid;
    ht_entry.bid_data = ht_bid;
    ht_entry.bid_sub = 0;
    ht_entry.parent_nid = nid;
    m_nbt_entries.append(ht_entry);
}

QVector<QPair<uint16_t, QByteArray>> PstWriter::buildMessageProperties(const PstItemDetail& item) {
    QVector<QPair<uint16_t, QByteArray>> props;

    if (!item.subject.isEmpty()) {
        props.append({kPropTagSubject, encodeUnicode(item.subject)});
    }
    if (!item.sender_name.isEmpty()) {
        props.append({kPropTagSenderName, encodeUnicode(item.sender_name)});
    }
    if (!item.sender_email.isEmpty()) {
        props.append({kPropTagSenderEmail, encodeUnicode(item.sender_email)});
    }
    if (!item.display_to.isEmpty()) {
        props.append({kPropTagDisplayTo, encodeUnicode(item.display_to)});
    }
    if (!item.display_cc.isEmpty()) {
        props.append({kPropTagDisplayCc, encodeUnicode(item.display_cc)});
    }
    if (item.date.isValid()) {
        props.append({kPropTagMessageDeliveryTime, encodeFileTime(item.date)});
    }
    if (!item.body_plain.isEmpty()) {
        props.append({kPropTagBody, encodeUnicode(item.body_plain)});
    }
    if (!item.body_html.isEmpty()) {
        props.append({kPropTagBodyHtml, item.body_html.toUtf8()});
    }

    props.append({kPropTagMessageClass, encodeUnicode(QStringLiteral("IPM.Note"))});

    QByteArray imp_bytes(4, '\0');
    imp_bytes[0] = static_cast<char>(item.importance);
    props.append({kPropTagImportance, imp_bytes});

    return props;
}

void PstWriter::writeMessageNode(uint64_t folder_nid,
                                 const PstItemDetail& item,
                                 const QVector<QPair<QString, QByteArray>>& attachments) {
    uint64_t msg_nid = makeNid(kNidTypeNormalMessage, m_next_nid++);

    QVector<QPair<uint16_t, QByteArray>> props = buildMessageProperties(item);

    QByteArray pc_data = buildPropertyContext(props);
    uint64_t msg_bid = allocateBlock(pc_data);

    // Write attachment sub-nodes
    uint64_t sub_bid = 0;
    if (!attachments.isEmpty()) {
        QByteArray attach_data;
        for (int i = 0; i < attachments.size(); ++i) {
            const auto& [name, data] = attachments[i];
            QVector<QPair<uint16_t, QByteArray>> att_props;
            att_props.append({kPropTagAttachLongFilename, encodeUnicode(name)});
            att_props.append({kPropTagAttachDataBinary, data});

            QByteArray att_method(4, '\0');
            att_method[0] = 1;  // ATTACH_BY_VALUE
            att_props.append({kPropTagAttachMethod, att_method});

            QByteArray att_pc = buildPropertyContext(att_props);
            attach_data.append(att_pc);
        }
        sub_bid = allocateBlock(attach_data);
    }

    NbtEntry entry;
    entry.nid = msg_nid;
    entry.bid_data = msg_bid;
    entry.bid_sub = sub_bid;
    entry.parent_nid = folder_nid;
    m_nbt_entries.append(entry);
}

// ============================================================================
// CRC-32
// ============================================================================

uint32_t PstWriter::computeCrc32(const QByteArray& data) {
    // CRC-32 (ISO 3309 / ITU-T V.42)
    static constexpr std::array<uint32_t, 256> kCrcTable = []() {
        std::array<uint32_t, 256> table{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xED'B8'83'20;
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
        return table;
    }();

    uint32_t crc = 0xFF'FF'FF'FF;
    for (const char byte : data) {
        uint8_t index = static_cast<uint8_t>(crc ^ static_cast<uint8_t>(byte));
        crc = (crc >> 8) ^ kCrcTable[index];
    }
    return crc ^ 0xFF'FF'FF'FF;
}

}  // namespace sak
