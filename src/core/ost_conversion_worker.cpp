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
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/mbox_writer.h"
#include "sak/msg_writer.h"
#include "sak/ost_converter_constants.h"
#include "sak/pdf_email_writer.h"
#include "sak/pst_parser.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <optional>
#include <tuple>

namespace {

/// Sanitize one folder-tree path segment so a parsed PST/OST folder name can
/// never escape the output root. Path separators, control and Windows-reserved
/// characters are replaced, trailing dots/spaces are stripped, and a segment
/// that reduces to "." / ".." / empty (which would traverse) becomes "_".
QString sanitizeFolderSegment(const QString& raw) {
    static const QString kForbidden = QStringLiteral("<>:\"/\\|?*");
    QString out;
    out.reserve(raw.size());
    for (QChar character : raw) {
        if (character.unicode() < 0x20 || kForbidden.contains(character)) {
            out += QLatin1Char('_');
        } else {
            out += character;
        }
    }
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) {
        out.chop(1);
    }
    if (out.isEmpty()) {
        return QStringLiteral("_");
    }
    return out;
}

/// Human-readable label for a gated-off output format, used in the error surfaced
/// to the user when they somehow request one.
QString unsupportedFormatLabel(sak::OstOutputFormat format) {
    switch (format) {
    case sak::OstOutputFormat::Pst:
        return QStringLiteral("PST");
    case sak::OstOutputFormat::Msg:
        return QStringLiteral("MSG");
    case sak::OstOutputFormat::Dbx:
        return QStringLiteral("Outlook Express DBX");
    default:
        return QStringLiteral("Selected");
    }
}

}  // namespace

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

    if (!computeSourceChecksumIfRequested(source_path, config, result)) {
        // Provenance was requested but the digest could not be computed over the
        // whole source. Abort closed rather than emit output missing its checksum.
        result.finished = QDateTime::currentDateTime();
        Q_EMIT conversionFinished(result);
        return;
    }

    if (m_cancelled.load()) {
        result.finished = QDateTime::currentDateTime();
        Q_EMIT conversionFinished(result);
        return;
    }

    auto parser = openSourceParser(source_path, result);
    if (!parser) {
        return;
    }

    PstFileInfo file_info = parser->fileInfo();
    m_items_total = file_info.total_items;
    result.output_path = config.output_directory;

    Q_EMIT conversionStarted(m_items_total);

    // Abort before touching the source if the output writer could not be created,
    // so nothing is reported as converted into a writer that was never opened.
    if (!initializeFormatWriters(config, source_path, result)) {
        result.finished = QDateTime::currentDateTime();
        Q_EMIT conversionFinished(result);
        return;
    }

    // Get folder tree and process
    PstFolderTree tree = parser->folderTree();
    processFolderTree(parser.get(), tree, config, result);

    // Process deleted/recovered items if requested
    if (config.recover_deleted_items && !m_cancelled.load()) {
        processRecoveredItems(parser.get(), config, result);
    }

    finalizeWriters();

    result.finished = QDateTime::currentDateTime();

    logInfo("OST Converter: completed {} -- {} items converted, {} failed",
            source_path.toStdString(),
            std::to_string(result.items_converted),
            std::to_string(result.items_failed));

    Q_EMIT conversionFinished(result);
}

bool OstConversionWorker::computeSourceChecksumIfRequested(const QString& source_path,
                                                           const OstConversionConfig& config,
                                                           OstConversionResult& result) {
    if (!config.include_source_checksums) {
        return true;
    }

    QFile source_file(source_path);
    if (!source_file.open(QIODevice::ReadOnly)) {
        const QString msg = QStringLiteral("Failed to open source for checksum: ") + source_path;
        result.errors.append(msg);
        Q_EMIT errorOccurred(msg);
        return false;
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    constexpr qint64 kChunkSize = 64 * 1024;
    bool read_failed = false;
    while (!source_file.atEnd() && !m_cancelled.load()) {
        const QByteArray chunk = source_file.read(kChunkSize);
        if (chunk.isEmpty()) {
            // read() returns empty only on error here (the loop guards atEnd()).
            read_failed = true;
            break;
        }
        hasher.addData(chunk);
    }

    // Fail closed: never store a checksum computed over a partial read. A cancelled
    // run leaves it empty (cancellation is surfaced by the caller); a mid-stream
    // read error is reported so the digest is not silently a prefix-only hash.
    if (m_cancelled.load()) {
        // Cancellation is surfaced by the caller's own m_cancelled check; leave the
        // digest empty and let the normal cancelled path run.
        return true;
    }
    if (read_failed || source_file.error() != QFileDevice::NoError) {
        const QString msg = QStringLiteral("Failed to read source for checksum: ") + source_path;
        result.errors.append(msg);
        Q_EMIT errorOccurred(msg);
        return false;
    }
    result.source_sha256 = hasher.result().toHex();
    source_file.close();
    return true;
}

std::unique_ptr<PstParser> OstConversionWorker::openSourceParser(const QString& source_path,
                                                                 OstConversionResult& result) {
    auto parser = std::make_unique<PstParser>();
    parser->open(source_path);
    if (parser->isOpen()) {
        return parser;
    }

    QString err_msg = "Failed to open file: " + source_path;
    logError("OST Converter: {}", err_msg.toStdString());
    result.errors.append(err_msg);
    result.finished = QDateTime::currentDateTime();
    Q_EMIT errorOccurred(err_msg);
    Q_EMIT conversionFinished(result);
    return nullptr;
}

namespace {
/// Give each source its own MBOX output subdirectory. The MboxWriter always emits
/// a fixed <dir>/mailbox.mbox (combined) or per-folder files under <dir>, so two
/// jobs sharing one output directory -- run concurrently or back to back -- would
/// otherwise truncate each other's mailbox. Pick a subdir that does not yet exist
/// so distinct sources never collide.
QString uniqueMboxSubdir(const QString& directory, const QString& base_name) {
    QString name = base_name.isEmpty() ? QStringLiteral("mbox") : base_name;
    QString candidate = name;
    int counter = 1;
    while (QFileInfo::exists(directory + QStringLiteral("/") + candidate)) {
        candidate = name + QStringLiteral("_") + QString::number(counter);
        ++counter;
    }
    return candidate;
}

}  // namespace

void OstConversionWorker::createPerItemWriter(const OstConversionConfig& config,
                                              const QString& source_path) {
    switch (config.format) {
    case OstOutputFormat::Eml:
        m_eml_writer = std::make_unique<EmlWriter>(config.output_directory,
                                                   config.prefix_filename_with_date,
                                                   config.preserve_folder_structure);
        break;
    case OstOutputFormat::Msg:
        m_msg_writer = std::make_unique<MsgWriter>(config.output_directory,
                                                   config.prefix_filename_with_date,
                                                   config.preserve_folder_structure);
        break;
    case OstOutputFormat::Html:
        m_html_writer = std::make_unique<HtmlEmailWriter>(config.output_directory,
                                                          config.prefix_filename_with_date,
                                                          config.preserve_folder_structure);
        break;
    case OstOutputFormat::Pdf:
        m_pdf_writer = std::make_unique<PdfEmailWriter>(config.output_directory,
                                                        config.prefix_filename_with_date,
                                                        config.preserve_folder_structure);
        break;
    case OstOutputFormat::Mbox: {
        // Isolate each source under its own subdirectory so concurrent/sequential
        // MBOX jobs sharing one output directory never truncate mailbox.mbox.
        const QString sub = uniqueMboxSubdir(config.output_directory,
                                             QFileInfo(source_path).completeBaseName());
        m_mbox_writer = std::make_unique<MboxWriter>(
            config.output_directory + QStringLiteral("/") + sub, config.one_mbox_per_folder);
        break;
    }
    case OstOutputFormat::Dbx:
        m_dbx_writer = std::make_unique<DbxWriter>(config.output_directory);
        break;
    default:
        // Pst and ImapUpload have no per-item writer; initializeFormatWriters refuses
        // both before this is reached.
        break;
    }
}

bool OstConversionWorker::initializeFormatWriters(const OstConversionConfig& config,
                                                  const QString& source_path,
                                                  OstConversionResult& result) {
    m_mbox_writer.reset();
    m_dbx_writer.reset();
    m_eml_writer.reset();
    m_msg_writer.reset();
    m_html_writer.reset();
    m_pdf_writer.reset();

    // IMAP upload is checked first so the message names the real reason: nothing wires
    // ImapUploader to this pipeline, so the per-item path would count every message as
    // converted while uploading none. It is now reported unsupported as well (which greys
    // it out in the picker), but the generic message below blames a missing spec-conformant
    // writer, which is not what is wrong here.
    if (config.format == OstOutputFormat::ImapUpload) {
        const QString msg =
            QStringLiteral("IMAP upload is not implemented (no messages are uploaded)");
        result.errors.append(msg);
        Q_EMIT errorOccurred(msg);
        return false;
    }

    // PST (no MS-PST writer at all), MSG (broken CFB directory tree) and DBX (no OE5/6
    // B-tree index) can only produce files no reader can open, so reject once here rather
    // than writing per-message garbage. PST used to be exempted from this gate and routed
    // to a PstWriter whose create() refused unconditionally, which reported the generic
    // "Failed to create PST output" -- a message that reads like a disk or permission
    // problem instead of naming the real reason.
    if (!isOutputFormatSupported(config.format)) {
        const QString msg = QStringLiteral("%1 output is not supported (no spec-conformant writer)")
                                .arg(unsupportedFormatLabel(config.format));
        result.errors.append(msg);
        Q_EMIT errorOccurred(msg);
        return false;
    }

    createPerItemWriter(config, source_path);
    return true;
}

void OstConversionWorker::finalizeWriters() {
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

bool OstConversionWorker::writeItemByFormat(const PstItemDetail& item,
                                            PstParser* parser,
                                            const QString& folder_path,
                                            const OstConversionConfig& config,
                                            OstConversionResult& result) {
    switch (config.format) {
    case OstOutputFormat::Eml:
        return writeItemEml(item, parser, folder_path, config, result);
    case OstOutputFormat::Msg:
        return writeItemMsg(item, parser, folder_path, config, result);
    case OstOutputFormat::Mbox:
        return writeItemMbox(item, parser, folder_path, config, result);
    case OstOutputFormat::Dbx:
        return writeItemDbx(item, parser, folder_path, config, result);
    case OstOutputFormat::Html:
        return writeItemHtml(item, parser, folder_path, config, result);
    case OstOutputFormat::Pdf:
        return writeItemPdf(item, parser, folder_path, config, result);
    case OstOutputFormat::Pst:
    case OstOutputFormat::ImapUpload:
        // Unreachable: initializeFormatWriters rejects both upfront -- PST has no
        // spec-conformant writer and no IMAP upload is wired. Fail closed defensively
        // so a message is never counted as converted when nothing was written or sent.
        ++result.items_failed;
        return false;
    }
    return true;
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

    // Count exactly one outcome per item: the writers already increment
    // items_failed on failure, so items_converted must only rise on success.
    if (writeItemByFormat(detail, parser, folder_path, config, result)) {
        ++result.items_converted;
    }

    ++m_items_done;

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
            // Surface the loss in items_failed instead of silently under-reporting:
            // every still-unread item in this folder is a message we could not
            // convert, so count the remaining declared items as failed.
            const int remaining = folder.content_count - offset;
            if (remaining > 0) {
                result.items_failed += remaining;
            }
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

    // Sanitize each segment: folder.display_name comes verbatim from the parsed
    // PST/OST and is appended to the output directory by every writer, so a name
    // containing "../" would otherwise write outside the chosen output root.
    const QString safe_segment = sanitizeFolderSegment(folder.display_name);
    QString folder_path = parent_path.isEmpty() ? safe_segment : parent_path + "/" + safe_segment;

    if (!folderPassesFilter(folder.display_name, config)) {
        return;
    }

    // Count each processed folder exactly once, not once per item it contains.
    ++result.folders_processed;

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
    const bool filter_active = !config.date_from.isNull() || !config.date_to.isNull();
    if (filter_active && !item.date.isValid()) {
        // Fail closed: an item with no valid date cannot be proven to fall inside a
        // requested date window, so exclude it rather than silently include it.
        return false;
    }
    if (!config.date_from.isNull() && item.date < config.date_from) {
        return false;
    }
    if (!config.date_to.isNull() && item.date > config.date_to) {
        return false;
    }
    return true;
}

// ============================================================================
// Format Writers
// ============================================================================

bool OstConversionWorker::collectAttachments(const PstItemDetail& item,
                                             PstParser* parser,
                                             OstConversionResult& result,
                                             QVector<QPair<QString, QByteArray>>& out) {
    bool complete = true;
    for (const auto& att : item.attachments) {
        const QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        auto data = parser->readAttachmentData(item.node_id, att.index);
        if (data.has_value()) {
            out.append({name, data.value()});
        } else {
            // An attachment declared in the metadata could not be read: surface the
            // loss and mark the item incomplete so it is not counted as converted.
            complete = false;
            result.errors.append(QStringLiteral("Attachment '%1' dropped from '%2' (read failed)")
                                     .arg(name, item.subject));
        }
    }
    return complete;
}

bool OstConversionWorker::writeItemEml(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_eml_writer) {
        ++result.items_failed;
        result.errors.append("EML writer not initialized for: " + item.subject);
        return false;
    }
    QVector<QPair<QString, QByteArray>> attachment_data;
    if (!collectAttachments(item, parser, result, attachment_data)) {
        // An attachment could not be read: fail the item closed rather than write
        // and count a message that is missing declared content.
        ++result.items_failed;
        return false;
    }

    auto write_result = m_eml_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write EML for: " + item.subject);
        return false;
    }

    // The writer persists across the run, so its byte count is cumulative: assign
    // it rather than add a per-message delta.
    result.bytes_written = m_eml_writer->totalBytesWritten();
    return true;
}

bool OstConversionWorker::writeItemMsg(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_msg_writer) {
        ++result.items_failed;
        result.errors.append("MSG writer not initialized for: " + item.subject);
        return false;
    }
    QVector<QPair<QString, QByteArray>> attachment_data;
    if (!collectAttachments(item, parser, result, attachment_data)) {
        // An attachment could not be read: fail the item closed rather than write
        // and count a message that is missing declared content.
        ++result.items_failed;
        return false;
    }

    // Get all MAPI properties for forensic-grade export
    QVector<MapiProperty> all_props;
    auto props_result = parser->readItemProperties(item.node_id);
    if (props_result.has_value()) {
        all_props = props_result.value();
    }

    auto write_result = m_msg_writer->writeMessage(item, all_props, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write MSG for: " + item.subject);
        return false;
    }

    result.bytes_written = m_msg_writer->totalBytesWritten();
    return true;
}

bool OstConversionWorker::writeItemMbox(const PstItemDetail& item,
                                        PstParser* parser,
                                        const QString& folder_path,
                                        const OstConversionConfig& config,
                                        OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_mbox_writer) {
        ++result.items_failed;
        result.errors.append("MBOX writer not initialized for: " + item.subject);
        return false;
    }
    QVector<QPair<QString, QByteArray>> attachment_data;
    if (!collectAttachments(item, parser, result, attachment_data)) {
        // An attachment could not be read: fail the item closed rather than write
        // and count a message that is missing declared content.
        ++result.items_failed;
        return false;
    }

    auto write_result = m_mbox_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write MBOX for: " + item.subject);
        return false;
    }

    // The writer persists across the run, so its byte count is cumulative: assign
    // it rather than add a per-message delta (which would triangular-overcount).
    result.bytes_written = m_mbox_writer->totalBytesWritten();
    return true;
}

bool OstConversionWorker::writeItemDbx(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_dbx_writer) {
        ++result.items_failed;
        result.errors.append("DBX writer not initialized for: " + item.subject);
        return false;
    }
    QVector<QPair<QString, QByteArray>> attachment_data;
    if (!collectAttachments(item, parser, result, attachment_data)) {
        // An attachment could not be read: fail the item closed rather than write
        // and count a message that is missing declared content.
        ++result.items_failed;
        return false;
    }

    auto write_result = m_dbx_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write DBX for: " + item.subject);
        return false;
    }

    // Cumulative writer total: assign, do not accumulate (avoids triangular overcount).
    result.bytes_written = m_dbx_writer->totalBytesWritten();
    return true;
}

bool OstConversionWorker::writeItemHtml(const PstItemDetail& item,
                                        PstParser* parser,
                                        const QString& folder_path,
                                        const OstConversionConfig& config,
                                        OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_html_writer) {
        ++result.items_failed;
        result.errors.append("HTML writer not initialized for: " + item.subject);
        return false;
    }
    QVector<QPair<QString, QByteArray>> attachment_data;
    if (!collectAttachments(item, parser, result, attachment_data)) {
        // An attachment could not be read: fail the item closed rather than write
        // and count a message that is missing declared content.
        ++result.items_failed;
        return false;
    }

    auto write_result = m_html_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write HTML for: " + item.subject);
        return false;
    }

    result.bytes_written = m_html_writer->totalBytesWritten();
    return true;
}

bool OstConversionWorker::writeItemPdf(const PstItemDetail& item,
                                       PstParser* parser,
                                       const QString& folder_path,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    Q_UNUSED(config);
    if (!m_pdf_writer) {
        ++result.items_failed;
        result.errors.append("PDF writer not initialized for: " + item.subject);
        return false;
    }
    QVector<QPair<QString, QByteArray>> attachment_data;
    if (!collectAttachments(item, parser, result, attachment_data)) {
        // An attachment could not be read: fail the item closed rather than write
        // and count a message that is missing declared content.
        ++result.items_failed;
        return false;
    }

    auto write_result = m_pdf_writer->writeMessage(item, attachment_data, folder_path);
    if (!write_result.has_value()) {
        ++result.items_failed;
        result.errors.append("Failed to write PDF for: " + item.subject);
        return false;
    }

    result.bytes_written = m_pdf_writer->totalBytesWritten();
    return true;
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

void OstConversionWorker::recordRecoveryReliability(bool recoverable_reliable,
                                                    bool orphan_reliable,
                                                    bool orphans_scanned,
                                                    OstConversionResult& result) {
    if (!recoverable_reliable) {
        result.recovery_complete = false;
        result.errors.append(
            QStringLiteral("Deleted-item recovery is INCOMPLETE: a read error truncated the "
                           "Recoverable Items scan, so recoverable items are missing from the "
                           "output. The recovered count is a floor, not a total."));
    }
    if (orphans_scanned && !orphan_reliable) {
        result.recovery_complete = false;
        result.errors.append(
            QStringLiteral("Deleted-item recovery is INCOMPLETE: the orphaned-node scan could "
                           "not enumerate every candidate, so orphaned items are missing from "
                           "the output. The recovered count is a floor, not a total."));
    }
}

void OstConversionWorker::processRecoveredItems(PstParser* parser,
                                                const OstConversionConfig& config,
                                                OstConversionResult& result) {
    DeletedItemScanner scanner(parser);

    QVector<PstItemDetail> recovered_items;
    const bool deep_recovery = config.recovery_mode == RecoveryMode::DeepRecovery;
    if (deep_recovery) {
        recovered_items = scanner.recoverAll();
    } else {
        recovered_items = scanner.scanRecoverableItems();
    }
    // Cancellation is reported through its own channel, not as unreliability (the scanner's
    // flags already exclude it); everything else must reach the result before a single item is
    // written, so a truncated scan can never be presented as a clean recovery.
    if (!m_cancelled.load()) {
        recordRecoveryReliability(
            scanner.recoverableReliable(), scanner.orphanReliable(), deep_recovery, result);
    }

    QString recovery_folder = QStringLiteral("Recovered Items");

    for (const auto& item : recovered_items) {
        if (m_cancelled.load()) {
            return;
        }

        // Only count a recovered item once it was actually written; a failed
        // write already increments items_failed inside the writer.
        if (writeItemByFormat(item, parser, recovery_folder, config, result)) {
            ++result.items_recovered;
        }

        ++m_items_done;
        Q_EMIT progressUpdated(m_items_done, m_items_total, recovery_folder);
    }

    logInfo("OST Converter: recovered {} deleted items", std::to_string(result.items_recovered));
}

}  // namespace sak
