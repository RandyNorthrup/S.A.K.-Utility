// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_types.h
/// @brief Shared data types for the Email & PST/OST Inspector panel
///
/// Contains the core value types used across the PST parser, MBOX parser,
/// search worker, export worker, controller, and panel UI: PstHeader,
/// PstNode, PstFolder, PstItemSummary, PstItemDetail, PstAttachmentInfo,
/// MapiProperty, MboxMessage, EmailSearchHit, EmailExportResult,
/// EmailClientProfile, and related enumerations.

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <type_traits>

namespace sak {

// ============================================================================
// PST File Header (MS-PST §2.2.2.6)
// ============================================================================

/// @brief PST/OST file header parsed from the first 564/580 bytes
struct PstHeader {
    uint32_t magic = 0;           ///< Must be 0x2142444E ("!BDN")
    uint32_t crc = 0;             ///< CRC32 of header fields
    uint16_t content_type = 0;    ///< 0x4D53 ("SM") PST, 0x534F ("SO") OST
    uint16_t data_version = 0;    ///< 14=ANSI, 23=Unicode, 36=Unicode4K
    uint16_t client_version = 0;  ///< Outlook version that created the file
    uint8_t platform_create = 0;  ///< 0x01 = Windows
    uint8_t platform_access = 0;  ///< 0x01 = Windows
    uint8_t encryption_type = 0;  ///< 0=None, 1=Compressible, 2=High

    // Root structure pointers (Unicode PST — 64-bit offsets)
    uint64_t root_nbt_page = 0;  ///< File offset of Node BTree root page
    uint64_t root_bbt_page = 0;  ///< File offset of Block BTree root page
    uint64_t file_size = 0;      ///< Recorded file size
};

// ============================================================================
// PST Node (NDB Layer)
// ============================================================================

/// @brief Represents a node in the PST Node BTree
struct PstNode {
    uint64_t node_id = 0;         ///< NID — encodes type in low bits
    uint64_t data_bid = 0;        ///< Block ID for this node's data
    uint64_t subnode_bid = 0;     ///< Block ID for sub-node BTree
    uint64_t parent_node_id = 0;  ///< Parent NID in hierarchy
};

// ============================================================================
// Enumerations
// ============================================================================

/// @brief PST node type extracted from low 5 bits of NID
enum class PstNodeType : uint8_t {
    HeapNode = 0x00,
    InternalNode = 0x01,
    NormalFolder = 0x02,
    SearchFolder = 0x03,
    NormalMessage = 0x04,
    Attachment = 0x05,
    SearchUpdateQueue = 0x06,
    SearchCriteriaObj = 0x07,
    AssociatedMessage = 0x08,
    HierarchyTable = 0x0D,
    ContentsTable = 0x0E,
    FaiContentsTable = 0x0F,
};

/// @brief Item type derived from PR_MESSAGE_CLASS
enum class EmailItemType {
    Email,           ///< IPM.Note
    Contact,         ///< IPM.Contact
    Calendar,        ///< IPM.Appointment
    Task,            ///< IPM.Task
    StickyNote,      ///< IPM.StickyNote
    JournalEntry,    ///< IPM.Activity
    DistList,        ///< IPM.DistList
    MeetingRequest,  ///< IPM.Schedule.Meeting.Request
    Unknown
};

/// @brief Export format for the export worker
enum class ExportFormat {
    Eml,             ///< RFC 5322 MIME .eml files
    CsvEmails,       ///< CSV with email metadata + body preview
    Vcf,             ///< vCard 3.0 .vcf files
    CsvContacts,     ///< CSV with contact fields
    Ics,             ///< iCalendar .ics files
    CsvCalendar,     ///< CSV with calendar fields
    CsvTasks,        ///< CSV with task fields
    PlainTextNotes,  ///< Plain .txt files for sticky notes
    Attachments      ///< Extract attachment files
};

// ============================================================================
// MAPI Property
// ============================================================================

/// @brief Raw MAPI property for the property inspector
struct MapiProperty {
    uint16_t tag_id = 0;    ///< Property ID (e.g., 0x0037 = PR_SUBJECT)
    uint16_t tag_type = 0;  ///< Property type (e.g., 0x001F = PT_UNICODE)
    QString property_name;  ///< Human-readable name (e.g., "PR_SUBJECT")
    QString display_value;  ///< Formatted value for display
    QByteArray raw_value;   ///< Raw bytes
};

// ============================================================================
// PST Folder
// ============================================================================

/// @brief Folder in the PST hierarchy tree
struct PstFolder {
    uint64_t node_id = 0;
    uint64_t parent_node_id = 0;
    QString display_name;
    int content_count = 0;  ///< Number of items in this folder
    int unread_count = 0;
    int subfolder_count = 0;
    QString container_class;      ///< IPF.Note, IPF.Contact, IPF.Appointment, etc.
    QVector<PstFolder> children;  ///< Subfolders
};

/// @brief Tree of folders from PST root
using PstFolderTree = QVector<PstFolder>;

// ============================================================================
// PST Attachment
// ============================================================================

/// @brief Attachment metadata for a message
struct PstAttachmentInfo {
    int index = 0;          ///< Attachment index within message
    QString filename;       ///< Display filename
    QString long_filename;  ///< Long filename (preferred)
    qint64 size_bytes = 0;
    QString mime_type;
    QString content_id;     ///< For inline images
    int attach_method = 0;  ///< 1=ByValue, 5=EmbeddedMessage, 6=OLE
    bool is_embedded_message = false;
};

// ============================================================================
// PST Item Summary (list view)
// ============================================================================

/// @brief Lightweight summary for list view (from contents table)
struct PstItemSummary {
    uint64_t node_id = 0;
    EmailItemType item_type = EmailItemType::Unknown;
    QString subject;
    QString sender_name;
    QString sender_email;
    QDateTime date;  ///< Delivery time or start date
    qint64 size_bytes = 0;
    bool has_attachments = false;
    bool is_read = false;
    int importance = 1;  ///< 0=Low, 1=Normal, 2=High
};

// ============================================================================
// PST Item Detail (full load)
// ============================================================================

/// @brief Full detail for a single item (loaded on demand)
struct PstItemDetail {
    uint64_t node_id = 0;
    EmailItemType item_type = EmailItemType::Unknown;

    // Common fields
    QString subject;
    QString sender_name;
    QString sender_email;
    QDateTime date;
    qint64 size_bytes = 0;
    int importance = 1;

    // Email-specific
    QString body_plain;              ///< Plain text body
    QString body_html;               ///< HTML body
    QByteArray body_rtf_compressed;  ///< Compressed RTF (MS-OXRTFCP)
    QString transport_headers;       ///< Full RFC 5322 headers
    QString display_to;
    QString display_cc;
    QString display_bcc;
    QString message_id;  ///< RFC 5322 Message-ID
    QString in_reply_to;

    // Attachments
    QVector<PstAttachmentInfo> attachments;

    // Contact-specific
    QString given_name;
    QString surname;
    QString company_name;
    QString job_title;
    QString email_address;
    QString business_phone;
    QString mobile_phone;
    QString home_phone;
    QByteArray contact_photo;  ///< JPEG/PNG bytes

    // Calendar-specific
    QDateTime start_time;
    QDateTime end_time;
    QString location;
    bool is_all_day = false;
    int busy_status = 0;  ///< 0=Free, 1=Tentative, 2=Busy, 3=OOF
    QString recurrence_description;
    QStringList attendees;

    // Task-specific
    QDateTime task_due_date;
    QDateTime task_start_date;
    int task_status = 0;  ///< 0=NotStarted, 1=InProgress, 2=Complete
    double task_percent_complete = 0.0;

    // Note-specific
    int note_color = 3;  ///< 0=Blue, 1=Green, 2=Pink, 3=Yellow, 4=White
};

// ============================================================================
// PST File Info
// ============================================================================

/// @brief File metadata returned after opening a PST/OST
struct PstFileInfo {
    QString file_path;
    QString display_name;         ///< Message store display name
    qint64 file_size_bytes = 0;
    bool is_unicode = false;      ///< Unicode (modern) vs ANSI (legacy)
    bool is_ost = false;          ///< OST file flag
    uint8_t encryption_type = 0;  ///< 0=None, 1=Compressible, 2=High
    int total_folders = 0;
    int total_items = 0;          ///< Estimated from internal counters
    QDateTime last_modified;
};

// ============================================================================
// MBOX Types
// ============================================================================

/// @brief Summary of an MBOX message (headers only)
struct MboxMessage {
    int message_index = 0;   ///< Sequential index in the MBOX file
    qint64 file_offset = 0;  ///< Byte offset of "From " line in MBOX
    qint64 message_size = 0;
    QString subject;
    QString from;
    QString to;
    QString cc;
    QDateTime date;
    bool has_attachments = false;
};

/// @brief Full MBOX message detail with body and attachments
struct MboxMessageDetail {
    int message_index = 0;
    QString subject;
    QString from;
    QString to;
    QString cc;
    QString bcc;
    QDateTime date;
    QString message_id;
    QString body_plain;
    QString body_html;
    QString raw_headers;
    QVector<PstAttachmentInfo> attachments;  ///< Reuse attachment struct
};

// ============================================================================
// Search Types
// ============================================================================

/// @brief Criteria for email search operations
struct EmailSearchCriteria {
    QString query_text;  ///< Free-text search string
    bool search_subject = true;
    bool search_body = true;
    bool search_sender = true;
    bool search_recipients = false;
    bool search_attachment_names = false;
    bool case_sensitive = false;

    // Filters
    QDateTime date_from;                                      ///< Null = no lower bound
    QDateTime date_to;                                        ///< Null = no upper bound
    bool has_attachment_only = false;
    EmailItemType item_type_filter = EmailItemType::Unknown;  ///< Unknown = all
    uint64_t folder_scope_id = 0;                             ///< 0 = search all folders

    // MAPI property search (advanced)
    uint16_t mapi_property_id = 0;  ///< 0 = disabled
    QString mapi_property_value;
};

/// @brief A single search result hit
struct EmailSearchHit {
    uint64_t item_node_id = 0;
    EmailItemType item_type = EmailItemType::Unknown;
    QString subject;
    QString sender;
    QDateTime date;
    QString context_snippet;  ///< Surrounding text around the match
    QString match_field;      ///< "subject", "body", "sender", etc.
    QString folder_path;      ///< "Inbox/Projects/Phase2"
};

// ============================================================================
// Export Types
// ============================================================================

/// @brief Configuration for an export operation
struct EmailExportConfig {
    ExportFormat format = ExportFormat::Eml;
    QString output_path;         ///< Directory to export into
    QVector<uint64_t> item_ids;  ///< Specific items (empty = entire folder)
    uint64_t folder_id = 0;      ///< Folder to export (0 = all selected)
    bool recurse_subfolders = false;

    // CSV options
    QStringList csv_columns;
    QChar csv_delimiter = QLatin1Char(',');
    bool csv_include_header = true;

    // Attachment options
    bool flatten_attachments = true;  ///< All into one folder vs per-message
    QString attachment_filter;        ///< Glob pattern, e.g., "*.pdf"
    bool skip_inline_images = true;

    // EML options
    bool eml_include_headers = true;

    // Naming
    bool prefix_with_date = true;  ///< "2024-01-15_Subject.eml"
};

/// @brief Result of an export operation
struct EmailExportResult {
    QString export_path;
    QString export_format;  ///< "EML", "CSV", "ICS", "VCF", etc.
    int items_exported = 0;
    int items_failed = 0;
    qint64 total_bytes = 0;
    QStringList errors;
    QDateTime started;
    QDateTime finished;
};

// ============================================================================
// Email Client Profile
// ============================================================================

/// @brief Email client type
enum class EmailClientType {
    Outlook,
    Thunderbird,
    WindowsMail,
    Other
};

/// @brief A data file belonging to an email client profile
struct EmailDataFile {
    QString path;
    QString type;            ///< "PST", "OST", "MBOX", "SQLite"
    qint64 size_bytes = 0;
    bool is_linked = false;  ///< Currently linked to a profile
};

/// @brief Discovered email client profile
struct EmailClientProfile {
    EmailClientType client_type = EmailClientType::Other;
    QString client_name;   ///< "Microsoft Outlook 2021"
    QString client_version;
    QString profile_name;  ///< "Default Outlook Profile"
    QString profile_path;  ///< Registry or file path
    QVector<EmailDataFile> data_files;
    qint64 total_size_bytes = 0;
};

// ============================================================================
// Compile-Time Invariants (TigerStyle)
// ============================================================================

// All data types must be default-constructible for QVector usage.
static_assert(std::is_default_constructible_v<PstHeader>,
              "PstHeader must be default-constructible.");
static_assert(std::is_default_constructible_v<PstNode>, "PstNode must be default-constructible.");
static_assert(std::is_default_constructible_v<MapiProperty>,
              "MapiProperty must be default-constructible.");
static_assert(std::is_default_constructible_v<PstFolder>,
              "PstFolder must be default-constructible.");
static_assert(std::is_default_constructible_v<PstAttachmentInfo>,
              "PstAttachmentInfo must be default-constructible.");
static_assert(std::is_default_constructible_v<PstItemSummary>,
              "PstItemSummary must be default-constructible.");
static_assert(std::is_default_constructible_v<PstItemDetail>,
              "PstItemDetail must be default-constructible.");
static_assert(std::is_default_constructible_v<PstFileInfo>,
              "PstFileInfo must be default-constructible.");
static_assert(std::is_default_constructible_v<MboxMessage>,
              "MboxMessage must be default-constructible.");
static_assert(std::is_default_constructible_v<MboxMessageDetail>,
              "MboxMessageDetail must be default-constructible.");
static_assert(std::is_default_constructible_v<EmailSearchCriteria>,
              "EmailSearchCriteria must be default-constructible.");
static_assert(std::is_default_constructible_v<EmailSearchHit>,
              "EmailSearchHit must be default-constructible.");
static_assert(std::is_default_constructible_v<EmailExportConfig>,
              "EmailExportConfig must be default-constructible.");
static_assert(std::is_default_constructible_v<EmailExportResult>,
              "EmailExportResult must be default-constructible.");
static_assert(std::is_default_constructible_v<EmailDataFile>,
              "EmailDataFile must be default-constructible.");
static_assert(std::is_default_constructible_v<EmailClientProfile>,
              "EmailClientProfile must be default-constructible.");

// All data types must be copyable for signal/slot passing.
static_assert(std::is_copy_constructible_v<PstHeader>,
              "PstHeader must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<PstNode>,
              "PstNode must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<MapiProperty>,
              "MapiProperty must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<PstFolder>,
              "PstFolder must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<PstAttachmentInfo>,
              "PstAttachmentInfo must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<PstItemSummary>,
              "PstItemSummary must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<PstItemDetail>,
              "PstItemDetail must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<PstFileInfo>,
              "PstFileInfo must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<MboxMessage>,
              "MboxMessage must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<MboxMessageDetail>,
              "MboxMessageDetail must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<EmailSearchCriteria>,
              "EmailSearchCriteria must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<EmailSearchHit>,
              "EmailSearchHit must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<EmailExportConfig>,
              "EmailExportConfig must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<EmailExportResult>,
              "EmailExportResult must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<EmailDataFile>,
              "EmailDataFile must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<EmailClientProfile>,
              "EmailClientProfile must be copy-constructible for signal/slot.");

}  // namespace sak

Q_DECLARE_METATYPE(sak::PstHeader)
Q_DECLARE_METATYPE(sak::PstNode)
Q_DECLARE_METATYPE(sak::MapiProperty)
Q_DECLARE_METATYPE(sak::PstFolder)
Q_DECLARE_METATYPE(sak::PstAttachmentInfo)
Q_DECLARE_METATYPE(sak::PstItemSummary)
Q_DECLARE_METATYPE(sak::PstItemDetail)
Q_DECLARE_METATYPE(sak::PstFileInfo)
Q_DECLARE_METATYPE(sak::MboxMessage)
Q_DECLARE_METATYPE(sak::MboxMessageDetail)
Q_DECLARE_METATYPE(sak::EmailSearchCriteria)
Q_DECLARE_METATYPE(sak::EmailSearchHit)
Q_DECLARE_METATYPE(sak::EmailExportConfig)
Q_DECLARE_METATYPE(sak::EmailExportResult)
Q_DECLARE_METATYPE(sak::EmailDataFile)
Q_DECLARE_METATYPE(sak::EmailClientProfile)
Q_DECLARE_METATYPE(QVector<sak::MapiProperty>)
Q_DECLARE_METATYPE(QVector<sak::PstFolder>)
Q_DECLARE_METATYPE(QVector<sak::PstAttachmentInfo>)
Q_DECLARE_METATYPE(QVector<sak::PstItemSummary>)
Q_DECLARE_METATYPE(QVector<sak::MboxMessage>)
Q_DECLARE_METATYPE(QVector<sak::EmailSearchHit>)
Q_DECLARE_METATYPE(QVector<sak::EmailClientProfile>)
Q_DECLARE_METATYPE(QVector<sak::EmailDataFile>)
