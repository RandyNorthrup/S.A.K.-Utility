// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file eml_writer.cpp
/// @brief RFC 5322 MIME .eml file writer implementation

#include "sak/eml_writer.h"

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/ost_converter_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace sak {

namespace {
constexpr qsizetype kEmlInitialReserveBytes = 4096;

// Remove header-injection vectors from a header value: CR/LF (which would start a
// forged header or the body) become spaces, and other C0 control characters are
// dropped. RFC 5322 unstructured header values are single logical lines.
QString sanitizeHeaderValue(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n') || ch == QLatin1Char('\t')) {
            out.append(QLatin1Char(' '));
        } else if (ch.unicode() >= 0x20) {
            out.append(ch);
        }
        // else: drop other C0 control characters
    }
    return out;
}

// Sanitize a value destined for a quoted-string MIME parameter (name=, filename=):
// strip CR/LF/controls and backslash-escape embedded quotes and backslashes so the
// value cannot break out of the quoted string.
QString sanitizeQuotedParam(const QString& value) {
    QString out = sanitizeHeaderValue(value);
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return out;
}

void appendHeader(QByteArray& eml, const char* name, const QString& value) {
    if (value.isEmpty()) {
        return;
    }

    eml.append(name);
    eml.append(": ");
    eml.append(sanitizeHeaderValue(value).toUtf8());
    eml.append("\r\n");
}

void appendStandardHeaders(QByteArray& eml, const PstItemDetail& item) {
    appendHeader(eml,
                 "From",
                 item.sender_email.isEmpty() ? item.sender_name
                                             : item.sender_name + " <" + item.sender_email + ">");
    appendHeader(eml, "To", item.display_to);
    appendHeader(eml, "Cc", item.display_cc);
    appendHeader(eml, "Subject", item.subject);
    appendHeader(eml, "Message-ID", item.message_id);
    appendHeader(eml, "In-Reply-To", item.in_reply_to);
    if (item.date.isValid()) {
        appendHeader(eml, "Date", item.date.toString(Qt::RFC2822Date));
    }
}

QByteArray emlBoundary(const PstItemDetail& item) {
    return "----=_SAK_Part_" + QByteArray::number(qHash(item.subject), kHexBase) + "_" +
           QByteArray::number(qHash(item.date.toString()), kHexBase);
}

void appendSimplePlainMessage(QByteArray& eml, const QString& body) {
    eml.append("MIME-Version: 1.0\r\n");
    eml.append("Content-Type: text/plain; charset=utf-8\r\n");
    // The body is emitted as raw UTF-8, not quoted-printable-encoded. Labelling
    // it quoted-printable makes a compliant reader decode "=XX" sequences and
    // treat a trailing '=' as a soft line break, corrupting the content. 8bit is
    // the correct label for raw UTF-8 bodies.
    eml.append("Content-Transfer-Encoding: 8bit\r\n");
    eml.append("\r\n");
    eml.append(body.toUtf8());
}

void appendBodyPart(QByteArray& eml,
                    const QByteArray& boundary,
                    const char* content_type,
                    const QString& body) {
    eml.append("--" + boundary + "\r\n");
    eml.append(content_type);
    eml.append("\r\n");
    // Raw UTF-8 body: label it 8bit, not quoted-printable (see
    // appendSimplePlainMessage).
    eml.append("Content-Transfer-Encoding: 8bit\r\n");
    eml.append("\r\n");
    eml.append(body.toUtf8());
    eml.append("\r\n");
}

void appendAttachmentParts(QByteArray& eml,
                           const QByteArray& boundary,
                           const QVector<QPair<QString, QByteArray>>& attachments) {
    for (const auto& [name, data] : attachments) {
        const QByteArray safe_name = sanitizeQuotedParam(name).toUtf8();
        eml.append("--" + boundary + "\r\n");
        eml.append("Content-Type: application/octet-stream; name=\"" + safe_name + "\"\r\n");
        eml.append("Content-Transfer-Encoding: base64\r\n");
        eml.append("Content-Disposition: attachment; filename=\"" + safe_name + "\"\r\n");
        eml.append("\r\n");
        eml.append(data.toBase64());
        eml.append("\r\n");
    }
}

void appendMultipartMessage(QByteArray& eml,
                            const PstItemDetail& item,
                            const QVector<QPair<QString, QByteArray>>& attachments) {
    const QByteArray boundary = emlBoundary(item);
    const bool has_attachments = !attachments.isEmpty();

    eml.append("MIME-Version: 1.0\r\n");
    if (has_attachments) {
        eml.append("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n");
    } else {
        eml.append("Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n");
    }
    eml.append("\r\n");

    if (!item.body_plain.isEmpty()) {
        appendBodyPart(eml, boundary, "Content-Type: text/plain; charset=utf-8", item.body_plain);
    }
    if (!item.body_html.isEmpty()) {
        appendBodyPart(eml, boundary, "Content-Type: text/html; charset=utf-8", item.body_html);
    }

    appendAttachmentParts(eml, boundary, attachments);
    eml.append("--" + boundary + "--\r\n");
}
}  // namespace

// ============================================================================
// Construction
// ============================================================================

EmlWriter::EmlWriter(const QString& output_dir, bool prefix_with_date, bool preserve_folders)
    : m_output_dir(output_dir)
    , m_prefix_with_date(prefix_with_date)
    , m_preserve_folders(preserve_folders) {}

// ============================================================================
// Public API
// ============================================================================

std::expected<QString, error_code> EmlWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& subfolder_path) {
    QString target_dir = m_output_dir;
    if (m_preserve_folders && !subfolder_path.isEmpty()) {
        target_dir = m_output_dir + "/" + subfolder_path;
    }

    QDir dir;
    if (!dir.mkpath(target_dir)) {
        logError("EmlWriter: failed to create directory: {}", target_dir.toStdString());
        return std::unexpected(error_code::write_error);
    }

    QString filename = sanitizeFilename(item.subject, item.date);

    // Resolve collisions
    QString full_path = target_dir + "/" + filename + ".eml";
    if (QFile::exists(full_path)) {
        int& counter = m_filename_counters[full_path];
        ++counter;
        full_path = target_dir + "/" + filename + "_(" + QString::number(counter) + ").eml";
    }

    QByteArray content = buildEmlContent(item, attachment_data);

    QFile file(full_path);
    if (!file.open(QIODevice::WriteOnly)) {
        logError("EmlWriter: failed to open file for writing: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    qint64 written = file.write(content);
    if (written != content.size()) {
        logError("EmlWriter: incomplete write to: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += written;
    return full_path;
}

// ============================================================================
// Content Building
// ============================================================================

QByteArray EmlWriter::buildEmlContent(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const {
    QByteArray eml;
    eml.reserve(kEmlInitialReserveBytes);

    appendStandardHeaders(eml, item);
    if (attachments.isEmpty() && item.body_html.isEmpty()) {
        appendSimplePlainMessage(eml, item.body_plain);
    } else {
        appendMultipartMessage(eml, item, attachments);
    }

    return eml;
}

// ============================================================================
// Filename Sanitization
// ============================================================================

QString EmlWriter::sanitizeFilename(const QString& subject, const QDateTime& date) const {
    QString base;
    if (m_prefix_with_date && date.isValid()) {
        base = date.toString("yyyy-MM-dd_HHmmss") + "_";
    }

    QString safe_subject = subject.trimmed();
    if (safe_subject.isEmpty()) {
        safe_subject = QStringLiteral("no_subject");
    }

    // Remove characters invalid in filenames
    static const QRegularExpression kInvalidChars(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    safe_subject.replace(kInvalidChars, QStringLiteral("_"));

    // Truncate to limit
    if (safe_subject.length() > ost::kMaxEmlSubjectLength) {
        safe_subject.truncate(ost::kMaxEmlSubjectLength);
    }

    return base + safe_subject;
}

}  // namespace sak
