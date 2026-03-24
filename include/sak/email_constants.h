// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_constants.h
/// @brief Centralized constants for the Email & PST/OST Inspector panel

#pragma once

#include <cstdint>

namespace sak::email {

// ============================================================================
// PST Parser Limits
// ============================================================================

/// Maximum nested folder depth during hierarchy traversal
constexpr int kMaxFolderDepth = 50;

/// Items loaded per page in list view
constexpr int kMaxItemsPerLoad = 500;

/// Maximum search result count
constexpr int kMaxSearchResults = 10'000;

/// Maximum single attachment size (500 MB)
constexpr int64_t kMaxAttachmentSize = 500LL * 1024 * 1024;

/// Maximum PST file size (50 GB — format limit)
constexpr int64_t kMaxFileSize = 50LL * 1024 * 1024 * 1024;

/// Max cached NDB node entries
constexpr int kNodeBTreeCacheSize = 50'000;

/// Max cached Block BTree entries
constexpr int kBlockBTreeCacheSize = 50'000;

/// NDB page size in bytes (ANSI PST)
constexpr int kAnsiPageSize = 512;

/// NDB page size in bytes (Unicode 4K variant)
constexpr int kUnicodePageSize = 4096;

/// Max MAPI properties per item
constexpr int kMaxPropertyCount = 5000;

/// PST data block size limit (8 KB for ANSI, 8 KB for Unicode)
constexpr int kMaxBlockSize = 8192;

/// Block alignment for Unicode4K files (512 bytes)
constexpr int kBlock4kAlignment = 512;

/// Block footer size for Unicode4K files (24 bytes)
constexpr int kBlock4kFooterSize = 24;

/// Maximum block data block size for Unicode4K files (64 KB)
constexpr int kMaxBlock4kSize = 65'536;

/// Offset of uncompressed_data_size within the 4K block footer
constexpr int kBlock4kUncompSizeOffset = 18;

/// Internal block data size (block size minus trailer)
constexpr int kAnsiBlockDataSize = 8180;

/// Internal block data size for Unicode PST
constexpr int kUnicodeBlockDataSize = 8176;

// ============================================================================
// MBOX Parser Limits
// ============================================================================

/// Maximum single message size in MBOX (100 MB)
constexpr int64_t kMboxMaxMessageSize = 100LL * 1024 * 1024;

/// Messages indexed per progress update
constexpr int kMboxIndexBatchSize = 10'000;

// ============================================================================
// Export Limits
// ============================================================================

/// Max items per export operation
constexpr int kMaxExportBatchSize = 50'000;

/// Max output filename characters
constexpr int kMaxFilenameLength = 200;

/// Body preview length in CSV export
constexpr int kCsvMaxBodyPreviewChars = 500;

// ============================================================================
// Search Limits
// ============================================================================

/// Characters of context around search match
constexpr int kSearchContextSnippetChars = 120;

/// Sequential search (disk I/O bound)
constexpr int kSearchMaxConcurrent = 1;

// ============================================================================
// Profile Manager
// ============================================================================

/// Registry key traversal depth
constexpr int kMaxProfileDiscoveryDepth = 5;

/// File copy buffer size (4 MB)
constexpr int kFileCopyBufferSize = 4 * 1024 * 1024;

// ============================================================================
// Orphaned File Scanner
// ============================================================================

/// Progress update interval in milliseconds
constexpr int kScanProgressIntervalMs = 250;

/// Max directory recursion depth
constexpr int kScanMaxDepth = 20;

/// Bytes read for format validation
constexpr int kQuickPeekReadSize = 1024;

// ============================================================================
// UI Constants
// ============================================================================

/// Minimum width for the folder tree panel
constexpr int kFolderTreeMinWidth = 200;

/// Default width for the folder tree panel
constexpr int kFolderTreeDefaultWidth = 280;

/// Minimum height for item list
constexpr int kItemListMinHeight = 150;

/// Minimum height for detail panel
constexpr int kDetailPanelMinHeight = 200;

/// Minimum width for detail/preview panel
constexpr int kDetailPanelMinWidth = 400;

/// Delay before triggering search (milliseconds)
constexpr int kSearchDebounceMs = 300;

// ============================================================================
// PST Magic Numbers
// ============================================================================

/// PST file magic: bytes { 0x21, 0x42, 0x44, 0x4E } = "!BDN"
constexpr uint32_t kPstMagic = 0x4E'44'42'21;

/// PST content type: bytes { 0x53, 0x4D } = "SM"
constexpr uint16_t kPstContentType = 0x4D53;

/// OST content type: bytes { 0x53, 0x4F } = "SO"
constexpr uint16_t kOstContentType = 0x4F53;

/// ANSI PST format version
constexpr uint16_t kAnsiVersion = 14;

/// Unicode PST format version
constexpr uint16_t kUnicodeVersion = 23;

/// Unicode 4K-page PST format version
constexpr uint16_t kUnicode4kVersion = 36;

// ============================================================================
// PST Encryption Types
// ============================================================================

/// No encryption
constexpr uint8_t kEncryptNone = 0x00;

/// Compressible encryption (byte-substitution, reversible without key)
constexpr uint8_t kEncryptCompressible = 0x01;

/// High encryption (not supported — password-protected)
constexpr uint8_t kEncryptHigh = 0x02;

// ============================================================================
// MAPI Property Types (PT_*)
// ============================================================================

constexpr uint16_t kPropTypeInt16 = 0x0002;
constexpr uint16_t kPropTypeInt32 = 0x0003;
constexpr uint16_t kPropTypeBoolean = 0x000B;
constexpr uint16_t kPropTypeFloat64 = 0x0005;
constexpr uint16_t kPropTypeCurrency = 0x0006;
constexpr uint16_t kPropTypeAppTime = 0x0007;
constexpr uint16_t kPropTypeInt64 = 0x0014;
constexpr uint16_t kPropTypeString8 = 0x001E;
constexpr uint16_t kPropTypeUnicode = 0x001F;
constexpr uint16_t kPropTypeSysTime = 0x0040;
constexpr uint16_t kPropTypeGuid = 0x0048;
constexpr uint16_t kPropTypeBinary = 0x0102;
constexpr uint16_t kPropTypeMultiInt32 = 0x1003;
constexpr uint16_t kPropTypeMultiString = 0x101F;
constexpr uint16_t kPropTypeMultiBinary = 0x1102;

// ============================================================================
// MAPI Property IDs (PR_*)
// ============================================================================

// -- Message properties --
constexpr uint16_t kPropIdSubject = 0x0037;
constexpr uint16_t kPropIdMessageClass = 0x001A;
constexpr uint16_t kPropIdSenderName = 0x0C1A;
constexpr uint16_t kPropIdSenderEmail = 0x0C1F;
constexpr uint16_t kPropIdSentRepresentingName = 0x0042;
constexpr uint16_t kPropIdSentRepresentingEmail = 0x0065;
constexpr uint16_t kPropIdDisplayTo = 0x0E04;
constexpr uint16_t kPropIdDisplayCc = 0x0E03;
constexpr uint16_t kPropIdDisplayBcc = 0x0E02;
constexpr uint16_t kPropIdMessageDeliveryTime = 0x0E06;
constexpr uint16_t kPropIdClientSubmitTime = 0x0039;
constexpr uint16_t kPropIdBody = 0x1000;
constexpr uint16_t kPropIdHtmlBody = 0x1013;
constexpr uint16_t kPropIdRtfCompressed = 0x1009;
constexpr uint16_t kPropIdTransportHeaders = 0x007D;
constexpr uint16_t kPropIdMessageFlags = 0x0E07;
constexpr uint16_t kPropIdMessageSize = 0x0E08;
constexpr uint16_t kPropIdHasAttachments = 0x0E1B;
constexpr uint16_t kPropIdImportance = 0x0017;
constexpr uint16_t kPropIdSensitivity = 0x0036;
constexpr uint16_t kPropIdInternetMessageId = 0x1035;
constexpr uint16_t kPropIdInReplyTo = 0x1042;

// -- Folder properties --
constexpr uint16_t kPropIdDisplayName = 0x3001;
constexpr uint16_t kPropIdContentCount = 0x3602;
constexpr uint16_t kPropIdContentUnreadCount = 0x3603;
constexpr uint16_t kPropIdSubfolders = 0x360A;
constexpr uint16_t kPropIdContainerClass = 0x3613;

// -- Attachment properties --
constexpr uint16_t kPropIdAttachFilename = 0x3704;
constexpr uint16_t kPropIdAttachLongFilename = 0x3707;
constexpr uint16_t kPropIdAttachData = 0x3701;
constexpr uint16_t kPropIdAttachSize = 0x0E20;
constexpr uint16_t kPropIdAttachMimeTag = 0x370E;
constexpr uint16_t kPropIdAttachContentId = 0x3712;
constexpr uint16_t kPropIdAttachMethod = 0x3705;

// -- Contact properties --
constexpr uint16_t kPropIdEmailAddress = 0x3003;
constexpr uint16_t kPropIdCompanyName = 0x3A16;
constexpr uint16_t kPropIdBusinessPhone = 0x3A08;
constexpr uint16_t kPropIdMobilePhone = 0x3A1C;
constexpr uint16_t kPropIdHomePhone = 0x3A09;
constexpr uint16_t kPropIdJobTitle = 0x3A17;
constexpr uint16_t kPropIdGivenName = 0x3A06;
constexpr uint16_t kPropIdSurname = 0x3A11;

// -- Task properties --
constexpr uint16_t kPropIdTaskPriority = 0x0026;

// ============================================================================
// PST Node ID Types (NID_TYPE)
// ============================================================================

constexpr uint8_t kNidTypeHid = 0x00;
constexpr uint8_t kNidTypeInternal = 0x01;
constexpr uint8_t kNidTypeNormalFolder = 0x02;
constexpr uint8_t kNidTypeSearchFolder = 0x03;
constexpr uint8_t kNidTypeNormalMessage = 0x04;
constexpr uint8_t kNidTypeAttachment = 0x05;
constexpr uint8_t kNidTypeSearchUpdateQueue = 0x06;
constexpr uint8_t kNidTypeSearchCriteria = 0x07;
constexpr uint8_t kNidTypeAssocMessage = 0x08;
constexpr uint8_t kNidTypeContentsTable = 0x0E;
constexpr uint8_t kNidTypeFaiContentsTable = 0x0F;
constexpr uint8_t kNidTypeHierarchyTable = 0x0D;

// Well-known NIDs
constexpr uint32_t kNidMessageStore = 0x21;
constexpr uint32_t kNidNameToIdMap = 0x61;
constexpr uint32_t kNidRootFolder = 0x122;

// ============================================================================
// Attachment Methods
// ============================================================================

constexpr int kAttachByValue = 1;
constexpr int kAttachByReference = 2;
constexpr int kAttachEmbeddedMessage = 5;
constexpr int kAttachOle = 6;

}  // namespace sak::email
