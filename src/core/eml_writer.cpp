// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file eml_writer.cpp
/// @brief RFC 5322 MIME .eml file writer implementation

#include "sak/eml_writer.h"

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/io_write_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/ost_converter_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>

#ifdef Q_OS_WIN
#include <vector>

#include <windows.h>
#endif

namespace sak {

namespace {
constexpr qsizetype kEmlInitialReserveBytes = 4096;
constexpr qsizetype kMimeBase64LineLength = 76;
constexpr ushort kAsciiMaxCodePoint = 0x7F;
constexpr int kBoundaryAttempts = 8;
// Byte length of a CRLF ("\r\n") line terminator; reserve slack for the final wrapped line.
constexpr qsizetype kCrlfLength = 2;
// First code point that is not a C0 control character (space, U+0020); characters below it are
// dropped from a header value.
constexpr ushort kFirstNonControlCodePoint = 0x20;

// True when any character is outside 7-bit ASCII (needs RFC 2047 header encoding).
bool hasNonAscii(const QString& value) {
    return std::ranges::any_of(value, [](QChar c) { return c.unicode() > kAsciiMaxCodePoint; });
}

// Encode bytes as base64 wrapped to RFC 2045 76-column lines. A single unbroken
// base64 run is rejected by strict MIME readers; each emitted line ends with CRLF.
QByteArray base64Wrapped(const QByteArray& data) {
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    out.reserve(b64.size() + (b64.size() / kMimeBase64LineLength) + kCrlfLength);
    for (qsizetype i = 0; i < b64.size(); i += kMimeBase64LineLength) {
        out.append(b64.mid(i, kMimeBase64LineLength));
        out.append("\r\n");
    }
    return out;
}

// Normalize a body's line endings to CRLF (RFC 5322 requires CRLF; a lone LF or CR
// from the source store would otherwise be emitted verbatim and split lines wrong).
QByteArray bodyToCrlf(const QString& body) {
    QString norm = body;
    norm.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    norm.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    norm.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    return norm.toUtf8();
}

// Remove header-injection vectors from a header value: CR/LF (which would start a
// forged header or the body) become spaces, and other C0 control characters are
// dropped. RFC 5322 unstructured header values are single logical lines.
QString sanitizeHeaderValue(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n') || ch == QLatin1Char('\t')) {
            out.append(QLatin1Char(' '));
        } else if (ch.unicode() >= kFirstNonControlCodePoint) {
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

// RFC 2047 'B' encoded-word for an unstructured header (e.g. Subject). Raw UTF-8 in
// a header is non-conformant and mis-decoded by RFC 5322 readers lacking RFC 6532.
QByteArray rfc2047Encode(const QString& value) {
    return "=?UTF-8?B?" + sanitizeHeaderValue(value).toUtf8().toBase64() + "?=";
}

// Append an unstructured header, RFC 2047-encoding the value when it has non-ASCII.
void appendEncodedHeader(QByteArray& eml, const char* name, const QString& value) {
    if (value.isEmpty()) {
        return;
    }
    eml.append(name);
    eml.append(": ");
    eml.append(hasNonAscii(value) ? rfc2047Encode(value) : sanitizeHeaderValue(value).toUtf8());
    eml.append("\r\n");
}

// Encode an RFC 5322 display-name phrase: RFC 2047 encoded-word when non-ASCII
// (an encoded-word MUST NOT appear inside the addr-spec, so only the name is
// encoded), else the raw name.
QString encodedDisplayName(const QString& name) {
    return hasNonAscii(name) ? QString::fromLatin1(rfc2047Encode(name)) : name;
}

void appendStandardHeaders(QByteArray& eml, const PstItemDetail& item) {
    const QString from = item.sender_email.isEmpty() ? encodedDisplayName(item.sender_name)
                                                     : encodedDisplayName(item.sender_name) + " <" +
                                                           item.sender_email + ">";
    appendHeader(eml, "From", from);
    appendHeader(eml, "To", item.display_to);
    appendHeader(eml, "Cc", item.display_cc);
    appendEncodedHeader(eml, "Subject", item.subject);
    appendHeader(eml, "Message-ID", item.message_id);
    appendHeader(eml, "In-Reply-To", item.in_reply_to);
    if (item.date.isValid()) {
        appendHeader(eml, "Date", item.date.toString(Qt::RFC2822Date));
    }
}

// Produce a high-entropy multipart boundary verified not to occur in either body,
// so no body content can be mistaken for a delimiter (a fixed qHash-derived value
// could collide). Empty on the astronomically unlikely failure to find a free one.
QByteArray emlBoundary(const PstItemDetail& item) {
    auto* rng = QRandomGenerator::global();
    for (int attempt = 0; attempt < kBoundaryAttempts; ++attempt) {
        const QByteArray candidate = "----=_SAK_Part_" +
                                     QByteArray::number(rng->generate64(), kHexBase) + "_" +
                                     QByteArray::number(rng->generate64(), kHexBase);
        const QString needle = QString::fromLatin1(candidate);
        if (!item.body_plain.contains(needle) && !item.body_html.contains(needle)) {
            return candidate;
        }
    }
    return {};
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
    eml.append(bodyToCrlf(body));
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
    eml.append(bodyToCrlf(body));
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
        eml.append(base64Wrapped(data));  // ends with CRLF
    }
}

// Emit a multipart body. Returns false when no collision-free boundary could be
// produced, so the caller fails closed rather than emit a corruptible structure.
[[nodiscard]] bool appendMultipartMessage(QByteArray& eml,
                                          const PstItemDetail& item,
                                          const QVector<QPair<QString, QByteArray>>& attachments) {
    const QByteArray boundary = emlBoundary(item);
    if (boundary.isEmpty()) {
        return false;
    }
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
    return true;
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

// Resolve the write target under the output root: append the subfolder when preserving folders,
// reject a lexical escape, create the directory tree, and reject a junction/symlink escape of the
// created target. Returns the target directory, or the error the caller must propagate. Extracted
// from writeMessage to stay under the complexity gate; the checks and their order are unchanged.
std::expected<QString, error_code> resolveTargetDir(const QString& output_dir,
                                                    bool preserve_folders,
                                                    const QString& subfolder_path) {
    QString target_dir = output_dir;
    if (preserve_folders && !subfolder_path.isEmpty()) {
        target_dir = output_dir + "/" + subfolder_path;
        if (subfolderEscapes(output_dir, target_dir)) {
            logError("EmlWriter: subfolder path escapes output dir: {}",
                     subfolder_path.toStdString());
            return std::unexpected(error_code::path_traversal_attempt);
        }
    }

    const QDir dir;
    if (!dir.mkpath(target_dir)) {
        logError("EmlWriter: failed to create directory: {}", target_dir.toStdString());
        return std::unexpected(error_code::write_error);
    }

    // The lexical check above is prefix-only; a junction/symlink pre-planted under the output dir
    // would redirect the created target elsewhere while still passing it. Resolve the target
    // through every reparse point and fail closed if its real location leaves the output root.
    if (preserve_folders && !subfolder_path.isEmpty() &&
        !targetWithinRoot(output_dir, target_dir)) {
        logError("EmlWriter: target dir resolves outside output dir (junction/symlink): {}",
                 target_dir.toStdString());
        return std::unexpected(error_code::path_traversal_attempt);
    }
    return target_dir;
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
    const auto target = resolveTargetDir(m_output_dir, m_preserve_folders, subfolder_path);
    if (!target.has_value()) {
        return std::unexpected(target.error());
    }
    const QString& target_dir = target.value();

    const QString filename = sanitizeFilename(item.subject, item.date);
    const QString full_path = resolveCollisionPath(target_dir, filename);

    auto content = buildEmlContent(item, attachment_data);
    if (!content.has_value()) {
        logError("EmlWriter: failed to build MIME content for: {}", full_path.toStdString());
        return std::unexpected(content.error());
    }

    // QSaveFile writes to a temp sibling and atomically renames on commit(), so a
    // write/flush/close error never leaves a truncated .eml the caller trusts.
    QSaveFile file(full_path);
    if (!file.open(QIODevice::WriteOnly)) {
        logError("EmlWriter: failed to open file for writing: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }
    if (!writeFully(file, content.value()) || !file.commit()) {
        logError("EmlWriter: incomplete write to: {}", full_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += content.value().size();
    return full_path;
}

// Resolve collisions. Keep incrementing until the suffixed candidate is actually
// free: a single _(n) attempt could still collide (e.g. a message whose subject
// already ends in "_(1)", or a file left by a prior run), which would silently
// overwrite a distinct earlier message.
QString EmlWriter::resolveCollisionPath(const QString& target_dir, const QString& filename) {
    QString full_path = target_dir + "/" + filename + ".eml";
    if (QFile::exists(full_path)) {
        int& counter = m_filename_counters[target_dir + "/" + filename];
        do {
            ++counter;
            full_path = target_dir + "/" + filename + "_(" + QString::number(counter) + ").eml";
        } while (QFile::exists(full_path));
    }
    return full_path;
}

// ============================================================================
// Content Building
// ============================================================================

std::expected<QByteArray, error_code> EmlWriter::buildEmlContent(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) const {
    QByteArray eml;
    eml.reserve(kEmlInitialReserveBytes);

    appendStandardHeaders(eml, item);
    if (attachments.isEmpty() && item.body_html.isEmpty()) {
        appendSimplePlainMessage(eml, item.body_plain);
        return eml;
    }
    if (!appendMultipartMessage(eml, item, attachments)) {
        return std::unexpected(error_code::internal_error);
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
