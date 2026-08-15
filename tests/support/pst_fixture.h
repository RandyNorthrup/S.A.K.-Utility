// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pst_fixture.h
/// @brief Reusable spec-conformant PST store builders (fuzz seeds + accept-path fixture).
///
/// The single home for building legacy-Unicode PST fixtures the PstParser accepts as genuine.
/// Every header CRC, PAGETRAILER, and BLOCKTRAILER is stamped with the same MS-PST weak CRC-32
/// and ComputeSig the parser authenticates against, so these are real spec-conformant files,
/// not near-misses. Two depths:
///
///  * buildEmptyUnicodeStore(): genuine header + Node/Block BTree PAGETRAILERs but EMPTY BTrees.
///    A mutated seed survives the page-trailer checks, so the parser walks INTO parseBTreePage,
///    verifyPageTrailer (success), and buildFolderHierarchy before failing closed there (no root
///    folder node). A deeper reject-path seed than a header-only one.
///
///  * buildOpenableUnicodeStore(): adds a root-folder NBT entry, a BBT entry, and a Heap-on-Node
///    Property-Context data block, so PstParser::open() SUCCEEDS. A seed built from it drives the
///    LTP/messaging accept path -- readPropertyContext, readHeapOnNode, the folder-tree walk --
///    exercising the SUCCESS branches of the integrity gates, not just their rejects.
///
/// The byte layout mirrors test_pst_parser.cpp's own builders (which additionally cover the
/// ANSI, Unicode4k, and compressible-encrypted variants); this header is the shared, reusable
/// version consumed by both the PST fuzz harness and test_pst_parser's lock-in test.

#pragma once

#include "sak/email_constants.h"

#include <QByteArray>

#include <array>
#include <cstdint>
#include <vector>

namespace sak::pst_fixture {

// MS-PST header field offsets (MS-PST 2.2.2.6) and legacy-Unicode page geometry.
inline constexpr int kMagicByte0 = 0x21;  // '!'
inline constexpr int kMagicByte1 = 0x42;  // 'B'
inline constexpr int kMagicByte2 = 0x44;  // 'D'
inline constexpr int kMagicByte3 = 0x4E;  // 'N'
inline constexpr int kContentTypeOffset = 8;
inline constexpr int kVersionOffset = 10;
inline constexpr int kCrcPartialOffset = 4;
inline constexpr int kCrcPartialStart = 8;
inline constexpr int kCrcPartialLen = 471;
inline constexpr int kCrcFullOffset = 0x20C;
inline constexpr int kCrcFullStart = 8;
inline constexpr int kCrcFullLen = 516;
inline constexpr int kCryptOffsetUnicode = 513;
inline constexpr int kHeaderSize = 580;
inline constexpr int kRootOffset = 0xB4;
inline constexpr int kRootFileSizeField = 4;
inline constexpr int kRootNbtField = 44;
inline constexpr int kRootBbtField = 60;

inline constexpr int kPageSize = sak::email::kLegacyUnicodePageSize;  // 512
inline constexpr int kTrailerSize = 16;
inline constexpr int kMetaSize = 4;
inline constexpr int kMetaPad = 4;
inline constexpr int kNbtEntrySize = 32;
inline constexpr int kBbtEntrySize = 24;
inline constexpr int kMaxEntriesPerPage = 0x14;
inline constexpr uint8_t kPtypeNbt = 0x81;
inline constexpr uint8_t kPtypeBbt = 0x80;

inline constexpr uint32_t kWeakCrcPoly = 0xED'B8'83'20u;
inline constexpr int kByteBits = 8;
inline constexpr int kCrcTableSize = 256;
inline constexpr int kSigFold16 = 16;
inline constexpr int kSigFold32 = 32;

// NBTENTRY / BBTENTRY leaf-record field offsets (MS-PST 2.2.2.7.7.3-4, Unicode).
inline constexpr int kNbtEntryNidOffset = 0;
inline constexpr int kNbtEntryDataBidOffset = 8;
inline constexpr int kNbtEntrySubBidOffset = 16;
inline constexpr int kNbtEntryParentOffset = 24;
inline constexpr int kBbtEntryBidOffset = 0;
inline constexpr int kBbtEntryIbOffset = 8;
inline constexpr int kBbtEntryCbOffset = 16;
inline constexpr int kBbtEntryRefOffset = 18;

// Root-folder Property-Context data block: a Heap-on-Node (MS-PST 2.3.1) carrying an
// empty PC BTree-on-Heap (2.3.2). The parser authenticates every one of these fields,
// so their values are the spec, not arbitrary.
inline constexpr uint8_t kHnSignature = 0xEC;        // HNHDR.bSig
inline constexpr uint8_t kPcClientSignature = 0xBC;  // HNHDR.bClientSig for a PC
inline constexpr uint8_t kBthSignature = 0xB5;       // BTHHEADER.bType
inline constexpr int kHnHdrIbHnpmOffset = 0;         // HNHDR.ibHnpm (u16)
inline constexpr int kHnHdrSigOffset = 2;            // HNHDR.bSig
inline constexpr int kHnHdrClientSigOffset = 3;      // HNHDR.bClientSig
inline constexpr int kHnHdrRootHidOffset = 4;        // HNHDR.hidUserRoot (u32)
inline constexpr int kHnHdrSize = 12;                // HNHDR then heap allocations
inline constexpr int kBthHdrOffset = 12;             // first heap allocation == BTHHEADER
inline constexpr int kBthHdrSize = 8;
inline constexpr int kBthKeySizeField = 1;
inline constexpr int kBthDataSizeField = 2;
inline constexpr int kBthIdxLevelsField = 3;
inline constexpr int kBthRootHidField = 4;          // (u32) 0 == empty PC (parser returns {})
inline constexpr int kPcBthKeySize = 2;             // wPropId
inline constexpr int kPcBthDataSize = 6;            // wPropType(2) + dwValueHnid(4)
inline constexpr int kHnPageMapOffset = 20;         // HNPAGEMAP begins here (== ibHnpm)
inline constexpr int kHnPmCallocOffset = 20;        // HNPAGEMAP.cAlloc (u16)
inline constexpr int kHnPmCfreeOffset = 22;         // HNPAGEMAP.cFree (u16)
inline constexpr int kHnPmRgib0Offset = 24;         // rgibAlloc[0] (u16) == BTHHEADER start
inline constexpr int kHnPmRgib1Offset = 26;         // rgibAlloc[1] (u16) == BTHHEADER end
inline constexpr int kRootBlockCb = 28;             // HNHDR + BTHHEADER + HNPAGEMAP
inline constexpr int kRootBlockDiskSize = 64;       // (cb + 16 trailer) rounded up to 64
inline constexpr int kRootBlockTrailerOffset = 48;  // kRootBlockDiskSize - 16
inline constexpr int kBlockTrailerCbField = 0;      // BLOCKTRAILER.cb (u16)
inline constexpr int kBlockTrailerSigField = 2;     // BLOCKTRAILER.wSig (u16)
inline constexpr int kBlockTrailerCrcField = 4;     // BLOCKTRAILER.dwCRC (u32)
inline constexpr int kBlockTrailerBidField = 8;     // BLOCKTRAILER.bid (u64)
inline constexpr uint32_t kRootFolderNid = sak::email::kNidRootFolder;  // 0x122
inline constexpr uint64_t kRootFolderDataBid = 4;  // external (fInternal bit 0x02 clear)
inline constexpr uint32_t kBthHeaderHid = 0x20;    // HID: index 1, block 0 (1 << 5)

// buildOpenableUnicodeStore region offsets (two zero pages of headroom, then NBT / BBT /
// PC block). Exposed so a structure-aware fuzz can mutate one region's BODY and re-stamp
// just that region's integrity fields, keeping the file integral (see the restamp*() below).
inline constexpr int kOpenableNbtOffset = kPageSize * 2;           // 0x400
inline constexpr int kOpenableBbtOffset = kPageSize * 3;           // 0x600
inline constexpr int kOpenableBlockOffset = kPageSize * 4;         // 0x800
inline constexpr int kOpenableFileSize = kOpenableBlockOffset + kRootBlockDiskSize;
inline constexpr int kLeafPageBodyLen = kPageSize - kTrailerSize;  // CRC-covered page body

// Foldered store: a root folder whose hierarchy Table Context lists one child folder, so
// PstParser::open() walks readTableContext -> parseTcInfo -> buildTcRows -> extractChildNids
// -> recurse into the child. The hierarchy TC node NID is (root & ~0x1F) | NID_TYPE_HIERARCHY.
inline constexpr uint32_t kHierarchyTableNid = 0x12D;  // (0x122 & ~0x1F) | 0x0D
inline constexpr uint32_t kChildFolderNid = 0x142;
inline constexpr uint64_t kHierarchyDataBid = 8;     // external (0x02 clear); generic 2nd-block BID
inline constexpr uint64_t kChildFolderDataBid = 12;  // generic 3rd-block BID

// Messaging store: the root folder's CONTENTS Table Context lists one message, so open() + a
// readFolderItems() walk drive readContentsTable -> readTableContext and readItemDetail ->
// readMessage. Same three-block layout as the foldered store; only the TC node type (0x0E
// contents vs 0x0D hierarchy) and the leaf node type (0x04 message vs a folder) differ.
inline constexpr uint32_t kContentsTableNid = 0x12E;  // (0x122 & ~0x1F) | 0x0E
inline constexpr uint32_t kMessageNid = 0x24;         // (1 << 5) | 0x04 normal-message type

// Populated message PC: a non-empty PC BTree-on-Heap with one Subject record whose value is an
// HNID pointing at a heap-stored UTF-16 string. Exercises parsePropertyRecords' variable-type
// branch -> resolveHnid -> formatUnicodeValue -> the Subject detail setter. Three heap allocations:
// the BTHHEADER, a one-record BTH leaf, and the subject string.
inline constexpr uint16_t kSubjectPropId =
    0x0037;                                  // PidTagSubjectW (== sak::email::kPropIdSubject)
inline constexpr int kPcRecordKeySize = 2;   // wPropId
inline constexpr int kPcValueRefOffset = 2;  // dwValueHnid sits key+2 into the record
inline constexpr int kPcRecordSize = 8;      // key(2) + type(2) + HNID(4)
inline constexpr int kMsgBthHdrOffset = kHnHdrSize;                           // 12
inline constexpr int kMsgBthLeafOffset = kMsgBthHdrOffset + kBthHdrSize;      // 20
inline constexpr int kMsgSubjectOffset = kMsgBthLeafOffset + kPcRecordSize;   // 28
inline constexpr int kMsgSubjectCharCount = 4;                                // "FUZZ"
inline constexpr int kMsgSubjectLen = kMsgSubjectCharCount * 2;               // UTF-16LE bytes == 8
inline constexpr int kMsgPageMapOffset = kMsgSubjectOffset + kMsgSubjectLen;  // 36
inline constexpr int kMessagePcCb = kMsgPageMapOffset +
                                    12;           // + HNPAGEMAP(cAlloc/cFree + 4 rgib)==48
inline constexpr uint32_t kMsgBthLeafHid = 0x40;  // HID index 2 (BTH leaf)
inline constexpr uint32_t kMsgSubjectHid = 0x60;  // HID index 3 (subject string)

// Generic Heap-on-Node PC geometry, for a block carrying N variable-type records (each an 8-byte
// BTH record whose HNID resolves to a heap value). HID == (allocation-index << 5); the BTHHEADER is
// index 1, the BTH leaf index 2, and the first value index 3.
inline constexpr int kHnPmHeaderLen = 4;       // HNPAGEMAP: cAlloc(u16) + cFree(u16)
inline constexpr int kHnPmEntryLen = 2;        // each rgibAlloc boundary entry (u16)
inline constexpr int kPcFixedAllocCount = 2;   // BTHHEADER + BTH leaf, before the value allocations
inline constexpr int kHidIndexShift = 5;       // HID = (allocation index << 5), block 0
inline constexpr int kFirstValueHidIndex = 3;  // first value allocation is HID index 3

// Table Context on-heap layout (MS-PST 2.3.4). A single 4-byte column (PidTagLtpRowId, whose
// cell IS the child NID) and no TCROWID BTH (hidRowIndex 0 -> the parser enumerates the one row).
inline constexpr uint8_t kTcClientSignature = 0x7C;  // HNHDR.bClientSig for a TC; also TCINFO.bType
inline constexpr int kTcinfoBTypeOffset = 0;
inline constexpr int kTcinfoColCountOffset = 1;
inline constexpr int kTcinfoRgib4bOffset = 2;
inline constexpr int kTcinfoRgib2bOffset = 4;
inline constexpr int kTcinfoRgib1bOffset = 6;  // CEB offset within a row
inline constexpr int kTcinfoRgibBmOffset = 8;  // row size
inline constexpr int kTcinfoHidRowIndexOffset = 10;
inline constexpr int kTcinfoHnidRowsOffset = 14;
inline constexpr int kTcinfoHeaderLen = 22;
inline constexpr int kTcColDescLen = 8;
inline constexpr int kTcColTypeOffset = 0;
inline constexpr int kTcColPropIdOffset = 2;
inline constexpr int kTcColIbDataOffset = 4;
inline constexpr int kTcColDataSizeOffset = 6;
inline constexpr int kTcColBitIndexOffset = 7;
inline constexpr uint16_t kPidTagLtpRowId = 0x67F2;
inline constexpr int kTcRowCellSize = 4;              // the LtpRowId cell (a 4-byte NID)
inline constexpr int kTcRowSize = 5;                  // 4-byte cell + 1 CEB byte
inline constexpr uint8_t kCebFirstColPresent = 0x80;  // CEB high bit set == column 0 present
inline constexpr uint32_t kTcHidUserRoot = 0x20;      // HID index 1 (TCINFO)
inline constexpr uint32_t kTcHnidRows = 0x40;         // HID index 2 (row matrix)
inline constexpr int kTcinfoOffset = kHnHdrSize;      // 12
inline constexpr int kTcRowMatrixOffset = kTcinfoOffset + kTcinfoHeaderLen + kTcColDescLen;  // 42
inline constexpr int kTcPageMapOffset = kTcRowMatrixOffset + kTcRowSize;                     // 47
inline constexpr int kTcBlockCb = kTcPageMapOffset +
                                  10;  // + HNPAGEMAP(cAlloc/cFree + 3 rgib) == 57
inline constexpr int kTcBlockDiskSize = ((kTcBlockCb + kTrailerSize + 63) / 64) * 64;  // 128

// Row-indexed TC variant: the same one-column single-row TC, but with a real TCROWID BTH after the
// row matrix so hidRowIndex != 0. The parser then walks the live-row BTH
// (collectTcLiveRowIndices -> extractTcRowIndicesFromLeaf) to find the one live row instead of
// enumerating physical matrix slots -- the fallback path the hidRowIndex==0 TC takes. Two extra
// heap allocations follow the row matrix: the BTHHEADER (HID index 3, 0x60) and its 8-byte TCROWID
// leaf (HID index 4, 0x80). TCROWID record == dwRowID(4, the key) + dwRowIndex(4, the logical row).
inline constexpr uint32_t kTcRowIndexBthHid = 0x60;         // HID index 3 (BTHHEADER)
inline constexpr uint32_t kTcRowIndexLeafHid = 0x80;        // HID index 4 (BTH leaf)
inline constexpr int kTcRiBthHdrOffset = kTcPageMapOffset;  // 47 (alloc 3)
inline constexpr int kTcRiBthLeafOffset = kTcRiBthHdrOffset + kBthHdrSize;  // 55 (alloc 4)
inline constexpr int kTcRowIdRecordSize = 8;  // dwRowID(4) + dwRowIndex(4)
inline constexpr int kTcRiPageMapOffset = kTcRiBthLeafOffset + kTcRowIdRecordSize;  // 63
inline constexpr int kTcRiAllocCount = 4;  // TCINFO, row matrix, BTHHEADER, BTH leaf
inline constexpr int kTcRiPageMapLen = kHnPmHeaderLen +
                                       (kHnPmEntryLen * (kTcRiAllocCount + 1));            // 14
inline constexpr int kTcRiBlockCb = kTcRiPageMapOffset + kTcRiPageMapLen;                  // 77
inline constexpr int kTcRiBlockDiskSize = ((kTcRiBlockCb + kTrailerSize + 63) / 64) * 64;  // 128
inline constexpr uint8_t kTcRowIdKeyDataSize = 4;  // TCROWID key and data are both 4 bytes

// Two-column TC variant: column 0 is the literal PidTagLtpRowId (so the row's NID is still
// extractable), column 1 is an HNID-resolvable Unicode column whose 4-byte cell is an HID pointing
// at a heap allocation holding the value bytes. Reading the row drives buildTcCell's resolveHnid
// branch (isHnidResolvableType && cb_data == kPropertyValueRefSize) -- the HNID-cell path the
// literal Int32 column never triggers. Three heap allocations: TCINFO (two columns), the row
// matrix, and the resolved value.
inline constexpr int kTc2ColCount = 2;
inline constexpr int kTc2CellSize = 4;                  // each cell: the LtpRowId NID / the HNID
inline constexpr int kTc2CebOffset = kTc2CellSize * 2;  // 8: CEB byte after the two 4-byte cells
inline constexpr int kTc2RowSize = kTc2CebOffset + 1;   // 9: two cells + one CEB byte
inline constexpr int kTc2ColDescOffset = kTcinfoOffset + kTcinfoHeaderLen;    // 34: first col desc
inline constexpr int kTc2Col1DescOffset = kTc2ColDescOffset + kTcColDescLen;  // 42: second col desc
inline constexpr int kTc2RowMatrixOffset = kTc2ColDescOffset +
                                           (kTcColDescLen * kTc2ColCount);    // 50
inline constexpr int kTc2ValueOffset = kTc2RowMatrixOffset + kTc2RowSize;     // 59
inline constexpr int kTc2ValueLen = 4;                                        // "HI" as UTF-16LE
inline constexpr uint32_t kTc2ValueHid = 0x60;  // HID index 3 (the resolved value alloc)
inline constexpr uint16_t kTc2HnidColPropId = kSubjectPropId;  // Subject (Unicode, HNID-resolved)
inline constexpr int kTc2PageMapOffset = kTc2ValueOffset + kTc2ValueLen;  // 63
inline constexpr int kTc2AllocCount = 3;  // TCINFO, row matrix, resolved value
inline constexpr int kTc2PageMapLen = kHnPmHeaderLen +
                                      (kHnPmEntryLen * (kTc2AllocCount + 1));  // 12
inline constexpr int kTc2BlockCb = kTc2PageMapOffset + kTc2PageMapLen;         // 75
inline constexpr uint8_t kTc2Col1BitIndex = 1;  // column 1's CEB bit (column 0 is bit 0)

// Foldered store block offsets (same NBT/BBT pages as the openable store; three blocks after).
inline constexpr int kFolderedRootBlockOffset = kOpenableBlockOffset;                     // 0x800
inline constexpr int kFolderedTcBlockOffset = kFolderedRootBlockOffset +
                                              kRootBlockDiskSize;                         // 0x840
inline constexpr int kFolderedChildBlockOffset = kFolderedTcBlockOffset +
                                                 kTcBlockDiskSize;                        // 0x8C0
inline constexpr int kFolderedFileSize = kFolderedChildBlockOffset + kRootBlockDiskSize;  // 0x900

// Attachment store: the messaging store's message extended with a sub-node BTree carrying ONE
// attachment node (NID type 0x05). Drives the whole attachment path no other fixture reaches --
// readSubNodeBTree -> readSubNodeLeafEntries, readAttachments -> readSingleAttachment ->
// populateAttachmentFromLeaf, and readAttachmentData -> extractAttachmentFromSubnode ->
// resolveHnid.
inline constexpr uint32_t kAttachmentNid = (1u << 5) | sak::email::kNidTypeAttachment;  // 0x25
inline constexpr uint64_t kAttachSubnodeBid = 16;  // message's sub-node BTree block (external BID)
inline constexpr uint64_t kAttachDataBid = 20;     // attachment PC data block
// Sub-node SLBLOCK (MS-PST 2.2.2.8.3.3): an 8-byte header (btype, cLevel, cEnt[u16], pad[4]) then
// 24-byte Unicode SLENTRYs (nid[8] + bidData[8] + bidSub[8]).
inline constexpr int kSubnodeBlockHeaderLen = 8;
inline constexpr int kSubnodeCEntOffset = 2;
inline constexpr int kSlEntryDataBidOffset = 8;
inline constexpr int kSlEntrySubBidOffset = 16;
inline constexpr int kSlEntryLen = 24;
inline constexpr int kSubnodeBlockCb = kSubnodeBlockHeaderLen + kSlEntryLen;  // 32
inline constexpr uint8_t kSubnodeBlockBType = 0x02;  // SLBLOCK btype (realistic; not validated)
// Attachment store block offsets: the three foldered blocks, then the sub-node block + attach PC.
inline constexpr int kAttachSubnodeBlockOffset = kFolderedChildBlockOffset +
                                                 kRootBlockDiskSize;               // 0x900
inline constexpr int kAttachPcBlockOffset = kAttachSubnodeBlockOffset +
                                            kRootBlockDiskSize;                    // 0x940
inline constexpr int kAttachFileSize = kAttachPcBlockOffset + kRootBlockDiskSize;  // 0x980

// XBLOCK store: the message's data is an internal XBLOCK (MS-PST 2.2.2.8.3.2) referencing two
// external child data blocks whose bytes concatenate into the 48-byte message PC. readDataTree sees
// the internal bit, expands the XBLOCK, and reassembles the children -- the multi-block data-tree
// path (readInternalDataBlock / readXblockChildren) no single-block fixture reaches.
inline constexpr uint64_t kXblockBid = 10;        // internal (fInternal bit 0x02 SET)
inline constexpr uint64_t kXblockChild0Bid = 12;  // external data block (bit 0x02 clear)
inline constexpr uint64_t kXblockChild1Bid = 16;  // external data block
inline constexpr int kXblockHeaderLen = 8;        // btype + cLevel + cEnt(u16) + lcbTotal(u32)
inline constexpr int kXblockCb = kXblockHeaderLen +
                                 (2 * 8);         // header + two 8-byte child BIDs == 24
inline constexpr uint8_t kXblockBType = 0x01;     // internal block btype
inline constexpr uint8_t kXblockLevel = 1;        // 1 == XBLOCK (2 == XXBLOCK)
inline constexpr int kXblockChild0Cb = 32;        // first slice of the 48-byte message PC
inline constexpr int kXblockChild1Cb = kMessagePcCb - kXblockChild0Cb;                // 16
inline constexpr int kXblockRootBlockOffset = kFolderedRootBlockOffset;               // 0x800
inline constexpr int kXblockTcBlockOffset = kFolderedTcBlockOffset;                   // 0x840
inline constexpr int kXblockBlockOffset = kFolderedChildBlockOffset;                  // 0x8C0
inline constexpr int kXblockChild0Offset = kXblockBlockOffset + kRootBlockDiskSize;   // 0x900
inline constexpr int kXblockChild1Offset = kXblockChild0Offset + kRootBlockDiskSize;  // 0x940
inline constexpr int kXblockFileSize = kXblockChild1Offset + kRootBlockDiskSize;      // 0x980

// Two-level XXBLOCK message store: the message data BID is an XXBLOCK (cLevel==2) whose two entries
// are XBLOCKs (cLevel==1), and each XBLOCK's two entries are external data blocks. readDataTree
// recurses XXBLOCK -> XBLOCK -> data and concatenates the four external children into the 48-byte
// message PC. This is the readXxblockChildren path (the deepest data-tree level) that the flat
// single-XBLOCK fixture never reaches. Internal blocks carry fInternal (bid & 0x02 SET); external
// data blocks clear it -- hence the 2-mod-4 vs 0-mod-4 bid split.
inline constexpr uint64_t kXxblockBid = 10;           // internal XXBLOCK (fInternal SET)
inline constexpr uint64_t kXxXblock0Bid = 14;         // internal XBLOCK  (fInternal SET)
inline constexpr uint64_t kXxXblock1Bid = 18;         // internal XBLOCK  (fInternal SET)
inline constexpr uint64_t kXxData0Bid = 12;           // external data block (fInternal clear)
inline constexpr uint64_t kXxData1Bid = 16;           // external data block
inline constexpr uint64_t kXxData2Bid = 20;           // external data block
inline constexpr uint64_t kXxData3Bid = 24;           // external data block
inline constexpr uint8_t kXxblockLevel = 2;           // 2 == XXBLOCK
inline constexpr int kXxDataCb = kMessagePcCb / 4;    // 12: each external block holds a quarter
inline constexpr int kXxXblockTotal = kXxDataCb * 2;  // 24: bytes under one XBLOCK
inline constexpr int kXxblockRootBlockOffset = kFolderedRootBlockOffset;           // 0x800
inline constexpr int kXxblockTcBlockOffset = kFolderedTcBlockOffset;               // 0x840
inline constexpr int kXxblockBlockOffset = kFolderedChildBlockOffset;              // 0x8C0 XXBLOCK
inline constexpr int kXxXblock0Offset = kXxblockBlockOffset + kRootBlockDiskSize;  // 0x900
inline constexpr int kXxXblock1Offset = kXxXblock0Offset + kRootBlockDiskSize;     // 0x940
inline constexpr int kXxData0Offset = kXxXblock1Offset + kRootBlockDiskSize;       // 0x980
inline constexpr int kXxData1Offset = kXxData0Offset + kRootBlockDiskSize;         // 0x9C0
inline constexpr int kXxData2Offset = kXxData1Offset + kRootBlockDiskSize;         // 0xA00
inline constexpr int kXxData3Offset = kXxData2Offset + kRootBlockDiskSize;         // 0xA40
inline constexpr int kXxblockFileSize = kXxData3Offset + kRootBlockDiskSize;       // 0xA80

// Enrichable message store: the message PC carries Subject + MessageClass + SenderName, so a
// loadFolderItems() enrichment pass (extractSenderFromLeaf + scanBthForSubjectAndClass +
// classifyMessageClass) actually populates item_type and sender_name -- the enrichment code the
// single-Subject message never triggers. Same three-slot layout; the leaf PC's cb is dynamic, so
// its block offsets reuse the foldered offsets and its disk size is computed at build time.
inline constexpr int kEnrichRootBlockOffset = kFolderedRootBlockOffset;  // 0x800
inline constexpr int kEnrichTcBlockOffset = kFolderedTcBlockOffset;      // 0x840
inline constexpr int kEnrichPcBlockOffset = kFolderedChildBlockOffset;   // 0x8C0

inline void writeLe16(QByteArray& data, int offset, uint16_t value) {
    data[offset] = static_cast<char>(value & 0xFF);
    data[offset + 1] = static_cast<char>((value >> kByteBits) & 0xFF);
}

inline void writeLe32(QByteArray& data, int offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * kByteBits)) & 0xFF);
    }
}

inline void writeLe64(QByteArray& data, int offset, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        data[offset + i] = static_cast<char>((value >> (i * kByteBits)) & 0xFF);
    }
}

// MS-PST weak CRC-32: reflected polynomial, zero init, no final inversion.
inline uint32_t weakCrc(const QByteArray& data, int offset, int len) {
    static const std::array<uint32_t, kCrcTableSize> table = [] {
        std::array<uint32_t, kCrcTableSize> built{};
        for (uint32_t i = 0; i < kCrcTableSize; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < kByteBits; ++bit) {
                c = (c & 1u) ? (kWeakCrcPoly ^ (c >> 1)) : (c >> 1);
            }
            built[i] = c;
        }
        return built;
    }();
    uint32_t crc = 0;
    for (int i = 0; i < len; ++i) {
        const auto byte = static_cast<uint8_t>(data.at(offset + i));
        crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> kByteBits);
    }
    return crc;
}

// MS-PST 5.4 ComputeSig: fold (ib XOR bid) to 16 bits.
inline uint16_t computeSig(uint64_t ib, uint64_t bid) {
    uint64_t value = ib ^ bid;
    value ^= (value >> kSigFold16);
    value ^= (value >> kSigFold32);
    return static_cast<uint16_t>(value & 0xFFFFu);
}

// A legacy-Unicode BTree page with an empty entry list and a valid PAGETRAILER.
inline QByteArray makeEmptyBTreePage(uint8_t ptype, int entry_size, int page_offset) {
    QByteArray page(kPageSize, '\0');
    const int trailer_offset = kPageSize - kTrailerSize;
    const int meta_offset = trailer_offset - kMetaPad - kMetaSize;
    page[meta_offset] = 0;  // entry count
    page[meta_offset + 1] = static_cast<char>(kMaxEntriesPerPage);
    page[meta_offset + 2] = static_cast<char>(entry_size);
    page[meta_offset + 3] = 0;
    page[trailer_offset] = static_cast<char>(ptype);
    page[trailer_offset + 1] = static_cast<char>(ptype);
    // PAGETRAILER: wSig at +2, dwCRC at +4 (over the page body), bid (8 bytes) at +8 == 0.
    writeLe16(page, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(page, trailer_offset + 4, weakCrc(page, 0, kPageSize - kTrailerSize));
    return page;
}

/// Build a legacy-Unicode PST with empty Node and Block BTrees that PstParser::open()
/// accepts: genuine header CRCs and genuine page trailers, so the parser walks into the
/// BTree-load and folder-hierarchy code before finding nothing to enumerate.
inline QByteArray buildEmptyUnicodeStore() {
    const int nbt_offset = kPageSize * 2;
    const int bbt_offset = nbt_offset + kPageSize;
    const int file_size = bbt_offset + kPageSize;

    QByteArray file(file_size, '\0');
    file[0] = static_cast<char>(kMagicByte0);
    file[1] = static_cast<char>(kMagicByte1);
    file[2] = static_cast<char>(kMagicByte2);
    file[3] = static_cast<char>(kMagicByte3);
    writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
    writeLe16(file, kVersionOffset, sak::email::kUnicodeVersion);
    file[kCryptOffsetUnicode] = static_cast<char>(sak::email::kEncryptNone);

    writeLe64(file, kRootOffset + kRootFileSizeField, static_cast<uint64_t>(file_size));
    writeLe64(file, kRootOffset + kRootNbtField, static_cast<uint64_t>(nbt_offset));
    writeLe64(file, kRootOffset + kRootBbtField, static_cast<uint64_t>(bbt_offset));

    file.replace(nbt_offset, kPageSize, makeEmptyBTreePage(kPtypeNbt, kNbtEntrySize, nbt_offset));
    file.replace(bbt_offset, kPageSize, makeEmptyBTreePage(kPtypeBbt, kBbtEntrySize, bbt_offset));

    // Header CRCs last, over the finished ROOT pointers.
    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
    return file;
}

// A legacy-Unicode BTree leaf page holding entry_count entries (level 0) with a valid
// PAGETRAILER. entries (entry_count * entry_size bytes) is copied to page offset 0.
inline QByteArray makeLeafPageWithEntries(
    uint8_t ptype, int entry_size, int page_offset, const QByteArray& entries, int entry_count) {
    QByteArray page(kPageSize, '\0');
    const int trailer_offset = kPageSize - kTrailerSize;
    const int meta_offset = trailer_offset - kMetaPad - kMetaSize;
    page.replace(0, entries.size(), entries);
    page[meta_offset] = static_cast<char>(entry_count);  // cEnt
    page[meta_offset + 1] = static_cast<char>(kMaxEntriesPerPage);
    page[meta_offset + 2] = static_cast<char>(entry_size);
    page[meta_offset + 3] = 0;  // cLevel 0 == leaf
    page[trailer_offset] = static_cast<char>(ptype);
    page[trailer_offset + 1] = static_cast<char>(ptype);
    writeLe16(page, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(page, trailer_offset + 4, weakCrc(page, 0, kPageSize - kTrailerSize));
    return page;
}

// One-entry convenience wrapper (the openable store's NBT/BBT each carry a single entry).
inline QByteArray makeLeafPageWithEntry(uint8_t ptype,
                                        int entry_size,
                                        int page_offset,
                                        const QByteArray& entry_bytes) {
    return makeLeafPageWithEntries(ptype, entry_size, page_offset, entry_bytes, 1);
}

// The on-disk span of a block: cb data bytes + a 16-byte BLOCKTRAILER, rounded up to 64.
inline int blockDiskSize(int cb) {
    return ((cb + kTrailerSize + 63) / 64) * 64;
}

// Stamp a BLOCKTRAILER at the end of the block that begins at block_pos in buf: cb, wSig from
// (sig_offset, bid), dwCRC over the cb data bytes, and the bid. The single source for the
// trailer formula, used by every block builder and by the structure-fuzz re-stamp.
inline void stampBlockTrailer(
    QByteArray& buf, int block_pos, int cb, uint64_t bid, uint64_t sig_offset) {
    const int t = block_pos + blockDiskSize(cb) - kTrailerSize;
    writeLe16(buf, t + kBlockTrailerCbField, static_cast<uint16_t>(cb));
    writeLe16(buf, t + kBlockTrailerSigField, computeSig(sig_offset, bid));
    writeLe32(buf, t + kBlockTrailerCrcField, weakCrc(buf, block_pos, cb));
    writeLe64(buf, t + kBlockTrailerBidField, bid);
}

// The root/child folder's data block: a Heap-on-Node whose PC BTree-on-Heap has hidRoot == 0
// (an empty property context). PstParser::readPropertyContext walks HNHDR -> BTHHEADER, sees the
// zero root HID, and returns an empty-but-valid PC -- which is what lets buildFolderHierarchy
// succeed for that folder NID. A genuine BLOCKTRAILER authenticates the 64-byte disk region.
inline QByteArray buildRootFolderPcBlock(uint64_t file_offset, uint64_t bid) {
    QByteArray blk(kRootBlockDiskSize, '\0');
    // HNHDR
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(kHnPageMapOffset));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kPcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kBthHeaderHid);
    // BTHHEADER (heap allocation 1): empty PC -> root HID 0.
    blk[kBthHdrOffset] = static_cast<char>(kBthSignature);
    blk[kBthHdrOffset + kBthKeySizeField] = static_cast<char>(kPcBthKeySize);
    blk[kBthHdrOffset + kBthDataSizeField] = static_cast<char>(kPcBthDataSize);
    blk[kBthHdrOffset + kBthIdxLevelsField] = 0;
    writeLe32(blk, kBthHdrOffset + kBthRootHidField, 0);
    // HNPAGEMAP: one allocation spanning [kBthHdrOffset, kHnPageMapOffset).
    writeLe16(blk, kHnPmCallocOffset, 1);
    writeLe16(blk, kHnPmCfreeOffset, 0);
    writeLe16(blk, kHnPmRgib0Offset, static_cast<uint16_t>(kBthHdrOffset));
    writeLe16(blk, kHnPmRgib1Offset, static_cast<uint16_t>(kHnPageMapOffset));
    stampBlockTrailer(blk, 0, kRootBlockCb, bid, file_offset);
    return blk;
}

// Write the HNHDR + TCINFO + one PidTagLtpRowId column + the one-row matrix common to both TC
// variants. @p pagemap_off is the HNPAGEMAP start (it moves when extra allocations follow the row
// matrix); @p hid_row_index is the TCINFO hidRowIndex (0 == enumerate the single physical row;
// nonzero == an HID pointing at a TCROWID BTH the parser must walk). @p row_value is the row's one
// cell -- a child folder NID (hierarchy table) or message NID (contents table).
inline void writeTcInfoAndRow(QByteArray& blk,
                              int pagemap_off,
                              uint32_t hid_row_index,
                              uint32_t row_value) {
    // HNHDR -> TCINFO as the user root.
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(pagemap_off));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kTcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kTcHidUserRoot);
    // TCINFO (heap allocation 1): one 4-byte column, row size 5, CEB at offset 4.
    const int ti = kTcinfoOffset;
    blk[ti + kTcinfoBTypeOffset] = static_cast<char>(kTcClientSignature);
    blk[ti + kTcinfoColCountOffset] = 1;
    writeLe16(blk, ti + kTcinfoRgib4bOffset, static_cast<uint16_t>(kTcRowCellSize));
    writeLe16(blk, ti + kTcinfoRgib2bOffset, static_cast<uint16_t>(kTcRowCellSize));
    writeLe16(blk, ti + kTcinfoRgib1bOffset, static_cast<uint16_t>(kTcRowCellSize));
    writeLe16(blk, ti + kTcinfoRgibBmOffset, static_cast<uint16_t>(kTcRowSize));
    writeLe32(blk, ti + kTcinfoHidRowIndexOffset, hid_row_index);
    writeLe32(blk, ti + kTcinfoHnidRowsOffset, kTcHnidRows);
    // Column descriptor (PtypInteger32 so the 4 cell bytes are read literally, not HNID-resolved).
    const int col = ti + kTcinfoHeaderLen;
    writeLe16(blk, col + kTcColTypeOffset, sak::email::kPropTypeInt32);
    writeLe16(blk, col + kTcColPropIdOffset, kPidTagLtpRowId);
    writeLe16(blk, col + kTcColIbDataOffset, 0);
    blk[col + kTcColDataSizeOffset] = static_cast<char>(kTcRowCellSize);
    blk[col + kTcColBitIndexOffset] = 0;
    // Row matrix (heap allocation 2): the row-value cell + a CEB marking column 0 present.
    writeLe32(blk, kTcRowMatrixOffset, row_value);
    blk[kTcRowMatrixOffset + kTcRowCellSize] = static_cast<char>(kCebFirstColPresent);
}

// A single-row Table Context data block: an HN (client sig 0x7C) whose TCINFO declares one
// PidTagLtpRowId column and one row whose cell is @p row_value. hidRowIndex is 0, so the parser
// enumerates the single physical row. As a hierarchy table row_value is a child folder NID; as a
// contents table it is a message NID -- the byte layout is identical, only the owning node differs.
inline QByteArray buildSingleRowTcBlock(uint64_t file_offset, uint64_t bid, uint32_t row_value) {
    QByteArray blk(blockDiskSize(kTcBlockCb), '\0');
    writeTcInfoAndRow(blk, kTcPageMapOffset, 0, row_value);
    // HNPAGEMAP: two allocations -- TCINFO [12, 42) and the row matrix [42, 47).
    writeLe16(blk, kTcPageMapOffset, 2);
    writeLe16(blk, kTcPageMapOffset + 2, 0);
    writeLe16(blk, kTcPageMapOffset + 4, static_cast<uint16_t>(kTcinfoOffset));
    writeLe16(blk, kTcPageMapOffset + 6, static_cast<uint16_t>(kTcRowMatrixOffset));
    writeLe16(blk, kTcPageMapOffset + 8, static_cast<uint16_t>(kTcPageMapOffset));
    stampBlockTrailer(blk, 0, kTcBlockCb, bid, file_offset);
    return blk;
}

// The row-indexed TC variant: the same one-column single-row TC, but with a real TCROWID BTH so
// hidRowIndex != 0. The BTHHEADER (alloc 3, HID 0x60) has idxLevels 0 and roots at the 8-byte
// TCROWID leaf (alloc 4, HID 0x80); the leaf's one record names logical row 0. Reading this table
// drives collectTcLiveRowIndices -> extractTcRowIndicesFromLeaf (the live-row BTH walk) rather than
// the physical-slot fallback -- the hidRowIndex != 0 path buildSingleRowTcBlock never reaches.
inline QByteArray buildRowIndexedTcBlock(uint64_t file_offset, uint64_t bid, uint32_t row_value) {
    QByteArray blk(blockDiskSize(kTcRiBlockCb), '\0');
    writeTcInfoAndRow(blk, kTcRiPageMapOffset, kTcRowIndexBthHid, row_value);
    // BTHHEADER (alloc 3): key 4 / data 4, idxLevels 0, root HID -> the TCROWID leaf.
    blk[kTcRiBthHdrOffset] = static_cast<char>(kBthSignature);
    blk[kTcRiBthHdrOffset + kBthKeySizeField] = static_cast<char>(kTcRowIdKeyDataSize);
    blk[kTcRiBthHdrOffset + kBthDataSizeField] = static_cast<char>(kTcRowIdKeyDataSize);
    blk[kTcRiBthHdrOffset + kBthIdxLevelsField] = 0;
    writeLe32(blk, kTcRiBthHdrOffset + kBthRootHidField, kTcRowIndexLeafHid);
    // TCROWID leaf (alloc 4): dwRowID (the row key, here the same NID) + dwRowIndex (logical row
    // 0).
    writeLe32(blk, kTcRiBthLeafOffset, row_value);
    writeLe32(blk, kTcRiBthLeafOffset + kTcRowIdKeyDataSize, 0);
    // HNPAGEMAP: four allocations -- TCINFO [12, 42), row matrix [42, 47), BTHHEADER [47, 55),
    // TCROWID leaf [55, 63).
    writeLe16(blk, kTcRiPageMapOffset, static_cast<uint16_t>(kTcRiAllocCount));
    writeLe16(blk, kTcRiPageMapOffset + kHnPmEntryLen, 0);
    const int rgib = kTcRiPageMapOffset + kHnPmHeaderLen;
    const std::array<int, 5> boundaries{{kTcinfoOffset,
                                         kTcRowMatrixOffset,
                                         kTcRiBthHdrOffset,
                                         kTcRiBthLeafOffset,
                                         kTcRiPageMapOffset}};
    for (int i = 0; i < static_cast<int>(boundaries.size()); ++i) {
        writeLe16(blk, rgib + (i * kHnPmEntryLen), static_cast<uint16_t>(boundaries[i]));
    }
    stampBlockTrailer(blk, 0, kTcRiBlockCb, bid, file_offset);
    return blk;
}

// One TCINFO column descriptor: MAPI type, property id, ib_data (cell offset within the row),
// cb_data (cell byte width), and the CEB bit index that marks the column present.
struct TcColSpec {
    uint16_t type;
    uint16_t prop_id;
    int ib_data;
    int cb_data;
    int bit;
};

// Write one TCINFO column descriptor at @p off.
inline void writeTcColDesc(QByteArray& blk, int off, const TcColSpec& col) {
    writeLe16(blk, off + kTcColTypeOffset, col.type);
    writeLe16(blk, off + kTcColPropIdOffset, col.prop_id);
    writeLe16(blk, off + kTcColIbDataOffset, static_cast<uint16_t>(col.ib_data));
    blk[off + kTcColDataSizeOffset] = static_cast<char>(col.cb_data);
    blk[off + kTcColBitIndexOffset] = static_cast<char>(col.bit);
}

// A two-column single-row TC whose second column is an HNID-resolvable Unicode cell: the 4-byte
// cell holds an HID pointing at a heap allocation with the value "HI" (UTF-16LE). Reading the row
// drives buildTcCell's resolveHnid branch (isHnidResolvableType && cb_data == 4) -- the HNID-cell
// path the literal Int32 column never triggers. Column 0 stays PidTagLtpRowId so the row's NID is
// still extractable and the message still lists.
inline QByteArray buildHnidCellTcBlock(uint64_t file_offset, uint64_t bid, uint32_t row_value) {
    QByteArray blk(blockDiskSize(kTc2BlockCb), '\0');
    // HNHDR -> TCINFO as the user root.
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(kTc2PageMapOffset));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kTcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kTcHidUserRoot);
    // TCINFO: two 4-byte columns, CEB at offset 8, row size 9.
    const int ti = kTcinfoOffset;
    blk[ti + kTcinfoBTypeOffset] = static_cast<char>(kTcClientSignature);
    blk[ti + kTcinfoColCountOffset] = static_cast<char>(kTc2ColCount);
    writeLe16(blk, ti + kTcinfoRgib4bOffset, static_cast<uint16_t>(kTc2CebOffset));
    writeLe16(blk, ti + kTcinfoRgib2bOffset, static_cast<uint16_t>(kTc2CebOffset));
    writeLe16(blk, ti + kTcinfoRgib1bOffset, static_cast<uint16_t>(kTc2CebOffset));
    writeLe16(blk, ti + kTcinfoRgibBmOffset, static_cast<uint16_t>(kTc2RowSize));
    writeLe32(blk, ti + kTcinfoHidRowIndexOffset, 0);
    writeLe32(blk, ti + kTcinfoHnidRowsOffset, kTcHnidRows);
    // Column 0: literal LtpRowId (Int32). Column 1: HNID-resolved Unicode (cell is an HID).
    writeTcColDesc(blk,
                   kTc2ColDescOffset,
                   {sak::email::kPropTypeInt32, kPidTagLtpRowId, 0, kTc2CellSize, 0});
    writeTcColDesc(blk,
                   kTc2Col1DescOffset,
                   {sak::email::kPropTypeUnicode,
                    kTc2HnidColPropId,
                    kTc2CellSize,
                    kTc2CellSize,
                    kTc2Col1BitIndex});
    // Row matrix: cell 0 = the row NID, cell 1 = the HNID, then a CEB marking both columns present.
    writeLe32(blk, kTc2RowMatrixOffset, row_value);
    writeLe32(blk, kTc2RowMatrixOffset + kTc2CellSize, kTc2ValueHid);
    blk[kTc2RowMatrixOffset + kTc2CebOffset] =
        static_cast<char>(kCebFirstColPresent | (kCebFirstColPresent >> kTc2Col1BitIndex));
    // The resolved value (heap allocation 3): "HI" as UTF-16LE.
    blk[kTc2ValueOffset + 0] = 'H';
    blk[kTc2ValueOffset + 2] = 'I';
    // HNPAGEMAP: three allocations -- TCINFO [12, 50), row matrix [50, 59), value [59, 63).
    writeLe16(blk, kTc2PageMapOffset, static_cast<uint16_t>(kTc2AllocCount));
    writeLe16(blk, kTc2PageMapOffset + kHnPmEntryLen, 0);
    const int rgib = kTc2PageMapOffset + kHnPmHeaderLen;
    const std::array<int, 4> boundaries{
        {kTcinfoOffset, kTc2RowMatrixOffset, kTc2ValueOffset, kTc2PageMapOffset}};
    for (int i = 0; i < static_cast<int>(boundaries.size()); ++i) {
        writeLe16(blk, rgib + (i * kHnPmEntryLen), static_cast<uint16_t>(boundaries[i]));
    }
    stampBlockTrailer(blk, 0, kTc2BlockCb, bid, file_offset);
    return blk;
}

// One variable-type PC record: a property id + type whose value is stored (via an HNID) in the
// heap.
struct PcRecord {
    uint16_t prop_id;
    uint16_t prop_type;
    QByteArray value;
};

// Total cb (data bytes, no trailer) of a PC block holding @p records:
// HNHDR + BTHHEADER + BTH leaf (8 bytes each) + the concatenated values + HNPAGEMAP.
inline int multiRecordPcCb(const std::vector<PcRecord>& records) {
    const int n = static_cast<int>(records.size());
    int values_len = 0;
    for (const auto& rec : records) {
        values_len += static_cast<int>(rec.value.size());
    }
    const int alloc_count = kPcFixedAllocCount + n;
    const int pagemap_len = kHnPmHeaderLen + (kHnPmEntryLen * (alloc_count + 1));
    return kHnHdrSize + kBthHdrSize + (kPcRecordSize * n) + values_len + pagemap_len;
}

// A Heap-on-Node PC block carrying N variable-type records: HNHDR -> BTHHEADER -> an N-record BTH
// leaf, each record's HNID resolving to its heap value. readPropertyContext (and, for an attachment
// sub-node, collectBthLeafData + populateAttachmentFromLeaf) walks that chain. The single home for
// the PC heap layout -- the message-Subject block, the multi-property message, and the attachment
// data block are all thin wrappers, so the geometry (HNPAGEMAP boundaries, HID indices) lives here
// once. For a single 8-byte record this produces byte-for-byte the classic 48-byte message PC.
inline QByteArray buildMultiRecordPcBlock(uint64_t file_offset,
                                          uint64_t bid,
                                          const std::vector<PcRecord>& records) {
    const int n = static_cast<int>(records.size());
    const int cb = multiRecordPcCb(records);
    const int bth_leaf_off = kHnHdrSize + kBthHdrSize;
    const int values_off = bth_leaf_off + (kPcRecordSize * n);
    int values_len = 0;
    for (const auto& rec : records) {
        values_len += static_cast<int>(rec.value.size());
    }
    const int pagemap_off = values_off + values_len;

    QByteArray blk(blockDiskSize(cb), '\0');
    // HNHDR -> BTHHEADER as the user root.
    writeLe16(blk, kHnHdrIbHnpmOffset, static_cast<uint16_t>(pagemap_off));
    blk[kHnHdrSigOffset] = static_cast<char>(kHnSignature);
    blk[kHnHdrClientSigOffset] = static_cast<char>(kPcClientSignature);
    writeLe32(blk, kHnHdrRootHidOffset, kBthHeaderHid);
    // BTHHEADER (heap allocation 1): key 2 / data 6, root HID -> the BTH leaf.
    blk[kMsgBthHdrOffset] = static_cast<char>(kBthSignature);
    blk[kMsgBthHdrOffset + kBthKeySizeField] = static_cast<char>(kPcBthKeySize);
    blk[kMsgBthHdrOffset + kBthDataSizeField] = static_cast<char>(kPcBthDataSize);
    blk[kMsgBthHdrOffset + kBthIdxLevelsField] = 0;
    writeLe32(blk, kMsgBthHdrOffset + kBthRootHidField, kMsgBthLeafHid);
    // BTH leaf records + their heap values.
    int value_off = values_off;
    for (int i = 0; i < n; ++i) {
        const int rec = bth_leaf_off + (i * kPcRecordSize);
        const uint32_t value_hid = static_cast<uint32_t>((kFirstValueHidIndex + i)
                                                         << kHidIndexShift);
        writeLe16(blk, rec, records[i].prop_id);
        writeLe16(blk, rec + kPcRecordKeySize, records[i].prop_type);
        writeLe32(blk, rec + kPcRecordKeySize + kPcValueRefOffset, value_hid);
        blk.replace(value_off, static_cast<int>(records[i].value.size()), records[i].value);
        value_off += static_cast<int>(records[i].value.size());
    }
    // HNPAGEMAP: alloc_count allocations, then alloc_count+1 boundary offsets.
    const int alloc_count = kPcFixedAllocCount + n;
    writeLe16(blk, pagemap_off, static_cast<uint16_t>(alloc_count));
    writeLe16(blk, pagemap_off + kHnPmEntryLen, 0);  // cFree
    const int rgib = pagemap_off + kHnPmHeaderLen;
    writeLe16(blk, rgib, static_cast<uint16_t>(kMsgBthHdrOffset));
    writeLe16(blk, rgib + kHnPmEntryLen, static_cast<uint16_t>(bth_leaf_off));
    int boundary = values_off;
    for (int i = 0; i < n; ++i) {
        writeLe16(blk,
                  rgib + ((kPcFixedAllocCount + i) * kHnPmEntryLen),
                  static_cast<uint16_t>(boundary));
        boundary += static_cast<int>(records[i].value.size());
    }
    writeLe16(blk,
              rgib + ((kPcFixedAllocCount + n) * kHnPmEntryLen),
              static_cast<uint16_t>(pagemap_off));
    stampBlockTrailer(blk, 0, cb, bid, file_offset);
    return blk;
}

// One-record convenience wrapper (an 8-byte value): the message-Subject and attachment-data blocks.
inline QByteArray buildOneVarRecordPcBlock(uint64_t file_offset,
                                           uint64_t bid,
                                           uint16_t prop_id,
                                           uint16_t prop_type,
                                           const QByteArray& value8) {
    return buildMultiRecordPcBlock(file_offset, bid, {{prop_id, prop_type, value8}});
}

// A message's data block: the one-record PC above carrying the Subject (a variable-length Unicode
// string "FUZZ"). readMessage/readItemProperties return a populated subject.
inline QByteArray buildMessagePcBlock(uint64_t file_offset, uint64_t bid) {
    QByteArray subject(kMsgSubjectLen, '\0');
    const char text[] = "FUZZ";
    for (int i = 0; i < kMsgSubjectCharCount; ++i) {
        writeLe16(subject, i * 2, static_cast<uint16_t>(text[i]));
    }
    return buildOneVarRecordPcBlock(
        file_offset, bid, kSubjectPropId, sak::email::kPropTypeUnicode, subject);
}

// Re-stamp a legacy leaf page's PAGETRAILER over its (possibly mutated) body: recompute
// dwCRC over [page_offset, page_offset + kLeafPageBodyLen) and wSig from (page_offset, bid 0).
// A structure-aware fuzz mutates the page BODY then calls this so the file stays integral and
// the parser accepts the trailer and walks the corrupt entries/meta -- the accept-path code.
inline void restampLeafPageTrailer(QByteArray& file, int page_offset) {
    const int trailer_offset = page_offset + kPageSize - kTrailerSize;
    writeLe16(file, trailer_offset + 2, computeSig(static_cast<uint64_t>(page_offset), 0));
    writeLe32(file, trailer_offset + 4, weakCrc(file, page_offset, kLeafPageBodyLen));
}

// Re-stamp the PC block's BLOCKTRAILER over its (possibly mutated) kRootBlockCb data bytes.
inline void restampBlockTrailer(QByteArray& file, int block_offset, uint64_t bid) {
    stampBlockTrailer(file, block_offset, kRootBlockCb, bid, static_cast<uint64_t>(block_offset));
}

// Re-stamp the header dwCRCPartial + dwCRCFull. Call after any header-body change.
inline void restampHeaderCrc(QByteArray& file) {
    writeLe32(file, kCrcPartialOffset, weakCrc(file, kCrcPartialStart, kCrcPartialLen));
    writeLe32(file, kCrcFullOffset, weakCrc(file, kCrcFullStart, kCrcFullLen));
}

// A Unicode NBTENTRY: node NID -> data BID, no sub-node, no parent.
inline QByteArray makeNbtEntry(uint32_t nid, uint64_t data_bid) {
    QByteArray entry(kNbtEntrySize, '\0');
    writeLe64(entry, kNbtEntryNidOffset, nid);
    writeLe64(entry, kNbtEntryDataBidOffset, data_bid);
    writeLe64(entry, kNbtEntrySubBidOffset, 0);
    writeLe32(entry, kNbtEntryParentOffset, 0);
    return entry;
}

// A Unicode NBTENTRY whose node carries a sub-node BTree (bidSub != 0): the message node points at
// its attachment sub-node block. Same layout as makeNbtEntry with the sub-node BID filled in.
inline QByteArray makeNbtEntryWithSub(uint32_t nid, uint64_t data_bid, uint64_t sub_bid) {
    QByteArray entry = makeNbtEntry(nid, data_bid);
    writeLe64(entry, kNbtEntrySubBidOffset, sub_bid);
    return entry;
}

// A Unicode BBTENTRY: block BID -> file offset + cb.
inline QByteArray makeBbtEntry(uint64_t bid, int file_offset, int cb) {
    QByteArray entry(kBbtEntrySize, '\0');
    writeLe64(entry, kBbtEntryBidOffset, bid);
    writeLe64(entry, kBbtEntryIbOffset, static_cast<uint64_t>(file_offset));
    writeLe16(entry, kBbtEntryCbOffset, static_cast<uint16_t>(cb));
    writeLe16(entry, kBbtEntryRefOffset, 2);  // cRef (>=2; not validated by the reader)
    return entry;
}

// Magic, content type, Unicode version, no-encryption, and the ROOT BREFs (file size / NBT / BBT).
inline void writeUnicodeStoreHeader(QByteArray& file, int nbt_offset, int bbt_offset) {
    file[0] = static_cast<char>(kMagicByte0);
    file[1] = static_cast<char>(kMagicByte1);
    file[2] = static_cast<char>(kMagicByte2);
    file[3] = static_cast<char>(kMagicByte3);
    writeLe16(file, kContentTypeOffset, sak::email::kPstContentType);
    writeLe16(file, kVersionOffset, sak::email::kUnicodeVersion);
    file[kCryptOffsetUnicode] = static_cast<char>(sak::email::kEncryptNone);
    writeLe64(file, kRootOffset + kRootFileSizeField, static_cast<uint64_t>(file.size()));
    writeLe64(file, kRootOffset + kRootNbtField, static_cast<uint64_t>(nbt_offset));
    writeLe64(file, kRootOffset + kRootBbtField, static_cast<uint64_t>(bbt_offset));
}

/// Build a legacy-Unicode PST that PstParser::open() ACCEPTS: the Node BTree carries a
/// single leaf entry for the root folder (kNidRootFolder), the Block BTree maps that
/// node's data BID to a Heap-on-Node PC block, and every CRC/signature is genuine. Unlike
/// buildEmptyUnicodeStore (which fails closed in buildFolderHierarchy for want of a root
/// node), this drives the parser all the way into the LTP/messaging accept path --
/// readPropertyContext, readHeapOnNode, and the folder-tree walk -- so a fuzz seed built
/// from it exercises the success branches of the integrity gates, not just their rejects.
inline QByteArray buildOpenableUnicodeStore() {
    const int nbt_offset = kOpenableNbtOffset;
    const int bbt_offset = kOpenableBbtOffset;
    const int block_offset = kOpenableBlockOffset;
    const int file_size = kOpenableFileSize;

    QByteArray file(file_size, '\0');
    writeUnicodeStoreHeader(file, nbt_offset, bbt_offset);

    file.replace(nbt_offset,
                 kPageSize,
                 makeLeafPageWithEntry(kPtypeNbt,
                                       kNbtEntrySize,
                                       nbt_offset,
                                       makeNbtEntry(kRootFolderNid, kRootFolderDataBid)));
    file.replace(
        bbt_offset,
        kPageSize,
        makeLeafPageWithEntry(kPtypeBbt,
                              kBbtEntrySize,
                              bbt_offset,
                              makeBbtEntry(kRootFolderDataBid, block_offset, kRootBlockCb)));
    file.replace(block_offset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(block_offset), kRootFolderDataBid));

    restampHeaderCrc(file);
    return file;
}

// Whether the leaf node's PC is empty (a folder) or a populated message (a Subject record).
enum class LeafPc {
    Empty,
    Message
};

// Which contents/hierarchy Table Context the store carries: the hidRowIndex==0 single-row TC (the
// parser enumerates the one physical row) or the row-indexed variant (a real TCROWID BTH the parser
// must walk).
enum class TcKind {
    SingleRow,
    RowIndexed,
    HnidCell
};

// The TC block's data length (cb) for each variant -- the row-indexed and two-column TCs are larger
// than the single-row TC, so the store's BBT entry must declare the right size.
inline int tcCbForKind(TcKind tc_kind) {
    switch (tc_kind) {
    case TcKind::RowIndexed:
        return kTcRiBlockCb;
    case TcKind::HnidCell:
        return kTc2BlockCb;
    default:
        return kTcBlockCb;
    }
}

// The TC data block for each variant, stamped at @p tc_block for BID kHierarchyDataBid.
inline QByteArray buildTcForKind(TcKind tc_kind, int tc_block, uint32_t row_value) {
    const auto offset = static_cast<uint64_t>(tc_block);
    switch (tc_kind) {
    case TcKind::RowIndexed:
        return buildRowIndexedTcBlock(offset, kHierarchyDataBid, row_value);
    case TcKind::HnidCell:
        return buildHnidCellTcBlock(offset, kHierarchyDataBid, row_value);
    default:
        return buildSingleRowTcBlock(offset, kHierarchyDataBid, row_value);
    }
}

/// Build a legacy-Unicode PST with three nodes: the root folder PC, a single-row Table Context
/// (@p tc_nid), and a leaf node (@p leaf_nid, also the TC row's value). @p leaf_kind selects the
/// leaf's PC -- an empty folder PC or a populated message PC. Whether the TC is a hierarchy table
/// (open() recurses into a child folder) or a contents table (readFolderItems() lists a message)
/// is decided by tc_nid's type; the block layout is identical (both leaf PCs are a 64-byte block).
inline QByteArray buildStoreWithSingleRowTc(uint32_t tc_nid,
                                            uint32_t leaf_nid,
                                            LeafPc leaf_kind,
                                            TcKind tc_kind = TcKind::SingleRow) {
    const int nbt_offset = kOpenableNbtOffset;
    const int bbt_offset = kOpenableBbtOffset;
    const int root_block = kFolderedRootBlockOffset;   // root PC, disk 64
    const int tc_block = kFolderedTcBlockOffset;       // TC, disk 128 (both variants)
    const int leaf_block = kFolderedChildBlockOffset;  // leaf PC, disk 64
    const int leaf_cb = (leaf_kind == LeafPc::Message) ? kMessagePcCb : kRootBlockCb;
    const int tc_cb = tcCbForKind(tc_kind);

    QByteArray file(kFolderedFileSize, '\0');
    writeUnicodeStoreHeader(file, nbt_offset, bbt_offset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(tc_nid, kHierarchyDataBid) +
                           makeNbtEntry(leaf_nid, kChildFolderDataBid);
    file.replace(nbt_offset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, nbt_offset, nbt, 3));

    const QByteArray bbt = makeBbtEntry(kRootFolderDataBid, root_block, kRootBlockCb) +
                           makeBbtEntry(kHierarchyDataBid, tc_block, tc_cb) +
                           makeBbtEntry(kChildFolderDataBid, leaf_block, leaf_cb);
    file.replace(bbt_offset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, bbt_offset, bbt, 3));

    file.replace(root_block,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(root_block), kRootFolderDataBid));
    file.replace(tc_block, kTcBlockDiskSize, buildTcForKind(tc_kind, tc_block, leaf_nid));
    const QByteArray leaf_pc =
        (leaf_kind == LeafPc::Message)
            ? buildMessagePcBlock(static_cast<uint64_t>(leaf_block), kChildFolderDataBid)
            : buildRootFolderPcBlock(static_cast<uint64_t>(leaf_block), kChildFolderDataBid);
    file.replace(leaf_block, kRootBlockDiskSize, leaf_pc);

    restampHeaderCrc(file);
    return file;
}

/// The root folder has ONE child folder, reached through a hierarchy Table Context. open() walks
/// loadChildFolders -> readTableContext -> parseTcInfo -> buildTcRows -> extractChildNids and
/// recurses into the child's (empty) PC -- TC/row-matrix code the openable store never reaches.
inline QByteArray buildFolderedUnicodeStore() {
    return buildStoreWithSingleRowTc(kHierarchyTableNid, kChildFolderNid, LeafPc::Empty);
}

/// The root folder has a CONTENTS Table Context listing ONE message, and the message's PC carries
/// a Subject. readFolderItems(root) drives readContentsTable -> the summary loop, and
/// readItemDetail(message) drives readMessage -> readPropertyContext -> the populated-PC path
/// (parsePropertyRecords variable-type -> resolveHnid -> formatUnicodeValue) no folder-only store
/// reaches.
inline QByteArray buildMessagingUnicodeStore() {
    return buildStoreWithSingleRowTc(kContentsTableNid, kMessageNid, LeafPc::Message);
}

/// Same contents-table store as buildMessagingUnicodeStore, but the TC carries a real TCROWID BTH
/// (hidRowIndex != 0). readFolderItems(root) then drives readContentsTable -> buildTcRows ->
/// collectTcLiveRowIndices -> extractTcRowIndicesFromLeaf -- the live-row BTH walk that resolves
/// the one message from the BTH rather than enumerating physical matrix slots. The message still
/// reads back identically (its NID is row 0), so the row is exercised end-to-end through the BTH
/// path.
inline QByteArray buildRowIndexedTcStore() {
    return buildStoreWithSingleRowTc(
        kContentsTableNid, kMessageNid, LeafPc::Message, TcKind::RowIndexed);
}

/// Same contents-table store as buildMessagingUnicodeStore, but the TC has a second,
/// HNID-resolvable Unicode column whose cell is an HID. readFolderItems(root) -> readContentsTable
/// -> buildTcRows -> materializeTcRow -> buildTcCell must resolve that HNID to the heap-stored "HI"
/// -- the resolveHnid cell branch the single literal-Int32 column never triggers.
inline QByteArray buildHnidCellTcStore() {
    return buildStoreWithSingleRowTc(
        kContentsTableNid, kMessageNid, LeafPc::Message, TcKind::HnidCell);
}

// ASCII text as a UTF-16LE byte string (the PtypString on-disk encoding). Test values are ASCII.
inline QByteArray asciiUtf16le(const char* text) {
    QByteArray out;
    for (const char* p = text; *p != '\0'; ++p) {
        out.append(*p);
        out.append('\0');
    }
    return out;
}

/// The root folder's CONTENTS Table Context lists ONE message whose PC carries three properties --
/// Subject, MessageClass ("IPM.Note"), and SenderName ("Alice"). loadFolderItems(root) then runs
/// the enrichment pass (enrichItemSenders -> enrichSingleItemProps -> enrichItemFromBth ->
/// extractSenderFromLeaf + scanBthForSubjectAndClass -> classifyMessageClass), populating the
/// summary's sender_name and item_type -- the enrichment code the single-Subject message never
/// triggers.
inline QByteArray buildEnrichableMessageStore() {
    const std::vector<PcRecord> records = {
        {kSubjectPropId, sak::email::kPropTypeUnicode, asciiUtf16le("FUZZ")},
        {sak::email::kPropIdMessageClass, sak::email::kPropTypeUnicode, asciiUtf16le("IPM.Note")},
        {sak::email::kPropIdSenderName, sak::email::kPropTypeUnicode, asciiUtf16le("Alice")},
    };
    const int pc_cb = multiRecordPcCb(records);
    const int pc_disk = blockDiskSize(pc_cb);
    const int file_size = kEnrichPcBlockOffset + pc_disk;

    QByteArray file(file_size, '\0');
    writeUnicodeStoreHeader(file, kOpenableNbtOffset, kOpenableBbtOffset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(kContentsTableNid, kHierarchyDataBid) +
                           makeNbtEntry(kMessageNid, kChildFolderDataBid);
    file.replace(kOpenableNbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, kOpenableNbtOffset, nbt, 3));

    const QByteArray bbt = makeBbtEntry(kRootFolderDataBid, kEnrichRootBlockOffset, kRootBlockCb) +
                           makeBbtEntry(kHierarchyDataBid, kEnrichTcBlockOffset, kTcBlockCb) +
                           makeBbtEntry(kChildFolderDataBid, kEnrichPcBlockOffset, pc_cb);
    file.replace(kOpenableBbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, kOpenableBbtOffset, bbt, 3));

    file.replace(kEnrichRootBlockOffset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(kEnrichRootBlockOffset),
                                        kRootFolderDataBid));
    file.replace(kEnrichTcBlockOffset,
                 kTcBlockDiskSize,
                 buildSingleRowTcBlock(
                     static_cast<uint64_t>(kEnrichTcBlockOffset), kHierarchyDataBid, kMessageNid));
    file.replace(kEnrichPcBlockOffset,
                 pc_disk,
                 buildMultiRecordPcBlock(
                     static_cast<uint64_t>(kEnrichPcBlockOffset), kChildFolderDataBid, records));

    restampHeaderCrc(file);
    return file;
}

// The message store's own node (NID_MESSAGE_STORE, 0x21) and its data BID.
inline constexpr uint32_t kMessageStoreNid = sak::email::kNidMessageStore;  // 0x21
inline constexpr uint64_t kMessageStoreDataBid = 8;  // external (fInternal bit 0x02 clear)
inline constexpr int kMessageStoreBlockOffset = kFolderedRootBlockOffset +
                                                kRootBlockDiskSize;  // 0x840

/// A store with the root folder plus ONE extra node (@p node_nid) whose PC carries @p records. The
/// root folder lets open() succeed with one folder; the extra node is read on demand -- as the
/// message store (loadMessageStoreDisplayName) or a message (readItemDetail), depending on
/// node_nid.
inline QByteArray buildRootPlusNodeStore(uint32_t node_nid, const std::vector<PcRecord>& records) {
    const int pc_cb = multiRecordPcCb(records);
    const int pc_disk = blockDiskSize(pc_cb);
    const int file_size = kMessageStoreBlockOffset + pc_disk;

    QByteArray file(file_size, '\0');
    writeUnicodeStoreHeader(file, kOpenableNbtOffset, kOpenableBbtOffset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(node_nid, kMessageStoreDataBid);
    file.replace(kOpenableNbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, kOpenableNbtOffset, nbt, 2));

    const QByteArray bbt =
        makeBbtEntry(kRootFolderDataBid, kFolderedRootBlockOffset, kRootBlockCb) +
        makeBbtEntry(kMessageStoreDataBid, kMessageStoreBlockOffset, pc_cb);
    file.replace(kOpenableBbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, kOpenableBbtOffset, bbt, 2));

    file.replace(kFolderedRootBlockOffset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(kFolderedRootBlockOffset),
                                        kRootFolderDataBid));
    file.replace(kMessageStoreBlockOffset,
                 pc_disk,
                 buildMultiRecordPcBlock(static_cast<uint64_t>(kMessageStoreBlockOffset),
                                         kMessageStoreDataBid,
                                         records));

    restampHeaderCrc(file);
    return file;
}

/// A store whose NID_MESSAGE_STORE node's PC carries PidTagDisplayName ("STORE"). open() calls
/// loadMessageStoreDisplayName, which reads that node's property context and surfaces the name on
/// PstFileInfo -- the message-store-name path no root-folder-only fixture reaches (they lack 0x21).
inline QByteArray buildMessageStoreNamedStore() {
    return buildRootPlusNodeStore(
        kMessageStoreNid,
        {{sak::email::kPropIdDisplayName, sak::email::kPropTypeUnicode, asciiUtf16le("STORE")}});
}

/// A store whose message node's PC carries PidTagHtmlBody but NO plain-text body.
/// readItemDetail(message) -> readMessage takes the HTML-to-plain-text branch (body_plain empty,
/// body_html set), sanitizing "<b>hi</b>" through the email-body sanitizer and flattening it to
/// "hi" -- the body-derivation path a Subject-only message never triggers.
inline QByteArray buildHtmlBodyMessageStore() {
    return buildRootPlusNodeStore(kMessageNid,
                                  {{sak::email::kPropIdHtmlBody,
                                    sak::email::kPropTypeBinary,
                                    QByteArrayLiteral("<b>hi</b>")}});
}

// The 8 heap bytes an attachment's PidTagAttachData HNID resolves to. Exposed so the accept-path
// test can assert readAttachmentData returns exactly these bytes.
inline QByteArray attachPayloadBytes() {
    return QByteArrayLiteral("ATCHDATA");  // exactly kMsgSubjectLen (8) bytes
}

// The message's attachment sub-node BTree: a leaf SLBLOCK with ONE SLENTRY whose NID has the
// attachment type (0x05), pointing at the attachment PC data block (no nested sub-node). A genuine
// BLOCKTRAILER authenticates the region; the structure fuzz can mutate the body and re-stamp it.
inline QByteArray buildAttachmentSubnodeBlock(uint64_t file_offset, uint64_t bid) {
    QByteArray blk(blockDiskSize(kSubnodeBlockCb), '\0');
    blk[0] = static_cast<char>(kSubnodeBlockBType);
    blk[1] = 0;  // cLevel 0 == leaf
    writeLe16(blk, kSubnodeCEntOffset, 1);
    const int entry = kSubnodeBlockHeaderLen;
    writeLe64(blk, entry, kAttachmentNid);
    writeLe64(blk, entry + kSlEntryDataBidOffset, kAttachDataBid);
    writeLe64(blk, entry + kSlEntrySubBidOffset, 0);
    stampBlockTrailer(blk, 0, kSubnodeBlockCb, bid, file_offset);
    return blk;
}

// The attachment's data block: the one-record PC carrying PidTagAttachData (binary) whose HNID
// resolves to attachPayloadBytes().
inline QByteArray buildAttachmentPcBlock(uint64_t file_offset, uint64_t bid) {
    return buildOneVarRecordPcBlock(file_offset,
                                    bid,
                                    sak::email::kPropIdAttachData,
                                    sak::email::kPropTypeBinary,
                                    attachPayloadBytes());
}

/// The root folder's CONTENTS Table Context lists ONE message, and that message node carries a
/// sub-node BTree with one attachment. open() + readFolderItems reach the message exactly as the
/// messaging store does; readAttachments(kMessageNid) then drives readSubNodeBTree ->
/// readSubNodeLeafEntries -> readSingleAttachment -> populateAttachmentFromLeaf, and
/// readAttachmentData(kMessageNid, 0) drives extractAttachmentFromSubnode -> resolveHnid to the
/// payload -- the sub-node/attachment code path no other fixture exercises.
inline QByteArray buildAttachmentUnicodeStore() {
    const int nbt_offset = kOpenableNbtOffset;
    const int bbt_offset = kOpenableBbtOffset;

    QByteArray file(kAttachFileSize, '\0');
    writeUnicodeStoreHeader(file, nbt_offset, bbt_offset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(kContentsTableNid, kHierarchyDataBid) +
                           makeNbtEntryWithSub(kMessageNid, kChildFolderDataBid, kAttachSubnodeBid);
    file.replace(nbt_offset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, nbt_offset, nbt, 3));

    const QByteArray bbt =
        makeBbtEntry(kRootFolderDataBid, kFolderedRootBlockOffset, kRootBlockCb) +
        makeBbtEntry(kHierarchyDataBid, kFolderedTcBlockOffset, kTcBlockCb) +
        makeBbtEntry(kChildFolderDataBid, kFolderedChildBlockOffset, kMessagePcCb) +
        makeBbtEntry(kAttachSubnodeBid, kAttachSubnodeBlockOffset, kSubnodeBlockCb) +
        makeBbtEntry(kAttachDataBid, kAttachPcBlockOffset, kMessagePcCb);
    file.replace(bbt_offset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, bbt_offset, bbt, 5));

    file.replace(kFolderedRootBlockOffset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(kFolderedRootBlockOffset),
                                        kRootFolderDataBid));
    file.replace(kFolderedTcBlockOffset,
                 kTcBlockDiskSize,
                 buildSingleRowTcBlock(static_cast<uint64_t>(kFolderedTcBlockOffset),
                                       kHierarchyDataBid,
                                       kMessageNid));
    file.replace(kFolderedChildBlockOffset,
                 kRootBlockDiskSize,
                 buildMessagePcBlock(static_cast<uint64_t>(kFolderedChildBlockOffset),
                                     kChildFolderDataBid));
    file.replace(kAttachSubnodeBlockOffset,
                 kRootBlockDiskSize,
                 buildAttachmentSubnodeBlock(static_cast<uint64_t>(kAttachSubnodeBlockOffset),
                                             kAttachSubnodeBid));
    file.replace(kAttachPcBlockOffset,
                 kRootBlockDiskSize,
                 buildAttachmentPcBlock(static_cast<uint64_t>(kAttachPcBlockOffset),
                                        kAttachDataBid));

    restampHeaderCrc(file);
    return file;
}

// A raw external data block: @p data bytes followed by a genuine BLOCKTRAILER. Used for the XBLOCK
// child blocks, which hold opaque slices of a larger data stream (not a Heap-on-Node of their own).
inline QByteArray buildRawDataBlock(uint64_t file_offset, uint64_t bid, const QByteArray& data) {
    const int cb = static_cast<int>(data.size());
    QByteArray blk(blockDiskSize(cb), '\0');
    blk.replace(0, cb, data);
    stampBlockTrailer(blk, 0, cb, bid, file_offset);
    return blk;
}

// An internal XBLOCK: an 8-byte header (btype, cLevel==1, cEnt[u16], lcbTotal[u32]) then @p entry
// 8-byte child BIDs. readDataTree expands it and concatenates the children's bytes.
inline QByteArray buildXblock(uint64_t file_offset,
                              uint64_t bid,
                              uint64_t child0_bid,
                              uint64_t child1_bid,
                              uint32_t total_bytes) {
    QByteArray blk(blockDiskSize(kXblockCb), '\0');
    blk[0] = static_cast<char>(kXblockBType);
    blk[1] = static_cast<char>(kXblockLevel);
    writeLe16(blk, 2, 2);  // cEnt == two children
    writeLe32(blk, 4, total_bytes);
    writeLe64(blk, kXblockHeaderLen, child0_bid);
    writeLe64(blk, kXblockHeaderLen + 8, child1_bid);
    stampBlockTrailer(blk, 0, kXblockCb, bid, file_offset);
    return blk;
}

/// The root folder's CONTENTS Table Context lists ONE message whose data BID is an internal XBLOCK,
/// so readItemProperties(kMessageNid) drives readPropertyContext -> readHeapOnNode -> readDataTree,
/// which sees the internal bit, expands the XBLOCK, and reassembles the two child blocks into the
/// 48-byte message PC (the "FUZZ" Subject). This is the multi-block data-tree path
/// (readInternalDataBlock / readXblockChildren) no single-block store reaches.
inline QByteArray buildXblockMessageStore() {
    QByteArray file(kXblockFileSize, '\0');
    writeUnicodeStoreHeader(file, kOpenableNbtOffset, kOpenableBbtOffset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(kContentsTableNid, kHierarchyDataBid) +
                           makeNbtEntry(kMessageNid, kXblockBid);
    file.replace(kOpenableNbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, kOpenableNbtOffset, nbt, 3));

    const QByteArray bbt = makeBbtEntry(kRootFolderDataBid, kXblockRootBlockOffset, kRootBlockCb) +
                           makeBbtEntry(kHierarchyDataBid, kXblockTcBlockOffset, kTcBlockCb) +
                           makeBbtEntry(kXblockBid, kXblockBlockOffset, kXblockCb) +
                           makeBbtEntry(kXblockChild0Bid, kXblockChild0Offset, kXblockChild0Cb) +
                           makeBbtEntry(kXblockChild1Bid, kXblockChild1Offset, kXblockChild1Cb);
    file.replace(kOpenableBbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, kOpenableBbtOffset, bbt, 5));

    file.replace(kXblockRootBlockOffset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(kXblockRootBlockOffset),
                                        kRootFolderDataBid));
    file.replace(kXblockTcBlockOffset,
                 kTcBlockDiskSize,
                 buildSingleRowTcBlock(
                     static_cast<uint64_t>(kXblockTcBlockOffset), kHierarchyDataBid, kMessageNid));

    // The 48 message-PC data bytes (the HN, without its own trailer), split across two child
    // blocks.
    const QByteArray pc = buildMessagePcBlock(0, kXblockBid).left(kMessagePcCb);
    file.replace(kXblockBlockOffset,
                 kRootBlockDiskSize,
                 buildXblock(static_cast<uint64_t>(kXblockBlockOffset),
                             kXblockBid,
                             kXblockChild0Bid,
                             kXblockChild1Bid,
                             static_cast<uint32_t>(kMessagePcCb)));
    file.replace(kXblockChild0Offset,
                 kRootBlockDiskSize,
                 buildRawDataBlock(static_cast<uint64_t>(kXblockChild0Offset),
                                   kXblockChild0Bid,
                                   pc.left(kXblockChild0Cb)));
    file.replace(kXblockChild1Offset,
                 kRootBlockDiskSize,
                 buildRawDataBlock(static_cast<uint64_t>(kXblockChild1Offset),
                                   kXblockChild1Bid,
                                   pc.mid(kXblockChild0Cb)));

    restampHeaderCrc(file);
    return file;
}

// An internal XXBLOCK (cLevel==2): the same 8-byte header as an XBLOCK, then @p entry 8-byte child
// BIDs that point at XBLOCKs rather than external data. readInternalDataBlock routes level 2 to
// readXxblockChildren, which recurses each XBLOCK before concatenating.
inline QByteArray buildXxblock(uint64_t file_offset,
                               uint64_t bid,
                               uint64_t xblock0_bid,
                               uint64_t xblock1_bid,
                               uint32_t total_bytes) {
    QByteArray blk(blockDiskSize(kXblockCb), '\0');
    blk[0] = static_cast<char>(kXblockBType);
    blk[1] = static_cast<char>(kXxblockLevel);
    writeLe16(blk, 2, 2);  // cEnt == two child XBLOCKs
    writeLe32(blk, 4, total_bytes);
    writeLe64(blk, kXblockHeaderLen, xblock0_bid);
    writeLe64(blk, kXblockHeaderLen + 8, xblock1_bid);
    stampBlockTrailer(blk, 0, kXblockCb, bid, file_offset);
    return blk;
}

// Place the four external data blocks that hold the quartered message PC, one per XXBLOCK leaf.
inline void writeXxblockDataBlocks(QByteArray& file, const QByteArray& pc) {
    const std::array<std::pair<int, uint64_t>, 4> leaves{{
        {kXxData0Offset, kXxData0Bid},
        {kXxData1Offset, kXxData1Bid},
        {kXxData2Offset, kXxData2Bid},
        {kXxData3Offset, kXxData3Bid},
    }};
    for (int i = 0; i < static_cast<int>(leaves.size()); ++i) {
        const auto [offset, bid] = leaves[i];
        file.replace(offset,
                     kRootBlockDiskSize,
                     buildRawDataBlock(
                         static_cast<uint64_t>(offset), bid, pc.mid(i * kXxDataCb, kXxDataCb)));
    }
}

/// The root folder's CONTENTS Table Context lists ONE message whose data BID is an XXBLOCK. Reading
/// its property context drives readDataTree, which sees the internal bit, expands the XXBLOCK to
/// its two XBLOCKs, expands each XBLOCK to its two external data blocks, and reassembles all four
/// slices (in order) into the 48-byte message PC (the "FUZZ" Subject). This exercises
/// readXxblockChildren
/// -- the two-level data-tree path no single-level XBLOCK fixture reaches.
inline QByteArray buildXxblockMessageStore() {
    QByteArray file(kXxblockFileSize, '\0');
    writeUnicodeStoreHeader(file, kOpenableNbtOffset, kOpenableBbtOffset);

    const QByteArray nbt = makeNbtEntry(kRootFolderNid, kRootFolderDataBid) +
                           makeNbtEntry(kContentsTableNid, kHierarchyDataBid) +
                           makeNbtEntry(kMessageNid, kXxblockBid);
    file.replace(kOpenableNbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeNbt, kNbtEntrySize, kOpenableNbtOffset, nbt, 3));

    const QByteArray bbt = makeBbtEntry(kRootFolderDataBid, kXxblockRootBlockOffset, kRootBlockCb) +
                           makeBbtEntry(kHierarchyDataBid, kXxblockTcBlockOffset, kTcBlockCb) +
                           makeBbtEntry(kXxblockBid, kXxblockBlockOffset, kXblockCb) +
                           makeBbtEntry(kXxXblock0Bid, kXxXblock0Offset, kXblockCb) +
                           makeBbtEntry(kXxXblock1Bid, kXxXblock1Offset, kXblockCb) +
                           makeBbtEntry(kXxData0Bid, kXxData0Offset, kXxDataCb) +
                           makeBbtEntry(kXxData1Bid, kXxData1Offset, kXxDataCb) +
                           makeBbtEntry(kXxData2Bid, kXxData2Offset, kXxDataCb) +
                           makeBbtEntry(kXxData3Bid, kXxData3Offset, kXxDataCb);
    file.replace(kOpenableBbtOffset,
                 kPageSize,
                 makeLeafPageWithEntries(kPtypeBbt, kBbtEntrySize, kOpenableBbtOffset, bbt, 9));

    file.replace(kXxblockRootBlockOffset,
                 kRootBlockDiskSize,
                 buildRootFolderPcBlock(static_cast<uint64_t>(kXxblockRootBlockOffset),
                                        kRootFolderDataBid));
    file.replace(kXxblockTcBlockOffset,
                 kTcBlockDiskSize,
                 buildSingleRowTcBlock(
                     static_cast<uint64_t>(kXxblockTcBlockOffset), kHierarchyDataBid, kMessageNid));

    // The 48 message-PC data bytes (the HN, without its own trailer), quartered across four
    // external data blocks under two XBLOCKs under one XXBLOCK.
    const QByteArray pc = buildMessagePcBlock(0, kXxblockBid).left(kMessagePcCb);
    file.replace(kXxblockBlockOffset,
                 kRootBlockDiskSize,
                 buildXxblock(static_cast<uint64_t>(kXxblockBlockOffset),
                              kXxblockBid,
                              kXxXblock0Bid,
                              kXxXblock1Bid,
                              static_cast<uint32_t>(kMessagePcCb)));
    file.replace(kXxXblock0Offset,
                 kRootBlockDiskSize,
                 buildXblock(static_cast<uint64_t>(kXxXblock0Offset),
                             kXxXblock0Bid,
                             kXxData0Bid,
                             kXxData1Bid,
                             static_cast<uint32_t>(kXxXblockTotal)));
    file.replace(kXxXblock1Offset,
                 kRootBlockDiskSize,
                 buildXblock(static_cast<uint64_t>(kXxXblock1Offset),
                             kXxXblock1Bid,
                             kXxData2Bid,
                             kXxData3Bid,
                             static_cast<uint32_t>(kXxXblockTotal)));
    writeXxblockDataBlocks(file, pc);

    restampHeaderCrc(file);
    return file;
}

}  // namespace sak::pst_fixture
