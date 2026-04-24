// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_export_worker.h
/// @brief Worker for exporting email items to standard formats

#pragma once

#include "sak/email_types.h"

#include <QObject>

#include <atomic>

class PstParser;
class MboxParser;

class EmailExportWorker : public QObject {
    Q_OBJECT

public:
    explicit EmailExportWorker(QObject* parent = nullptr);

    /// Export items from a PST/OST file
    void exportItems(PstParser* parser, const sak::EmailExportConfig& config);

    /// Export items from an MBOX file
    void exportMboxItems(MboxParser* parser, const sak::EmailExportConfig& config);

    /// Cancel the current export
    void cancel();

Q_SIGNALS:
    void exportStarted(int total_items);
    void exportProgress(int items_done, int total_items, qint64 bytes_written);
    void exportComplete(sak::EmailExportResult result);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    // Format writers
    [[nodiscard]] bool writeEml(const sak::PstItemDetail& item,
                                const QString& output_dir,
                                int index);
    [[nodiscard]] bool writeVcf(const sak::PstItemDetail& contact,
                                const QString& output_dir,
                                int index);
    [[nodiscard]] bool writeIcs(const QVector<sak::PstItemDetail>& events,
                                const QString& output_path);
    [[nodiscard]] bool writeCsv(const QVector<sak::PstItemDetail>& items,
                                const QString& output_path,
                                const QStringList& columns,
                                QChar delimiter);
    [[nodiscard]] bool extractAttachment(PstParser* parser,
                                         uint64_t msg_nid,
                                         int att_index,
                                         const QString& output_dir);

    // Content builders
    [[nodiscard]] QByteArray buildEmlContent(const sak::PstItemDetail& item);
    [[nodiscard]] QByteArray buildVcfContent(const sak::PstItemDetail& contact);
    [[nodiscard]] QByteArray buildIcsContent(const QVector<sak::PstItemDetail>& events);
    [[nodiscard]] static QByteArray buildMboxEmlContent(const sak::MboxMessageDetail& msg);

    // Per-item helpers
    [[nodiscard]] bool writePlainText(const sak::PstItemDetail& item,
                                      const QString& output_dir,
                                      int index);
    [[nodiscard]] bool exportAttachments(PstParser* parser,
                                         const sak::PstItemDetail& item,
                                         const QString& output_dir,
                                         const sak::EmailExportConfig& config);

    // Early-failure helper: emits an exportComplete result carrying a single error.
    void emitEarlyFailure(const QString& error_message);

    // Export format helpers
    [[nodiscard]] static QString formatDisplayName(sak::ExportFormat format);
    void exportPerItemFormats(PstParser* parser,
                              const QVector<uint64_t>& item_ids,
                              const sak::EmailExportConfig& config,
                              sak::EmailExportResult& result);
    void exportIcsFormat(PstParser* parser,
                         const QVector<uint64_t>& item_ids,
                         const sak::EmailExportConfig& config,
                         sak::EmailExportResult& result);
    void exportCsvFormat(PstParser* parser,
                         const QVector<uint64_t>& item_ids,
                         const sak::EmailExportConfig& config,
                         sak::EmailExportResult& result);

    // CSV helpers
    [[nodiscard]] static QString csvFieldValue(const sak::PstItemDetail& item,
                                               const QString& column);
    [[nodiscard]] static QStringList defaultCsvColumns(sak::ExportFormat format);
    [[nodiscard]] static QString csvFilename(sak::ExportFormat format);

    // Filename utilities
    [[nodiscard]] QString sanitizeFilename(const QString& name, int max_length);
    [[nodiscard]] QString resolveFilenameConflict(const QString& dir, const QString& filename);
};
