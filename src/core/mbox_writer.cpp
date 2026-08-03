// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_writer.cpp
/// @brief Unix mbox file writer implementation

#include "sak/mbox_writer.h"

#include "sak/io_write_utils.h"
#include "sak/logger.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimeZone>

#include <algorithm>

namespace sak {

namespace {
constexpr qsizetype kMimeBase64LineLength = 76;
constexpr ushort kAsciiMaxCodePoint = 0x7F;

// True when any character is outside 7-bit ASCII (needs RFC 2047 header encoding).
bool hasNonAscii(const QString& value) {
    return std::any_of(value.cbegin(), value.cend(), [](QChar c) {
        return c.unicode() > kAsciiMaxCodePoint;
    });
}

// Strip header-injection vectors from a header value: CR/LF/tab collapse to a
// single space and other C0 control characters are dropped, so a value cannot
// forge a second header or terminate the header block.
QString sanitizeHeaderValue(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n') || ch == QLatin1Char('\t')) {
            out.append(QLatin1Char(' '));
        } else if (ch.unicode() >= 0x20) {
            out.append(ch);
        }
    }
    return out;
}

// Sanitize a quoted-string MIME parameter value (filename=): strip controls and
// backslash-escape quotes/backslashes so it cannot break out of the quotes.
QString sanitizeQuotedParam(const QString& value) {
    QString out = sanitizeHeaderValue(value);
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return out;
}

// RFC 2047 'B' encoded-word for an unstructured header value with non-ASCII bytes;
// raw UTF-8 in a header is non-conformant and mis-decoded by RFC 5322 readers.
QByteArray rfc2047Encode(const QString& value) {
    return "=?UTF-8?B?" + sanitizeHeaderValue(value).toUtf8().toBase64() + "?=";
}

// Header value: RFC 2047-encode when non-ASCII, else the sanitized raw UTF-8.
QByteArray encodedHeaderValue(const QString& value) {
    return hasNonAscii(value) ? rfc2047Encode(value) : sanitizeHeaderValue(value).toUtf8();
}

// Encode an RFC 5322 display-name phrase (an encoded-word MUST NOT appear inside
// the addr-spec, so only the name is encoded), else the sanitized raw name.
QString encodedDisplayName(const QString& name) {
    return hasNonAscii(name) ? QString::fromLatin1(rfc2047Encode(name)) : sanitizeHeaderValue(name);
}

// Encode bytes as base64 wrapped to RFC 2045 76-column lines (a single unbroken
// base64 run is rejected by strict MIME readers); each line ends with CRLF.
QByteArray base64Wrapped(const QByteArray& data) {
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    out.reserve(b64.size() + b64.size() / kMimeBase64LineLength + 2);
    for (qsizetype i = 0; i < b64.size(); i += kMimeBase64LineLength) {
        out.append(b64.mid(i, kMimeBase64LineLength));
        out.append("\r\n");
    }
    return out;
}
}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

MboxWriter::MboxWriter(const QString& output_dir,
                       bool one_per_folder,
                       const QString& combined_basename)
    : m_output_dir(output_dir)
    , m_one_per_folder(one_per_folder)
    , m_combined_basename(combined_basename) {}

MboxWriter::~MboxWriter() {
    finalize();
}

// ============================================================================
// Public API
// ============================================================================

std::expected<void, error_code> MboxWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& folder_path) {
    QFile* file = getOrCreateFile(folder_path);
    if (!file) {
        return std::unexpected(error_code::write_error);
    }

    auto entry = formatMboxEntry(item, attachment_data);
    if (!entry) {
        return std::unexpected(entry.error());
    }
    if (!writeFully(*file, *entry)) {
        // A short write splits one message mid-stream and corrupts the mbox (the
        // next From_ line lands inside the previous body); fail closed.
        logError("MboxWriter: incomplete write for folder: {}", folder_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += entry->size();
    return {};
}

void MboxWriter::finalize() {
    for (auto* file : m_open_files) {
        if (file && file->isOpen()) {
            file->close();
        }
        delete file;
    }
    m_open_files.clear();
}

// ============================================================================
// File Management
// ============================================================================

QFile* MboxWriter::getOrCreateFile(const QString& folder_path) {
    QString key = m_one_per_folder ? folder_path : QStringLiteral("combined");

    if (m_open_files.contains(key)) {
        return m_open_files.value(key);
    }

    QString file_path;
    if (m_one_per_folder && !folder_path.isEmpty()) {
        const QString safe_name = sanitizeFolderName(folder_path);
        const QString base = m_output_dir + QStringLiteral("/") + safe_name;
        // Two distinct folders can sanitize to the same name (e.g. "A_B/C" and
        // "A/B_C" -> "A_B_C"); disambiguate with a numeric suffix so their mail
        // does not interleave into one mailbox.
        file_path = base + QStringLiteral(".mbox");
        for (int suffix = 2; m_used_file_paths.contains(file_path); ++suffix) {
            file_path = base + QStringLiteral(" (%1).mbox").arg(suffix);
        }
    } else {
        // Namespace the single combined mailbox by source-derived basename so distinct jobs sharing
        // one output directory do not truncate each other; empty keeps the legacy "mailbox.mbox".
        const QString base = m_combined_basename.isEmpty()
                                 ? QStringLiteral("mailbox")
                                 : sanitizeFolderName(m_combined_basename);
        file_path = m_output_dir + QStringLiteral("/") + base + QStringLiteral(".mbox");
    }
    m_used_file_paths.insert(file_path);

    QDir().mkpath(QFileInfo(file_path).absolutePath());

    // Truncate, not Append: each file is opened exactly ONCE per run (guarded by
    // m_open_files above) and then written sequentially through the kept-open handle,
    // so within-run messages still accumulate. Append would instead MERGE this run's
    // output onto a stale file left by a previous run, duplicating old mail (B7-32).
    auto* file = new QFile(file_path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        logError("MboxWriter: failed to open mbox file: {}", file_path.toStdString());
        delete file;
        return nullptr;
    }

    m_open_files.insert(key, file);
    return file;
}

// ============================================================================
// MBOX Formatting
// ============================================================================

QString MboxWriter::makeUniqueBoundary(const PstItemDetail& item) {
    constexpr int kMaxBoundaryAttempts = 8;
    constexpr int kHexBase = 16;
    auto* rng = QRandomGenerator::global();
    for (int attempt = 0; attempt < kMaxBoundaryAttempts; ++attempt) {
        const QString candidate = QStringLiteral("----=_Part_%1_%2")
                                      .arg(rng->generate64(), 0, kHexBase)
                                      .arg(rng->generate64(), 0, kHexBase);
        // Verify the delimiter cannot collide with anything emitted raw in the body. Attachments
        // are base64 (no '-'), so only the plain/html bodies can contain arbitrary bytes.
        if (!item.body_html.contains(candidate) && !item.body_plain.contains(candidate)) {
            return candidate;
        }
    }
    return QString();
}

void MboxWriter::appendMessageHeaders(QByteArray& message,
                                      const PstItemDetail& item,
                                      const QString& sender,
                                      const QDateTime& date) {
    // From: encode only the display-name phrase (RFC 2047), never the addr-spec.
    const QByteArray from_value = item.sender_name.isEmpty()
                                      ? sanitizeHeaderValue(sender).toUtf8()
                                      : encodedDisplayName(item.sender_name).toUtf8() + " <" +
                                            sanitizeHeaderValue(sender).toUtf8() + ">";
    message.append("From: " + from_value + "\r\n");

    if (!item.display_to.isEmpty()) {
        message.append("To: " + sanitizeHeaderValue(item.display_to).toUtf8() + "\r\n");
    }
    if (!item.display_cc.isEmpty()) {
        message.append("Cc: " + sanitizeHeaderValue(item.display_cc).toUtf8() + "\r\n");
    }
    if (!item.subject.isEmpty()) {
        message.append("Subject: " + encodedHeaderValue(item.subject) + "\r\n");
    }
    message.append("Date: " + date.toString(Qt::RFC2822Date).toUtf8() + "\r\n");
    if (!item.message_id.isEmpty()) {
        message.append("Message-ID: " + sanitizeHeaderValue(item.message_id).toUtf8() + "\r\n");
    }
}

void MboxWriter::appendMultipartBody(QByteArray& message,
                                     const PstItemDetail& item,
                                     const QString& boundary,
                                     const QVector<QPair<QString, QByteArray>>& attachments) {
    message.append("MIME-Version: 1.0\r\n");
    message.append("Content-Type: multipart/mixed;\r\n");
    message.append(" boundary=\"" + boundary.toUtf8() + "\"\r\n");
    message.append("\r\n");

    // Emit BOTH bodies when present: dropping the plain-text alternative whenever
    // HTML exists silently loses content the source message carried. The body is
    // emitted raw, labelled 8bit (mbox bodies are 8-bit clean); quoted-printable
    // would make a reader decode "=XX" sequences and swallow trailing '='.
    if (!item.body_plain.isEmpty()) {
        message.append("--" + boundary.toUtf8() + "\r\n");
        message.append("Content-Type: text/plain; charset=utf-8\r\n");
        message.append("Content-Transfer-Encoding: 8bit\r\n");
        message.append("\r\n");
        message.append(item.body_plain.toUtf8());
        message.append("\r\n");
    }
    if (!item.body_html.isEmpty()) {
        message.append("--" + boundary.toUtf8() + "\r\n");
        message.append("Content-Type: text/html; charset=utf-8\r\n");
        message.append("Content-Transfer-Encoding: 8bit\r\n");
        message.append("\r\n");
        message.append(item.body_html.toUtf8());
        message.append("\r\n");
    }

    for (const auto& [att_name, att_data] : attachments) {
        message.append("--" + boundary.toUtf8() + "\r\n");
        message.append("Content-Type: application/octet-stream\r\n");
        message.append("Content-Disposition: attachment; filename=\"" +
                       sanitizeQuotedParam(att_name).toUtf8() + "\"\r\n");
        message.append("Content-Transfer-Encoding: base64\r\n");
        message.append("\r\n");
        message.append(base64Wrapped(att_data));  // ends with CRLF
    }
    message.append("--" + boundary.toUtf8() + "--\r\n");
}

std::expected<QByteArray, error_code> MboxWriter::buildMimeMessage(
    const PstItemDetail& item,
    const QString& sender,
    const QDateTime& date,
    const QVector<QPair<QString, QByteArray>>& attachments) {
    QByteArray message;
    appendMessageHeaders(message, item, sender, date);

    if (item.body_html.isEmpty() && attachments.isEmpty()) {
        message.append("Content-Type: text/plain; charset=utf-8\r\n");
        message.append("\r\n");
        message.append(item.body_plain.toUtf8());
        return message;
    }

    const QString boundary = makeUniqueBoundary(item);
    if (boundary.isEmpty()) {
        // No collision-free boundary could be produced: fail closed rather than emit a
        // multipart whose delimiter might appear in the body and corrupt the structure.
        return std::unexpected(error_code::internal_error);
    }
    appendMultipartBody(message, item, boundary, attachments);
    return message;
}

std::expected<QByteArray, error_code> MboxWriter::formatMboxEntry(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) {
    QByteArray entry;

    // Fail closed rather than fabricate envelope metadata: the mbox "From_" line and
    // the RFC 5322 Date are load-bearing, and inventing "unknown@localhost" or the
    // current time would masquerade missing data as real (no-fallback rule). A caller
    // that reaches here with a message lacking a sender or date must surface that gap.
    if (item.sender_email.isEmpty()) {
        logError("MboxWriter: message has no sender address; refusing to fabricate one");
        return std::unexpected(error_code::missing_required_field);
    }
    if (!item.date.isValid()) {
        logError("MboxWriter: message has no valid date; refusing to fabricate one");
        return std::unexpected(error_code::missing_required_field);
    }
    const QString sender = item.sender_email;
    const QDateTime date = item.date;

    // asctime format: "Mon Jan 01 00:00:00 2024"
    QString date_str = date.toUTC().toString(QStringLiteral("ddd MMM dd HH:mm:ss yyyy"));

    entry.append("From ");
    // Strip CR/LF/controls from the sender: an unescaped newline here would forge a second From_
    // line and split one message into two on the next read.
    entry.append(sanitizeHeaderValue(sender).toUtf8());
    entry.append(' ');
    entry.append(date_str.toUtf8());
    entry.append('\n');

    auto message = buildMimeMessage(item, sender, date, attachments);
    if (!message) {
        return std::unexpected(message.error());
    }

    // Escape "From " at the start of lines within message body
    entry.append(escapeFromLines(*message));

    // Blank line separator between messages
    entry.append('\n');

    return entry;
}

QByteArray MboxWriter::escapeFromLines(const QByteArray& content) {
    QByteArray result;
    result.reserve(content.size());

    int pos = 0;
    while (pos < content.size()) {
        int newline = content.indexOf('\n', pos);
        if (newline < 0) {
            result.append(content.mid(pos));
            break;
        }

        QByteArray line = content.mid(pos, newline - pos + 1);
        // Only escape lines starting with "From " (after the first)
        if (pos > 0 && line.startsWith("From ")) {
            result.append('>');
        }
        result.append(line);
        pos = newline + 1;
    }

    return result;
}

QString MboxWriter::sanitizeFolderName(const QString& name) {
    QString safe = name;
    safe.replace(QStringLiteral("/"), QStringLiteral("_"));
    safe.replace(QStringLiteral("\\"), QStringLiteral("_"));
    static const QRegularExpression kInvalid(QStringLiteral("[<>:\"|?*\\x00-\\x1F]"));
    safe.replace(kInvalid, QStringLiteral("_"));
    return safe;
}

}  // namespace sak
