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

class DbxWriter;
class HtmlEmailWriter;
class MboxWriter;
class PdfEmailWriter;
class PstSplitter;
class PstWriter;

/// @brief Worker that converts a single OST/PST file to the target format.
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

    /// Process a single folder and its children
    void processFolder(PstParser* parser,
                       const PstFolder& folder,
                       const QString& parent_path,
                       const OstConversionConfig& config,
                       OstConversionResult& result);

    /// Initialize format-specific writers based on config
    void initializeFormatWriters(const OstConversionConfig& config, const QString& source_path);

    /// Finalize all active writers
    void finalizeWriters(OstConversionResult& result);

    /// Check if a folder passes the include/exclude filter
    [[nodiscard]] bool folderPassesFilter(const QString& folder_name,
                                          const OstConversionConfig& config) const;

    /// Check if an item passes the date filter
    [[nodiscard]] bool itemPassesDateFilter(const PstItemDetail& item,
                                            const OstConversionConfig& config) const;

    /// Write an item using the EML writer
    void writeItemEml(const PstItemDetail& item,
                      PstParser* parser,
                      const QString& folder_path,
                      const OstConversionConfig& config,
                      OstConversionResult& result);

    /// Write an item using the PST writer / splitter
    void writeItemPst(const PstItemDetail& item,
                      PstParser* parser,
                      const QString& folder_path,
                      const OstConversionConfig& config,
                      OstConversionResult& result);

    /// Write an item as MSG (OLE2 compound file)
    void writeItemMsg(const PstItemDetail& item,
                      PstParser* parser,
                      const QString& folder_path,
                      const OstConversionConfig& config,
                      OstConversionResult& result);

    /// Write an item to MBOX
    void writeItemMbox(const PstItemDetail& item,
                       PstParser* parser,
                       const QString& folder_path,
                       const OstConversionConfig& config,
                       OstConversionResult& result);

    /// Write an item as DBX (Outlook Express)
    void writeItemDbx(const PstItemDetail& item,
                      PstParser* parser,
                      const QString& folder_path,
                      const OstConversionConfig& config,
                      OstConversionResult& result);

    /// Write an item as HTML
    void writeItemHtml(const PstItemDetail& item,
                       PstParser* parser,
                       const QString& folder_path,
                       const OstConversionConfig& config,
                       OstConversionResult& result);

    /// Write an item as PDF
    void writeItemPdf(const PstItemDetail& item,
                      PstParser* parser,
                      const QString& folder_path,
                      const OstConversionConfig& config,
                      OstConversionResult& result);

    /// Collect attachment data for an item
    [[nodiscard]] QVector<QPair<QString, QByteArray>> collectAttachments(const PstItemDetail& item,
                                                                         PstParser* parser);

    /// Check if an item passes sender/recipient filter
    [[nodiscard]] bool itemPassesSenderFilter(const PstItemDetail& item,
                                              const OstConversionConfig& config) const;

    /// Dispatch an item to the correct format-specific writer
    void writeItemByFormat(const PstItemDetail& item,
                           PstParser* parser,
                           const QString& folder_path,
                           const OstConversionConfig& config,
                           OstConversionResult& result);

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

    /// Ensure the PST folder hierarchy exists, returning the leaf NID
    [[nodiscard]] std::optional<uint64_t> ensurePstFolderHierarchy(const QString& folder_path);

    /// Process deleted/recovered items
    void processRecoveredItems(PstParser* parser,
                               const OstConversionConfig& config,
                               OstConversionResult& result);

    std::atomic<bool> m_cancelled{false};
    int m_items_done = 0;
    int m_items_total = 0;

    // Per-conversion format writer instances
    std::unique_ptr<PstWriter> m_pst_writer;
    std::unique_ptr<PstSplitter> m_pst_splitter;
    std::unique_ptr<MboxWriter> m_mbox_writer;
    std::unique_ptr<DbxWriter> m_dbx_writer;
    QHash<QString, uint64_t> m_pst_folder_nids;
};

// Compile-Time Invariants
static_assert(!std::is_copy_constructible_v<OstConversionWorker>,
              "OstConversionWorker must not be copy-constructible.");

}  // namespace sak
