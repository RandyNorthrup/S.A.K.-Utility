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
///
/// There is no output format here. The converter has exactly one job -- turn an OST
/// or PST store into an MBOX mailbox another mail client can import. Per-message
/// output (EML, HTML, Text, PDF, CSV) is the Email Inspector's job, whole-store or
/// per-folder, so the two do not overlap.
struct OstConversionConfig {
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

    // MBOX options. One .mbox per source folder keeps the folder tree legible to the
    // importing client; the alternative is a single mailbox.mbox holding everything.
    bool one_mbox_per_folder = true;

    // Reporting
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
    /// True when convert() returned early because the user cancelled the run, so the counts
    /// above are partial and the output is incomplete. classifyOutcome maps this to Cancelled
    /// (never Complete) even when no item failed and no error was recorded.
    bool cancelled = false;
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
