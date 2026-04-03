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
#include <QRegularExpression>

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
    // Build a simplified DBX message entry
    // Format: 4-byte size + RFC 5322 message content
    QByteArray message;

    // RFC 5322 header
    message.append("From: ");
    if (!item.sender_name.isEmpty()) {
        message.append(item.sender_name.toUtf8());
        message.append(" <");
        message.append(item.sender_email.toUtf8());
        message.append(">");
    } else {
        message.append(item.sender_email.toUtf8());
    }
    message.append("\r\n");

    if (!item.display_to.isEmpty()) {
        message.append("To: " + item.display_to.toUtf8() + "\r\n");
    }
    if (!item.subject.isEmpty()) {
        message.append("Subject: " + item.subject.toUtf8() + "\r\n");
    }
    if (item.date.isValid()) {
        message.append("Date: " + item.date.toString(Qt::RFC2822Date).toUtf8() + "\r\n");
    }

    // Body
    message.append("Content-Type: text/plain; charset=utf-8\r\n");
    message.append("\r\n");
    message.append(item.body_plain.toUtf8());
    message.append("\r\n");

    Q_UNUSED(attachments);

    // DBX entry: 4-byte LE size prefix + message
    QByteArray entry;
    QDataStream ds(&entry, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << static_cast<uint32_t>(message.size());
    entry.append(message);

    return entry;
}

}  // namespace sak
