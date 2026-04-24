// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_conversion_worker.cpp
/// @brief Per-file OST/PST conversion worker implementation

#include "sak/ost_conversion_worker.h"

#include "sak/dbx_writer.h"
#include "sak/deleted_item_scanner.h"
#include "sak/email_types.h"
#include "sak/eml_writer.h"
#include "sak/error_codes.h"
#include "sak/html_email_writer.h"
#include "sak/logger.h"
#include "sak/mbox_writer.h"
#include "sak/msg_writer.h"
#include "sak/ost_converter_constants.h"
#include "sak/pdf_email_writer.h"
#include "sak/pst_parser.h"
#include "sak/pst_splitter.h"
#include "sak/pst_writer.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <optional>
#include <tuple>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

OstConversionWorker::OstConversionWorker(QObject* parent) : QObject(parent) {}

OstConversionWorker::~OstConversionWorker() = default;

// ============================================================================
// Public API
// ============================================================================

void OstConversionWorker::convert(const QString& source_path, const OstConversionConfig& config) {
    m_cancelled.store(false);
    m_items_done = 0;
    m_items_total = 0;

    OstConversionResult result;
    result.source_path = source_path;
    result.format = config.format;
    result.started = QDateTime::currentDateTime();

    logInfo("OST Converter: starting conversion of {}", source_path.toStdString());

    // Compute source checksum if requested
    if (config.include_source_checksums) {
        QFile source_file(source_path);
        if (source_file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hasher(QCryptographicHash::Sha256);
            constexpr qint64 kChunkSize = 64 * 1024;
            while (!source_file.atEnd() && !m_cancelled.load()) {
                hasher.addData(source_file.read(kChunkSize));
            }
            result.source_sha256 = hasher.result().toHex();
            source_file.close();
        }
    }

    if (m_cancelled.load()) {
        result.finished = QDateTime::currentDateTime();
        Q_EMIT conversionFinished(result);
        return;
    }

    // Open the source file with PstParser
    auto parser = std::make_unique<PstParser>();
    parser->open(source_path);

    // Brief synchronous wait: the parser emits fileOpened on success
    // or errorOccurred on failure. We use the sync API for workers.
    if (!parser->isOpen()) {
        // Attempt a brief busy-wait for async open to complete
        // The parser may have opened synchronously or we need to
        // fallback to checking state directly.
        if (!parser->isOpen()) {
            QString err_msg = "Failed to open file: " + source_path;
            logError("OST Converter: {}", err_msg.toStdString());
            result.errors.append(err_msg);
            result.finished = QDateTime::currentDateTime();
            Q_EMIT errorOccurred(err_msg);
            Q_EMIT conversionFinished(result);
            return;
        }
    }

    PstFileInfo file_info = parser->fileInfo();
    m_items_total = file_info.total_items;
    result.output_path = config.output_directory;

    Q_EMIT conversionStarted(m_items_total);

    initializeFormatWriters(config, source_path);

    // Get folder tree and process
    PstFolderTree tree = parser->folderTree();
    processFolderTree(parser.get(), tree, config, result);

    // Process deleted/recovered items if requested
    if (config.recover_deleted_items && !m_cancelled.load()) {
        processRecoveredItems(parser.get(), config, result);
    }

    finalizeWriters(result);

    result.finished = QDateTime::currentDateTime();

    logInfo("OST Converter: completed {} — {} items converted, {} failed",
            source_path.toStdString(),
            std::to_string(result.items_converted),
            std::to_string(result.items_failed));

    Q_EMIT conversionFinished(result);
}

namespace {

qint64 splitSizeToBytes(PstSplitSize split_size, qint64 custom_mb) {
    switch (split_size) {
    case PstSplitSize::Split2Gb:
        return ost::kSplit2GbBytes;
    case PstSplitSize::Split10Gb:
        return ost::kSplit10GbBytes;
    case PstSplitSize::Custom:
        return custom_mb * 1024 * 1024;
    default:
        return ost::kSplit5GbBytes;
    }
}

}  // namespace

void OstConversionWorker::initializeFormatWriters(const OstConversionConfig& config,
                                                  const QString& source_path) {
    m_pst_writer.reset();
    m_pst_splitter.reset();
    m_mbox_writer.reset();
    m_dbx_writer.reset();
    m_pst_folder_nids.clear();

    if (config.format == OstOutputFormat::Pst) {
        QString base_name = QFileInfo(source_path).completeBaseName();
        if (config.split_size != PstSplitSize::NoSplit) {
            qint64 max_bytes = splitSizeToBytes(config.split_size, config.custom_split_mb);
            QString base_path = config.output_directory + QStringLiteral("/") + base_name;
            m_pst_splitter = std::make_unique<PstSplitter>(base_path, max_bytes);
            if (!m_pst_splitter->create().has_value()) {
                Q_EMIT errorOccurred(QStringLiteral("Failed to create PST output"));
            }
        } else {
            QString out_path = config.output_directory + QStringLiteral("/") + base_name +
                               QStringLiteral(".pst");
            m_pst_writer = std::make_unique<PstWriter>(out_path);
            if (!m_pst_writer->create().has_value()) {
                Q_EMIT errorOccurred(QStringLiteral("Failed to create PST output"));
            }
        }
    } else if (config.format == OstOutputFormat::Mbox) {
        m_mbox_writer = std::make_unique<MboxWriter>(config.output_directory,
                                                     config.one_mbox_per_folder);
    } else if (config.format == OstOutputFormat::Dbx) {
        m_dbx_writer = std::make_unique<DbxWriter>(config.output_directory);
    }
}

void OstConversionWorker::finalizeWriters(OstConversionResult& result) {
    if (m_pst_writer) {
        std::ignore = m_pst_writer->finalize();
    }
    if (m_pst_splitter) {
        std::ignore = m_pst_splitter->finalizeAll();
        result.pst_volumes_created = m_pst_splitter->volumeCount();
    }
    if (m_mbox_writer) {
        m_mbox_writer->finalize();
    }
    if (m_dbx_writer) {
        m_dbx_writer->finalize();
    }
}

void OstConversionWorker::cancel() {
    m_cancelled.store(true);
}

// ============================================================================
// Folder Processing
// ============================================================================

void OstConversionWorker::processFolderTree(PstParser* parser,
                                            const PstFolderTree& tree,
                                            const OstConversionConfig& config,
                                            OstConversionResult& result) {
    for (const auto& root_folder : tree) {
        if (m_cancelled.load()) {
            return;
        }
        processFolder(parser, root_folder, QString(), config, result);
    }
}

void OstConversionWorker::writeItemByFormat(const PstItemDetail& item,
                                            PstParser* parser,
                                            const QString& folder_path,
                                            const OstConversionConfig& config,
                                            OstConversionResult& result) {
    switch (config.format) {
    case OstOutputFormat::Eml:
        writeItemEml(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::Pst:
        writeItemPst(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::Msg:
        writeItemMsg(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::Mbox:
        writeItemMbox(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::Dbx:
        writeItemDbx(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::Html:
        writeItemHtml(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::Pdf:
        writeItemPdf(item, parser, folder_path, config, result);
        break;
    case OstOutputFormat::ImapUpload:
        // IMAP upload is handled by the controller, not per-item
        break;
    }
}

void OstConversionWorker::processItemInFolder(const PstItemSummary& item_summary,
                                              PstParser* parser,
                                              const QString& folder_path,
                                              const OstConversionConfig& config,
                                              OstConversionResult& result) {
    if (m_cancelled.load()) {
        return;
    }

    auto detail_result = parser->readItemDetail(item_summary.node_id);
    if (!detail_result.has_value()) {
        if (config.recovery_mode == RecoveryMode::SkipCorrupt) {
            ++result.items_failed;
            Q_EMIT warningOccurred("Skipped corrupt item in " + folder_path);
            return;
        }
        ++result.items_failed;
        result.errors.append("Failed to read item " + item_summary.subject + " in " + folder_path);
        return;
    }

    const auto& detail = detail_result.value();

    if (!itemPassesDateFilter(detail, config)) {
        return;
    }

    if (!itemPassesSenderFilter(detail, config)) {
        return;
    }

    writeItemByFormat(detail, parser, folder_path, config, result);

    ++m_items_done;
    ++result.items_converted;
    ++result.folders_processed;

    Q_EMIT progressUpdated(m_items_done, m_items_total, folder_path);
}

void OstConversionWorker::loadAndProcessFolderItems(PstParser* parser,
                                                    const PstFolder& folder,
                                                    const QString& folder_path,
                                                    const OstConversionConfig& config,
                                                    OstConversionResult& result) {
    if (folder.content_count <= 0) {
        return;
    }

    constexpr int kBatchSize = 100;
    int offset = 0;

    while (offset < folder.content_count && !m_cancelled.load()) {
        auto items_result = parser->readFolderItems(folder.node_id, offset, kBatchSize);

        if (!items_result.has_value()) {
            QString err = "Failed to read items from folder: " + folder_path;
            logWarning("OST Converter: {}", err.toStdString());
            result.errors.append(err);
            break;
        }

        const auto& items = items_result.value();
        if (items.isEmpty()) {
            break;
        }

        for (const auto& item_summary : items) {
            processItemInFolder(item_summary, parser, folder_path, config, result);
        }

        offset += items.size();
    }
}

void OstConversionWorker::processFolder(PstParser* parser,
                                        const PstFolder& folder,
                                        const QString& parent_path,
                                        const OstConversionConfig& config,
                                        OstConversionResult& result) {
    if (m_cancelled.load()) {
        return;
    }

    QString folder_path = parent_path.isEmpty() ? folder.display_name
                                                : parent_path + "/" + folder.display_name;

    if (!folderPassesFilter(folder.display_name, config)) {
        return;
    }

    Q_EMIT progressUpdated(m_items_done, m_items_total, folder_path);

    loadAndProcessFolderItems(parser, folder, folder_path, config, result);

    for (const auto& child : folder.children) {
        processFolder(parser, child, folder_path, config, result);
    }
}

// ============================================================================
// Filters
// ============================================================================

bool OstConversionWorker::folderPassesFilter(const QString& folder_name,
                                             const OstConversionConfig& config) const {
    // If include list is non-empty, folder must be in it
    if (!config.folder_include.isEmpty()) {
        bool found = false;
        for (const auto& inc : config.folder_include) {
            if (folder_name.compare(inc, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }

    // Check exclude list
    for (const auto& exc : config.folder_exclude) {
        if (folder_name.compare(exc, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }

    return true;
}

bool OstConversionWorker::itemPassesDateFilter(const PstItemDetail& item,
                                               const OstConversionConfig& config) const {
    if (!config.date_from.isNull() && item.date.isValid()) {
        if (item.date < config.date_from) {
            return false;
        }
    }
    if (!config.date_to.isNull() && item.date.isValid()) {
        if (item.date > config.date_to) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// Format Writers
// ============================================================================

QVector<QPair<QString, QByteArray>> OstConversionWorker::collectAttachments(
    const PstItemDetail& item, PstParser* parser) {
    QVector<QPair<QString, QByteArray>> attachment_data;
    for (const auto& att : item.attachments) {
        auto data = parser->readAttachmentData(item.node_id, att.index);
        if (data.has_value()) {
            QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
            attachment_data.append({name, data.value()});
        }
    }
    return attachment_data;
}

void OstConversionWorker::writeItemEml(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    auto attachment_data = collectAttachments(item, parser);

    EmlWriter writer(config.output_directory,
                     config.prefix_filename_with_date,
                     config.preserve_folder_structure);

    auto write_result = writer.writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write EML for: " + item.subject);
        return;
    }

    result.bytes_written += writer.totalBytesWritten();
}

std::optional<uint64_t> OstConversionWorker::ensurePstFolderHierarchy(const QString& folder_path) {
    if (m_pst_folder_nids.contains(folder_path)) {
        return m_pst_folder_nids.value(folder_path);
    }

    QStringList parts = folder_path.split(QStringLiteral("/"), Qt::SkipEmptyParts);
    QString accumulated;
    uint64_t parent_nid = 0x122;  // Root folder NID

    for (const auto& part : parts) {
        accumulated = accumulated.isEmpty() ? part : accumulated + QStringLiteral("/") + part;
        if (!m_pst_folder_nids.contains(accumulated)) {
            std::expected<uint64_t, error_code> folder_result;
            if (m_pst_splitter) {
                folder_result =
                    m_pst_splitter->createFolder(parent_nid, part, QStringLiteral("IPF.Note"));
            } else if (m_pst_writer) {
                folder_result =
                    m_pst_writer->createFolder(parent_nid, part, QStringLiteral("IPF.Note"));
            } else {
                return std::nullopt;
            }
            if (folder_result.has_value()) {
                m_pst_folder_nids[accumulated] = folder_result.value();
                parent_nid = folder_result.value();
            }
        } else {
            parent_nid = m_pst_folder_nids[accumulated];
        }
    }

    return m_pst_folder_nids.value(folder_path, 0x122);
}

void OstConversionWorker::writeItemPst(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    Q_UNUSED(config);
    auto attachment_data = collectAttachments(item, parser);

    auto hierarchy_result = ensurePstFolderHierarchy(folder_path);
    if (!hierarchy_result.has_value()) {
        ++result.items_failed;
        return;
    }

    uint64_t folder_nid = hierarchy_result.value();

    std::expected<void, error_code> write_result;
    if (m_pst_splitter) {
        write_result = m_pst_splitter->writeMessage(folder_nid, item, attachment_data);
    } else if (m_pst_writer) {
        write_result = m_pst_writer->writeMessage(folder_nid, item, attachment_data);
    } else {
        ++result.items_failed;
        return;
    }

    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write PST item: " + item.subject);
    } else {
        if (m_pst_writer) {
            result.bytes_written = m_pst_writer->currentSize();
        }
    }
}

void OstConversionWorker::writeItemMsg(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    auto attachment_data = collectAttachments(item, parser);

    // Get all MAPI properties for forensic-grade export
    QVector<MapiProperty> all_props;
    auto props_result = parser->readItemProperties(item.node_id);
    if (props_result.has_value()) {
        all_props = props_result.value();
    }

    MsgWriter writer(config.output_directory,
                     config.prefix_filename_with_date,
                     config.preserve_folder_structure);

    auto write_result = writer.writeMessage(item, all_props, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write MSG for: " + item.subject);
        return;
    }

    result.bytes_written += writer.totalBytesWritten();
}

void OstConversionWorker::writeItemMbox(const PstItemDetail& item,
                                        PstParser* parser,
                                        const QString& folder_path,
                                        const OstConversionConfig& config,
                                        OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_mbox_writer) {
        ++result.items_failed;
        result.errors.append("MBOX writer not initialized for: " + item.subject);
        return;
    }
    auto attachment_data = collectAttachments(item, parser);

    auto write_result = m_mbox_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write MBOX for: " + item.subject);
        return;
    }

    result.bytes_written += m_mbox_writer->totalBytesWritten();
}

void OstConversionWorker::writeItemDbx(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_dbx_writer) {
        ++result.items_failed;
        result.errors.append("DBX writer not initialized for: " + item.subject);
        return;
    }
    auto attachment_data = collectAttachments(item, parser);

    auto write_result = m_dbx_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write DBX for: " + item.subject);
        return;
    }

    result.bytes_written += m_dbx_writer->totalBytesWritten();
}

void OstConversionWorker::writeItemHtml(const PstItemDetail& item,
                                        PstParser* parser,
                                        const QString& folder_path,
                                        const OstConversionConfig& config,
                                        OstConversionResult& result) {
    auto attachment_data = collectAttachments(item, parser);

    HtmlEmailWriter writer(config.output_directory,
                           config.prefix_filename_with_date,
                           config.preserve_folder_structure);

    auto write_result = writer.writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write HTML for: " + item.subject);
        return;
    }

    result.bytes_written += writer.totalBytesWritten();
}

void OstConversionWorker::writeItemPdf(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    auto attachment_data = collectAttachments(item, parser);

    PdfEmailWriter writer(config.output_directory,
                          config.prefix_filename_with_date,
                          config.preserve_folder_structure);

    auto write_result = writer.writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write PDF for: " + item.subject);
        return;
    }

    result.bytes_written += writer.totalBytesWritten();
}

// ============================================================================
// Sender/Recipient Filter
// ============================================================================

bool OstConversionWorker::itemPassesSenderFilter(const PstItemDetail& item,
                                                 const OstConversionConfig& config) const {
    if (!config.sender_filter.isEmpty()) {
        bool match = item.sender_email.contains(config.sender_filter, Qt::CaseInsensitive) ||
                     item.sender_name.contains(config.sender_filter, Qt::CaseInsensitive);
        if (!match) {
            return false;
        }
    }

    if (!config.recipient_filter.isEmpty()) {
        bool match = item.display_to.contains(config.recipient_filter, Qt::CaseInsensitive) ||
                     item.display_cc.contains(config.recipient_filter, Qt::CaseInsensitive) ||
                     item.display_bcc.contains(config.recipient_filter, Qt::CaseInsensitive);
        if (!match) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// Deleted Item Recovery
// ============================================================================

void OstConversionWorker::processRecoveredItems(PstParser* parser,
                                                const OstConversionConfig& config,
                                                OstConversionResult& result) {
    DeletedItemScanner scanner(parser);

    QVector<PstItemDetail> recovered_items;
    if (config.recovery_mode == RecoveryMode::DeepRecovery) {
        recovered_items = scanner.recoverAll();
    } else {
        recovered_items = scanner.scanRecoverableItems();
    }

    QString recovery_folder = QStringLiteral("Recovered Items");

    for (const auto& item : recovered_items) {
        if (m_cancelled.load()) {
            return;
        }

        writeItemByFormat(item, parser, recovery_folder, config, result);

        ++result.items_recovered;
        ++m_items_done;
        Q_EMIT progressUpdated(m_items_done, m_items_total, recovery_folder);
    }

    logInfo("OST Converter: recovered {} deleted items", std::to_string(result.items_recovered));
}

}  // namespace sak
