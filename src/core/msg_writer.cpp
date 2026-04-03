// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file msg_writer.cpp
/// @brief MS-OXMSG compound file writer implementation

#include "sak/msg_writer.h"

#include "sak/logger.h"
#include "sak/ost_converter_constants.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace sak {

namespace {

// OLE2 CFB magic number
constexpr uint64_t kCfbMagic = 0xE1'1A'B1'A1'E0'11'CF'D0ULL;
constexpr uint16_t kCfbMinorVersion = 0x003E;
constexpr uint16_t kCfbMajorVersion3 = 0x0003;
constexpr uint16_t kByteOrderLittle = 0xFFFE;
constexpr uint16_t kSectorSizePower = 9;
constexpr uint16_t kMiniSectorSizePower = 6;
constexpr uint32_t kMiniStreamCutoff = 4096;

// Directory entry types
constexpr uint8_t kDirTypeEmpty = 0;
constexpr uint8_t kDirTypeStorage = 1;
constexpr uint8_t kDirTypeStream = 2;
constexpr uint8_t kDirTypeRoot = 5;

// MAPI property tags for MSG
constexpr uint16_t kPropSubject = 0x0037;
constexpr uint16_t kPropSenderName = 0x0C1A;
constexpr uint16_t kPropBody = 0x1000;
constexpr uint16_t kPropImportance = 0x0017;
constexpr uint16_t kPropDeliveryTime = 0x0E06;
constexpr uint16_t kPropTypeUnicode = 0x001F;
constexpr uint16_t kPropTypeInt32 = 0x0003;
constexpr uint16_t kPropTypeBinary = 0x0102;

constexpr int kMaxFilenameLength = 200;

}  // namespace

// ============================================================================
// Construction
// ============================================================================

MsgWriter::MsgWriter(const QString& output_dir, bool prefix_with_date, bool preserve_folders)
    : m_output_dir(output_dir)
    , m_prefix_with_date(prefix_with_date)
    , m_preserve_folders(preserve_folders) {}

// ============================================================================
// Public API
// ============================================================================

std::expected<QString, error_code> MsgWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<MapiProperty>& all_properties,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& subfolder_path) {
    QString dir_path = m_output_dir;
    if (m_preserve_folders && !subfolder_path.isEmpty()) {
        dir_path += QStringLiteral("/") + subfolder_path;
    }
    QDir().mkpath(dir_path);

    QString filename = sanitizeFilename(item.subject, item.date);

    // Handle filename collisions
    QString key = dir_path + "/" + filename;
    if (m_filename_counters.contains(key)) {
        int count = ++m_filename_counters[key];
        QFileInfo fi(filename);
        filename = fi.completeBaseName() + QStringLiteral("_%1").arg(count) +
                   QStringLiteral(".msg");
    } else {
        m_filename_counters.insert(key, 1);
    }

    QString full_path = dir_path + QStringLiteral("/") + filename;

    auto result = createCompoundFile(full_path, item, all_properties, attachment_data);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    QFileInfo fi(full_path);
    m_bytes_written += fi.size();

    return full_path;
}

// ============================================================================
// Compound File Creation
// ============================================================================

void MsgWriter::collectMessageStreams(const PstItemDetail& item,
                                      const QVector<MapiProperty>& properties,
                                      const QVector<QPair<QString, QByteArray>>& attachments,
                                      QVector<QByteArray>& streams,
                                      QStringList& stream_names) {
    QByteArray prop_stream = buildPropertyStream(item, properties);
    streams.append(prop_stream);
    stream_names.append(QStringLiteral("__properties_version1.0"));

    if (!item.subject.isEmpty()) {
        QString tag = QStringLiteral("__substg1.0_%1%2")
                          .arg(kPropSubject, 4, 16, QChar('0'))
                          .arg(kPropTypeUnicode, 4, 16, QChar('0'));
        QByteArray data;
        for (const auto& ch : item.subject) {
            uint16_t code = ch.unicode();
            data.append(static_cast<char>(code & 0xFF));
            data.append(static_cast<char>((code >> 8) & 0xFF));
        }
        streams.append(data);
        stream_names.append(tag.toUpper());
    }

    if (!item.body_plain.isEmpty()) {
        QString tag = QStringLiteral("__substg1.0_%1%2")
                          .arg(kPropBody, 4, 16, QChar('0'))
                          .arg(kPropTypeUnicode, 4, 16, QChar('0'));
        QByteArray data;
        for (const auto& ch : item.body_plain) {
            uint16_t code = ch.unicode();
            data.append(static_cast<char>(code & 0xFF));
            data.append(static_cast<char>((code >> 8) & 0xFF));
        }
        streams.append(data);
        stream_names.append(tag.toUpper());
    }

    for (int i = 0; i < attachments.size(); ++i) {
        const auto& [att_name, att_data] = attachments[i];
        QString stream_name = QStringLiteral("__attach_version1.0_#%1").arg(i, 8, 16, QChar('0'));
        streams.append(att_data);
        stream_names.append(stream_name.toUpper());
    }
}

QByteArray MsgWriter::buildDirectoryData(const QVector<QByteArray>& streams,
                                         const QStringList& stream_names,
                                         const QVector<int>& stream_start_sectors) {
    QByteArray dir_data;

    dir_data.append(buildDirectoryEntry(
        QStringLiteral("Root Entry"), kDirTypeRoot, 0, 0, {(streams.size() > 0) ? 1 : -1, -1, -1}));

    for (int i = 0; i < streams.size(); ++i) {
        int right = -1;
        if (i > 0 && i < streams.size() - 1) {
            right = i + 2;
        }
        dir_data.append(buildDirectoryEntry(stream_names[i],
                                            kDirTypeStream,
                                            static_cast<uint32_t>(stream_start_sectors[i]),
                                            static_cast<uint32_t>(streams[i].size()),
                                            {-1, -1, right}));
    }

    int dir_rem = dir_data.size() % kSectorSize;
    if (dir_rem != 0) {
        dir_data.append(QByteArray(kSectorSize - dir_rem, '\0'));
    }
    return dir_data;
}

std::expected<void, error_code> MsgWriter::createCompoundFile(
    const QString& path,
    const PstItemDetail& item,
    const QVector<MapiProperty>& properties,
    const QVector<QPair<QString, QByteArray>>& attachments) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logError("MsgWriter: failed to create file: {}", path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    QVector<QByteArray> streams;
    QStringList stream_names;
    collectMessageStreams(item, properties, attachments, streams, stream_names);

    // Calculate total sectors
    int data_sectors = 0;
    QVector<int> stream_start_sectors;
    for (const auto& s : streams) {
        stream_start_sectors.append(data_sectors);
        int sectors_needed = qMax(1, (s.size() + kSectorSize - 1) / kSectorSize);
        data_sectors += sectors_needed;
    }

    int total_entries = 1 + streams.size();
    int dir_sectors = (total_entries > 4) ? ((total_entries + 3) / 4) : 1;
    int total_sectors = data_sectors + dir_sectors + 1;

    // Build FAT
    QVector<int32_t> fat_entries(total_sectors, static_cast<int32_t>(kFreeSector));
    int sector_idx = 0;
    for (const auto& s : streams) {
        int sectors_needed = qMax(1, (s.size() + kSectorSize - 1) / kSectorSize);
        for (int j = 0; j < sectors_needed - 1; ++j) {
            fat_entries[sector_idx + j] = sector_idx + j + 1;
        }
        fat_entries[sector_idx + sectors_needed - 1] = static_cast<int32_t>(kEndOfChain);
        sector_idx += sectors_needed;
    }

    for (int j = 0; j < dir_sectors - 1; ++j) {
        fat_entries[sector_idx + j] = sector_idx + j + 1;
    }
    fat_entries[sector_idx + dir_sectors - 1] = static_cast<int32_t>(kEndOfChain);
    int dir_start_sector = sector_idx;
    sector_idx += dir_sectors;
    fat_entries[sector_idx] = static_cast<int32_t>(kFatSector);

    // Write header
    QByteArray header = buildCompoundFileHeader(total_sectors);
    {
        QDataStream hs(&header, QIODevice::ReadWrite);
        hs.setByteOrder(QDataStream::LittleEndian);
        hs.device()->seek(48);
        hs << static_cast<int32_t>(dir_start_sector);
    }
    {
        QDataStream hs(&header, QIODevice::ReadWrite);
        hs.setByteOrder(QDataStream::LittleEndian);
        hs.device()->seek(76);
        hs << static_cast<int32_t>(sector_idx);
    }
    file.write(header);

    // Write data sectors
    for (const auto& s : streams) {
        QByteArray padded = s;
        int rem = padded.size() % kSectorSize;
        if (rem != 0) {
            padded.append(QByteArray(kSectorSize - rem, '\0'));
        }
        if (padded.isEmpty()) {
            padded = QByteArray(kSectorSize, '\0');
        }
        file.write(padded);
    }

    file.write(buildDirectoryData(streams, stream_names, stream_start_sectors));
    file.write(buildFatSector(fat_entries));

    file.close();
    return {};
}

QByteArray MsgWriter::buildCompoundFileHeader(int total_sectors) const {
    QByteArray header(kSectorSize, '\0');
    QDataStream ds(&header, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Magic number (8 bytes)
    ds << kCfbMagic;
    // CLSID (16 bytes)
    ds.writeRawData(QByteArray(16, '\0').constData(), 16);
    // Minor version
    ds << kCfbMinorVersion;
    // Major version
    ds << kCfbMajorVersion3;
    // Byte order
    ds << kByteOrderLittle;
    // Sector size power
    ds << kSectorSizePower;
    // Mini sector size power
    ds << kMiniSectorSizePower;
    // Reserved (6 bytes)
    ds.writeRawData(QByteArray(6, '\0').constData(), 6);
    // Total directory sectors (v3 = 0)
    ds << static_cast<int32_t>(0);
    // Total FAT sectors
    ds << static_cast<int32_t>(1);
    // First directory sector SECID (filled later)
    ds << static_cast<int32_t>(0);
    // Transaction signature
    ds << static_cast<int32_t>(0);
    // Mini stream cutoff
    ds << static_cast<uint32_t>(kMiniStreamCutoff);
    // First mini FAT sector
    ds << static_cast<int32_t>(static_cast<int32_t>(kEndOfChain));
    // Total mini FAT sectors
    ds << static_cast<int32_t>(0);
    // First DIFAT sector
    ds << static_cast<int32_t>(static_cast<int32_t>(kEndOfChain));
    // Total DIFAT sectors
    ds << static_cast<int32_t>(0);

    // DIFAT array (109 entries, rest is 0xFFFFFFFF)
    // First entry is the FAT sector (filled later at offset 76)
    for (int i = 0; i < 109; ++i) {
        ds << static_cast<int32_t>(kFreeSector);
    }

    Q_UNUSED(total_sectors);
    return header;
}

QByteArray MsgWriter::buildFatSector(const QVector<int32_t>& fat_entries) const {
    QByteArray sector(kSectorSize, '\xFF');
    QDataStream ds(&sector, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    int count = qMin(fat_entries.size(), kSectorSize / 4);
    for (int i = 0; i < count; ++i) {
        ds << fat_entries[i];
    }

    return sector;
}

QByteArray MsgWriter::buildDirectoryEntry(const QString& name,
                                          uint8_t type,
                                          uint32_t start_sector,
                                          uint32_t size,
                                          const DirEntryLinks& links) const {
    constexpr int kDirEntrySize = 128;
    QByteArray entry(kDirEntrySize, '\0');
    QDataStream ds(&entry, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Entry name (64 bytes max, UTF-16LE)
    int name_len = qMin(name.size(), 31);
    for (int i = 0; i < name_len; ++i) {
        ds << static_cast<uint16_t>(name.at(i).unicode());
    }
    ds << static_cast<uint16_t>(0);  // null terminator

    // Pad to 64 bytes
    int written = (name_len + 1) * 2;
    if (written < 64) {
        ds.writeRawData(QByteArray(64 - written, '\0').constData(), 64 - written);
    }

    // Name size in bytes (including null terminator)
    ds << static_cast<uint16_t>((name_len + 1) * 2);
    // Object type
    ds << type;
    // Color (red-black tree: 1=black)
    ds << static_cast<uint8_t>(1);
    // Left sibling
    ds << static_cast<int32_t>(links.left_id);
    // Right sibling
    ds << static_cast<int32_t>(links.right_id);
    // Child
    ds << static_cast<int32_t>(links.child_id);
    // CLSID (16 bytes)
    ds.writeRawData(QByteArray(16, '\0').constData(), 16);
    // State bits
    ds << static_cast<uint32_t>(0);
    // Created time (8 bytes)
    ds << static_cast<uint64_t>(0);
    // Modified time (8 bytes)
    ds << static_cast<uint64_t>(0);
    // Starting sector
    ds << start_sector;
    // Size
    ds << size;

    return entry;
}

QByteArray MsgWriter::buildPropertyStream(const PstItemDetail& item,
                                          const QVector<MapiProperty>& properties) const {
    // Build the __properties_version1.0 stream
    // Format: 8-byte header + 16-byte property entries (fixed-size props)
    QByteArray stream;
    QDataStream ds(&stream, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Header: reserved (8 bytes)
    ds << static_cast<uint32_t>(0);  // next_recipient_id
    ds << static_cast<uint32_t>(0);  // next_attachment_id

    // Write properties from the MAPI property vector
    for (const auto& prop : properties) {
        // Property tag (type + id)
        ds << prop.tag_type;
        ds << prop.tag_id;
        // Flags
        ds << static_cast<uint32_t>(0x02);  // PROPATTR_READABLE

        // Value (8 bytes, padded)
        if (prop.raw_value.size() <= 8) {
            QByteArray padded = prop.raw_value;
            padded.resize(8, '\0');
            ds.writeRawData(padded.constData(), 8);
        } else {
            // Variable-length: store size
            ds << static_cast<uint32_t>(prop.raw_value.size());
            ds << static_cast<uint32_t>(0);
        }
    }

    Q_UNUSED(item);
    return stream;
}

// ============================================================================
// Helpers
// ============================================================================

QString MsgWriter::sanitizeFilename(const QString& subject, const QDateTime& date) const {
    QString base = subject.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("no_subject");
    }

    // Remove invalid filename characters
    static const QRegularExpression kInvalidChars(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    base.replace(kInvalidChars, QStringLiteral("_"));

    if (base.size() > kMaxFilenameLength) {
        base.truncate(kMaxFilenameLength);
    }

    if (m_prefix_with_date && date.isValid()) {
        base = date.toString(QStringLiteral("yyyy-MM-dd_")) + base;
    }

    return base + QStringLiteral(".msg");
}

}  // namespace sak
