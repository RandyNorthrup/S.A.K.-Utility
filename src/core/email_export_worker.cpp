// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_export_worker.cpp
/// @brief Exports email items to standard file formats (EML, VCF, ICS, CSV)

#include "sak/email_export_worker.h"

#include "sak/email_constants.h"
#include "sak/mbox_parser.h"
#include "sak/pst_parser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <functional>

namespace {

constexpr int kProgressInterval = 10;
constexpr int kMaxFilenameLength = 200;
constexpr int kCsvBodyPreviewLength = 500;

/// Fold a long header line per RFC 5322 §2.2.3 (max 78 chars)
QString foldHeaderLine(const QString& name, const QString& value) {
    QString line = name + QStringLiteral(": ") + value;
    if (line.length() <= 78) {
        return line;
    }
    // Fold at word boundaries
    QString result;
    int pos = 0;
    while (pos < line.length()) {
        int remaining = line.length() - pos;
        int chunk = (pos == 0) ? 78 : 76;  // Subsequent lines lose 2 for CRLF+space
        if (remaining <= chunk) {
            result += line.mid(pos);
            break;
        }
        int break_at = line.lastIndexOf(QLatin1Char(' '), pos + chunk);
        if (break_at <= pos) {
            break_at = pos + chunk;
        }
        result += line.mid(pos, break_at - pos) + QStringLiteral("\r\n ");
        pos = break_at;
    }
    return result;
}

/// Escape a value for CSV output
QString csvEscape(const QString& value, QChar delimiter) {
    if (value.contains(delimiter) || value.contains(QLatin1Char('"')) ||
        value.contains(QLatin1Char('\n')) || value.contains(QLatin1Char('\r'))) {
        QString escaped = value;
        escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        return QLatin1Char('"') + escaped + QLatin1Char('"');
    }
    return value;
}

/// Format a datetime for RFC 5322 (email headers)
QString toRfc5322Date(const QDateTime& datetime) {
    if (!datetime.isValid()) {
        return {};
    }
    // Format: "Thu, 05 Jan 2024 14:30:00 +0000"
    return datetime.toUTC().toString(QStringLiteral("ddd, dd MMM yyyy HH:mm:ss +0000"));
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

/// Collect MBOX message indices to export
QVector<int> collectMboxIndices(MboxParser* parser, const QVector<uint64_t>& item_ids) {
    QVector<int> indices;
    for (auto nid : item_ids) {
        indices.append(static_cast<int>(nid));
    }
    if (!indices.isEmpty()) {
        return indices;
    }
    auto messages = parser->readMessages(0, sak::email::kMaxItemsPerLoad);
    if (messages.has_value()) {
        for (const auto& msg : messages.value()) {
            indices.append(msg.message_index);
        }
    }
    return indices;
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
        if (it.importance == 2) {
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
        if (it.task_status == 2) {
            return QStringLiteral("Complete");
        }
        return QStringLiteral("Unknown");
    });
    map.insert(QStringLiteral("% Complete"), [](const auto& it) {
        return QString::number(it.task_percent_complete * 100.0, 'f', 0);
    });
}

/// Build the complete CSV column→extractor dispatch table
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

}  // namespace

/// Append a vCard field line only if value is non-empty
void appendVcfField(QByteArray& vcf, const char* tag, const QString& value) {
    if (!value.isEmpty()) {
        vcf += tag + value.toUtf8() + "\r\n";
    }
}

// ============================================================================
// Construction
// ============================================================================

EmailExportWorker::EmailExportWorker(QObject* parent) : QObject(parent) {}

// ============================================================================
// PST Export
// ============================================================================

void EmailExportWorker::exportItems(PstParser* parser, const sak::EmailExportConfig& config) {
    Q_ASSERT(parser);
    Q_ASSERT(!config.output_path.isEmpty());

    if (!parser) {
        sak::EmailExportResult result;
        result.errors.append(QStringLiteral("No PST file open for export"));
        result.finished = QDateTime::currentDateTime();
        Q_EMIT exportComplete(result);
        return;
    }

    m_cancelled.store(false);

    sak::EmailExportResult result;
    result.export_path = config.output_path;
    result.started = QDateTime::currentDateTime();
    result.export_format = formatDisplayName(config.format);

    QDir output_dir(config.output_path);
    if (!output_dir.mkpath(QStringLiteral("."))) {
        result.errors.append(QStringLiteral("Failed to create output directory"));
        result.finished = QDateTime::currentDateTime();
        Q_EMIT exportComplete(result);
        return;
    }

    // Collect item IDs to export
    QVector<uint64_t> item_ids = config.item_ids;
    if (item_ids.isEmpty() && config.folder_id != 0) {
        auto items_result =
            parser->readFolderItems(config.folder_id, 0, sak::email::kMaxItemsPerLoad);
        if (items_result.has_value()) {
            for (const auto& item : items_result.value()) {
                item_ids.append(item.node_id);
            }
        }
    }

    if (item_ids.isEmpty()) {
        result.errors.append(QStringLiteral("No items to export"));
        result.finished = QDateTime::currentDateTime();
        Q_EMIT exportComplete(result);
        return;
    }

    Q_EMIT exportStarted(item_ids.size());

    // Single-file formats delegate to dedicated helpers
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
    for (int index = 0; index < item_ids.size(); ++index) {
        if (m_cancelled.load()) {
            break;
        }

        auto detail = parser->readItemDetail(item_ids[index]);
        if (!detail.has_value()) {
            result.items_failed++;
            result.errors.append(QStringLiteral("Failed to read item NID %1").arg(item_ids[index]));
            continue;
        }

        bool success = false;
        switch (config.format) {
        case sak::ExportFormat::Eml:
            success = writeEml(detail.value(), config.output_path, index);
            break;
        case sak::ExportFormat::Vcf:
            success = writeVcf(detail.value(), config.output_path, index);
            break;
        case sak::ExportFormat::PlainTextNotes:
            success = writePlainText(detail.value(), config.output_path, index);
            break;
        case sak::ExportFormat::Attachments:
            success = exportAttachments(parser, detail.value(), config.output_path, config);
            break;
        default:
            break;
        }

        if (success) {
            result.items_exported++;
        } else {
            result.items_failed++;
        }

        if ((index + 1) % kProgressInterval == 0) {
            Q_EMIT exportProgress(index + 1, item_ids.size(), result.total_bytes);
        }
    }

    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

// ============================================================================
// Export Format Helpers
// ============================================================================

QString EmailExportWorker::formatDisplayName(sak::ExportFormat format) {
    switch (format) {
    case sak::ExportFormat::Eml:
        return QStringLiteral("EML");
    case sak::ExportFormat::CsvEmails:
        return QStringLiteral("CSV (Emails)");
    case sak::ExportFormat::Vcf:
        return QStringLiteral("VCF");
    case sak::ExportFormat::CsvContacts:
        return QStringLiteral("CSV (Contacts)");
    case sak::ExportFormat::Ics:
        return QStringLiteral("ICS");
    case sak::ExportFormat::CsvCalendar:
        return QStringLiteral("CSV (Calendar)");
    case sak::ExportFormat::CsvTasks:
        return QStringLiteral("CSV (Tasks)");
    case sak::ExportFormat::PlainTextNotes:
        return QStringLiteral("TXT");
    case sak::ExportFormat::Attachments:
        return QStringLiteral("Attachments");
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
        if (detail.has_value() && detail.value().item_type == sak::EmailItemType::Calendar) {
            events.append(detail.value());
        }
    }
    QString ics_path = config.output_path + QStringLiteral("/calendar_export.ics");
    if (writeIcs(events, ics_path)) {
        result.items_exported = events.size();
    } else {
        result.items_failed = events.size();
        result.errors.append(QStringLiteral("Failed to write ICS file"));
    }
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
        }
        if ((index + 1) % kProgressInterval == 0) {
            Q_EMIT exportProgress(index + 1, item_ids.size(), 0);
        }
    }

    QStringList columns = config.csv_columns;
    if (columns.isEmpty()) {
        columns = defaultCsvColumns(config.format);
    }

    QString csv_name = csvFilename(config.format);
    QString csv_path = config.output_path + QLatin1Char('/') + csv_name;

    if (writeCsv(items, csv_path, columns, config.csv_delimiter)) {
        result.items_exported = items.size();
        QFileInfo fi(csv_path);
        result.total_bytes = fi.size();
    } else {
        result.items_failed = items.size();
        result.errors.append(QStringLiteral("Failed to write CSV file"));
    }
    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

// ============================================================================
// MBOX Export
// ============================================================================

void EmailExportWorker::exportMboxItems(MboxParser* parser, const sak::EmailExportConfig& config) {
    Q_ASSERT(parser);
    Q_ASSERT(!config.output_path.isEmpty());

    if (!parser) {
        sak::EmailExportResult result;
        result.errors.append(QStringLiteral("No MBOX file open for export"));
        result.finished = QDateTime::currentDateTime();
        Q_EMIT exportComplete(result);
        return;
    }

    m_cancelled.store(false);

    sak::EmailExportResult result;
    result.export_path = config.output_path;
    result.export_format = QStringLiteral("EML (from MBOX)");
    result.started = QDateTime::currentDateTime();

    QDir output_dir(config.output_path);
    if (!output_dir.mkpath(QStringLiteral("."))) {
        result.errors.append(QStringLiteral("Failed to create output directory"));
        result.finished = QDateTime::currentDateTime();
        Q_EMIT exportComplete(result);
        return;
    }

    QVector<int> indices = collectMboxIndices(parser, config.item_ids);

    Q_EMIT exportStarted(indices.size());

    for (int idx = 0; idx < indices.size(); ++idx) {
        if (m_cancelled.load()) {
            break;
        }

        auto detail = parser->readMessageDetail(indices[idx]);
        if (!detail.has_value()) {
            result.items_failed++;
            continue;
        }

        const auto& msg = detail.value();
        QString safe_sub = sanitizeFilename(msg.subject, kMaxFilenameLength);
        if (safe_sub.isEmpty()) {
            safe_sub = QStringLiteral("message_%1").arg(indices[idx]);
        }
        QString filename = safe_sub + QStringLiteral(".eml");
        filename = resolveFilenameConflict(config.output_path, filename);

        QFile file(config.output_path + QLatin1Char('/') + filename);
        if (file.open(QIODevice::WriteOnly)) {
            QByteArray content = buildMboxEmlContent(msg);
            file.write(content);
            file.close();
            result.items_exported++;
            result.total_bytes += content.size();
        } else {
            result.items_failed++;
        }

        if ((idx + 1) % kProgressInterval == 0) {
            Q_EMIT exportProgress(idx + 1, indices.size(), result.total_bytes);
        }
    }

    result.finished = QDateTime::currentDateTime();
    Q_EMIT exportComplete(result);
}

QByteArray EmailExportWorker::buildMboxEmlContent(const sak::MboxMessageDetail& msg) {
    QByteArray content;
    content += "From: " + msg.from.toUtf8() + "\r\n";
    content += "To: " + msg.to.toUtf8() + "\r\n";
    if (!msg.cc.isEmpty()) {
        content += "Cc: " + msg.cc.toUtf8() + "\r\n";
    }
    content += "Subject: " + msg.subject.toUtf8() + "\r\n";
    content += "Date: " + toRfc5322Date(msg.date).toUtf8() + "\r\n";
    if (!msg.message_id.isEmpty()) {
        content += "Message-ID: " + msg.message_id.toUtf8() + "\r\n";
    }
    content += "MIME-Version: 1.0\r\n";
    content += "Content-Type: text/plain; charset=UTF-8\r\n";
    content += "Content-Transfer-Encoding: 8bit\r\n";
    content += "\r\n";
    content += msg.body_plain.toUtf8();
    return content;
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

bool EmailExportWorker::writeEml(const sak::PstItemDetail& item,
                                 const QString& output_dir,
                                 int index) {
    QByteArray content = buildEmlContent(item);
    if (content.isEmpty()) {
        return false;
    }

    QString safe_sub = sanitizeFilename(item.subject, kMaxFilenameLength);
    if (safe_sub.isEmpty()) {
        safe_sub = QStringLiteral("message_%1").arg(index);
    }
    if (item.date.isValid()) {
        safe_sub = item.date.toString(QStringLiteral("yyyy-MM-dd")) + QLatin1Char('_') + safe_sub;
    }
    QString filename = safe_sub + QStringLiteral(".eml");
    filename = resolveFilenameConflict(output_dir, filename);

    QFile file(output_dir + QLatin1Char('/') + filename);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(content);
    file.close();
    return true;
}

QByteArray EmailExportWorker::buildEmlContent(const sak::PstItemDetail& item) {
    QByteArray eml;

    // Headers
    eml += "From: " + item.sender_email.toUtf8() + "\r\n";
    if (!item.display_to.isEmpty()) {
        eml += "To: " + item.display_to.toUtf8() + "\r\n";
    }
    if (!item.display_cc.isEmpty()) {
        eml += "Cc: " + item.display_cc.toUtf8() + "\r\n";
    }
    if (!item.display_bcc.isEmpty()) {
        eml += "Bcc: " + item.display_bcc.toUtf8() + "\r\n";
    }
    eml += "Subject: " + item.subject.toUtf8() + "\r\n";
    if (item.date.isValid()) {
        eml += "Date: " + toRfc5322Date(item.date).toUtf8() + "\r\n";
    }
    if (!item.message_id.isEmpty()) {
        eml += "Message-ID: " + item.message_id.toUtf8() + "\r\n";
    }
    if (!item.in_reply_to.isEmpty()) {
        eml += "In-Reply-To: " + item.in_reply_to.toUtf8() + "\r\n";
    }
    eml += "MIME-Version: 1.0\r\n";

    // Body — prefer HTML, fallback to plain
    if (!item.body_html.isEmpty()) {
        eml += "Content-Type: text/html; charset=UTF-8\r\n";
        eml += "Content-Transfer-Encoding: 8bit\r\n";
        eml += "\r\n";
        eml += item.body_html.toUtf8();
    } else {
        eml += "Content-Type: text/plain; charset=UTF-8\r\n";
        eml += "Content-Transfer-Encoding: 8bit\r\n";
        eml += "\r\n";
        eml += item.body_plain.toUtf8();
    }

    return eml;
}

// ============================================================================
// VCF (vCard 3.0) Writer
// ============================================================================

bool EmailExportWorker::writeVcf(const sak::PstItemDetail& contact,
                                 const QString& output_dir,
                                 int index) {
    QByteArray content = buildVcfContent(contact);
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

    QFile file(output_dir + QLatin1Char('/') + filename);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(content);
    file.close();
    return true;
}

QByteArray EmailExportWorker::buildVcfContent(const sak::PstItemDetail& contact) {
    QByteArray vcf;
    vcf += "BEGIN:VCARD\r\n";
    vcf += "VERSION:3.0\r\n";

    // N: Surname;Given name
    vcf += "N:" + contact.surname.toUtf8() + ";" + contact.given_name.toUtf8() + ";;;\r\n";

    // FN: Full name
    QString full_name = contact.given_name;
    if (!contact.surname.isEmpty()) {
        if (!full_name.isEmpty()) {
            full_name += QLatin1Char(' ');
        }
        full_name += contact.surname;
    }
    appendVcfField(vcf, "FN:", full_name);
    appendVcfField(vcf, "ORG:", contact.company_name);
    appendVcfField(vcf, "TITLE:", contact.job_title);
    appendVcfField(vcf, "EMAIL;TYPE=INTERNET:", contact.email_address);
    appendVcfField(vcf, "TEL;TYPE=WORK:", contact.business_phone);
    appendVcfField(vcf, "TEL;TYPE=CELL:", contact.mobile_phone);
    appendVcfField(vcf, "TEL;TYPE=HOME:", contact.home_phone);

    if (!contact.contact_photo.isEmpty()) {
        vcf += "PHOTO;ENCODING=b;TYPE=JPEG:" + contact.contact_photo.toBase64() + "\r\n";
    }

    vcf += "END:VCARD\r\n";
    return vcf;
}

// ============================================================================
// ICS (iCalendar) Writer
// ============================================================================

bool EmailExportWorker::writeIcs(const QVector<sak::PstItemDetail>& events,
                                 const QString& output_path) {
    QByteArray content = buildIcsContent(events);
    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(content);
    file.close();
    return true;
}

QByteArray EmailExportWorker::buildIcsContent(const QVector<sak::PstItemDetail>& events) {
    QByteArray ics;
    ics += "BEGIN:VCALENDAR\r\n";
    ics += "VERSION:2.0\r\n";
    ics += "PRODID:-//SAK Utility//Email Export//EN\r\n";
    ics += "CALSCALE:GREGORIAN\r\n";

    for (const auto& event : events) {
        ics += "BEGIN:VEVENT\r\n";

        if (event.start_time.isValid()) {
            ics += "DTSTART:" + toIcsDateTime(event.start_time).toUtf8() + "\r\n";
        }
        if (event.end_time.isValid()) {
            ics += "DTEND:" + toIcsDateTime(event.end_time).toUtf8() + "\r\n";
        }
        if (!event.subject.isEmpty()) {
            ics += "SUMMARY:" + event.subject.toUtf8() + "\r\n";
        }
        if (!event.location.isEmpty()) {
            ics += "LOCATION:" + event.location.toUtf8() + "\r\n";
        }
        if (!event.body_plain.isEmpty()) {
            // Escape newlines for iCalendar
            QString desc = event.body_plain;
            desc.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            desc.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
            desc.replace(QLatin1Char(','), QStringLiteral("\\,"));
            desc.replace(QLatin1Char(';'), QStringLiteral("\\;"));
            ics += "DESCRIPTION:" + desc.toUtf8() + "\r\n";
        }

        for (const auto& attendee : event.attendees) {
            ics += "ATTENDEE;CN=" + attendee.toUtf8() + ":mailto:" + attendee.toUtf8() + "\r\n";
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
                                 QChar delimiter) {
    QFile file(output_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream.setGenerateByteOrderMark(true);

    // Write header row
    for (int col = 0; col < columns.size(); ++col) {
        if (col > 0) {
            stream << delimiter;
        }
        stream << csvEscape(columns[col], delimiter);
    }
    stream << "\r\n";

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

    file.close();
    return true;
}

// ============================================================================
// Attachment Extraction
// ============================================================================

bool EmailExportWorker::extractAttachment(PstParser* parser,
                                          uint64_t msg_nid,
                                          int att_index,
                                          const QString& output_dir) {
    auto data = parser->readAttachmentData(msg_nid, att_index);
    if (!data.has_value()) {
        return false;
    }

    // Get attachment info for filename
    auto detail = parser->readItemDetail(msg_nid);
    if (!detail.has_value()) {
        return false;
    }

    QString filename;
    if (att_index < detail.value().attachments.size()) {
        const auto& att = detail.value().attachments[att_index];
        filename = att.long_filename.isEmpty() ? att.filename : att.long_filename;
    }
    if (filename.isEmpty()) {
        filename = QStringLiteral("attachment_%1_%2").arg(msg_nid).arg(att_index);
    }
    filename = sanitizeFilename(filename, kMaxFilenameLength);
    filename = resolveFilenameConflict(output_dir, filename);

    QFile file(output_dir + QLatin1Char('/') + filename);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(data.value());
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
    for (QChar forbidden : kForbidden) {
        safe.replace(forbidden, QLatin1Char('_'));
    }

    // Replace control characters
    QString result;
    result.reserve(safe.length());
    for (QChar character : safe) {
        if (character.unicode() >= 32) {
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
    QString full_path = dir + QLatin1Char('/') + filename;
    if (!QFile::exists(full_path)) {
        return filename;
    }

    QFileInfo fi(filename);
    QString base = fi.completeBaseName();
    QString ext = fi.suffix();

    constexpr int kMaxConflictAttempts = 9999;
    for (int attempt = 2; attempt <= kMaxConflictAttempts; ++attempt) {
        QString candidate = base + QStringLiteral("_%1").arg(attempt);
        if (!ext.isEmpty()) {
            candidate += QLatin1Char('.') + ext;
        }
        if (!QFile::exists(dir + QLatin1Char('/') + candidate)) {
            return candidate;
        }
    }

    // Fallback with timestamp
    return base + QLatin1Char('_') + QString::number(QDateTime::currentMSecsSinceEpoch()) +
           QLatin1Char('.') + ext;
}

// ============================================================================
// Helper: Plain text export (sticky notes)
// ============================================================================

bool EmailExportWorker::writePlainText(const sak::PstItemDetail& item,
                                       const QString& output_dir,
                                       int index) {
    QString safe_sub = sanitizeFilename(item.subject, kMaxFilenameLength);
    if (safe_sub.isEmpty()) {
        safe_sub = QStringLiteral("note_%1").arg(index);
    }
    QString filename = safe_sub + QStringLiteral(".txt");
    filename = resolveFilenameConflict(output_dir, filename);

    QFile file(output_dir + QLatin1Char('/') + filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    if (!item.subject.isEmpty()) {
        stream << item.subject << "\r\n";
        stream << QStringLiteral("---") << "\r\n";
    }
    stream << item.body_plain;
    file.close();
    return true;
}

// ============================================================================
// Helper: Export all attachments for a message
// ============================================================================

bool EmailExportWorker::exportAttachments(PstParser* parser,
                                          const sak::PstItemDetail& item,
                                          const QString& output_dir,
                                          const sak::EmailExportConfig& config) {
    if (item.attachments.isEmpty()) {
        return true;
    }

    bool any_success = false;
    for (int att_idx = 0; att_idx < item.attachments.size(); ++att_idx) {
        const auto& att = item.attachments[att_idx];

        // Skip inline images if configured
        if (config.skip_inline_images && !att.content_id.isEmpty()) {
            continue;
        }

        // Apply attachment filter if set
        if (!config.attachment_filter.isEmpty()) {
            QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
            QRegularExpression filter(QRegularExpression::wildcardToRegularExpression(
                                          config.attachment_filter),
                                      QRegularExpression::CaseInsensitiveOption);
            if (!filter.match(name).hasMatch()) {
                continue;
            }
        }

        if (extractAttachment(parser, item.node_id, att_idx, output_dir)) {
            any_success = true;
        }
    }
    return any_success;
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
