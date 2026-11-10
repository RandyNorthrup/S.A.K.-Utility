// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_types.h
/// @brief Shared data types for the OST/PST Converter tab

#pragma once

#include "sak/layout_constants.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>
#include <cstdint>
#include <type_traits>

namespace sak {

inline constexpr int kDefaultOstConversionThreads = 2;

// ============================================================================
// Recovery Mode
// ============================================================================

/// @brief How far a conversion looks for deleted mail
enum class RecoveryMode {
    Normal,       ///< Only what recover_deleted_items asks for
    DeepRecovery  ///< Also scan all NBT nodes for orphaned messages; implies recover_deleted_items
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
    /// Items left out of the MBOX because they are not mail, or sit in a system folder. They are
    /// not failures: an MBOX holds mail, and these belong in the Contacts and Calendar windows.
    /// Counted by what they are, so the run can say exactly what it did not convert.
    int skipped_contacts = 0;
    int skipped_calendar = 0;
    int skipped_tasks = 0;
    int skipped_notes = 0;
    int skipped_other = 0;
    /// The same items by the message class the store gives them, so a technician can say why a
    /// mailbox's count differs. Class names and counts only: nothing from the items themselves.
    QMap<QString, int> left_out_by_class;
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

    [[nodiscard]] int itemsSkipped() const {
        return skipped_contacts + skipped_calendar + skipped_tasks + skipped_notes + skipped_other;
    }

    /// left_out_by_class on one line, largest count first: "IPM.Contact: 12, IPM.Task: 3".
    [[nodiscard]] QString leftOutByClassText() const {
        QList<QPair<QString, int>> rows;
        for (auto it = left_out_by_class.cbegin(); it != left_out_by_class.cend(); ++it) {
            rows.append({it.key(), it.value()});
        }
        std::ranges::stable_sort(rows,
                                 [](const auto& a, const auto& b) { return a.second > b.second; });
        QStringList parts;
        parts.reserve(rows.size());
        for (const auto& [name, count] : rows) {
            parts.append(QStringLiteral("%1: %2").arg(name).arg(count));
        }
        return parts.join(QStringLiteral(", "));
    }
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
    int total_items_failed = 0;
    int total_items_skipped = 0;
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
