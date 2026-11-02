// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_store_builder.h
/// @brief Builds a whole spec-conformant Unicode PST from a description of its folders and mail.
///
/// pst_fixture.h makes tiny stores one structure at a time, for walking the parser's code paths.
/// This builds what Outlook would write for a small mailbox: a message store with its Name-to-ID
/// map and IPM subtree pointer, a folder tree, table contexts, messages with bodies in sub-nodes,
/// attachments, attached emails, and enough B-tree pages to hold them. Everything it stamps
/// (header and page CRCs, block trailers, signatures) is what the parser authenticates.
///
/// Used by the tests that need a realistic store and by tests/tools/make_sample_pst.cpp, which
/// writes the fictional harbor-lane-sample.pst for the wiki's screenshots.

#pragma once

#include "sak/email_constants.h"

#include "pst_fixture.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QTimeZone>
#include <QVector>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sak::pst_fixture {

inline constexpr int kBuilderHeapValueLimit = 3580;  // HN allocation limit; bigger values sub-node
inline constexpr int kBuilderBlockDataLimit = 8176;  // Largest external data block
inline constexpr int kBuilderXblockEntries = 1021;   // BIDs one XBLOCK holds
inline constexpr int kBuilderNbtPerPage = 15;        // 488 bytes / 32
inline constexpr int kBuilderBbtPerPage = 20;        // 488 bytes / 24
inline constexpr int kBuilderIndexEntrySize = 24;    // key(8) + BREF(16)
inline constexpr int kBuilderIndexChildOffset = 16;  // BREF.ib within an index entry
inline constexpr int kBuilderIndexPerPage = 20;      // 488 bytes / 24
inline constexpr uint16_t kBuilderPropTypeObject = 0x000D;
inline constexpr uint16_t kBuilderNidTypeLtp = 0x1F;
inline constexpr uint32_t kBuilderMsgFlagRead = 0x01;
inline constexpr uint32_t kBuilderMsgFlagHasAttach = 0x10;

/// One property in a property context.
struct BuilderProp {
    uint16_t id = 0;
    uint16_t type = 0;
    QByteArray value;
};

inline QByteArray builderUnicode(const QString& text) {
    QByteArray bytes(reinterpret_cast<const char*>(text.utf16()),
                     static_cast<qsizetype>(text.size()) * 2);
    return bytes;
}

inline QByteArray builderInt32(uint32_t value) {
    QByteArray bytes(4, '\0');
    writeLe32(bytes, 0, value);
    return bytes;
}

inline QByteArray builderFiletime(const QDateTime& when) {
    constexpr int64_t kEpochDiff = 116'444'736'000'000'000LL;
    constexpr int64_t kTicksPerMs = 10'000;
    QByteArray bytes(8, '\0');
    writeLe64(bytes,
              0,
              static_cast<uint64_t>((when.toMSecsSinceEpoch() * kTicksPerMs) + kEpochDiff));
    return bytes;
}

inline QByteArray builderDouble(double value) {
    QByteArray bytes(8, '\0');
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

/// A named property, stated by set and long id; the builder assigns its store-local property id.
struct BuilderNamedProp {
    uint8_t set_byte = 0;  ///< xx of {000620xx-0000-0000-C000-000000000046}
    uint32_t long_id = 0;
    uint16_t type = 0;
    QByteArray value;
};

struct BuilderAttachment {
    QString name;
    QString mime_type;
    QByteArray data;
    QString content_id;
    bool ole_object = false;  ///< Store @c data the way an OLE attachment (method 6) is stored
};

struct BuilderMessage {
    QString message_class = QStringLiteral("IPM.Note");
    QString subject;
    QString sender_name;
    QString sender_email;
    QString display_to;
    QString display_cc;
    QDateTime date;  ///< Invalid: no delivery time, as an unsent draft has
    QDateTime created;
    QString body_plain;
    QString body_html;
    QByteArray body_rtf_compressed;
    QString internet_message_id;
    bool read = true;
    QVector<BuilderAttachment> attachments;
    QVector<BuilderMessage> attached_messages;  ///< Emails attached to this one
    QVector<BuilderProp> extra_props;
    QVector<BuilderNamedProp> named_props;
    bool data_bid_reserved_bit = false;  ///< Set MS-PST's ignored bit 0 on the NBT data reference
};

struct BuilderFolder {
    QString name;
    QString container_class = QStringLiteral("IPF.Note");
    QVector<BuilderMessage> messages;
    QVector<BuilderFolder> children;
    int declared_count = -1;     ///< PR_CONTENT_COUNT to state; -1 states the real count
    bool minimal_table = false;  ///< Contents table with only the row-id column (for large folders)
};

class PstStoreBuilder {
public:
    QString store_name = QStringLiteral("Sample Store");
    QString ipm_subtree_name = QStringLiteral("Top of Personal Folders");
    bool bbt_keys_reserved_bit = false;       ///< Record every BBT key with the ignored bit 0 set
    QVector<BuilderMessage> orphan_messages;  ///< In the NBT but in no folder: hard-deleted mail

    /// Build a store whose IPM subtree holds @p folders.
    [[nodiscard]] QByteArray build(const QVector<BuilderFolder>& folders) {
        m_blocks.clear();
        m_nodes.clear();
        m_next_external_bid = 0x100;
        m_next_internal_bid = 0x102;
        m_next_folder_index = 0x10;
        m_next_message_index = 0x400;
        m_next_ltp_index = 0x10;
        m_named_ids.clear();
        collectNamedProps(folders);

        const uint32_t subtree_nid = allocateFolderNid();
        BuilderFolder subtree;
        subtree.name = ipm_subtree_name;
        subtree.children = folders;

        addFolderNode(subtree, subtree_nid, kRootFolderNid);
        addRootNode(subtree_nid);
        for (const BuilderMessage& orphan : orphan_messages) {
            addMessageNode(orphan, allocateMessageNid(), 0);
        }
        addStoreNode(subtree_nid);
        addNameToIdNode();
        return assembleFile();
    }

private:
    struct Block {
        uint64_t bid = 0;
        QByteArray data;
    };
    struct Node {
        uint64_t nid = 0;
        uint64_t data_bid = 0;
        uint64_t sub_bid = 0;
        uint32_t parent = 0;
    };
    struct SubEntry {
        uint64_t nid = 0;
        uint64_t data_bid = 0;
        uint64_t sub_bid = 0;
    };

    std::vector<Block> m_blocks;
    std::vector<Node> m_nodes;
    uint64_t m_next_external_bid = 0;
    uint64_t m_next_internal_bid = 0;
    uint32_t m_next_folder_index = 0;
    uint32_t m_next_message_index = 0;
    uint32_t m_next_ltp_index = 0;
    QVector<BuilderNamedProp> m_named_ids;  ///< Position N is property id 0x8000 + N

    uint32_t allocateFolderNid() {
        return (m_next_folder_index++ << 5) | sak::email::kNidTypeNormalFolder;
    }
    uint32_t allocateMessageNid() {
        return (m_next_message_index++ << 5) | sak::email::kNidTypeNormalMessage;
    }
    uint32_t allocateAttachmentNid() {
        return (m_next_message_index++ << 5) | sak::email::kNidTypeAttachment;
    }
    uint32_t allocateLtpNid() { return (m_next_ltp_index++ << 5) | kBuilderNidTypeLtp; }

    uint64_t addExternalBlock(const QByteArray& data) {
        const uint64_t bid = m_next_external_bid;
        m_next_external_bid += 4;
        m_blocks.push_back({bid, data});
        return bid;
    }

    uint64_t addInternalBlock(const QByteArray& data) {
        const uint64_t bid = m_next_internal_bid;
        m_next_internal_bid += 4;
        m_blocks.push_back({bid, data});
        return bid;
    }

    /// Store @p data as one external block, or as an XBLOCK over several when it does not fit.
    uint64_t addDataTree(const QByteArray& data) {
        if (data.size() <= kBuilderBlockDataLimit) {
            return addExternalBlock(data);
        }
        QVector<uint64_t> children;
        for (qsizetype pos = 0; pos < data.size(); pos += kBuilderBlockDataLimit) {
            children.append(addExternalBlock(data.mid(pos, kBuilderBlockDataLimit)));
        }
        Q_ASSERT(children.size() <= kBuilderXblockEntries);
        QByteArray xblock(kXblockHeaderLen + (children.size() * 8), '\0');
        xblock[0] = static_cast<char>(kXblockBType);
        xblock[1] = static_cast<char>(kXblockLevel);
        writeLe16(xblock, 2, static_cast<uint16_t>(children.size()));
        writeLe32(xblock, 4, static_cast<uint32_t>(data.size()));
        for (int i = 0; i < children.size(); ++i) {
            writeLe64(xblock, kXblockHeaderLen + (i * 8), children[i]);
        }
        return addInternalBlock(xblock);
    }

    uint64_t addSubnodeBlock(const std::vector<SubEntry>& entries) {
        if (entries.empty()) {
            return 0;
        }
        QByteArray block(
            kSubnodeBlockHeaderLen + (static_cast<qsizetype>(entries.size()) * kSlEntryLen), '\0');
        block[0] = static_cast<char>(kSubnodeBlockBType);
        block[1] = 0;  // cLevel 0: SLENTRY leaf
        writeLe16(block, kSubnodeCEntOffset, static_cast<uint16_t>(entries.size()));
        for (size_t i = 0; i < entries.size(); ++i) {
            const int off = kSubnodeBlockHeaderLen + (static_cast<int>(i) * kSlEntryLen);
            writeLe64(block, off, entries[i].nid);
            writeLe64(block, off + kSlEntryDataBidOffset, entries[i].data_bid);
            writeLe64(block, off + kSlEntrySubBidOffset, entries[i].sub_bid);
        }
        return addInternalBlock(block);
    }

    static QByteArray buildHeap(uint8_t client_sig,
                                uint32_t root_hid,
                                const QVector<QByteArray>& allocs) {
        int pagemap = kHnHdrSize;
        for (const auto& alloc : allocs) {
            pagemap += static_cast<int>(alloc.size());
        }
        // The page map starts on an even offset; the parser reads it as 16-bit fields.
        const int padded = pagemap + (pagemap % 2);
        const int count = static_cast<int>(allocs.size());
        QByteArray block(padded + kHnPmHeaderLen + (kHnPmEntryLen * (count + 1)), '\0');
        writeLe16(block, kHnHdrIbHnpmOffset, static_cast<uint16_t>(padded));
        block[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
        block[kHnHdrClientSigOffset] = static_cast<char>(client_sig);
        writeLe32(block, kHnHdrRootHidOffset, root_hid);
        writeLe16(block, padded, static_cast<uint16_t>(count));
        writeLe16(block, padded + kHnPmEntryLen, 0);
        int pos = kHnHdrSize;
        const int rgib = padded + kHnPmHeaderLen;
        for (int i = 0; i < count; ++i) {
            writeLe16(block, rgib + (i * kHnPmEntryLen), static_cast<uint16_t>(pos));
            block.replace(pos, static_cast<int>(allocs[i].size()), allocs[i]);
            pos += static_cast<int>(allocs[i].size());
        }
        writeLe16(block, rgib + (count * kHnPmEntryLen), static_cast<uint16_t>(pos));
        return block;
    }

    /// Build a property context; values too big for the heap go to sub-nodes added to @p subs.
    QByteArray buildPropertyContext(QVector<BuilderProp> props, std::vector<SubEntry>& subs) {
        std::sort(props.begin(), props.end(), [](const BuilderProp& a, const BuilderProp& b) {
            return a.id < b.id;
        });
        QVector<QByteArray> allocs;
        QByteArray bth_header(kBthHdrSize, '\0');
        bth_header[0] = static_cast<char>(kBthSignature);
        bth_header[kBthKeySizeField] = static_cast<char>(kPcBthKeySize);
        bth_header[kBthDataSizeField] = static_cast<char>(kPcBthDataSize);
        allocs.append(bth_header);
        QByteArray leaf(static_cast<qsizetype>(props.size()) * kPcRecordSize, '\0');
        allocs.append(leaf);
        for (int i = 0; i < props.size(); ++i) {
            const int rec = i * kPcRecordSize;
            writeLe16(allocs[1], rec, props[i].id);
            writeLe16(allocs[1], rec + kPcRecordKeySize, props[i].type);
            writeLe32(allocs[1],
                      rec + kPcRecordKeySize + kPcValueRefOffset,
                      valueRef(props[i], allocs, subs));
        }
        writeLe32(allocs[0], kBthRootHidField, props.isEmpty() ? 0 : kMsgBthLeafHid);
        return buildHeap(kPcClientSignature, kBthHeaderHid, allocs);
    }

    uint32_t valueRef(const BuilderProp& prop,
                      QVector<QByteArray>& allocs,
                      std::vector<SubEntry>& subs) {
        if (prop.type == sak::email::kPropTypeInt16 || prop.type == sak::email::kPropTypeInt32 ||
            prop.type == sak::email::kPropTypeBoolean) {
            QByteArray padded = prop.value;
            padded.resize(4, '\0');
            return static_cast<uint32_t>(static_cast<uint8_t>(padded[0])) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(padded[1])) << 8) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(padded[2])) << 16) |
                   (static_cast<uint32_t>(static_cast<uint8_t>(padded[3])) << 24);
        }
        if (prop.value.isEmpty()) {
            return 0;
        }
        if (prop.value.size() > kBuilderHeapValueLimit) {
            const uint32_t nid = allocateLtpNid();
            subs.push_back({nid, addDataTree(prop.value), 0});
            return nid;
        }
        allocs.append(prop.value);
        return static_cast<uint32_t>(allocs.size()) << kHidIndexShift;
    }

    uint64_t addPropertyNode(const QVector<BuilderProp>& props, uint64_t* sub_bid_out) {
        std::vector<SubEntry> subs;
        const QByteArray heap = buildPropertyContext(props, subs);
        const uint64_t data_bid = addDataTree(heap);
        const uint64_t sub_bid = addSubnodeBlock(subs);
        if (sub_bid_out != nullptr) {
            *sub_bid_out = sub_bid;
        }
        return data_bid;
    }

    struct TcColumn {
        uint16_t type;
        uint16_t id;
        int offset;
        int size;
        int bit;
    };

    /// Append the TCROWID row index to a table's allocations and point its TCINFO at it.
    static void appendRowIndex(QVector<QByteArray>& allocs, const QVector<uint32_t>& row_ids) {
        if (row_ids.isEmpty()) {
            return;
        }
        // Sorted by row id, as a BTH must be; the value is the row's place in the matrix.
        QVector<QPair<uint32_t, uint32_t>> index;
        for (int i = 0; i < row_ids.size(); ++i) {
            index.append({row_ids[i], static_cast<uint32_t>(i)});
        }
        std::sort(index.begin(), index.end());
        QByteArray leaf(static_cast<qsizetype>(index.size()) * kTcRowIdRecordSize, '\0');
        for (int i = 0; i < index.size(); ++i) {
            writeLe32(leaf, i * kTcRowIdRecordSize, index[i].first);
            writeLe32(leaf, (i * kTcRowIdRecordSize) + kTcRowIdKeyDataSize, index[i].second);
        }
        const uint32_t header_hid = static_cast<uint32_t>(allocs.size() + 1) << kHidIndexShift;
        const uint32_t leaf_hid = static_cast<uint32_t>(allocs.size() + 2) << kHidIndexShift;
        QByteArray bth_header(kBthHdrSize, '\0');
        bth_header[0] = static_cast<char>(kBthSignature);
        bth_header[kBthKeySizeField] = static_cast<char>(kTcRowIdKeyDataSize);
        bth_header[kBthDataSizeField] = static_cast<char>(kTcRowIdKeyDataSize);
        writeLe32(bth_header, kBthRootHidField, leaf_hid);
        allocs.append(bth_header);
        allocs.append(leaf);
        writeLe32(allocs[0], kTcinfoHidRowIndexOffset, header_hid);
    }

    /// Where a row's cell-existence bitmap starts, and how long the row is.
    struct TcRowLayout {
        int ceb_offset = 0;
        int row_size = 0;
    };

    /// A table context over @p rows of already-formatted row bytes (cells + CEB), with the TCROWID
    /// row index Outlook writes, mapping each row's id in @p row_ids to its position.
    static QByteArray buildTable(const QVector<TcColumn>& cols,
                                 const TcRowLayout& layout,
                                 const QByteArray& rows,
                                 const QVector<QByteArray>& values,
                                 const QVector<uint32_t>& row_ids) {
        QByteArray tcinfo(kTcinfoHeaderLen + (cols.size() * kTcColDescLen), '\0');
        tcinfo[kTcinfoBTypeOffset] = static_cast<char>(kTcClientSignature);
        tcinfo[kTcinfoColCountOffset] = static_cast<char>(cols.size());
        writeLe16(tcinfo, kTcinfoRgib4bOffset, static_cast<uint16_t>(layout.ceb_offset));
        writeLe16(tcinfo, kTcinfoRgib2bOffset, static_cast<uint16_t>(layout.ceb_offset));
        writeLe16(tcinfo, kTcinfoRgib1bOffset, static_cast<uint16_t>(layout.ceb_offset));
        writeLe16(tcinfo, kTcinfoRgibBmOffset, static_cast<uint16_t>(layout.row_size));
        writeLe32(tcinfo, kTcinfoHnidRowsOffset, rows.isEmpty() ? 0 : kTcHnidRows);
        for (int i = 0; i < cols.size(); ++i) {
            writeTcColDesc(tcinfo,
                           kTcinfoHeaderLen + (i * kTcColDescLen),
                           {cols[i].type, cols[i].id, cols[i].offset, cols[i].size, cols[i].bit});
        }
        QVector<QByteArray> allocs{tcinfo, rows};
        allocs += values;
        appendRowIndex(allocs, row_ids);
        return buildHeap(kTcClientSignature, kTcHidUserRoot, allocs);
    }

    /// One-column table of row ids, the shape a hierarchy table needs.
    uint64_t addRowIdTable(const QVector<uint32_t>& nids) {
        QByteArray rows;
        rows.resize(static_cast<qsizetype>(nids.size()) * kTcRowSize);
        rows.fill('\0');
        for (int i = 0; i < nids.size(); ++i) {
            writeLe32(rows, i * kTcRowSize, nids[i]);
            rows[(i * kTcRowSize) + kTcRowCellSize] = static_cast<char>(kCebFirstColPresent);
        }
        const QVector<TcColumn> cols{
            {sak::email::kPropTypeInt32, kPidTagLtpRowId, 0, kTcRowCellSize, 0}};
        return addDataTree(buildTable(cols, {kTcRowCellSize, kTcRowSize}, rows, {}, nids));
    }

    /// Contents table with the columns Outlook lists mail by.
    uint64_t addContentsTable(const QVector<uint32_t>& nids,
                              const QVector<BuilderMessage>& messages) {
        constexpr int kCellSubject = 4;
        constexpr int kCellSenderName = 8;
        constexpr int kCellSenderEmail = 12;
        constexpr int kCellFlags = 16;
        constexpr int kCellSize = 20;
        constexpr int kCellClass = 24;
        constexpr int kCellDate = 28;
        constexpr int kCeb = 36;
        constexpr int kRow = 37;
        const QVector<TcColumn> cols{
            {sak::email::kPropTypeInt32, kPidTagLtpRowId, 0, 4, 0},
            {sak::email::kPropTypeUnicode, sak::email::kPropIdSubject, kCellSubject, 4, 1},
            {sak::email::kPropTypeUnicode, sak::email::kPropIdSenderName, kCellSenderName, 4, 2},
            {sak::email::kPropTypeUnicode, sak::email::kPropIdSenderEmail, kCellSenderEmail, 4, 3},
            {sak::email::kPropTypeInt32, sak::email::kPropIdMessageFlags, kCellFlags, 4, 4},
            {sak::email::kPropTypeInt32, sak::email::kPropIdMessageSize, kCellSize, 4, 5},
            {sak::email::kPropTypeUnicode, sak::email::kPropIdMessageClass, kCellClass, 4, 6},
            {sak::email::kPropTypeSysTime, sak::email::kPropIdMessageDeliveryTime, kCellDate, 8, 7},
        };
        QVector<QByteArray> values;
        const auto addValue = [&values](const QString& text) -> uint32_t {
            if (text.isEmpty()) {
                return 0;
            }
            values.append(builderUnicode(text));
            // Allocation 1 is TCINFO, 2 the row matrix, values follow.
            return static_cast<uint32_t>(values.size() + 2) << kHidIndexShift;
        };
        QByteArray rows(static_cast<qsizetype>(nids.size()) * kRow, '\0');
        for (int i = 0; i < nids.size(); ++i) {
            const BuilderMessage& msg = messages[i];
            const int row = i * kRow;
            writeLe32(rows, row, nids[i]);
            writeLe32(rows, row + kCellSubject, addValue(msg.subject));
            writeLe32(rows, row + kCellSenderName, addValue(msg.sender_name));
            writeLe32(rows, row + kCellSenderEmail, addValue(msg.sender_email));
            writeLe32(rows, row + kCellFlags, messageFlags(msg));
            writeLe32(rows, row + kCellSize, static_cast<uint32_t>(msg.body_plain.size() + 1024));
            writeLe32(rows, row + kCellClass, addValue(msg.message_class));
            uint8_t ceb = 0xFF;
            if (msg.date.isValid()) {
                rows.replace(row + kCellDate, 8, builderFiletime(msg.date));
            } else {
                ceb = static_cast<uint8_t>(ceb & ~0x01U);  // bit 7 of the CEB's first byte: no date
            }
            if (msg.sender_name.isEmpty()) {
                ceb = static_cast<uint8_t>(ceb & ~(0x80U >> 2));
            }
            if (msg.sender_email.isEmpty()) {
                ceb = static_cast<uint8_t>(ceb & ~(0x80U >> 3));
            }
            if (msg.subject.isEmpty()) {
                ceb = static_cast<uint8_t>(ceb & ~(0x80U >> 1));
            }
            rows[row + kCeb] = static_cast<char>(ceb);
        }
        return addDataTree(buildTable(cols, {kCeb, kRow}, rows, values, nids));
    }

    static uint32_t messageFlags(const BuilderMessage& msg) {
        uint32_t flags = msg.read ? kBuilderMsgFlagRead : 0;
        if (!msg.attachments.isEmpty() || !msg.attached_messages.isEmpty()) {
            flags |= kBuilderMsgFlagHasAttach;
        }
        return flags;
    }

    void addFolderNode(const BuilderFolder& folder, uint32_t nid, uint32_t parent) {
        const auto base = nid & ~static_cast<uint32_t>(kNidTypeFolderMask);

        QVector<uint32_t> message_nids;
        for (const BuilderMessage& msg : folder.messages) {
            const uint32_t msg_nid = allocateMessageNid();
            addMessageNode(msg, msg_nid, nid);
            message_nids.append(msg_nid);
        }
        const uint64_t contents_bid = folder.minimal_table
                                          ? addRowIdTable(message_nids)
                                          : addContentsTable(message_nids, folder.messages);
        m_nodes.push_back({base | sak::email::kNidTypeContentsTable, contents_bid, 0, nid});

        QVector<uint32_t> child_nids;
        for (int i = 0; i < folder.children.size(); ++i) {
            child_nids.append(allocateFolderNid());
        }
        for (int i = 0; i < folder.children.size(); ++i) {
            addFolderNode(folder.children[i], child_nids[i], nid);
        }
        if (!child_nids.isEmpty()) {
            m_nodes.push_back(
                {base | sak::email::kNidTypeHierarchyTable, addRowIdTable(child_nids), 0, nid});
        }

        const int declared = folder.declared_count >= 0 ? folder.declared_count
                                                        : static_cast<int>(folder.messages.size());
        QVector<BuilderProp> props{
            {sak::email::kPropIdDisplayName,
             sak::email::kPropTypeUnicode,
             builderUnicode(folder.name)},
            {sak::email::kPropIdContentCount,
             sak::email::kPropTypeInt32,
             builderInt32(static_cast<uint32_t>(declared))},
            {sak::email::kPropIdContentUnreadCount, sak::email::kPropTypeInt32, builderInt32(0)},
            {sak::email::kPropIdSubfolders,
             sak::email::kPropTypeBoolean,
             QByteArray(1, static_cast<char>(child_nids.isEmpty() ? 0 : 1))},
        };
        if (!folder.container_class.isEmpty()) {
            props.append({sak::email::kPropIdContainerClass,
                          sak::email::kPropTypeUnicode,
                          builderUnicode(folder.container_class)});
        }
        uint64_t sub_bid = 0;
        const uint64_t data_bid = addPropertyNode(props, &sub_bid);
        m_nodes.push_back({nid, data_bid, sub_bid, parent});
    }

    [[nodiscard]] QVector<BuilderProp> messageProps(const BuilderMessage& msg) const {
        QVector<BuilderProp> props{
            {sak::email::kPropIdMessageClass,
             sak::email::kPropTypeUnicode,
             builderUnicode(msg.message_class)},
            {sak::email::kPropIdMessageFlags,
             sak::email::kPropTypeInt32,
             builderInt32(messageFlags(msg))},
            {sak::email::kPropIdMessageSize,
             sak::email::kPropTypeInt32,
             builderInt32(static_cast<uint32_t>(msg.body_plain.size() + 1024))},
        };
        const auto addText = [&props](uint16_t id, const QString& text) {
            if (!text.isEmpty()) {
                props.append({id, sak::email::kPropTypeUnicode, builderUnicode(text)});
            }
        };
        addText(sak::email::kPropIdSubject, msg.subject);
        addText(sak::email::kPropIdSenderName, msg.sender_name);
        addText(sak::email::kPropIdSenderEmail, msg.sender_email);
        addText(sak::email::kPropIdDisplayTo, msg.display_to);
        addText(sak::email::kPropIdDisplayCc, msg.display_cc);
        addText(sak::email::kPropIdBody, msg.body_plain);
        addText(sak::email::kPropIdInternetMessageId, msg.internet_message_id);
        if (!msg.body_html.isEmpty()) {
            props.append(
                {sak::email::kPropIdHtmlBody, sak::email::kPropTypeBinary, msg.body_html.toUtf8()});
        }
        if (!msg.body_rtf_compressed.isEmpty()) {
            props.append({sak::email::kPropIdRtfCompressed,
                          sak::email::kPropTypeBinary,
                          msg.body_rtf_compressed});
        }
        if (msg.date.isValid()) {
            props.append({sak::email::kPropIdMessageDeliveryTime,
                          sak::email::kPropTypeSysTime,
                          builderFiletime(msg.date)});
        }
        if (msg.created.isValid()) {
            props.append({0x3007, sak::email::kPropTypeSysTime, builderFiletime(msg.created)});
        }
        props += msg.extra_props;
        for (const BuilderNamedProp& named : msg.named_props) {
            props.append({namedId(named), named.type, named.value});
        }
        return props;
    }

    void addMessageNode(const BuilderMessage& msg, uint32_t nid, uint32_t parent) {
        std::vector<SubEntry> subs;
        const QByteArray heap = buildMessageHeap(msg, subs);
        uint64_t data_bid = addDataTree(heap);
        const uint64_t sub_bid = addSubnodeBlock(subs);
        if (msg.data_bid_reserved_bit) {
            data_bid |= 1U;
        }
        m_nodes.push_back({nid, data_bid, sub_bid, parent});
    }

    QByteArray buildMessageHeap(const BuilderMessage& msg, std::vector<SubEntry>& subs) {
        const QByteArray heap = buildPropertyContext(messageProps(msg), subs);
        int attach_number = 0;
        for (const BuilderAttachment& att : msg.attachments) {
            addAttachmentSubnode(att, attach_number++, subs);
        }
        for (const BuilderMessage& attached : msg.attached_messages) {
            addAttachedMessageSubnode(attached, attach_number++, subs);
        }
        return heap;
    }

    void addAttachmentSubnode(const BuilderAttachment& att,
                              int number,
                              std::vector<SubEntry>& subs) {
        if (att.ole_object) {
            addOleSubnode(att, subs);
            return;
        }
        QVector<BuilderProp> props{
            {sak::email::kPropIdAttachLongFilename,
             sak::email::kPropTypeUnicode,
             builderUnicode(att.name)},
            {sak::email::kPropIdAttachMethod,
             sak::email::kPropTypeInt32,
             builderInt32(sak::email::kAttachByValue)},
            {sak::email::kPropIdAttachSize,
             sak::email::kPropTypeInt32,
             builderInt32(static_cast<uint32_t>(att.data.size()))},
            {sak::email::kPropIdAttachData, sak::email::kPropTypeBinary, att.data},
        };
        if (!att.mime_type.isEmpty()) {
            props.append({sak::email::kPropIdAttachMimeTag,
                          sak::email::kPropTypeUnicode,
                          builderUnicode(att.mime_type)});
        }
        if (!att.content_id.isEmpty()) {
            props.append({sak::email::kPropIdAttachContentId,
                          sak::email::kPropTypeUnicode,
                          builderUnicode(att.content_id)});
        }
        Q_UNUSED(number);
        uint64_t sub_bid = 0;
        const uint64_t data_bid = addPropertyNode(props, &sub_bid);
        subs.push_back({allocateAttachmentNid(), data_bid, sub_bid});
    }

    void addOleSubnode(const BuilderAttachment& att, std::vector<SubEntry>& subs) {
        const uint32_t object_nid = allocateLtpNid();
        const uint64_t object_data = addDataTree(att.data);
        QByteArray object_ref(8, '\0');
        writeLe32(object_ref, 0, object_nid);
        writeLe32(object_ref, 4, static_cast<uint32_t>(att.data.size()));
        const QVector<BuilderProp> props{
            {sak::email::kPropIdAttachLongFilename,
             sak::email::kPropTypeUnicode,
             builderUnicode(att.name)},
            {sak::email::kPropIdAttachMethod,
             sak::email::kPropTypeInt32,
             builderInt32(sak::email::kAttachOle)},
            {sak::email::kPropIdAttachData, kBuilderPropTypeObject, object_ref},
        };
        std::vector<SubEntry> attachment_subs{{object_nid, object_data, 0}};
        const QByteArray heap = buildPropertyContext(props, attachment_subs);
        const uint64_t data_bid = addDataTree(heap);
        const uint64_t sub_bid = addSubnodeBlock(attachment_subs);
        subs.push_back({allocateAttachmentNid(), data_bid, sub_bid});
    }

    void addAttachedMessageSubnode(const BuilderMessage& attached,
                                   int number,
                                   std::vector<SubEntry>& subs) {
        Q_UNUSED(number);
        // The attached email lives in a sub-node of the attachment; PR_ATTACH_DATA_OBJ's value is
        // an HNID leading to {dwNid, ulSize} (MS-PST 2.3.3.5).
        std::vector<SubEntry> message_subs;
        const QByteArray message_heap = buildMessageHeap(attached, message_subs);
        const uint32_t message_nid = allocateLtpNid();
        const uint64_t message_data = addDataTree(message_heap);
        const uint64_t message_sub = addSubnodeBlock(message_subs);

        QByteArray object_ref(8, '\0');
        writeLe32(object_ref, 0, message_nid);
        writeLe32(object_ref, 4, static_cast<uint32_t>(message_heap.size()));
        QVector<BuilderProp> props{
            {sak::email::kPropIdDisplayName,
             sak::email::kPropTypeUnicode,
             builderUnicode(attached.subject)},
            {sak::email::kPropIdAttachMethod,
             sak::email::kPropTypeInt32,
             builderInt32(sak::email::kAttachEmbeddedMessage)},
            {sak::email::kPropIdAttachSize,
             sak::email::kPropTypeInt32,
             builderInt32(static_cast<uint32_t>(message_heap.size()))},
            {sak::email::kPropIdAttachData, kBuilderPropTypeObject, object_ref},
        };
        std::vector<SubEntry> attachment_subs{{message_nid, message_data, message_sub}};
        const QByteArray heap = buildPropertyContext(props, attachment_subs);
        const uint64_t data_bid = addDataTree(heap);
        const uint64_t sub_bid = addSubnodeBlock(attachment_subs);
        subs.push_back({allocateAttachmentNid(), data_bid, sub_bid});
    }

    void collectNamedProps(const QVector<BuilderFolder>& folders) {
        for (const BuilderFolder& folder : folders) {
            for (const BuilderMessage& msg : folder.messages) {
                for (const BuilderNamedProp& named : msg.named_props) {
                    (void)registerNamed(named);
                }
            }
            collectNamedProps(folder.children);
        }
    }

    int registerNamed(const BuilderNamedProp& named) {
        for (int i = 0; i < m_named_ids.size(); ++i) {
            if (m_named_ids[i].set_byte == named.set_byte &&
                m_named_ids[i].long_id == named.long_id) {
                return i;
            }
        }
        m_named_ids.append(named);
        return static_cast<int>(m_named_ids.size()) - 1;
    }

    [[nodiscard]] uint16_t namedId(const BuilderNamedProp& named) const {
        for (int i = 0; i < m_named_ids.size(); ++i) {
            if (m_named_ids[i].set_byte == named.set_byte &&
                m_named_ids[i].long_id == named.long_id) {
                return static_cast<uint16_t>(0x8000 + i);
            }
        }
        return 0;
    }

    void addRootNode(uint32_t subtree_nid) {
        // A second child, as every real store has, so the IPM subtree is not simply "the only one".
        const uint32_t search_root_nid = allocateFolderNid();
        BuilderFolder search_root;
        search_root.name = QStringLiteral("Search Root");
        search_root.container_class.clear();
        addFolderNode(search_root, search_root_nid, kRootFolderNid);

        const uint32_t base = kRootFolderNid & ~static_cast<uint32_t>(kNidTypeFolderMask);
        m_nodes.push_back({base | sak::email::kNidTypeHierarchyTable,
                           addRowIdTable({subtree_nid, search_root_nid}),
                           0,
                           kRootFolderNid});
        m_nodes.push_back(
            {base | sak::email::kNidTypeContentsTable, addRowIdTable({}), 0, kRootFolderNid});
        uint64_t sub_bid = 0;
        const QVector<BuilderProp> props{
            {sak::email::kPropIdContentCount, sak::email::kPropTypeInt32, builderInt32(0)},
            {sak::email::kPropIdSubfolders, sak::email::kPropTypeBoolean, QByteArray(1, 1)},
        };
        const uint64_t data_bid = addPropertyNode(props, &sub_bid);
        m_nodes.push_back({kRootFolderNid, data_bid, sub_bid, kRootFolderNid});
    }

    void addStoreNode(uint32_t subtree_nid) {
        QByteArray entry_id(sak::email::kEntryIdBytes, '\0');
        writeLe32(entry_id, sak::email::kEntryIdNidOffset, subtree_nid);
        const QVector<BuilderProp> props{
            {sak::email::kPropIdDisplayName,
             sak::email::kPropTypeUnicode,
             builderUnicode(store_name)},
            {sak::email::kPropIdIpmSubtreeEntryId, sak::email::kPropTypeBinary, entry_id},
        };
        uint64_t sub_bid = 0;
        const uint64_t data_bid = addPropertyNode(props, &sub_bid);
        m_nodes.push_back({sak::email::kNidMessageStore, data_bid, sub_bid, 0});
    }

    void addNameToIdNode() {
        // GUID stream: one entry per set in use, then NAMEIDs pointing at them by wGuid = 3 +
        // index.
        QVector<uint8_t> sets;
        for (const BuilderNamedProp& named : m_named_ids) {
            if (!sets.contains(named.set_byte)) {
                sets.append(named.set_byte);
            }
        }
        QByteArray guids(static_cast<qsizetype>(sets.size()) * 16, '\0');
        for (int i = 0; i < sets.size(); ++i) {
            const int off = i * 16;
            guids[off] = static_cast<char>(sets[i]);
            guids[off + 1] = 0x20;
            guids[off + 2] = 0x06;
            guids[off + 8] = static_cast<char>(0xC0);
            guids[off + 15] = 0x46;
        }
        QByteArray entries(static_cast<qsizetype>(m_named_ids.size()) * 8, '\0');
        for (int i = 0; i < m_named_ids.size(); ++i) {
            const int off = i * 8;
            writeLe32(entries, off, m_named_ids[i].long_id);
            const int guid_index = static_cast<int>(sets.indexOf(m_named_ids[i].set_byte)) + 3;
            writeLe16(entries, off + 4, static_cast<uint16_t>(guid_index << 1));
            writeLe16(entries, off + 6, static_cast<uint16_t>(i));
        }
        const QVector<BuilderProp> props{
            {0x0002, sak::email::kPropTypeBinary, guids},
            {0x0003, sak::email::kPropTypeBinary, entries},
        };
        uint64_t sub_bid = 0;
        const uint64_t data_bid = addPropertyNode(props, &sub_bid);
        m_nodes.push_back({sak::email::kNidNameToIdMap, data_bid, sub_bid, 0});
    }

    static QByteArray makeIndexPage(uint8_t ptype,
                                    int page_offset,
                                    const QVector<int>& child_offsets) {
        QByteArray page(kPageSize, '\0');
        const int trailer = kPageSize - kTrailerSize;
        const int meta = trailer - kMetaPad - kMetaSize;
        for (int i = 0; i < child_offsets.size(); ++i) {
            writeLe64(page,
                      (i * kBuilderIndexEntrySize) + kBuilderIndexChildOffset,
                      static_cast<uint64_t>(child_offsets[i]));
        }
        page[meta] = static_cast<char>(child_offsets.size());
        page[meta + 1] = static_cast<char>(kBuilderIndexPerPage);
        page[meta + 2] = static_cast<char>(kBuilderIndexEntrySize);
        page[meta + 3] = 1;  // cLevel 1: index page
        page[trailer] = static_cast<char>(ptype);
        page[trailer + 1] = static_cast<char>(ptype);
        writeLe16(page, trailer + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
        writeLe32(page, trailer + 4, weakCrc(page, 0, kPageSize - kTrailerSize));
        return page;
    }

    /// Append leaf pages over @p entries and an index page above them; returns the root offset.
    static int appendBTree(QByteArray& file,
                           uint8_t ptype,
                           int entry_size,
                           int per_page,
                           const QVector<QByteArray>& entries) {
        QVector<int> leaf_offsets;
        for (int start = 0; start < entries.size() || start == 0; start += per_page) {
            const int count = std::min(per_page, static_cast<int>(entries.size()) - start);
            QByteArray body;
            for (int i = 0; i < count; ++i) {
                body.append(entries[start + i]);
            }
            const int offset = static_cast<int>(file.size());
            leaf_offsets.append(offset);
            file.append(makeLeafPageWithEntries(ptype, entry_size, offset, body, count));
        }
        if (leaf_offsets.size() == 1) {
            return leaf_offsets.first();
        }
        Q_ASSERT(leaf_offsets.size() <= kBuilderIndexPerPage);
        const int root = static_cast<int>(file.size());
        file.append(makeIndexPage(ptype, root, leaf_offsets));
        return root;
    }

    [[nodiscard]] QByteArray assembleFile() const {
        QByteArray file(kPageSize * 2, '\0');
        file[0] = static_cast<char>(kMagicByte0);
        file[1] = static_cast<char>(kMagicByte1);
        file[2] = static_cast<char>(kMagicByte2);
        file[3] = static_cast<char>(kMagicByte3);
        writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
        writeLe16(file, kVersionOffset, sak::email::kUnicodeVersion);
        file[kCryptOffsetUnicode] = static_cast<char>(sak::email::kEncryptNone);

        QVector<QByteArray> bbt_entries;
        for (const Block& block : m_blocks) {
            const int offset = static_cast<int>(file.size());
            const uint64_t recorded_bid = bbt_keys_reserved_bit ? (block.bid | 1U) : block.bid;
            QByteArray disk(blockDiskSize(static_cast<int>(block.data.size())), '\0');
            disk.replace(0, static_cast<int>(block.data.size()), block.data);
            stampBlockTrailer(disk,
                              0,
                              static_cast<int>(block.data.size()),
                              recorded_bid,
                              static_cast<uint64_t>(offset));
            file.append(disk);

            QByteArray entry(kBbtEntrySize, '\0');
            writeLe64(entry, kBbtEntryBidOffset, recorded_bid);
            writeLe64(entry, kBbtEntryIbOffset, static_cast<uint64_t>(offset));
            writeLe16(entry, kBbtEntryCbOffset, static_cast<uint16_t>(block.data.size()));
            writeLe16(entry, kBbtEntryRefOffset, 1);
            bbt_entries.append(entry);
        }
        // Pages sit on 512-byte boundaries.
        file.append(QByteArray(
            ((file.size() + kPageSize - 1) / kPageSize * kPageSize) - file.size(), '\0'));

        QVector<QByteArray> nbt_entries;
        std::vector<Node> nodes = m_nodes;
        std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) {
            return a.nid < b.nid;
        });
        for (const Node& node : nodes) {
            QByteArray entry(kNbtEntrySize, '\0');
            writeLe64(entry, kNbtEntryNidOffset, node.nid);
            writeLe64(entry, kNbtEntryDataBidOffset, node.data_bid);
            writeLe64(entry, kNbtEntrySubBidOffset, node.sub_bid);
            writeLe32(entry, kNbtEntryParentOffset, node.parent);
            nbt_entries.append(entry);
        }
        const int nbt_root =
            appendBTree(file, kPtypeNbt, kNbtEntrySize, kBuilderNbtPerPage, nbt_entries);
        const int bbt_root =
            appendBTree(file, kPtypeBbt, kBbtEntrySize, kBuilderBbtPerPage, bbt_entries);

        writeLe64(file, kRootOffset + kRootFileSizeField, static_cast<uint64_t>(file.size()));
        writeLe64(file, kRootOffset + kRootNbtField, static_cast<uint64_t>(nbt_root));
        writeLe64(file, kRootOffset + kRootBbtField, static_cast<uint64_t>(bbt_root));
        writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
        writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
        return file;
    }

    static constexpr uint32_t kNidTypeFolderMask = 0x1F;
};

}  // namespace sak::pst_fixture
