// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_conversion_worker.cpp
/// @brief Per-file OST/PST conversion worker implementation

#include "sak/ost_conversion_worker.h"

#include "sak/deleted_item_scanner.h"
#include "sak/email_constants.h"
#include "sak/email_folder_selection.h"
#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/mbox_writer.h"
#include "sak/ost_converter_constants.h"
#include "sak/pst_parser.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <functional>
#include <optional>
#include <tuple>

namespace {

/// Upper bound on folder-tree recursion. Real mailbox hierarchies are only a handful deep; a
/// crafted store declaring a far deeper tree must fail closed rather than overflow the stack.
constexpr int kMaxFolderRecursionDepth = 256;

/// First printable ASCII code point; anything below it is a C0 control character.
constexpr char16_t kFirstPrintableCodePoint = 0x20;

/// Sanitize one folder-tree path segment so a parsed PST/OST folder name can
/// never escape the output root. Path separators, control and Windows-reserved
/// characters are replaced, trailing dots/spaces are stripped, and a segment
/// that reduces to "." / ".." / empty (which would traverse) becomes "_".
QString sanitizeFolderSegment(const QString& raw) {
    static const QString kForbidden = QStringLiteral("<>:\"/\\|?*");
    QString out;
    out.reserve(raw.size());
    for (QChar character : raw) {
        if (character.unicode() < kFirstPrintableCodePoint || kForbidden.contains(character)) {
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
        result.cancelled = true;
        result.finished = QDateTime::currentDateTime();
        Q_EMIT conversionFinished(result);
        return;
    }

    auto parser = openSourceParser(source_path, result);
    if (!parser) {
        return;
    }

    const PstFolderTree tree = parser->folderTree();
    m_items_total = countConvertibleItems(parser.get(), tree);
    result.output_path = config.output_directory;

    Q_EMIT conversionStarted(m_items_total);

    // Abort before touching the source if the output writer could not be created,
    // so nothing is reported as converted into a writer that was never opened.
    if (!initializeMboxWriter(config, source_path, result)) {
        result.finished = QDateTime::currentDateTime();
        Q_EMIT conversionFinished(result);
        return;
    }

    convertStore(parser.get(), tree, config, result);

    // Record whether a user cancel cut the run short (processFolderTree/processRecoveredItems
    // return early on cancel), so the controller classifies it Cancelled rather than Complete:
    // a cancelled run's counts are partial even when nothing failed and no error was recorded.
    result.cancelled = m_cancelled.load();

    result.finished = QDateTime::currentDateTime();

    logInfo("OST Converter: completed {} -- {} items converted, {} failed",
            source_path.toStdString(),
            std::to_string(result.items_converted),
            std::to_string(result.items_failed));

    Q_EMIT conversionFinished(result);
}

void OstConversionWorker::convertStore(PstParser* parser,
                                       const PstFolderTree& tree,
                                       const OstConversionConfig& config,
                                       OstConversionResult& result) {
    processFolderTree(parser, tree, config, result);

    // Process deleted/recovered items if requested. Deep recovery is the same scan plus the
    // orphaned-node walk, so asking for it asks for the recovery.
    const bool wants_recovery = config.recover_deleted_items ||
                                config.recovery_mode == RecoveryMode::DeepRecovery;
    if (wants_recovery && !m_cancelled.load()) {
        processRecoveredItems(parser, config, result);
    }

    finalizeWriters(result);
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

    const QString err_msg = "Failed to open file: " + source_path;
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
    const QString name = base_name.isEmpty() ? QStringLiteral("mbox") : base_name;
    QString candidate = name;
    int counter = 1;
    while (QFileInfo::exists(directory + QStringLiteral("/") + candidate)) {
        candidate = name + QStringLiteral("_") + QString::number(counter);
        ++counter;
    }
    return candidate;
}

}  // namespace

bool OstConversionWorker::initializeMboxWriter(const OstConversionConfig& config,
                                               const QString& source_path,
                                               OstConversionResult& result) {
    m_mbox_writer.reset();

    if (config.output_directory.trimmed().isEmpty()) {
        // Refuse rather than construct a writer rooted at the process working directory,
        // which would scatter mailbox files somewhere the technician never chose.
        const QString msg = QStringLiteral("No output directory was set for the conversion");
        result.errors.append(msg);
        Q_EMIT errorOccurred(msg);
        return false;
    }

    // Isolate each source under its own subdirectory so concurrent/sequential MBOX jobs
    // sharing one output directory never truncate each other's mailbox.mbox.
    const QString sub = uniqueMboxSubdir(config.output_directory,
                                         QFileInfo(source_path).completeBaseName());
    m_mbox_writer = std::make_unique<MboxWriter>(
        config.output_directory + QStringLiteral("/") + sub, config.one_mbox_per_folder);
    return true;
}

void OstConversionWorker::finalizeWriters(OstConversionResult& result) {
    if (!m_mbox_writer) {
        return;
    }
    m_mbox_writer->finalize();
    if (m_mbox_writer->hadFailure()) {
        // finalize() latched a flush/close failure: the mailbox on disk is NOT what the run
        // produced, so the conversion must not be reported clean. Record and surface it.
        const QString msg = QStringLiteral(
            "MBOX export did not finalize cleanly: a flush or close failed, so the mailbox on "
            "disk is incomplete.");
        logError("OST Converter: {}", msg.toStdString());
        result.errors.append(msg);
        Q_EMIT errorOccurred(msg);
    }
}

void OstConversionWorker::cancel() {
    m_cancelled.store(true);
}

// ============================================================================
// Folder Processing
// ============================================================================

namespace {

/// The folders mail lives under: the same starting point the Email Inspector's tree and "Export
/// ALL Mail Folders" use, so a conversion covers exactly the mail a technician can see there.
const PstFolderTree& mailFolderRoots(const PstFolderTree& tree) {
    const PstFolder* ipm = findIpmSubtree(tree);
    return (ipm != nullptr && !ipm->children.isEmpty()) ? ipm->children : tree;
}

/// A search folder lists messages that live in other folders; converting it would duplicate them.
bool isSearchFolder(const PstFolder& folder) {
    constexpr uint64_t kNidTypeMask = 0x1F;
    return (folder.node_id & kNidTypeMask) == email::kNidTypeSearchFolder;
}

void recordSkippedItem(EmailItemType type, OstConversionResult& result) {
    switch (type) {
    case EmailItemType::Contact:
    case EmailItemType::DistList:
        ++result.skipped_contacts;
        break;
    case EmailItemType::Calendar:
    case EmailItemType::MeetingRequest:
        ++result.skipped_calendar;
        break;
    case EmailItemType::Task:
        ++result.skipped_tasks;
        break;
    case EmailItemType::StickyNote:
        ++result.skipped_notes;
        break;
    default:
        ++result.skipped_other;
        break;
    }
}

}  // namespace

int OstConversionWorker::countConvertibleItems(PstParser* parser, const PstFolderTree& tree) {
    int total = 0;
    for (const auto& folder : tree) {
        if (!isSearchFolder(folder)) {
            const auto items = parser->readFolderItems(folder.node_id, 0, 0);
            if (items.has_value()) {
                total += static_cast<int>(std::ranges::count_if(*items, [](const auto& item) {
                    return isRealEmail(item.item_type);
                }));
            }
        }
        total += countConvertibleItems(parser, folder.children);
    }
    return total;
}

void OstConversionWorker::processFolderTree(PstParser* parser,
                                            const PstFolderTree& tree,
                                            const OstConversionConfig& config,
                                            OstConversionResult& result) {
    const PstFolderTree& mail_roots = mailFolderRoots(tree);
    for (const auto& root_folder : mail_roots) {
        if (m_cancelled.load()) {
            return;
        }
        processFolder(parser, root_folder, FolderPosition{}, config, result);
    }
    if (&mail_roots == &tree) {
        return;
    }
    const PstFolder* mail_tree = findIpmSubtree(tree);
    for (const auto& root_folder : tree) {
        processFoldersOutsideMailTree(parser, root_folder, mail_tree, config, result);
    }
}

void OstConversionWorker::processFoldersOutsideMailTree(PstParser* parser,
                                                        const PstFolder& folder,
                                                        const PstFolder* mail_tree,
                                                        const OstConversionConfig& config,
                                                        OstConversionResult& result) {
    if (&folder == mail_tree || m_cancelled.load()) {
        return;
    }
    if (!isSearchFolder(folder) && folderPassesFilter(folder.display_name, config)) {
        // Listed apart from the mail tree, the same way the Email Inspector shows them.
        const QString path = QStringLiteral("Other Folders/") +
                             sanitizeFolderSegment(folder.display_name);
        if (convertFolderItems(parser, folder, path, config, result)) {
            ++result.folders_processed;
        }
    }
    for (const auto& child : folder.children) {
        processFoldersOutsideMailTree(parser, child, mail_tree, config, result);
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
        // One unreadable item never stops the run: it is named here, the rest carry on, and the
        // file ends Failed so the gap cannot be missed.
        ++result.items_failed;
        result.errors.append("Failed to read item " + item_summary.subject + " in " + folder_path);
        ++m_items_done;
        Q_EMIT progressUpdated(m_items_done, m_items_total, folder_path);
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
    if (writeItemMbox(detail, parser, folder_path, result)) {
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
    // Every row the table holds, whatever PR_CONTENT_COUNT declares. The declared count can be low
    // or absent, and walking up to it silently dropped the rest of the folder. What is not mail is
    // left out and counted, in this folder or any other: real mail turns up in system folders, and
    // a filed contact or a deleted appointment turns up among mail.
    auto items_result = parser->readFolderItems(folder.node_id, 0, 0);
    if (!items_result.has_value()) {
        // Surface the loss in items_failed instead of silently under-reporting: the table could
        // not be read, so every item the folder declares is a message we could not convert.
        result.items_failed += std::max(folder.content_count, 1);
        const QString err = "Failed to read items from folder: " + folder_path;
        logWarning("OST Converter: {}", err.toStdString());
        result.errors.append(err);
        return;
    }

    for (const auto& item_summary : *items_result) {
        if (m_cancelled.load()) {
            return;
        }
        if (isRealEmail(item_summary.item_type)) {
            processItemInFolder(item_summary, parser, folder_path, config, result);
        } else {
            recordSkippedItem(item_summary.item_type, result);
        }
    }
}

bool OstConversionWorker::convertFolderItems(PstParser* parser,
                                             const PstFolder& folder,
                                             const QString& folder_path,
                                             const OstConversionConfig& config,
                                             OstConversionResult& result) {
    const int converted_before = result.items_converted;
    const int failed_before = result.items_failed;

    Q_EMIT progressUpdated(m_items_done, m_items_total, folder_path);

    loadAndProcessFolderItems(parser, folder, folder_path, config, result);

    return result.items_converted != converted_before || result.items_failed != failed_before;
}

void OstConversionWorker::processFolder(PstParser* parser,
                                        const PstFolder& folder,
                                        const FolderPosition& position,
                                        const OstConversionConfig& config,
                                        OstConversionResult& result) {
    if (m_cancelled.load()) {
        return;
    }

    // A crafted PST/OST can declare a folder tree nested far deeper than any real mailbox;
    // recursing it unbounded would overflow the stack. Fail closed with a surfaced error
    // (which classifyOutcome treats as a failed job) rather than crash.
    if (position.depth > kMaxFolderRecursionDepth) {
        const QString err = QStringLiteral(
                                "Folder nesting exceeds the supported depth (%1); deeper "
                                "folders were not converted under: ")
                                .arg(kMaxFolderRecursionDepth) +
                            position.parent_path;
        logWarning("OST Converter: {}", err.toStdString());
        result.errors.append(err);
        return;
    }

    // Sanitize each segment: folder.display_name comes verbatim from the parsed
    // PST/OST and is appended to the output directory by every writer, so a name
    // containing "../" would otherwise write outside the chosen output root.
    const QString safe_segment = sanitizeFolderSegment(folder.display_name);
    const QString folder_path =
        position.parent_path.isEmpty() ? safe_segment : position.parent_path + "/" + safe_segment;

    if (isSearchFolder(folder)) {
        return;
    }

    if (!folderPassesFilter(folder.display_name, config)) {
        return;
    }

    // Each folder once, not once per item. One that is not a mail folder counts only if it held
    // mail.
    if (convertFolderItems(parser, folder, folder_path, config, result) ||
        isMailFolder(folder.display_name, folder.container_class)) {
        ++result.folders_processed;
    }

    for (const auto& child : folder.children) {
        processFolder(parser,
                      child,
                      {.parent_path = folder_path, .depth = position.depth + 1},
                      config,
                      result);
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

bool OstConversionWorker::writeItemMbox(const PstItemDetail& item,
                                        PstParser* parser,
                                        const QString& folder_path,
                                        OstConversionResult& result) {
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

// ============================================================================
// Sender/Recipient Filter
// ============================================================================

bool OstConversionWorker::itemPassesSenderFilter(const PstItemDetail& item,
                                                 const OstConversionConfig& config) const {
    if (!config.sender_filter.isEmpty()) {
        const bool match = item.sender_email.contains(config.sender_filter, Qt::CaseInsensitive) ||
                           item.sender_name.contains(config.sender_filter, Qt::CaseInsensitive);
        if (!match) {
            return false;
        }
    }

    if (!config.recipient_filter.isEmpty()) {
        const bool match = item.display_to.contains(config.recipient_filter, Qt::CaseInsensitive) ||
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

    const QString recovery_folder = QStringLiteral("Recovered Items");

    for (const auto& item : recovered_items) {
        if (m_cancelled.load()) {
            return;
        }

        // Only count a recovered item once it was actually written; a failed
        // write already increments items_failed inside the writer.
        if (writeItemMbox(item, parser, recovery_folder, result)) {
            ++result.items_recovered;
        }

        ++m_items_done;
        Q_EMIT progressUpdated(m_items_done, m_items_total, recovery_folder);
    }

    logInfo("OST Converter: recovered {} deleted items", std::to_string(result.items_recovered));
}

}  // namespace sak
