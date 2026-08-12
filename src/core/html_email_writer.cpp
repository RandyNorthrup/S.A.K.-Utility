// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file html_email_writer.cpp
/// @brief HTML email page writer implementation

#include "sak/html_email_writer.h"

#include "sak/email_attachment_saver.h"
#include "sak/email_html_sanitizer.h"
#include "sak/io_write_utils.h"
#include "sak/logger.h"
#include "sak/ost_converter_constants.h"
#include "sak/report_style_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <vector>

#include <windows.h>
#endif

namespace sak {

namespace {

constexpr int kMaxFilenameLength = 200;
constexpr qsizetype kWebpRiffMinimumBytes = 8;
constexpr qsizetype kWebpSignatureOffset = 8;
constexpr qsizetype kWebpSignatureLength = 4;

/// Content-ID of the attachment whose filename matches `name`, or empty if none.
/// HTML inline images reference cid:<Content-ID>, not cid:<filename>, so the
/// embedder needs the real Content-ID to substitute the data URI (B7-34).
QString contentIdForAttachmentName(const PstItemDetail& item, const QString& name) {
    for (const auto& att : item.attachments) {
        const QString att_name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        if (att_name == name && !att.content_id.isEmpty()) {
            return att.content_id;
        }
    }
    return QString();
}

/// Sanitize the untrusted email HTML, then substitute any cid: inline-image
/// references (by Content-ID, filename fallback) with embedded data URIs.
QString embedInlineImages(const PstItemDetail& item,
                          const QVector<QPair<QString, QByteArray>>& attachments);

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
    if (data.startsWith("RIFF") && data.size() > kWebpRiffMinimumBytes &&
        data.mid(kWebpSignatureOffset, kWebpSignatureLength) == "WEBP") {
        return QStringLiteral("image/webp");
    }
    return QString();
}

QString embedInlineImages(const PstItemDetail& item,
                          const QVector<QPair<QString, QByteArray>>& attachments) {
    // Strip active content (script/handlers/js: URIs/framing tags) before writing;
    // the page CSP is the hard backstop for anything that slips past.
    QString body_html = sanitizeEmailBodyHtml(item.body_html);
    for (const auto& [name, data] : attachments) {
        const QString mime = detectImageMime(data);
        if (mime.isEmpty()) {
            continue;
        }
        const QString data_uri = QStringLiteral("data:") + mime + QStringLiteral(";base64,") +
                                 QString::fromLatin1(data.toBase64());
        // Primary: replace by the attachment's real Content-ID (how inline images are
        // actually referenced); fall back to the filename for malformed messages.
        const QString cid = contentIdForAttachmentName(item, name);
        if (!cid.isEmpty()) {
            body_html.replace(QStringLiteral("cid:") + cid, data_uri);
        }
        // An empty attachment name makes the key exactly "cid:", turning this into a
        // replace-ALL that overwrites every other inline reference with this image;
        // skip the filename fallback when there is no name to key on.
        if (!name.isEmpty()) {
            body_html.replace(QStringLiteral("cid:") + name, data_uri);
        }
    }
    return body_html;
}

// Reject a subfolder_path that escapes the output directory (a ".." traversal or an
// absolute/UNC root). Defense-in-depth: current callers sanitize, but the writer
// must not trust raw input and quietly write outside its tree.
bool subfolderEscapes(const QString& output_dir, const QString& target_dir) {
    const QString base = QDir::cleanPath(output_dir);
    const QString resolved = QDir::cleanPath(target_dir);
    return resolved != base && !resolved.startsWith(base + QLatin1Char('/'));
}

#ifdef Q_OS_WIN
// wchar_t capacity for the GetFinalPathNameByHandleW output buffer.
constexpr int kFinalPathBufferChars = 4096;
// Length of the Win32 "\\?\" extended-length path prefix stripped from a resolved path.
constexpr int kWin32ExtendedPrefixLength = 4;

// Fully resolve an EXISTING path through every reparse point (junction AND symlink) to its real
// on-disk location via GetFinalPathNameByHandleW. QFileInfo::canonicalFilePath does NOT follow
// directory junctions on Windows, so it cannot detect a junction-based escape. Returns a
// forward-slash path with the \\?\ prefix stripped, or empty if the path cannot be opened.
QString realCanonicalPath(const QString& path) {
    HANDLE handle = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()),
                                0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,  // required to open a directory handle
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<wchar_t> buffer(kFinalPathBufferChars);
    const DWORD written = GetFinalPathNameByHandleW(
        handle, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    QString result = QString::fromWCharArray(buffer.data(), static_cast<int>(written));
    if (result.startsWith(QStringLiteral("\\\\?\\"))) {
        result = result.mid(kWin32ExtendedPrefixLength);
    }
    return QDir::fromNativeSeparators(result);
}
#else
QString realCanonicalPath(const QString& path) {
    return QFileInfo(path).canonicalFilePath();
}
#endif

// Deepest existing ancestor directory of @p abs_path (which itself may not exist yet), or empty if
// none of its ancestors exist. A junction/symlink can only redirect through an EXISTING reparse
// point, so this is the component that must be canonicalized for a real containment check.
QString deepestExistingAncestor(const QString& abs_path) {
    QString existing = abs_path;
    while (!existing.isEmpty() && !QFileInfo::exists(existing)) {
        const QString parent = QFileInfo(existing).path();
        if (parent == existing) {
            return {};  // reached the filesystem root without finding an existing component
        }
        existing = parent;
    }
    return existing;
}

// True iff @p target_dir's real on-disk location -- junctions/symlinks resolved -- stays inside
// the real @p output_dir. The lexical subfolderEscapes check above collapses "..", but a junction
// pre-planted under the output dir would pass a textual prefix check while redirecting the write
// elsewhere. Mirrors EmailProfileManager::destinationWithinRoot.
bool targetWithinRoot(const QString& output_dir, const QString& target_dir) {
    QString root_real = realCanonicalPath(output_dir);
    if (root_real.isEmpty()) {
        root_real = QDir::cleanPath(QDir(output_dir).absolutePath());
    }
    const QString abs = QDir::cleanPath(QFileInfo(target_dir).absoluteFilePath());
    const QString existing = deepestExistingAncestor(abs);
    if (existing.isEmpty()) {
        return false;
    }
    const QString existing_real = realCanonicalPath(existing);
    if (existing_real.isEmpty()) {
        return false;
    }
    QString root_slash = root_real;
    if (!root_slash.endsWith(QLatin1Char('/'))) {
        root_slash += QLatin1Char('/');
    }
    return existing_real.compare(root_real, Qt::CaseInsensitive) == 0 ||
           (existing_real + QLatin1Char('/')).startsWith(root_slash, Qt::CaseInsensitive);
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

std::expected<QString, error_code> HtmlEmailWriter::resolveMessageDirectory(
    const QString& subfolder_path) const {
    // Fail closed on an unusable output root: an empty or non-absolute root would
    // resolve against the process CWD or a drive root and, running elevated, drop
    // exported messages in an unintended place. A directory picker always yields an
    // absolute path, so this only ever rejects a malformed/missing root.
    if (m_output_dir.trimmed().isEmpty() || !QFileInfo(m_output_dir).isAbsolute()) {
        logError("HtmlEmailWriter: output dir empty or not absolute: {}",
                 m_output_dir.toStdString());
        return std::unexpected(error_code::write_error);
    }
    QString dir_path = m_output_dir;
    if (m_preserve_folders && !subfolder_path.isEmpty()) {
        dir_path += QStringLiteral("/") + subfolder_path;
        if (subfolderEscapes(m_output_dir, dir_path)) {
            logError("HtmlEmailWriter: subfolder path escapes output dir: {}",
                     subfolder_path.toStdString());
            return std::unexpected(error_code::path_traversal_attempt);
        }
    }
    QDir().mkpath(dir_path);
    // The lexical subfolderEscapes check is prefix-only; a junction/symlink pre-planted under the
    // output dir would redirect the created target elsewhere while still passing it. Resolve the
    // target through every reparse point and fail closed if its real location leaves the root.
    if (m_preserve_folders && !subfolder_path.isEmpty() &&
        !targetWithinRoot(m_output_dir, dir_path)) {
        logError("HtmlEmailWriter: target dir resolves outside output dir (junction/symlink): {}",
                 dir_path.toStdString());
        return std::unexpected(error_code::path_traversal_attempt);
    }
    return dir_path;
}

std::expected<QString, error_code> HtmlEmailWriter::writeMessage(
    const PstItemDetail& item,
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& subfolder_path) {
    const std::expected<QString, error_code> dir_result = resolveMessageDirectory(subfolder_path);
    if (!dir_result) {
        return std::unexpected(dir_result.error());
    }
    const QString dir_path = *dir_result;

    QString filename = sanitizeFilename(item.subject, item.date);

    // Resolve collisions like EmlWriter: keep incrementing until the suffixed candidate is actually
    // FREE on disk. The old counter-only scheme never re-checked existence, so a crafted subject
    // (e.g. "X" appearing twice plus a distinct "X_2") could make the generated "_N" name collide
    // with an already-written message and silently overwrite it.
    QString full_path = dir_path + QStringLiteral("/") + filename;
    if (QFile::exists(full_path)) {
        const QString base = QFileInfo(filename).completeBaseName();
        int& counter = m_filename_counters[dir_path + QStringLiteral("/") + base];
        do {
            ++counter;
            filename = base + QStringLiteral("_%1.html").arg(counter);
            full_path = dir_path + QStringLiteral("/") + filename;
        } while (QFile::exists(full_path));
    }

    if (!saveFileAttachments(attachment_data, dir_path, filename)) {
        // A listed attachment failed to save. The HTML page enumerates every
        // attachment, so emitting it now would advertise files that are not on
        // disk; fail closed instead of reporting a partial message as success.
        logError("HtmlEmailWriter: attachment save failed for: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    // Build and write HTML. QSaveFile writes to a temp sibling and atomically
    // renames on commit(), so a write/flush/close error never leaves a truncated
    // .html the user would trust as a complete message; fail closed on any error.
    const QString html = buildHtmlPage(item, attachment_data);
    QSaveFile file(full_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("HtmlEmailWriter: failed to create: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    const QByteArray content = html.toUtf8();
    if (!writeFully(file, content) || !file.commit()) {
        logError("HtmlEmailWriter: incomplete write to: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }
    m_bytes_written += content.size();

    return full_path;
}

bool HtmlEmailWriter::saveFileAttachments(
    const QVector<QPair<QString, QByteArray>>& attachment_data,
    const QString& dir_path,
    const QString& filename) {
    if (attachment_data.isEmpty()) {
        return true;
    }

    const QFileInfo fi(filename);
    const QString attachments_dir = dir_path + QStringLiteral("/") + fi.completeBaseName() +
                                    QStringLiteral("_files");
    QDir().mkpath(attachments_dir);

    bool all_saved = true;
    for (const auto& [name, data] : attachment_data) {
        // saveAttachmentToDirectory sanitizes the bare filename (QFileInfo::fileName
        // first, so a hostile "../../../pwned.html" cannot escape the _files dir),
        // DEDUPES colliding names (a second "image.png" becomes "image_1.png" instead
        // of truncating the first), and writes via QSaveFile so a short write never
        // leaves a truncated file reported as saved (B7-19).
        const AttachmentSaveResult result =
            saveAttachmentToDirectory(attachments_dir, QFileInfo(name).fileName(), data);
        if (result.success) {
            m_bytes_written += data.size();
        } else {
            // The generated HTML lists this attachment, so a failed write must fail
            // the whole message (fail closed), not just be logged and dropped.
            logError("HtmlEmailWriter: failed to write attachment '{}': {}",
                     name.toStdString(),
                     result.error_message.toStdString());
            all_saved = false;
        }
    }

    return all_saved;
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
        // Build with literal < > and escape ONCE: pre-inserting &lt;/&gt; here made
        // escapeHtml re-escape the ampersands into "&amp;lt;", so the brackets showed
        // as literal text instead of angle brackets (B7-34).
        ts << escapeHtml(item.sender_name.isEmpty()
                             ? item.sender_email
                             : item.sender_name + " <" + item.sender_email + ">");
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
    // The body is untrusted email HTML. A strict CSP is the hard guarantee: default-src 'none'
    // blocks scripts, frames, and ALL remote loads (no tracker/beacon/SSRF on open); img-src data:
    // permits only the inline data-URI images we embed; style-src 'unsafe-inline' allows our own
    // <style> and inline style attributes but no remote CSS. Belt-and-suspenders with the body
    // sanitizer below.
    ts << QStringLiteral(
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
        "img-src data:; style-src 'unsafe-inline'; font-src data:; base-uri 'none'; "
        "form-action 'none'\">\n");
    ts << QStringLiteral(
        "<meta name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1\">\n");
    ts << QStringLiteral("<title>") << escapeHtml(item.subject) << QStringLiteral("</title>\n");

    ts << QString::fromLatin1(report::kHtmlStyleTagOpen);
    ts << report::savedEmailStyleSheet();
    ts << QString::fromLatin1(report::kHtmlStyleTagClose);

    ts << QStringLiteral("</head>\n<body>\n");

    buildHtmlHeaderFields(ts, item);

    // Body section
    ts << QStringLiteral("<div class=\"body\">\n");
    if (!item.body_html.isEmpty()) {
        ts << embedInlineImages(item, attachments);
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
