// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file html_email_writer.cpp
/// @brief HTML email page writer implementation

#include "sak/html_email_writer.h"

#include "sak/logger.h"
#include "sak/ost_converter_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace sak {

namespace {

constexpr int kMaxFilenameLength = 200;

/// Escape HTML special characters
QString escapeHtml(const QString& text) {
    QString escaped = text;
    escaped.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
    escaped.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
    escaped.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
    return escaped;
}

/// Detect image MIME type from data
QString detectImageMime(const QByteArray& data) {
    if (data.startsWith("\x89PNG")) {
        return QStringLiteral("image/png");
    }
    if (data.startsWith("\xFF\xD8\xFF")) {
        return QStringLiteral("image/jpeg");
    }
    if (data.startsWith("GIF8")) {
        return QStringLiteral("image/gif");
    }
    if (data.startsWith("RIFF") && data.size() > 8 && data.mid(8, 4) == "WEBP") {
        return QStringLiteral("image/webp");
    }
    return QString();
}

/// Detect image from filename extension
bool isImageFilename(const QString& name) {
    QString lower = name.toLower();
    return lower.endsWith(QStringLiteral(".png")) || lower.endsWith(QStringLiteral(".jpg")) ||
           lower.endsWith(QStringLiteral(".jpeg")) || lower.endsWith(QStringLiteral(".gif")) ||
           lower.endsWith(QStringLiteral(".bmp")) || lower.endsWith(QStringLiteral(".webp"));
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

HtmlEmailWriter::HtmlEmailWriter(const QString& output_dir,
                                 bool prefix_with_date,
                                 bool preserve_folders)
    : m_output_dir(output_dir)
    , m_prefix_with_date(prefix_with_date)
    , m_preserve_folders(preserve_folders) {}

// ============================================================================
// Public API
// ============================================================================

std::expected<QString, error_code> HtmlEmailWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& subfolder_path) {
    QString dir_path = m_output_dir;
    if (m_preserve_folders && !subfolder_path.isEmpty()) {
        dir_path += QStringLiteral("/") + subfolder_path;
    }
    QDir().mkpath(dir_path);

    QString filename = sanitizeFilename(item.subject, item.date);

    // Handle collisions
    QString key = dir_path + "/" + filename;
    if (m_filename_counters.contains(key)) {
        int count = ++m_filename_counters[key];
        QFileInfo fi(filename);
        filename = fi.completeBaseName() + QStringLiteral("_%1").arg(count) +
                   QStringLiteral(".html");
    } else {
        m_filename_counters.insert(key, 1);
    }

    QString full_path = dir_path + QStringLiteral("/") + filename;

    QString attachments_dir = saveFileAttachments(attachment_data, dir_path, filename);

    // Build and write HTML
    QString html = buildHtmlPage(item, attachment_data);
    QFile file(full_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("HtmlEmailWriter: failed to create: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    QByteArray content = html.toUtf8();
    file.write(content);
    m_bytes_written += content.size();
    file.close();

    return full_path;
}

QString HtmlEmailWriter::saveFileAttachments(
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& dir_path,
    const QString& filename) {
    bool has_file_attachments = false;
    for (const auto& [name, data] : attachment_data) {
        if (!isImageFilename(name)) {
            has_file_attachments = true;
            break;
        }
    }

    QString attachments_dir;
    if (has_file_attachments) {
        QFileInfo fi(filename);
        attachments_dir = dir_path + QStringLiteral("/") + fi.completeBaseName() +
                          QStringLiteral("_files");
        QDir().mkpath(attachments_dir);
    }

    for (const auto& [name, data] : attachment_data) {
        if (!isImageFilename(name) && !attachments_dir.isEmpty()) {
            QFile att_file(attachments_dir + QStringLiteral("/") + name);
            if (att_file.open(QIODevice::WriteOnly)) {
                att_file.write(data);
                m_bytes_written += data.size();
            }
        }
    }

    return attachments_dir;
}

// ============================================================================
// HTML Generation
// ============================================================================

void HtmlEmailWriter::buildHtmlHeaderFields(QTextStream& ts, const PstItemDetail& item) const {
    ts << QStringLiteral("<div class=\"header\">\n");
    ts << QStringLiteral("<h2>") << escapeHtml(item.subject) << QStringLiteral("</h2>\n");

    if (!item.sender_name.isEmpty() || !item.sender_email.isEmpty()) {
        ts << QStringLiteral(
            "<div class=\"field\"><span class=\"label\">"
            "From:</span> ");
        ts << escapeHtml(item.sender_name.isEmpty()
                             ? item.sender_email
                             : item.sender_name + " &lt;" + item.sender_email + "&gt;");
        ts << QStringLiteral("</div>\n");
    }
    if (!item.display_to.isEmpty()) {
        ts << QStringLiteral(
                  "<div class=\"field\"><span class=\"label\">"
                  "To:</span> ")
           << escapeHtml(item.display_to) << QStringLiteral("</div>\n");
    }
    if (!item.display_cc.isEmpty()) {
        ts << QStringLiteral(
                  "<div class=\"field\"><span class=\"label\">"
                  "Cc:</span> ")
           << escapeHtml(item.display_cc) << QStringLiteral("</div>\n");
    }
    if (item.date.isValid()) {
        ts << QStringLiteral(
                  "<div class=\"field\"><span class=\"label\">"
                  "Date:</span> ")
           << escapeHtml(item.date.toString(Qt::RFC2822Date)) << QStringLiteral("</div>\n");
    }
    ts << QStringLiteral("</div>\n");
}

QString HtmlEmailWriter::buildHtmlPage(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const {
    QString html;
    QTextStream ts(&html);

    ts << QStringLiteral("<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n");
    ts << QStringLiteral("<meta charset=\"utf-8\">\n");
    ts << QStringLiteral(
        "<meta name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1\">\n");
    ts << QStringLiteral("<title>") << escapeHtml(item.subject) << QStringLiteral("</title>\n");

    // Inline CSS
    ts << QStringLiteral("<style>\n");
    ts << QStringLiteral(
        "body { font-family: 'Segoe UI', sans-serif; "
        "max-width: 800px; margin: 24px auto; "
        "padding: 0 16px; color: #333; }\n");
    ts << QStringLiteral(
        ".header { border-bottom: 2px solid #0078D4; "
        "padding-bottom: 12px; margin-bottom: 16px; }\n");
    ts << QStringLiteral(".field { margin: 4px 0; }\n");
    ts << QStringLiteral(".label { font-weight: bold; color: #555; }\n");
    ts << QStringLiteral(".body { margin-top: 16px; }\n");
    ts << QStringLiteral(
        ".attachments { margin-top: 24px; "
        "border-top: 1px solid #ddd; padding-top: 12px; }\n");
    ts << QStringLiteral(".att-item { margin: 4px 0; }\n");
    ts << QStringLiteral("img.embedded { max-width: 100%%; height: auto; }\n");
    ts << QStringLiteral("</style>\n");

    ts << QStringLiteral("</head>\n<body>\n");

    buildHtmlHeaderFields(ts, item);

    // Body section
    ts << QStringLiteral("<div class=\"body\">\n");
    if (!item.body_html.isEmpty()) {
        // Embed images inline using data URIs
        QString body_html = item.body_html;
        for (const auto& [name, data] : attachments) {
            QString mime = detectImageMime(data);
            if (!mime.isEmpty()) {
                QString data_uri = QStringLiteral("data:") + mime + QStringLiteral(";base64,") +
                                   QString::fromLatin1(data.toBase64());
                // Replace CID references
                body_html.replace(QStringLiteral("cid:") + name, data_uri);
            }
        }
        ts << body_html;
    } else {
        ts << QStringLiteral("<pre>") << escapeHtml(item.body_plain) << QStringLiteral("</pre>\n");
    }
    ts << QStringLiteral("</div>\n");

    // Attachments list
    if (!attachments.isEmpty()) {
        ts << QStringLiteral("<div class=\"attachments\">\n");
        ts << QStringLiteral("<h3>Attachments</h3>\n");
        for (const auto& [name, data] : attachments) {
            ts << QStringLiteral("<div class=\"att-item\">") << escapeHtml(name)
               << QStringLiteral(" (") << QString::number(data.size())
               << QStringLiteral(" bytes)</div>\n");
        }
        ts << QStringLiteral("</div>\n");
    }

    ts << QStringLiteral("</body>\n</html>\n");
    return html;
}

// ============================================================================
// Helpers
// ============================================================================

QString HtmlEmailWriter::sanitizeFilename(const QString& subject, const QDateTime& date) const {
    QString base = subject.trimmed();
    if (base.isEmpty()) {
        base = QStringLiteral("no_subject");
    }

    static const QRegularExpression kInvalid(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]"));
    base.replace(kInvalid, QStringLiteral("_"));

    if (base.size() > kMaxFilenameLength) {
        base.truncate(kMaxFilenameLength);
    }

    if (m_prefix_with_date && date.isValid()) {
        base = date.toString(QStringLiteral("yyyy-MM-dd_")) + base;
    }

    return base + QStringLiteral(".html");
}

}  // namespace sak
