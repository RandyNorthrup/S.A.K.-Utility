// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_export_worker.h
/// @brief Worker for exporting email items to standard formats

#pragma once

#include "sak/email_types.h"

#include <QByteArray>
#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

#include <atomic>

class PstParser;
class MboxParser;

namespace sak {
class EmlWriter;
class HtmlEmailWriter;
class PdfEmailWriter;
}  // namespace sak

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
    struct PerItemWriterSet {
        sak::EmlWriter* eml{nullptr};
        sak::HtmlEmailWriter* html{nullptr};
        sak::PdfEmailWriter* pdf{nullptr};
    };

    struct PstItemExportContext {
        PstParser* parser{nullptr};
        const sak::EmailExportConfig& config;
        sak::EmailExportResult& result;
        const PerItemWriterSet& writers;
    };

    struct MboxItemExportContext {
        MboxParser* parser{nullptr};
        const sak::EmailExportConfig& config;
        sak::EmailExportResult& result;
        const PerItemWriterSet& writers;
        sak::ExportFormat effective_format{sak::ExportFormat::Eml};
    };

    struct PlainTextWriteRequest {
        const sak::PstItemDetail& item;
        const QString& output_dir;
        int index{0};
        const QVector<QPair<QString, QByteArray>>& attachment_data;
        bool save_attachments{false};
    };

    /// Collected attachment payloads plus how many eligible attachments could not
    /// be read (so the caller can mark a message a partial export -- B7-25).
    struct AttachmentCollection {
        QVector<QPair<QString, QByteArray>> data;
        int dropped{0};
    };

    std::atomic<bool> m_cancelled{false};

    // Format writers
    [[nodiscard]] bool writeEml(sak::EmlWriter& writer,
                                const sak::PstItemDetail& item,
                                const QVector<QPair<QString, QByteArray>>& attachment_data,
                                qint64& bytes_written);
    [[nodiscard]] bool writeHtml(sak::HtmlEmailWriter& writer,
                                 const sak::PstItemDetail& item,
                                 const QVector<QPair<QString, QByteArray>>& attachment_data,
                                 qint64& bytes_written);
    [[nodiscard]] bool writePdf(sak::PdfEmailWriter& writer,
                                const sak::PstItemDetail& item,
                                const QVector<QPair<QString, QByteArray>>& attachment_data,
                                qint64& bytes_written);
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
                                         const sak::PstAttachmentInfo& att,
                                         const QString& output_dir);

    // Content builders
    [[nodiscard]] QByteArray buildVcfContent(const sak::PstItemDetail& contact);
    [[nodiscard]] QByteArray buildIcsContent(const QVector<sak::PstItemDetail>& events);

    // Per-item helpers
    [[nodiscard]] bool writePlainText(const PlainTextWriteRequest& request, qint64& bytes_written);
    [[nodiscard]] bool exportAttachments(PstParser* parser,
                                         const sak::PstItemDetail& item,
                                         const QString& output_dir,
                                         const sak::EmailExportConfig& config,
                                         sak::EmailExportResult& result);
    // Returns the readable attachment payloads plus a dropped count (unreadable
    // eligible attachments), so the caller can mark a partial vs clean export.
    [[nodiscard]] AttachmentCollection collectAttachmentData(PstParser* parser,
                                                             const sak::PstItemDetail& item,
                                                             const sak::EmailExportConfig& config,
                                                             sak::EmailExportResult& result);
    [[nodiscard]] AttachmentCollection collectMboxAttachmentData(
        MboxParser* parser,
        int message_index,
        const sak::PstItemDetail& item,
        const sak::EmailExportConfig& config,
        sak::EmailExportResult& result);
    // Write one already-read PST item's body in the configured per-item format.
    [[nodiscard]] bool writePstItemFormat(
        const PstItemExportContext& context,
        const sak::PstItemDetail& detail,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        int index);
    [[nodiscard]] bool saveSidecarAttachments(
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& exported_file_path,
        qint64& bytes_written);

    // Early-failure helper: emits an exportComplete result carrying a single error.
    void emitEarlyFailure(const QString& error_message);

    // Resolve effective item-id list (explicit ids or derived from folder). Records
    // an error into result if folder paging stopped early on a read failure (B7-27).
    [[nodiscard]] static QVector<uint64_t> collectItemIds(PstParser* parser,
                                                          const sak::EmailExportConfig& config,
                                                          sak::EmailExportResult& result);

    // Dispatch to ICS / CSV / per-item export helpers based on config.format.
    void dispatchExportFormat(PstParser* parser,
                              const QVector<uint64_t>& item_ids,
                              const sak::EmailExportConfig& config,
                              sak::EmailExportResult& result);

    // Export format helpers
    [[nodiscard]] static QString formatDisplayName(sak::ExportFormat format);
    void exportPerItemFormats(PstParser* parser,
                              const QVector<uint64_t>& item_ids,
                              const sak::EmailExportConfig& config,
                              sak::EmailExportResult& result);
    [[nodiscard]] bool exportOnePstItem(const PstItemExportContext& context,
                                        uint64_t item_id,
                                        int index);
    void exportIcsFormat(PstParser* parser,
                         const QVector<uint64_t>& item_ids,
                         const sak::EmailExportConfig& config,
                         sak::EmailExportResult& result);
    void exportCsvFormat(PstParser* parser,
                         const QVector<uint64_t>& item_ids,
                         const sak::EmailExportConfig& config,
                         sak::EmailExportResult& result);
    [[nodiscard]] bool exportOneMboxItem(const MboxItemExportContext& context,
                                         int message_index,
                                         int loop_index);

    // CSV helpers
    [[nodiscard]] static QString csvFieldValue(const sak::PstItemDetail& item,
                                               const QString& column);
    [[nodiscard]] static QStringList defaultCsvColumns(sak::ExportFormat format);
    [[nodiscard]] static QString csvFilename(sak::ExportFormat format);

    // Filename utilities
    [[nodiscard]] QString sanitizeFilename(const QString& name, int max_length);
    [[nodiscard]] QString resolveFilenameConflict(const QString& dir, const QString& filename);
};
