// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dbx_writer.cpp
/// @brief Outlook Express DBX file writer implementation

#include "sak/dbx_writer.h"

#include "sak/logger.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QRegularExpression>
#include <QUuid>

namespace sak {

namespace {

/// Sanitize folder name for filesystem use
QString sanitizeName(const QString& name) {
    QString safe = name;
    static const QRegularExpression kInvalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    safe.replace(kInvalid, QStringLiteral("_"));
    if (safe.isEmpty()) {
        safe = QStringLiteral("folder");
    }
    return safe;
}

/// Encode bytes as base64, wrapped to RFC 2045 76-column lines.
QByteArray encodeBase64Wrapped(const QByteArray& data) {
    QByteArray b64 = data.toBase64();
    QByteArray wrapped;
    wrapped.reserve(b64.size() + (b64.size() / 76) + 4);
    constexpr int kLineLen = 76;
    for (int i = 0; i < b64.size(); i += kLineLen) {
        wrapped.append(b64.mid(i, kLineLen));
        wrapped.append("\r\n");
    }
    return wrapped;
}

/// RFC 2047 encode a header value if it contains non-ASCII characters.
QByteArray encodeHeaderValue(const QString& value) {
    bool needs_encoding =
        std::any_of(value.cbegin(), value.cend(), [](QChar c) { return c.unicode() > 0x7F; });
    if (!needs_encoding) {
        return value.toUtf8();
    }
    return "=?UTF-8?B?" + value.toUtf8().toBase64() + "?=";
}

/// Sanitize an attachment filename for inclusion in a Content-Disposition header.
QString sanitizeAttachmentFilename(const QString& name) {
    QString safe = name;
    safe.replace(QChar('"'), QChar('_'));
    safe.replace(QChar('\r'), QChar('_'));
    safe.replace(QChar('\n'), QChar('_'));
    if (safe.isEmpty()) {
        safe = QStringLiteral("attachment");
    }
    return safe;
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

DbxWriter::DbxWriter(const QString& output_dir) : m_output_dir(output_dir) {}

DbxWriter::~DbxWriter() {
    finalize();
}

// ============================================================================
// Public API
// ============================================================================

std::expected<void, error_code> DbxWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& folder_path) {
    QString folder_name = folder_path.isEmpty() ? QStringLiteral("Inbox")
                                                : sanitizeName(folder_path);

    if (!m_open_files.contains(folder_name)) {
        QDir().mkpath(m_output_dir);
        QString file_path = m_output_dir + QStringLiteral("/") + folder_name +
                            QStringLiteral(".dbx");

        auto* file = new QFile(file_path);
        if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            logError("DbxWriter: failed to create: {}", file_path.toStdString());
            delete file;
            return std::unexpected(error_code::write_error);
        }

        writeDbxHeader(*file, folder_name);
        m_open_files.insert(folder_name, file);
        m_message_counts.insert(folder_name, 0);
    }

    QFile* file = m_open_files.value(folder_name);
    Q_ASSERT(file);

    QByteArray entry = buildDbxMessageEntry(item, attachment_data);
    qint64 written = file->write(entry);
    if (written < 0) {
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += written;
    ++m_message_counts[folder_name];
    return {};
}

void DbxWriter::finalize() {
    for (auto* file : m_open_files) {
        if (file && file->isOpen()) {
            // Update message count in header before closing
            auto it = m_message_counts.constFind(m_open_files.key(file));
            if (it != m_message_counts.constEnd()) {
                file->seek(0x24);
                QDataStream ds(file);
                ds.setByteOrder(QDataStream::LittleEndian);
                ds << static_cast<uint32_t>(it.value());
            }
            file->close();
        }
        delete file;
    }
    m_open_files.clear();
    m_message_counts.clear();
}

// ============================================================================
// DBX Format Helpers
// ============================================================================

void DbxWriter::writeDbxHeader(QFile& file, const QString& folder_name) {
    // DBX file header (simplified structure)
    // Real OE5/6 DBX has a complex B-tree structure;
    // we write a minimal header that identifies the file as DBX
    QByteArray header(kDbxHeaderSize, '\0');
    QDataStream ds(&header, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);

    // Magic number (offset 0)
    ds << kDbxFolderMagic;
    // DBX version marker
    ds << static_cast<uint32_t>(0x00'00'00'06);
    // Reserved fields
    for (int i = 0; i < 6; ++i) {
        ds << static_cast<uint32_t>(0);
    }
    // Folder name offset and length (simplified)
    ds << static_cast<uint32_t>(0x68);  // name offset
    ds << static_cast<uint32_t>(folder_name.size());
    // Message count placeholder (offset 0x24)
    ds << static_cast<uint32_t>(0);

    // Write folder name at offset 0x68
    if (header.size() > 0x68 + folder_name.size()) {
        header.replace(0x68, folder_name.size(), folder_name.toUtf8());
    }

    file.write(header);
}

QByteArray DbxWriter::buildDbxMessageEntry(const PstItemDetail& item,
                                           const QVector<QPair<QString, QByteArray>>& attachments) {
    // DBX (Outlook Express 5/6) stores each message as length-prefixed RFC 5322
    // content. We emit a proper multipart/mixed message when attachments are
    // present so receiving readers (or anything that reads the raw .dbx) can
    // recover both the body and any attached files.
    QByteArray message;
    appendRfc5322Headers(message, item);
    message.append("MIME-Version: 1.0\r\n");

    if (attachments.isEmpty()) {
        appendSinglePartBody(message, item);
    } else {
        appendMultipartBody(message, item, attachments);
    }

    // DBX entry: 4-byte LE size prefix + message
    QByteArray entry;
    QDataStream ds(&entry, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << static_cast<uint32_t>(message.size());
    entry.append(message);

    return entry;
}

void DbxWriter::appendRfc5322Headers(QByteArray& message, const PstItemDetail& item) {
    message.append("From: ");
    if (!item.sender_name.isEmpty()) {
        message.append(encodeHeaderValue(item.sender_name));
        message.append(" <");
        message.append(item.sender_email.toUtf8());
        message.append(">");
    } else {
        message.append(item.sender_email.toUtf8());
    }
    message.append("\r\n");

    if (!item.display_to.isEmpty()) {
        message.append("To: ");
        message.append(encodeHeaderValue(item.display_to));
        message.append("\r\n");
    }
    if (!item.display_cc.isEmpty()) {
        message.append("Cc: ");
        message.append(encodeHeaderValue(item.display_cc));
        message.append("\r\n");
    }
    if (!item.subject.isEmpty()) {
        message.append("Subject: ");
        message.append(encodeHeaderValue(item.subject));
        message.append("\r\n");
    }
    if (item.date.isValid()) {
        message.append("Date: " + item.date.toString(Qt::RFC2822Date).toUtf8() + "\r\n");
    }
    if (!item.message_id.isEmpty()) {
        message.append("Message-ID: " + item.message_id.toUtf8() + "\r\n");
    }
}

void DbxWriter::appendSinglePartBody(QByteArray& message, const PstItemDetail& item) {
    const bool has_html = !item.body_html.isEmpty();
    if (has_html) {
        message.append("Content-Type: text/html; charset=utf-8\r\n");
    } else {
        message.append("Content-Type: text/plain; charset=utf-8\r\n");
    }
    message.append("Content-Transfer-Encoding: 8bit\r\n");
    message.append("\r\n");
    message.append((has_html ? item.body_html : item.body_plain).toUtf8());
    message.append("\r\n");
}

void DbxWriter::appendMultipartBody(QByteArray& message,
                                    const PstItemDetail& item,
                                    const QVector<QPair<QString, QByteArray>>& attachments) {
    const QByteArray boundary = "----=_SAK_DBX_" +
                                QUuid::createUuid().toByteArray(QUuid::WithoutBraces);
    message.append("Content-Type: multipart/mixed; boundary=\"");
    message.append(boundary);
    message.append("\"\r\n\r\n");

    // Body part
    message.append("--");
    message.append(boundary);
    message.append("\r\n");
    const bool has_html = !item.body_html.isEmpty();
    if (has_html) {
        message.append("Content-Type: text/html; charset=utf-8\r\n");
    } else {
        message.append("Content-Type: text/plain; charset=utf-8\r\n");
    }
    message.append("Content-Transfer-Encoding: 8bit\r\n\r\n");
    message.append((has_html ? item.body_html : item.body_plain).toUtf8());
    message.append("\r\n");

    // Attachment parts
    QMimeDatabase mime_db;
    for (const auto& [att_name, att_data] : attachments) {
        appendAttachmentPart(message, boundary, att_name, att_data, mime_db);
    }

    // Closing boundary
    message.append("--");
    message.append(boundary);
    message.append("--\r\n");
}

void DbxWriter::appendAttachmentPart(QByteArray& message,
                                     const QByteArray& boundary,
                                     const QString& att_name,
                                     const QByteArray& att_data,
                                     QMimeDatabase& mime_db) {
    const QString safe_name = sanitizeAttachmentFilename(att_name);
    const QMimeType mime = mime_db.mimeTypeForFileNameAndData(safe_name, att_data);
    const QByteArray content_type = mime.isValid() ? mime.name().toUtf8()
                                                   : QByteArray("application/octet-stream");

    message.append("--");
    message.append(boundary);
    message.append("\r\n");
    message.append("Content-Type: ");
    message.append(content_type);
    message.append("; name=\"");
    message.append(encodeHeaderValue(safe_name));
    message.append("\"\r\n");
    message.append("Content-Transfer-Encoding: base64\r\n");
    message.append("Content-Disposition: attachment; filename=\"");
    message.append(encodeHeaderValue(safe_name));
    message.append("\"\r\n\r\n");
    message.append(encodeBase64Wrapped(att_data));
}

}  // namespace sak
