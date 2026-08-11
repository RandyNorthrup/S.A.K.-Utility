// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file pdf_email_writer.cpp
/// @brief Renders emails as PDF documents using QTextDocument + QPdfWriter

#include "sak/pdf_email_writer.h"

#include "sak/email_html_sanitizer.h"
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
#include <QUrl>
#include <QVariant>

#include <algorithm>

namespace sak {

namespace {
constexpr qsizetype kPdfHtmlInitialReserveChars = 4096;
}

namespace {
constexpr int kPdfDpi = 300;
constexpr int kPdfMarginMm = 15;
constexpr int kMaxSubjectLength = 80;

/// A QTextDocument that refuses to load ANY external resource. An email body's
/// <img src="file:///C:/secret"> (or a relative/UNC/remote ref) would otherwise be resolved by
/// QTextDocument::loadResource and its bytes embedded into the rendered PDF -- a local-file
/// disclosure. Only self-contained data: URIs are allowed through; everything else returns empty,
/// so no disk or network access ever happens during rendering.
class ResourceDenyingTextDocument : public QTextDocument {
public:
    using QTextDocument::QTextDocument;

protected:
    QVariant loadResource(int type, const QUrl& name) override {
        if (name.scheme().compare(QLatin1String("data"), Qt::CaseInsensitive) == 0) {
            return QTextDocument::loadResource(type, name);
        }
        return {};  // deny file://, http(s)://, cid:, absolute/UNC paths -- no resource load
    }
};

/// Reject a subfolder_path that escapes the output directory (a ".." traversal or
/// an absolute/UNC root). Defense-in-depth: current callers sanitize, but the
/// writer must not trust raw input and quietly write outside its tree.
bool subfolderEscapes(const QString& output_dir, const QString& target_dir) {
    const QString base = QDir::cleanPath(output_dir);
    const QString resolved = QDir::cleanPath(target_dir);
    return resolved != base && !resolved.startsWith(base + QLatin1Char('/'));
}

/// Render the message HTML into an open QSaveFile via QPdfWriter/QTextDocument.
void renderPdfDocument(QSaveFile& out_file, const QString& html, const PstItemDetail& item) {
    QPdfWriter writer(&out_file);
    const QPageLayout layout(QPageSize(QPageSize::Letter),
                             QPageLayout::Portrait,
                             QMarginsF(kPdfMarginMm, kPdfMarginMm, kPdfMarginMm, kPdfMarginMm),
                             QPageLayout::Millimeter);
    writer.setPageLayout(layout);
    writer.setResolution(kPdfDpi);
    writer.setTitle(item.subject);
    writer.setCreator(QStringLiteral("S.A.K. Utility"));

    // ResourceDenyingTextDocument refuses external resource loads (no local-file disclosure).
    ResourceDenyingTextDocument doc;
    doc.setHtml(html);
    doc.setPageSize(QSizeF(writer.width(), writer.height()));
    doc.print(&writer);
}

/// A committed nonzero file is not proof of a valid PDF: a partial render can flush
/// a truncated/corrupt file. Verify the %PDF- magic and the trailing %%EOF marker
/// so a structurally-broken PDF is reported failed rather than counted a success.
bool pdfLooksValid(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    constexpr qint64 kTrailerScan = 1024;
    const QByteArray head = file.read(5);
    if (!head.startsWith("%PDF-")) {
        return false;
    }
    const qint64 size = file.size();
    file.seek(std::max<qint64>(0, size - kTrailerScan));
    return file.readAll().contains("%%EOF");
}
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
    const auto target_dir = resolveTargetDirectory(subfolder_path);
    if (!target_dir) {
        return std::unexpected(target_dir.error());
    }

    const QString filename = sanitizeFilename(item.subject, item.date);
    const QString full_path = resolveCollisionPath(*target_dir, filename);

    // Build HTML content for the PDF
    const QString html_content = buildHtmlForPdf(item, attachment_data);

    // Render via QPdfWriter into a QSaveFile so a locked/read-only destination
    // fails loudly instead of leaving a stale file that exists()-based success
    // would report as a fresh export.
    QSaveFile out_file(full_path);
    if (!out_file.open(QIODevice::WriteOnly)) {
        return std::unexpected(error_code::write_error);
    }
    {
        renderPdfDocument(out_file, html_content, item);
    }  // writer destroyed here: PDF fully flushed to out_file before commit

    // resolveCollisionPath() chose a name that was free, but a concurrent writer could
    // have created it since. QSaveFile::commit() replaces an existing target, so fail
    // closed rather than overwrite a raced-in file (narrows the inherent check-then-act
    // window; it never widens it).
    if (QFile::exists(full_path)) {
        return std::unexpected(error_code::file_already_exists);
    }

    if (!out_file.commit()) {
        return std::unexpected(error_code::write_error);
    }

    // Verify the committed file is a structurally-complete PDF, not just nonzero.
    if (!pdfLooksValid(full_path)) {
        QFile::remove(full_path);  // do not leave a corrupt .pdf behind
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += QFileInfo(full_path).size();
    return full_path;
}

// ======================================================================
// Private helpers
// ======================================================================

std::expected<QString, error_code> PdfEmailWriter::resolveTargetDirectory(
    const QString& subfolder_path) const {
    // Fail closed on a blank or relative output directory: it would otherwise resolve
    // against the process current directory / current drive and write the PDF to a
    // guessed location. The export root must be an explicit absolute path.
    if (m_output_dir.isEmpty() || !QDir::isAbsolutePath(m_output_dir)) {
        return std::unexpected(error_code::invalid_path);
    }

    QString target_dir = m_output_dir;
    if (m_preserve_folders && !subfolder_path.isEmpty()) {
        target_dir = m_output_dir + QStringLiteral("/") + subfolder_path;
        if (subfolderEscapes(m_output_dir, target_dir)) {
            return std::unexpected(error_code::path_traversal_attempt);
        }
    }

    const QDir dir(target_dir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return std::unexpected(error_code::write_error);
    }
    return target_dir;
}

// De-duplicate by re-checking existence (mirrors EmlWriter). A counter-only scheme
// never verifies the "_N" candidate is free, so a crafted subject could overwrite a
// distinct earlier message's .pdf whose name collided.
QString PdfEmailWriter::resolveCollisionPath(const QString& target_dir, const QString& filename) {
    QString name = filename;
    QString full_path = target_dir + QStringLiteral("/") + name;
    if (QFile::exists(full_path)) {
        const QString base = QFileInfo(name).completeBaseName();
        int& counter = m_filename_counters[target_dir + QStringLiteral("/") + base];
        do {
            ++counter;
            name = base + QStringLiteral("_%1.pdf").arg(counter);
            full_path = target_dir + QStringLiteral("/") + name;
        } while (QFile::exists(full_path));
    }
    return full_path;
}

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
        // Strip active content from the untrusted body first; loadResource denial (above) is the
        // hard guarantee against local-file disclosure, this removes script/handlers as well.
        QString body = sanitizeEmailBodyHtml(item.body_html);
        // Strip outer html/head/body tags if present and use inner content
        const qsizetype body_start = body.indexOf(QStringLiteral("<body"), Qt::CaseInsensitive);
        if (body_start >= 0) {
            const qsizetype close = body.indexOf(QStringLiteral(">"), body_start);
            if (close >= 0) {
                const qsizetype body_end = body.indexOf(QStringLiteral("</body>"),
                                                        Qt::CaseInsensitive);
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
