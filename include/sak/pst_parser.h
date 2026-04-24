// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_parser.h
/// @brief PST/OST binary file parser per the MS-PST open specification
///
/// Parses Microsoft PST and OST files without requiring Outlook or any
/// COM/MAPI library. Implements the three-layer architecture:
///   NDB (Node Database) → LTP (Lists, Tables, Properties) → Messaging
///
/// Reference: [MS-PST]: Outlook Personal Folders (.pst) File Format

#pragma once

#include "sak/email_constants.h"
#include "sak/email_types.h"

#include <QFile>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

namespace sak {

/// @brief Forward declaration for error code
enum class error_code;

}  // namespace sak

class PstParser : public QObject {
    Q_OBJECT

public:
    explicit PstParser(QObject* parent = nullptr);
    ~PstParser() override;

    // Disable copy/move
    PstParser(const PstParser&) = delete;
    PstParser& operator=(const PstParser&) = delete;
    PstParser(PstParser&&) = delete;
    PstParser& operator=(PstParser&&) = delete;

    /// Open a PST or OST file and parse header + BTrees
    void open(const QString& file_path);

    /// Close the currently opened file and release all resources
    void close();

    /// Whether a file is currently open and parsed
    [[nodiscard]] bool isOpen() const;

    /// Get file info for the currently open file
    [[nodiscard]] sak::PstFileInfo fileInfo() const;

    /// Get the folder tree (hierarchy only — items loaded on demand)
    [[nodiscard]] sak::PstFolderTree folderTree() const;

    /// Load items (messages, contacts, etc.) for a specific folder
    void loadFolderItems(uint64_t folder_node_id, int offset, int limit);

    /// Load full detail for a specific item
    void loadItemDetail(uint64_t item_node_id);

    /// Load raw MAPI properties for a specific item
    void loadItemProperties(uint64_t item_node_id);

    /// Load attachment content bytes
    void loadAttachmentContent(uint64_t message_node_id, int attachment_index);

    /// Request cancellation of any in-progress operation
    void cancel();

    /// Get all folder items (synchronous, for worker threads)
    [[nodiscard]] std::expected<QVector<sak::PstItemSummary>, sak::error_code> readFolderItems(
        uint64_t folder_node_id, int offset, int limit);

    /// Get full item detail (synchronous, for worker threads)
    [[nodiscard]] std::expected<sak::PstItemDetail, sak::error_code> readItemDetail(
        uint64_t item_node_id);

    /// Get raw MAPI properties (synchronous, for worker threads)
    [[nodiscard]] std::expected<QVector<sak::MapiProperty>, sak::error_code> readItemProperties(
        uint64_t item_node_id);

    /// Get attachment data (synchronous, for worker threads)
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readAttachmentData(
        uint64_t message_node_id, int attachment_index);

    /// Classify an item by its PR_MESSAGE_CLASS value
    [[nodiscard]] static sak::EmailItemType classifyMessageClass(const QString& message_class);

    /// Get all cached NBT node IDs (for deleted-item / orphan scanning)
    [[nodiscard]] QVector<uint64_t> allNodeIds() const;

    /// Table Context column descriptor (§2.3.4.2)
    struct TcColDesc {
        uint16_t prop_type = 0;
        uint16_t prop_id = 0;
        uint16_t ib_data = 0;
        uint8_t cb_data = 0;
        uint8_t i_bit = 0;
    };

    /// Parsed TCINFO header with column list and row HNID
    struct TcInfo {
        QVector<TcColDesc> columns;
        uint32_t hnid_rows = 0;      ///< HNID of row matrix (raw cell storage)
        uint32_t hid_row_index = 0;  ///< HID of TCROWID BTH (live-row enumerator)
        uint16_t rgib_tci_1b = 0;
        uint16_t rgib_tci_bm = 0;
    };

Q_SIGNALS:
    void fileOpened(sak::PstFileInfo file_info);
    void folderTreeLoaded(sak::PstFolderTree tree);
    void folderItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total_count);
    void itemDetailLoaded(sak::PstItemDetail detail);
    void itemPropertiesLoaded(uint64_t item_id, QVector<sak::MapiProperty> properties);
    void attachmentContentReady(uint64_t message_id, int index, QByteArray data, QString filename);
    void progressUpdated(int percent, QString status);
    void errorOccurred(QString error);

private:
    // -- File state --
    QFile m_file;
    sak::PstHeader m_header;
    sak::PstFileInfo m_file_info;
    sak::PstFolderTree m_folder_tree;
    bool m_is_open = false;
    bool m_is_unicode = false;
    bool m_is_4k = false;
    uint8_t m_encryption_type = 0;
    std::atomic<bool> m_cancelled{false};

    // -- BTree caches --
    QHash<uint64_t, sak::PstNode> m_nbt_cache;

    /// BBT entry: file offset and data size for a block
    struct BbtEntry {
        uint64_t file_offset = 0;
        uint16_t cb = 0;
    };
    QHash<uint64_t, BbtEntry> m_bbt_cache;

    // -- Internal helper types --

    /// Parsed BTree page metadata (NDB layer)
    struct BTreePageInfo {
        QByteArray data;
        int entry_count = 0;
        int entry_size = 0;
        int level = 0;
        int meta_offset = 0;
    };

    /// Format-dependent page sizes (ANSI / Unicode / Unicode4K)
    struct PageFormatSizes {
        int page_size = 0;
        int trailer_size = 0;
        int meta_size = 0;
        int meta_pad = 0;
    };

    /// Heap context: aggregated data for LTP property reading
    struct HeapContext {
        QByteArray heap_data;
        QVector<int> block_offsets;
        QHash<uint64_t, sak::PstNode> subnode_map;
    };

    /// Row-position descriptor for TC cell building
    struct TcRowView {
        int row_off = 0;
        int row_size = 0;
        int ceb_off = 0;
    };

    /// Collected BTH leaf records with key/data sizes
    struct BthLeafResult {
        QByteArray leaf_data;
        uint8_t key_size = 0;
        uint8_t data_size = 0;
    };

    // ======================================================================
    // NDB Layer — Node Database
    // ======================================================================

    /// Parse and validate the PST file header
    [[nodiscard]] std::expected<void, sak::error_code> parseHeader();

    /// Compute format-dependent page sizes for BTree parsing
    [[nodiscard]] PageFormatSizes pageFormatSizes() const;

    /// Parse a BTree page: read, validate, and extract metadata
    [[nodiscard]] std::expected<BTreePageInfo, sak::error_code> parseBTreePage(uint64_t page_offset,
                                                                               int depth);

    /// Load the Node BTree from the given page offset
    [[nodiscard]] std::expected<void, sak::error_code> loadNodeBTree(uint64_t page_offset,
                                                                     int depth = 0);

    /// Load the Block BTree from the given page offset
    [[nodiscard]] std::expected<void, sak::error_code> loadBlockBTree(uint64_t page_offset,
                                                                      int depth = 0);

    /// Read a single data block by Block ID
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readBlock(uint64_t bid);

    /// Read a multi-block data tree (XBLOCK/XXBLOCK)
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readDataTree(
        uint64_t bid, QVector<int>* block_offsets = nullptr);

    /// Extract attachment data bytes from a resolved subnode
    [[nodiscard]] std::expected<QByteArray, sak::error_code> extractAttachmentFromSubnode(
        const sak::PstNode& subnode);

    /// Read child blocks from an XBLOCK
    [[nodiscard]] std::expected<void, sak::error_code> readXblockChildren(
        const QByteArray& data,
        uint16_t entry_count,
        QByteArray& result,
        QVector<int>* block_offsets);

    /// Read child blocks from an XXBLOCK
    [[nodiscard]] std::expected<void, sak::error_code> readXxblockChildren(
        const QByteArray& data,
        uint16_t entry_count,
        QByteArray& result,
        QVector<int>* block_offsets);

    /// Decompress a 4K-page block if the footer indicates zlib compression
    [[nodiscard]] QByteArray decompressBlockIf4k(const QByteArray& raw,
                                                 uint64_t file_offset,
                                                 int cb);

    /// Decrypt a block in-place using compressible encryption
    void decryptBlock(std::span<uint8_t> data) const;

    // ======================================================================
    // LTP Layer — Lists, Tables, Properties
    // ======================================================================

    /// Read the Property Context (PC) for a node → MAPI properties
    [[nodiscard]] std::expected<QVector<sak::MapiProperty>, sak::error_code> readPropertyContext(
        uint64_t nid);

    /// Read the Table Context (TC) for a node → rows of MAPI properties
    [[nodiscard]] std::expected<QVector<QVector<sak::MapiProperty>>, sak::error_code>
    readTableContext(uint64_t nid);

    /// Row matrix payload with per-block end offsets (MS-PST §2.3.4.4.1).
    /// When the row matrix is stored as a sub-node, each data block ends with
    /// padding bytes that must be skipped when addressing rows by index.
    struct TcRowMatrix {
        QByteArray data;
        QVector<int> block_ends;  ///< Cumulative end offset of each block in data
    };

    /// Build rows from TC row data using parsed column descriptors
    [[nodiscard]] QVector<QVector<sak::MapiProperty>> buildTcRows(const TcRowMatrix& matrix,
                                                                  const TcInfo& tc,
                                                                  const HeapContext& ctx);

    /// Build a single cell from a TC row
    [[nodiscard]] sak::MapiProperty buildTcCell(const QByteArray& row_data,
                                                const TcRowView& row_view,
                                                const TcColDesc& col,
                                                const HeapContext& ctx);

    /// Validate heap data has correct HN + TC signatures
    [[nodiscard]] std::expected<void, sak::error_code> validateHeapForTc(
        const QByteArray& heap_data);

    /// Load row data for a Table Context from HID or sub-node
    [[nodiscard]] TcRowMatrix loadTcRowData(const TcInfo& tc,
                                            const sak::PstNode& node,
                                            HeapContext& ctx);

    /// Validate HN/BTH headers and collect all leaf records from a PC/TC heap
    [[nodiscard]] std::expected<BthLeafResult, sak::error_code> collectBthLeafData(
        const HeapContext& ctx, uint8_t expected_client_sig);

    /// Build MAPI property list from collected BTH leaf records
    [[nodiscard]] QVector<sak::MapiProperty> parsePropertyRecords(const BthLeafResult& bth,
                                                                  const HeapContext& ctx);

    /// Read data from a Heap-on-Node (HN) allocation
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readHeapOnNode(
        const QByteArray& heap_data, uint32_t hn_id, const QVector<int>& block_offsets = {});

    /// Resolve an HNID: subnode map → NID check → HID heap lookup
    [[nodiscard]] std::expected<QByteArray, sak::error_code> resolveHnid(
        uint32_t hnid,
        const QByteArray& heap_data,
        const QVector<int>& block_offsets,
        const QHash<uint64_t, sak::PstNode>& subnode_map);

    /// Collect all leaf records from a multi-level BTH
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readBthLeafData(
        const QByteArray& heap_data,
        uint32_t node_hid,
        uint8_t key_size,
        int level,
        const QVector<int>& block_offsets = {});

    /// Read the sub-node BTree for a node
    [[nodiscard]] std::expected<QHash<uint64_t, sak::PstNode>, sak::error_code> readSubNodeBTree(
        uint64_t subnode_bid, int depth = 0);

    /// Read sub-node BTree leaf entries
    [[nodiscard]] QHash<uint64_t, sak::PstNode> readSubNodeLeafEntries(const QByteArray& data,
                                                                       int header_size,
                                                                       uint16_t entry_count);

    /// Read sub-node BTree intermediate entries (recursive)
    [[nodiscard]] std::expected<QHash<uint64_t, sak::PstNode>, sak::error_code>
    readSubNodeIntermediateEntries(const QByteArray& data,
                                   int header_size,
                                   uint16_t entry_count,
                                   int depth);

    /// Format a MAPI property value for display
    [[nodiscard]] QString formatPropertyValue(uint16_t prop_type,
                                              const QByteArray& raw_value) const;

    /// Get human-readable name for a MAPI property ID
    [[nodiscard]] static QString propertyIdToName(uint16_t prop_id);

    // ======================================================================
    // Messaging Layer
    // ======================================================================

    /// Build the folder hierarchy starting from the root folder NID
    [[nodiscard]] std::expected<sak::PstFolderTree, sak::error_code> buildFolderHierarchy(
        uint64_t root_nid, int depth = 0);

    /// Load child folders from the hierarchy table into a parent folder
    void loadChildFolders(sak::PstFolder& folder, uint64_t root_nid, int depth);

    /// Read the contents table for a folder
    [[nodiscard]] std::expected<QVector<sak::PstItemSummary>, sak::error_code> readContentsTable(
        uint64_t folder_nid, int offset, int limit);

    /// Read a full message/contact/calendar/task item
    [[nodiscard]] std::expected<sak::PstItemDetail, sak::error_code> readMessage(
        uint64_t message_nid);

    /// Read just the sender name+email from a message PC (lightweight)
    [[nodiscard]] std::pair<QString, QString> readSenderFromPC(uint64_t message_nid);

    /// Extract sender fields from collected BTH leaf data
    [[nodiscard]] std::pair<QString, QString> extractSenderFromLeaf(const BthLeafResult& bth,
                                                                    const HeapContext& ctx);

    /// Load heap context (data tree + subnodes) for a node
    [[nodiscard]] bool loadNodeHeapContext(const sak::PstNode& entry, HeapContext& ctx);

    /// Read a single BTH record's property value as a formatted string
    [[nodiscard]] QString readBthRecordValue(const BthLeafResult& bth,
                                             const HeapContext& ctx,
                                             int rec_offset);

    /// Scan BTH leaf records for subject and/or message class properties
    void scanBthForSubjectAndClass(const BthLeafResult& bth,
                                   const HeapContext& ctx,
                                   sak::PstItemSummary& item,
                                   bool need_subject,
                                   bool need_class);

    /// Enrich a single item's sender, subject, and class from BTH data
    void enrichItemFromBth(sak::PstItemSummary& item,
                           const BthLeafResult& bth,
                           const HeapContext& ctx);

    /// Enrich a single item's properties from its node data
    void enrichSingleItemProps(sak::PstItemSummary& item);

    /// Enrich item summaries with sender names from their PCs
    void enrichItemSenders(QVector<sak::PstItemSummary>& items);

    /// Read attachment metadata for a message
    [[nodiscard]] std::expected<QVector<sak::PstAttachmentInfo>, sak::error_code> readAttachments(
        uint64_t message_nid);

    /// Read a single attachment's properties from a sub-node
    [[nodiscard]] std::expected<sak::PstAttachmentInfo, sak::error_code> readSingleAttachment(
        const sak::PstNode& subnode, int att_index);

    /// Populate attachment fields from collected BTH leaf records
    void populateAttachmentFromLeaf(sak::PstAttachmentInfo& att,
                                    const BthLeafResult& bth,
                                    const HeapContext& ctx);

    // ======================================================================
    // Utility
    // ======================================================================

    /// Extract a node type from its NID
    [[nodiscard]] static sak::PstNodeType nodeType(uint64_t nid);

    /// Read bytes from the file at the given offset
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readBytes(qint64 offset, qint64 count);

    /// Read a little-endian integer from raw bytes
    template <typename T>
    [[nodiscard]] static T readLE(const QByteArray& data, int offset);

    /// Count total items across all folders
    [[nodiscard]] int countTotalItems() const;

    /// Count total folders in the tree
    [[nodiscard]] static int countFolders(const sak::PstFolderTree& tree);
};
