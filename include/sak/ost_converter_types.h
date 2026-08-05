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

inline constexpr int kDefaultOstConversionThreads = 2;

// ============================================================================
// Output Format
// ============================================================================

/// @brief Output format for OST/PST conversion.
///
/// This tab READS an OST or PST store and converts it to portable, readable
/// formats. Writing a PST or an OST back out is deliberately not in scope, so
/// there is no Pst entry here -- reading a .pst source is PstParser's job and is
/// unaffected. DBX (Outlook Express) and direct IMAP upload were removed for the
/// same reason: the first is an obsolete format nobody converts TO, the second is
/// server migration rather than file conversion.
enum class OstOutputFormat {
    Eml,   ///< RFC 5322 MIME .eml files (one per message)
    Msg,   ///< MS-OXMSG compound files (one per message)
    Mbox,  ///< Unix mbox format (one file per folder)
    Html,  ///< HTML pages with embedded images
    Pdf    ///< PDF via QTextDocument/QPdfWriter
};

/// @brief True when the format's writer emits files a real reader can open.
///
/// MSG is the last entry that can return false: its writer emits a broken CFB
/// directory tree with no mini-stream allocation, so Outlook cannot open the
/// result. Once a spec-conformant MS-OXMSG writer lands, every format is
/// supported and this function -- along with the "not supported" labelling it
/// drives in the picker -- is deleted rather than left as a permanent home for
/// half-finished formats.
///
/// Single source of truth shared by the worker (which rejects unsupported formats
/// before touching the source), the GUI (which disables them in the format
/// picker) and the headless email.convert_ost action (which derives its advertised
/// format enum from it).
inline constexpr bool isOutputFormatSupported(OstOutputFormat format) {
    switch (format) {
    case OstOutputFormat::Eml:
    case OstOutputFormat::Mbox:
    case OstOutputFormat::Html:
    case OstOutputFormat::Pdf:
        return true;
    case OstOutputFormat::Msg:
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
    // produces readable output.
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

    // EML/MSG options
    bool prefix_filename_with_date = true;
    bool preserve_folder_structure = true;

    // MBOX options
    bool one_mbox_per_folder = true;

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
    /// Files the batch never attempted or abandoned because the user cancelled.
    /// Without this, succeeded + failed silently fell short of total after a cancel and the
    /// "X/N files succeeded" line left the shortfall unexplained.
    int files_cancelled = 0;
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

}  // namespace sak

Q_DECLARE_METATYPE(sak::OstConversionJob)
Q_DECLARE_METATYPE(sak::OstConversionResult)
Q_DECLARE_METATYPE(sak::OstConversionBatchResult)
