// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_conversion_worker.h
/// @brief Per-file conversion worker for OST/PST conversion

#pragma once

#include "sak/email_types.h"
#include "sak/ost_converter_types.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <optional>

class PstParser;

namespace sak {

class MboxWriter;

/// @brief Worker that converts a single OST/PST store into an MBOX mailbox.
///
/// Designed to be moved to a QThread. Communicates via signals only.
class OstConversionWorker : public QObject {
    Q_OBJECT

public:
    explicit OstConversionWorker(QObject* parent = nullptr);
    ~OstConversionWorker() override;

    // Non-copyable, non-movable
    OstConversionWorker(const OstConversionWorker&) = delete;
    OstConversionWorker& operator=(const OstConversionWorker&) = delete;
    OstConversionWorker(OstConversionWorker&&) = delete;
    OstConversionWorker& operator=(OstConversionWorker&&) = delete;

    /// Start converting the source file with the given config
    void convert(const QString& source_path, const OstConversionConfig& config);

    /// Request cancellation
    void cancel();

    /// @brief Fold a deleted-item scan's honesty flags into @p result.
    ///
    /// items_recovered alone cannot say whether the scan ENUMERATED everything: a read error in
    /// the Recoverable Items hierarchy, or an unreadable orphan node, silently shrinks the set.
    /// Recovering fewer items than exist and reporting a clean conversion is a fail-open, so an
    /// unreliable scan clears recovery_complete AND records an error, which classifyOutcome turns
    /// into a Failed job rather than a Complete one. @p orphans_scanned is false when the run
    /// never attempted the orphan pass, in which case @p orphan_reliable says nothing and is
    /// ignored. Pure and static so the rule is testable without a PST fixture.
    static void recordRecoveryReliability(bool recoverable_reliable,
                                          bool orphan_reliable,
                                          bool orphans_scanned,
                                          OstConversionResult& result);

Q_SIGNALS:
    void conversionStarted(int total_items);
    void progressUpdated(int items_done, int items_total, QString current_folder);
    void conversionFinished(sak::OstConversionResult result);
    void errorOccurred(QString message);
    void warningOccurred(QString message);

private:
    /// Process all folders recursively
    void processFolderTree(PstParser* parser,
                           const PstFolderTree& tree,
                           const OstConversionConfig& config,
                           OstConversionResult& result);

    /// Where a folder sits in the recursive walk: its parent's sanitized path (empty at a root)
    /// and its own nesting depth (0 at a root). Both are values a child inherits from its parent
    /// frame, so carrying them as one cursor keeps processFolder within the parameter limit; depth
    /// still bounds the recursion exactly as a standalone argument did.
    struct FolderPosition {
        QString parent_path;
        int depth = 0;
    };

    /// Process a single folder and its children. @p position.depth bounds the recursion so a
    /// crafted PST/OST declaring a pathologically deep folder tree fails closed with a surfaced
    /// error instead of overflowing the stack.
    void processFolder(PstParser* parser,
                       const PstFolder& folder,
                       const FolderPosition& position,
                       const OstConversionConfig& config,
                       OstConversionResult& result);

    /// Open the MBOX writer for this run, or refuse. @p source_path discriminates the
    /// per-source output subdirectory so two jobs sharing an output folder cannot
    /// truncate each other's mailbox.
    [[nodiscard]] bool initializeMboxWriter(const OstConversionConfig& config,
                                            const QString& source_path,
                                            OstConversionResult& result);

    /// Compute the source SHA-256 when the config requests provenance checksums.
    /// Returns true when not requested or the digest was computed over the whole
    /// file; returns false (with an error recorded) when the source could not be
    /// opened or fully read, so the caller aborts rather than producing output
    /// whose requested provenance record is missing or a partial-prefix hash.
    [[nodiscard]] bool computeSourceChecksumIfRequested(const QString& source_path,
                                                        const OstConversionConfig& config,
                                                        OstConversionResult& result);

    [[nodiscard]] std::unique_ptr<PstParser> openSourceParser(const QString& source_path,
                                                              OstConversionResult& result);

    /// Finalize the MBOX writer and surface a finalize-time failure. MboxWriter::finalize
    /// returns void but latches any flush/close failure in hadFailure(); leaving it unchecked
    /// would report an incomplete mailbox as a clean conversion, so it is recorded into
    /// @p result (which classifyOutcome then turns into a Failed job).
    void finalizeWriters(OstConversionResult& result);

    /// Check if a folder passes the include/exclude filter
    [[nodiscard]] bool folderPassesFilter(const QString& folder_name,
                                          const OstConversionConfig& config) const;

    /// Check if an item passes the date filter
    [[nodiscard]] bool itemPassesDateFilter(const PstItemDetail& item,
                                            const OstConversionConfig& config) const;

    /// Write an item to MBOX. Returns true on success.
    [[nodiscard]] bool writeItemMbox(const PstItemDetail& item,
                                     PstParser* parser,
                                     const QString& folder_path,
                                     const OstConversionConfig& config,
                                     OstConversionResult& result);


    /// Collect attachment data for an item into @p out. Records an error for every
    /// attachment whose bytes could not be read and returns false so the caller
    /// fails the item closed instead of writing a silently incomplete message.
    [[nodiscard]] bool collectAttachments(const PstItemDetail& item,
                                          PstParser* parser,
                                          OstConversionResult& result,
                                          QVector<QPair<QString, QByteArray>>& out);

    /// Check if an item passes sender/recipient filter
    [[nodiscard]] bool itemPassesSenderFilter(const PstItemDetail& item,
                                              const OstConversionConfig& config) const;


    /// Process a single item: read detail, filter, and write
    void processItemInFolder(const PstItemSummary& item_summary,
                             PstParser* parser,
                             const QString& folder_path,
                             const OstConversionConfig& config,
                             OstConversionResult& result);

    /// Load and process all items in a folder via batched reads
    void loadAndProcessFolderItems(PstParser* parser,
                                   const PstFolder& folder,
                                   const QString& folder_path,
                                   const OstConversionConfig& config,
                                   OstConversionResult& result);

    /// Process deleted/recovered items
    void processRecoveredItems(PstParser* parser,
                               const OstConversionConfig& config,
                               OstConversionResult& result);

    std::atomic<bool> m_cancelled{false};
    int m_items_done = 0;
    int m_items_total = 0;

    // Per-conversion format writer instances. The per-message writers below are
    // held for the whole conversion (not recreated per item) so their filename
    // collision counters persist -- otherwise every duplicate subject overwrote
    // the previous export.
    std::unique_ptr<MboxWriter> m_mbox_writer;
};

// Compile-Time Invariants
static_assert(!std::is_copy_constructible_v<OstConversionWorker>,
              "OstConversionWorker must not be copy-constructible.");

}  // namespace sak
