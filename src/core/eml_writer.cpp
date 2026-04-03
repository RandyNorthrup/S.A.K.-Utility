// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file eml_writer.cpp
/// @brief RFC 5322 MIME .eml file writer implementation

#include "sak/eml_writer.h"

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/logger.h"
#include "sak/ost_converter_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace sak {

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
    eml.reserve(4096);

    // Standard headers
    auto addHeader = [&eml](const char* name, const QString& value) {
        if (!value.isEmpty()) {
            eml.append(name);
            eml.append(": ");
            eml.append(value.toUtf8());
            eml.append("\r\n");
        }
    };

    addHeader("From",
              item.sender_email.isEmpty() ? item.sender_name
                                          : item.sender_name + " <" + item.sender_email + ">");
    addHeader("To", item.display_to);
    addHeader("Cc", item.display_cc);
    addHeader("Subject", item.subject);
    addHeader("Message-ID", item.message_id);
    addHeader("In-Reply-To", item.in_reply_to);

    if (item.date.isValid()) {
        addHeader("Date", item.date.toString(Qt::RFC2822Date));
    }

    bool has_attachments = !attachments.isEmpty();
    bool has_html = !item.body_html.isEmpty();
    bool has_plain = !item.body_plain.isEmpty();

    if (!has_attachments && !has_html) {
        // Simple plain-text message
        eml.append("MIME-Version: 1.0\r\n");
        eml.append("Content-Type: text/plain; charset=utf-8\r\n");
        eml.append("Content-Transfer-Encoding: quoted-printable\r\n");
        eml.append("\r\n");
        eml.append(item.body_plain.toUtf8());
    } else {
        // Multipart message
        QByteArray boundary = "----=_SAK_Part_" + QByteArray::number(qHash(item.subject), 16) +
                              "_" + QByteArray::number(qHash(item.date.toString()), 16);

        eml.append("MIME-Version: 1.0\r\n");

        if (has_attachments) {
            eml.append("Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n");
        } else {
            eml.append("Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n");
        }
        eml.append("\r\n");

        // Plain text part
        if (has_plain) {
            eml.append("--" + boundary + "\r\n");
            eml.append("Content-Type: text/plain; charset=utf-8\r\n");
            eml.append("Content-Transfer-Encoding: quoted-printable\r\n");
            eml.append("\r\n");
            eml.append(item.body_plain.toUtf8());
            eml.append("\r\n");
        }

        // HTML part
        if (has_html) {
            eml.append("--" + boundary + "\r\n");
            eml.append("Content-Type: text/html; charset=utf-8\r\n");
            eml.append("Content-Transfer-Encoding: quoted-printable\r\n");
            eml.append("\r\n");
            eml.append(item.body_html.toUtf8());
            eml.append("\r\n");
        }

        // Attachment parts
        for (const auto& [name, data] : attachments) {
            eml.append("--" + boundary + "\r\n");
            eml.append("Content-Type: application/octet-stream; name=\"" + name.toUtf8() +
                       "\"\r\n");
            eml.append("Content-Transfer-Encoding: base64\r\n");
            eml.append("Content-Disposition: attachment; filename=\"" + name.toUtf8() + "\"\r\n");
            eml.append("\r\n");
            eml.append(data.toBase64());
            eml.append("\r\n");
        }

        eml.append("--" + boundary + "--\r\n");
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
