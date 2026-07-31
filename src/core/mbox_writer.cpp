// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_writer.cpp
/// @brief Unix mbox file writer implementation

#include "sak/mbox_writer.h"

#include "sak/io_write_utils.h"
#include "sak/logger.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimeZone>

namespace sak {

namespace {
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
}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

MboxWriter::MboxWriter(const QString& output_dir, bool one_per_folder)
    : m_output_dir(output_dir), m_one_per_folder(one_per_folder) {}

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

    QByteArray entry = formatMboxEntry(item, attachment_data);
    if (!writeFully(*file, entry)) {
        // A short write splits one message mid-stream and corrupts the mbox (the
        // next From_ line lands inside the previous body); fail closed.
        logError("MboxWriter: incomplete write for folder: {}", folder_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += entry.size();
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
        file_path = m_output_dir + QStringLiteral("/mailbox.mbox");
    }
    m_used_file_paths.insert(file_path);

    QDir().mkpath(QFileInfo(file_path).absolutePath());

    auto* file = new QFile(file_path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Append)) {
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

QByteArray MboxWriter::buildMimeMessage(const PstItemDetail& item,
                                        const QString& sender,
                                        const QDateTime& date,
                                        const QVector<QPair<QString, QByteArray>>& attachments) {
    QByteArray message;
    const QString from_value = item.sender_name.isEmpty() ? sender
                                                          : item.sender_name + " <" + sender + ">";
    message.append("From: " + sanitizeHeaderValue(from_value).toUtf8() + "\r\n");

    if (!item.display_to.isEmpty()) {
        message.append("To: " + sanitizeHeaderValue(item.display_to).toUtf8() + "\r\n");
    }
    if (!item.display_cc.isEmpty()) {
        message.append("Cc: " + sanitizeHeaderValue(item.display_cc).toUtf8() + "\r\n");
    }
    if (!item.subject.isEmpty()) {
        message.append("Subject: " + sanitizeHeaderValue(item.subject).toUtf8() + "\r\n");
    }
    message.append("Date: " + date.toString(Qt::RFC2822Date).toUtf8() + "\r\n");
    if (!item.message_id.isEmpty()) {
        message.append("Message-ID: " + sanitizeHeaderValue(item.message_id).toUtf8() + "\r\n");
    }

    if (!item.body_html.isEmpty() || !attachments.isEmpty()) {
        QString boundary =
            QStringLiteral("----=_Part_%1_%2").arg(item.node_id).arg(date.toSecsSinceEpoch());
        message.append("MIME-Version: 1.0\r\n");
        message.append("Content-Type: multipart/mixed;\r\n");
        message.append(" boundary=\"" + boundary.toUtf8() + "\"\r\n");
        message.append("\r\n");

        message.append("--" + boundary.toUtf8() + "\r\n");
        if (!item.body_html.isEmpty()) {
            message.append("Content-Type: text/html; charset=utf-8\r\n");
            // The body is emitted raw, not quoted-printable-encoded; labelling it
            // quoted-printable makes a reader decode "=XX" sequences in the HTML
            // (e.g. tracking URLs) and swallow trailing '=' as soft breaks. mbox
            // bodies are 8-bit clean, so 8bit is the correct label.
            message.append("Content-Transfer-Encoding: 8bit\r\n");
            message.append("\r\n");
            message.append(item.body_html.toUtf8());
        } else {
            message.append("Content-Type: text/plain; charset=utf-8\r\n");
            message.append("\r\n");
            message.append(item.body_plain.toUtf8());
        }
        message.append("\r\n");

        for (const auto& [att_name, att_data] : attachments) {
            message.append("--" + boundary.toUtf8() + "\r\n");
            message.append("Content-Type: application/octet-stream\r\n");
            message.append("Content-Disposition: attachment; filename=\"" +
                           sanitizeQuotedParam(att_name).toUtf8() + "\"\r\n");
            message.append("Content-Transfer-Encoding: base64\r\n");
            message.append("\r\n");
            message.append(att_data.toBase64());
            message.append("\r\n");
        }
        message.append("--" + boundary.toUtf8() + "--\r\n");
    } else {
        message.append("Content-Type: text/plain; charset=utf-8\r\n");
        message.append("\r\n");
        message.append(item.body_plain.toUtf8());
    }

    return message;
}

QByteArray MboxWriter::formatMboxEntry(const PstItemDetail& item,
                                       const QVector<QPair<QString, QByteArray>>& attachments) {
    QByteArray entry;

    QString sender = item.sender_email.isEmpty() ? QStringLiteral("unknown@localhost")
                                                 : item.sender_email;
    QDateTime date = item.date.isValid() ? item.date : QDateTime::currentDateTime();

    // asctime format: "Mon Jan 01 00:00:00 2024"
    QString date_str = date.toUTC().toString(QStringLiteral("ddd MMM dd HH:mm:ss yyyy"));

    entry.append("From ");
    entry.append(sender.toUtf8());
    entry.append(' ');
    entry.append(date_str.toUtf8());
    entry.append('\n');

    QByteArray message = buildMimeMessage(item, sender, date, attachments);

    // Escape "From " at the start of lines within message body
    entry.append(escapeFromLines(message));

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
