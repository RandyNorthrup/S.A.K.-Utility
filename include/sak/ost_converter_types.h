// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_types.h
/// @brief Shared data types for the OST/PST Converter tab

#pragma once

#include "sak/layout_constants.h"

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <type_traits>

namespace sak {

inline constexpr uint16_t kDefaultImapSslPort = 993;
inline constexpr int kDefaultImapTimeoutSeconds = 30;
inline constexpr int kDefaultImapMaxRetries = 3;
inline constexpr int kDefaultOstConversionThreads = 2;
inline constexpr qint64 kDefaultPstCustomSplitMb = 5120;

// ============================================================================
// Output Format
// ============================================================================

/// @brief Output format for OST/PST conversion
enum class OstOutputFormat {
    Pst,        ///< Microsoft PST file (MS-PST format)
    Eml,        ///< RFC 5322 MIME .eml files (one per message)
    Msg,        ///< MS-OXMSG compound files (one per message)
    Mbox,       ///< Unix mbox format (one file per folder)
    Dbx,        ///< Outlook Express DBX format
    Html,       ///< HTML pages with embedded images
    Pdf,        ///< PDF via QTextDocument/QPdfWriter
    ImapUpload  ///< Direct IMAP upload (not a file format)
};

/// @brief True when the format's writer emits files a real reader can open.
///
/// PST (no ROOT/BREF pointers, no page/block trailers, no CRCs), MSG (broken
/// CFB directory tree, no mini-stream allocation) and DBX (no OE5/6 B-tree
/// index) writers cannot produce output Outlook / MAPI / Outlook Express can
/// mount, so they are gated off until spec-conformant writers exist.
///
/// IMAP upload is gated off for a different reason: ImapUploader itself works,
/// but nothing wires it to the conversion pipeline, so the run would report
/// every message as converted while uploading none. OstConversionWorker has
/// always refused it at run time - this entry used to say true, which meant the
/// GUI left it selectable and the user only discovered it did nothing after
/// starting a conversion.
///
/// EML, MBOX, HTML and PDF are fully supported. Single source of truth shared by
/// the worker (which rejects unsupported formats before touching the source) and
/// the GUI (which disables them in the format picker).
inline constexpr bool isOutputFormatSupported(OstOutputFormat format) {
    switch (format) {
    case OstOutputFormat::Eml:
    case OstOutputFormat::Mbox:
    case OstOutputFormat::Html:
    case OstOutputFormat::Pdf:
        return true;
    case OstOutputFormat::Pst:
    case OstOutputFormat::Msg:
    case OstOutputFormat::Dbx:
    case OstOutputFormat::ImapUpload:
        return false;
    }
    return false;
}

// ============================================================================
// Recovery Mode
// ============================================================================

/// @brief Recovery mode for damaged files
enum class RecoveryMode {
    Normal,       ///< Standard parsing - stop on critical errors
    SkipCorrupt,  ///< Skip corrupt blocks, log errors, continue
    DeepRecovery  ///< Scan all NBT nodes including orphaned ones
};

// ============================================================================
// PST Split Size
// ============================================================================

/// @brief PST split size preset
enum class PstSplitSize {
    NoSplit,    ///< Single file (no splitting)
    Split2Gb,   ///< 2 GB per volume (ANSI PST compat)
    Split5Gb,   ///< 5 GB per volume (recommended)
    Split10Gb,  ///< 10 GB per volume
    Custom      ///< User-specified size in MB
};

// ============================================================================
// IMAP Authentication
// ============================================================================

/// @brief IMAP authentication method
enum class ImapAuthMethod {
    Plain,   ///< PLAIN SASL mechanism
    Login,   ///< LOGIN command
    XOAuth2  ///< XOAUTH2 for Gmail / Microsoft 365
};

/// @brief IMAP server connection settings
struct ImapServerConfig {
    QString host;
    uint16_t port = kDefaultImapSslPort;
    bool use_ssl = true;
    ImapAuthMethod auth_method = ImapAuthMethod::Plain;
    QString username;
    QString password;
    int timeout_seconds = kDefaultImapTimeoutSeconds;
    int max_retries = kDefaultImapMaxRetries;
};

/// @brief Folder mapping for IMAP upload
struct ImapFolderMapping {
    QString source_folder;
    QString target_folder;
    bool skip = false;
};

// ============================================================================
// Conversion Job
// ============================================================================

/// @brief A single file in the conversion queue
struct OstConversionJob {
    QString source_path;   ///< Full path to OST/PST file
    QString display_name;  ///< Filename for display
    qint64 file_size_bytes = 0;
    bool is_ost = false;
    int estimated_items = 0;  ///< From PstFileInfo
    int estimated_folders = 0;

    /// @brief Conversion state
    enum class Status {
        Queued,
        Parsing,
        Converting,
        Uploading,
        Complete,
        Failed,
        Cancelled
    };
    Status status = Status::Queued;

    // Progress
    int items_processed = 0;
    int items_total = 0;
    int items_recovered = 0;  ///< Deleted items found
    int items_failed = 0;
    qint64 bytes_written = 0;
    QString current_folder;  ///< Currently processing folder
    QString error_message;
};

// ============================================================================
// Conversion Configuration
// ============================================================================

/// @brief Global conversion configuration
struct OstConversionConfig {
    // Output. Defaults to EML: it is the widest-compatibility format with a
    // spec-conformant writer, so a convert launched without changing the format
    // produces readable output instead of failing on the gated PST writer.
    OstOutputFormat format = OstOutputFormat::Eml;
    QString output_directory;

    // Threading
    int max_threads = kDefaultOstConversionThreads;  ///< Concurrent file conversions

    // Filtering
    QDateTime date_from;         ///< Null = no lower bound
    QDateTime date_to;           ///< Null = no upper bound
    QStringList folder_include;  ///< Empty = all folders
    QStringList folder_exclude;  ///< Folders to skip
    QString sender_filter;       ///< Sender email contains
    QString recipient_filter;    ///< Recipient email contains

    // Recovery
    RecoveryMode recovery_mode = RecoveryMode::Normal;
    bool recover_deleted_items = false;

    // PST output options
    PstSplitSize split_size = PstSplitSize::NoSplit;
    qint64 custom_split_mb = kDefaultPstCustomSplitMb;  ///< Custom split size in MB

    // EML/MSG options
    bool prefix_filename_with_date = true;
    bool preserve_folder_structure = true;

    // MBOX options
    bool one_mbox_per_folder = true;

    // IMAP upload options
    ImapServerConfig imap_config;
    QVector<ImapFolderMapping> folder_mappings;

    // Reporting
    bool generate_properties_manifest = false;
    bool generate_html_report = true;
    bool include_source_checksums = true;
};

// ============================================================================
// Conversion Result
// ============================================================================

/// @brief Result of a single file conversion
struct OstConversionResult {
    QString source_path;
    QString output_path;
    OstOutputFormat format = OstOutputFormat::Eml;
    int items_converted = 0;
    int items_failed = 0;
    int items_recovered = 0;  ///< Deleted items recovered
    /// False when the deleted-item scan could not enumerate every candidate (a non-cancel read
    /// error truncated the Recoverable Items walk, or an orphan node could not be read), so
    /// items_recovered is a floor and NOT the count of what the source holds. An empty or small
    /// recovered set is only authoritative when this is true.
    bool recovery_complete = true;
    int folders_processed = 0;
    qint64 bytes_written = 0;
    int pst_volumes_created = 0;  ///< For split PST (1 if no split)
    QStringList errors;
    QDateTime started;
    QDateTime finished;
    QString source_sha256;  ///< SHA-256 of source file
};

/// @brief Aggregate result of the entire conversion batch
struct OstConversionBatchResult {
    int files_total = 0;
    int files_succeeded = 0;
    int files_failed = 0;
    int total_items_converted = 0;
    int total_items_recovered = 0;
    qint64 total_bytes_written = 0;
    QVector<OstConversionResult> file_results;
    QDateTime batch_started;
    QDateTime batch_finished;
};

// ============================================================================
// Compile-Time Invariants
// ============================================================================

static_assert(std::is_default_constructible_v<OstConversionJob>,
              "OstConversionJob must be default-constructible.");
static_assert(std::is_default_constructible_v<OstConversionConfig>,
              "OstConversionConfig must be default-constructible.");
static_assert(std::is_default_constructible_v<OstConversionResult>,
              "OstConversionResult must be default-constructible.");
static_assert(std::is_default_constructible_v<ImapServerConfig>,
              "ImapServerConfig must be default-constructible.");

}  // namespace sak

Q_DECLARE_METATYPE(sak::OstConversionJob)
Q_DECLARE_METATYPE(sak::OstConversionResult)
Q_DECLARE_METATYPE(sak::OstConversionBatchResult)
