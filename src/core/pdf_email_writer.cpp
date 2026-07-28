// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pdf_email_writer.cpp
/// @brief Renders emails as PDF documents using QTextDocument + QPdfWriter

#include "sak/pdf_email_writer.h"

#include "sak/report_style_constants.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextDocument>

#include <algorithm>

namespace sak {

namespace {
constexpr qsizetype kPdfHtmlInitialReserveChars = 4096;
}

namespace {
constexpr int kPdfDpi = 300;
constexpr int kPdfMarginMm = 15;
constexpr int kMaxSubjectLength = 80;
}  // namespace

// ======================================================================
// Construction
// ======================================================================

PdfEmailWriter::PdfEmailWriter(const QString& output_dir,
                               bool prefix_with_date,
                               bool preserve_folders)
    : m_output_dir(output_dir)
    , m_prefix_with_date(prefix_with_date)
    , m_preserve_folders(preserve_folders) {}

// ======================================================================
// Public API
// ======================================================================

std::expected<QString, error_code> PdfEmailWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& subfolder_path) {
    QString target_dir = m_output_dir;
    if (m_preserve_folders && !subfolder_path.isEmpty()) {
        target_dir = m_output_dir + QStringLiteral("/") + subfolder_path;
    }

    QDir dir(target_dir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return std::unexpected(error_code::write_error);
    }

    QString filename = sanitizeFilename(item.subject, item.date);

    // De-duplicate by re-checking existence (mirrors EmlWriter). The old counter-only scheme never
    // verified the "_N" candidate was free, so a crafted subject could overwrite a distinct earlier
    // message's .pdf whose name collided.
    QString full_path = target_dir + QStringLiteral("/") + filename;
    if (QFile::exists(full_path)) {
        const QString base = QFileInfo(filename).completeBaseName();
        int& counter = m_filename_counters[target_dir + QStringLiteral("/") + base];
        do {
            ++counter;
            filename = base + QStringLiteral("_%1.pdf").arg(counter);
            full_path = target_dir + QStringLiteral("/") + filename;
        } while (QFile::exists(full_path));
    }

    // Build HTML content for the PDF
    QString html_content = buildHtmlForPdf(item, attachment_data);

    // Render via QPdfWriter into a QSaveFile so a locked/read-only destination
    // fails loudly instead of leaving a stale file that exists()-based success
    // would report as a fresh export.
    QSaveFile out_file(full_path);
    if (!out_file.open(QIODevice::WriteOnly)) {
        return std::unexpected(error_code::write_error);
    }
    {
        QPdfWriter writer(&out_file);
        QPageLayout layout(QPageSize(QPageSize::Letter),
                           QPageLayout::Portrait,
                           QMarginsF(kPdfMarginMm, kPdfMarginMm, kPdfMarginMm, kPdfMarginMm),
                           QPageLayout::Millimeter);
        writer.setPageLayout(layout);
        writer.setResolution(kPdfDpi);
        writer.setTitle(item.subject);
        writer.setCreator(QStringLiteral("S.A.K. Utility"));

        QTextDocument doc;
        doc.setHtml(html_content);
        doc.setPageSize(QSizeF(writer.width(), writer.height()));
        doc.print(&writer);
    }  // writer destroyed here: the PDF is fully flushed to out_file before commit

    if (!out_file.commit()) {
        return std::unexpected(error_code::write_error);
    }

    QFileInfo fi(full_path);
    if (fi.size() == 0) {
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += fi.size();
    return full_path;
}

// ======================================================================
// Private helpers
// ======================================================================

QString PdfEmailWriter::buildHtmlForPdf(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const {
    QString html;
    html.reserve(kPdfHtmlInitialReserveChars);

    html += QString::fromLatin1(report::kPdfEmailDocumentOpen).arg(report::pdfEmailStyleSheet());

    // Header table
    html += QStringLiteral("<table class='hdr'>");

    auto addRow = [&](const QString& label, const QString& value) {
        if (!value.isEmpty()) {
            html += QStringLiteral("<tr><td class='lbl'>") + label.toHtmlEscaped() +
                    QStringLiteral(":</td><td>") + value.toHtmlEscaped() +
                    QStringLiteral("</td></tr>");
        }
    };

    addRow(QStringLiteral("From"),
           item.sender_name + QStringLiteral(" <") + item.sender_email + QStringLiteral(">"));
    addRow(QStringLiteral("To"), item.display_to);
    addRow(QStringLiteral("Cc"), item.display_cc);
    addRow(QStringLiteral("Date"), item.date.toString(Qt::RFC2822Date));
    addRow(QStringLiteral("Subject"), item.subject);

    html += QStringLiteral("</table><hr>");

    // Body
    if (!item.body_html.isEmpty()) {
        // Strip outer html/head/body tags if present and use inner content
        QString body = item.body_html;
        int body_start = body.indexOf(QStringLiteral("<body"), Qt::CaseInsensitive);
        if (body_start >= 0) {
            int close = body.indexOf(QStringLiteral(">"), body_start);
            if (close >= 0) {
                int body_end = body.indexOf(QStringLiteral("</body>"), Qt::CaseInsensitive);
                if (body_end > close) {
                    body = body.mid(close + 1, body_end - close - 1);
                }
            }
        }
        html += body;
    } else if (!item.body_plain.isEmpty()) {
        html += QStringLiteral("<pre>") + item.body_plain.toHtmlEscaped() +
                QStringLiteral("</pre>");
    }

    // Attachment list (names only -- not embedded in PDF)
    if (!attachments.isEmpty()) {
        html += QStringLiteral("<div class='att'><b>Attachments:</b><ul>");
        for (const auto& att : attachments) {
            html += QStringLiteral("<li>") + att.first.toHtmlEscaped() + QStringLiteral("</li>");
        }
        html += QStringLiteral("</ul></div>");
    }

    html += QStringLiteral("</body></html>");
    return html;
}

QString PdfEmailWriter::sanitizeFilename(const QString& subject, const QDateTime& date) const {
    QString safe = subject.left(kMaxSubjectLength);
    safe.replace(QRegularExpression(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1F]")),
                 QStringLiteral("_"));
    safe = safe.trimmed();
    if (safe.isEmpty()) {
        safe = QStringLiteral("untitled");
    }

    if (m_prefix_with_date && date.isValid()) {
        safe = date.toString(QStringLiteral("yyyy-MM-dd_HHmmss")) + QStringLiteral("_") + safe;
    }

    return safe + QStringLiteral(".pdf");
}

}  // namespace sak
