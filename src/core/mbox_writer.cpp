// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_writer.cpp
/// @brief Unix mbox file writer implementation

#include "sak/mbox_writer.h"

#include "sak/io_write_utils.h"
#include "sak/logger.h"
#include "sak/partition_export_containment.h"

#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <string>

namespace sak {

namespace {
constexpr qsizetype kMimeBase64LineLength = 76;
constexpr ushort kAsciiMaxCodePoint = 0x7F;
constexpr ushort kAsciiDelCodePoint = 0x7F;
constexpr ushort kAsciiFirstPrintable = 0x20;
// RFC 2047: an encoded-word is at most 75 characters. "=?UTF-8?B?" plus "?=" costs 12 of them and
// base64 turns 3 raw bytes into 4, so 45 raw bytes is the largest chunk that always fits.
constexpr qsizetype kMaxEncodedWordRawBytes = 45;
// RFC 5322 limits a header line to 998 characters. Fold long values at existing spaces well before
// that; a whitespace-free run longer than kMaxUnfoldableRunBytes has no legal fold point and must
// be carried as encoded-words (which may be split anywhere) instead.
constexpr qsizetype kHeaderFoldColumn = 78;
constexpr qsizetype kMaxUnfoldableRunBytes = 900;
// Assembly ceiling for a single message. Base64 encoding, MIME assembly and From_ escaping each
// hold a full copy, so an unbounded body/attachment set from a hostile mail store would exhaust
// memory; refuse it instead of dying mid-export.
constexpr qint64 kMaxMboxEntryBytes = 1LL << 30;  // 1 GiB
constexpr qint64 kUtf8WorstBytesPerUnit = 3;
constexpr qint64 kBase64GrowthNumerator = 4;
constexpr qint64 kBase64GrowthDenominator = 3;
// Mailboxes kept open at once. A mail store can declare far more folders than the process has file
// handles; past this cap the least-recently-used mailbox is closed and reopened (appending) when
// its folder is written again, so the export completes instead of failing on handle exhaustion.
constexpr int kMaxOpenMboxFiles = 64;
constexpr int kImportanceLow = 0;
constexpr int kImportanceHigh = 2;
// Extra capacity reserved for base64Wrapped output beyond data + one separator per line, covering
// the final line's CRLF; reserve is only a hint, so an approximate slack is sufficient.
constexpr qsizetype kBase64ReserveSlackBytes = 2;
// The two enclosing angle brackets ("<" and ">") stripped from a Content-ID value.
constexpr qsizetype kAngleBracketPairLength = 2;
// First numeric suffix used to disambiguate a colliding mbox filename ("base (2).mbox").
constexpr int kFirstDisambiguationSuffix = 2;

// True when any character is outside 7-bit ASCII (needs RFC 2047 header encoding).
bool hasNonAscii(const QString& value) {
    return std::any_of(value.cbegin(), value.cend(), [](QChar c) {
        return c.unicode() > kAsciiMaxCodePoint;
    });
}

// Strip header-injection vectors from a header value: CR/LF/tab collapse to a
// single space and other C0 control characters (and DEL) are dropped, so a value
// cannot forge a second header or terminate the header block.
QString sanitizeHeaderValue(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n') || ch == QLatin1Char('\t')) {
            out.append(QLatin1Char(' '));
        } else if (ch.unicode() >= kAsciiFirstPrintable && ch.unicode() != kAsciiDelCodePoint) {
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

// RFC 2047 'B' encoded-words for an unstructured header value with non-ASCII bytes;
// raw UTF-8 in a header is non-conformant and mis-decoded by RFC 5322 readers. A value
// too long for one 75-character encoded-word becomes several words on folded
// continuation lines, split only on whole code points so no character is cut in half.
QByteArray rfc2047Encode(const QString& value) {
    const QString clean = sanitizeHeaderValue(value);
    QByteArray out;
    QByteArray chunk;
    const auto flush_chunk = [&out, &chunk]() {
        if (chunk.isEmpty()) {
            return;
        }
        if (!out.isEmpty()) {
            out.append("\r\n ");
        }
        out.append("=?UTF-8?B?" + chunk.toBase64() + "?=");
        chunk.clear();
    };
    for (qsizetype i = 0; i < clean.size();) {
        const bool is_pair = clean.at(i).isHighSurrogate() && i + 1 < clean.size() &&
                             clean.at(i + 1).isLowSurrogate();
        const qsizetype units = is_pair ? 2 : 1;
        const QByteArray piece = clean.mid(i, units).toUtf8();
        if (chunk.size() + piece.size() > kMaxEncodedWordRawBytes) {
            flush_chunk();
        }
        chunk.append(piece);
        i += units;
    }
    flush_chunk();
    return out.isEmpty() ? QByteArray("=?UTF-8?B??=") : out;
}

// True when @p value contains a whitespace-free run too long to fold inside an RFC 5322 line.
bool hasUnfoldableRun(const QByteArray& value) {
    qsizetype run = 0;
    for (const char ch : value) {
        run = (ch == ' ') ? 0 : run + 1;
        if (run > kMaxUnfoldableRunBytes) {
            return true;
        }
    }
    return false;
}

// Fold an over-long header value at existing spaces (RFC 5322 FWS): the space becomes CRLF+SP, so
// unfolding reproduces the original byte for byte. Short values are returned untouched.
QByteArray foldLongHeaderValue(const QByteArray& value) {
    if (value.size() <= kMaxUnfoldableRunBytes) {
        return value;
    }
    const QList<QByteArray> tokens = value.split(' ');
    QByteArray out;
    qsizetype column = 0;
    for (qsizetype i = 0; i < tokens.size(); ++i) {
        const QByteArray& token = tokens.at(i);
        if (i == 0) {
            out = token;
            column = token.size();
            continue;
        }
        if (column + 1 + token.size() > kHeaderFoldColumn) {
            out.append("\r\n ");
            column = 1 + token.size();
        } else {
            out.append(' ');
            column += 1 + token.size();
        }
        out.append(token);
    }
    return out;
}

// Header value: RFC 2047-encode when non-ASCII or when the value has no legal fold point and would
// otherwise emit an over-long line, else the sanitized (and folded when long) raw UTF-8.
QByteArray encodedHeaderValue(const QString& value) {
    const QString clean = sanitizeHeaderValue(value);
    const QByteArray raw = clean.toUtf8();
    if (hasNonAscii(clean) || hasUnfoldableRun(raw)) {
        return rfc2047Encode(clean);
    }
    return foldLongHeaderValue(raw);
}

// RFC 5322 specials: unquoted in a display-name phrase they end the phrase, so a name carrying
// them could forge a second mailbox in the address list.
bool needsQuotedPhrase(const QString& name) {
    static const QString kSpecials = QStringLiteral("()<>[]:;@\\,.\"");
    return std::any_of(name.cbegin(), name.cend(), [](QChar c) { return kSpecials.contains(c); });
}

// Encode an RFC 5322 display-name phrase (an encoded-word MUST NOT appear inside
// the addr-spec, so only the name is encoded); an ASCII name holding specials is
// carried as a quoted-string rather than emitted raw.
QString encodedDisplayName(const QString& name) {
    if (hasNonAscii(name)) {
        return QString::fromLatin1(rfc2047Encode(name));
    }
    const QString clean = sanitizeHeaderValue(name);
    if (!needsQuotedPhrase(clean)) {
        return clean;
    }
    return QStringLiteral("\"") + sanitizeQuotedParam(clean) + QStringLiteral("\"");
}

// Encode bytes as base64 wrapped to RFC 2045 76-column lines (a single unbroken
// base64 run is rejected by strict MIME readers); each line ends with CRLF.
QByteArray base64Wrapped(const QByteArray& data) {
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    out.reserve(b64.size() + (b64.size() / kMimeBase64LineLength) + kBase64ReserveSlackBytes);
    for (qsizetype i = 0; i < b64.size(); i += kMimeBase64LineLength) {
        out.append(b64.mid(i, kMimeBase64LineLength));
        out.append("\r\n");
    }
    return out;
}

// Windows resolves "Inbox.mbox" and "inbox.mbox" to the SAME file, so path identity -- for
// collision checks and for "already truncated this run" -- must be case-folded.
QString pathIdentity(const QString& path) {
    return path.toCaseFolded();
}

// DOS device names address a DEVICE, not a file, even with an extension: opening "NUL.mbox"
// succeeds and every message written to it is silently discarded.
bool isReservedDeviceName(const QString& name) {
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    const QString stem = name.section(QLatin1Char('.'), 0, 0).trimmed();
    return kReserved.contains(stem, Qt::CaseInsensitive);
}

// Only a well-formed RFC 2045 type/subtype is carried through; anything else becomes the
// MIME-defined unknown type rather than injecting foreign text into the header.
QByteArray sanitizedMimeType(const QString& mime_type) {
    static const QRegularExpression kType(
        QStringLiteral("^[A-Za-z0-9!#$%&'*+._-]+/[A-Za-z0-9!#$%&'*+._-]+$"));
    return kType.match(mime_type).hasMatch() ? mime_type.toLatin1()
                                             : QByteArray("application/octet-stream");
}

// A Content-ID is an addr-spec-like token inside <>; a value with spaces, brackets or controls
// would break the header, so an unusable one is dropped rather than emitted.
QByteArray sanitizedContentId(const QString& content_id) {
    QString id = content_id;
    if (id.startsWith(QLatin1Char('<')) && id.endsWith(QLatin1Char('>')) && id.size() > 1) {
        id = id.mid(1, id.size() - kAngleBracketPairLength);
    }
    static const QRegularExpression kCid(QStringLiteral("^[A-Za-z0-9!#$%&'*+/=?^_`{|}~.@-]+$"));
    return kCid.match(id).hasMatch() ? id.toLatin1() : QByteArray();
}

// Metadata the source declared for the attachment written under @p name. The caller names an
// attachment by its long filename when it has one, so match the same way.
const PstAttachmentInfo* findAttachmentInfo(const PstItemDetail& item, const QString& name) {
    for (const auto& att : item.attachments) {
        const QString declared = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        if (declared == name) {
            return &att;
        }
    }
    return nullptr;
}

// filename="..." is ASCII-only: a non-ASCII name is emitted RFC 2231 style so strict readers
// neither reject the part nor mis-decode the name.
void appendFilenameParam(QByteArray& message, const QString& name) {
    const QString clean = sanitizeHeaderValue(name);
    if (clean.isEmpty()) {
        return;
    }
    if (hasNonAscii(clean)) {
        message.append("; filename*=UTF-8''" + QUrl::toPercentEncoding(clean));
    } else {
        message.append("; filename=\"" + sanitizeQuotedParam(clean).toUtf8() + "\"");
    }
}

// One text body part. The body is emitted raw, labelled 8bit (mbox bodies are 8-bit clean);
// quoted-printable would make a reader decode "=XX" sequences and swallow trailing '='.
void appendTextPart(QByteArray& message,
                    const QString& boundary,
                    const char* content_type,
                    const QString& body) {
    message.append("--" + boundary.toUtf8() + "\r\n");
    message.append("Content-Type: ");
    message.append(content_type);
    message.append("; charset=utf-8\r\n");
    message.append("Content-Transfer-Encoding: 8bit\r\n");
    message.append("\r\n");
    message.append(body.toUtf8());
    message.append("\r\n");
}

// One attachment part, preserving the declared media type and, for a part the HTML body
// references by cid:, its inline disposition and Content-ID (without them the exported message
// renders with the picture missing).
void appendAttachmentPart(QByteArray& message,
                          const QString& boundary,
                          const PstItemDetail& item,
                          const QString& name,
                          const QByteArray& data) {
    const PstAttachmentInfo* info = findAttachmentInfo(item, name);
    const QByteArray content_type = info ? sanitizedMimeType(info->mime_type)
                                         : QByteArray("application/octet-stream");
    const QByteArray content_id = info ? sanitizedContentId(info->content_id) : QByteArray();

    message.append("--" + boundary.toUtf8() + "\r\n");
    message.append("Content-Type: " + content_type + "\r\n");
    message.append("Content-Disposition: ");
    message.append(content_id.isEmpty() ? "attachment" : "inline");
    appendFilenameParam(message, name);
    message.append("\r\n");
    if (!content_id.isEmpty()) {
        message.append("Content-ID: <" + content_id + ">\r\n");
    }
    message.append("Content-Transfer-Encoding: base64\r\n");
    message.append("\r\n");
    message.append(base64Wrapped(data));  // ends with CRLF
}

// Emit the message bodies. A non-empty @p alt_boundary means the plain/HTML alternatives are
// nested in their own multipart/alternative inside the mixed container that carries attachments.
void appendBodyParts(QByteArray& message,
                     const PstItemDetail& item,
                     const QString& boundary,
                     const QString& alt_boundary) {
    const bool nested = !alt_boundary.isEmpty();
    if (nested) {
        message.append("--" + boundary.toUtf8() + "\r\n");
        message.append("Content-Type: multipart/alternative;\r\n");
        message.append(" boundary=\"" + alt_boundary.toUtf8() + "\"\r\n");
        message.append("\r\n");
    }
    const QString& body_boundary = nested ? alt_boundary : boundary;
    // Emit BOTH bodies when present: dropping the plain-text alternative whenever HTML exists
    // silently loses content the source message carried.
    if (!item.body_plain.isEmpty()) {
        appendTextPart(message, body_boundary, "text/plain", item.body_plain);
    }
    if (!item.body_html.isEmpty()) {
        appendTextPart(message, body_boundary, "text/html", item.body_html);
    }
    if (nested) {
        message.append("--" + alt_boundary.toUtf8() + "--\r\n");
    }
}

void appendImportanceHeader(QByteArray& message, int importance) {
    if (importance == kImportanceLow) {
        message.append("Importance: Low\r\n");
    } else if (importance == kImportanceHigh) {
        message.append("Importance: High\r\n");
    }
}

// Saturating add: a hostile size must not wrap the running total back below the cap.
qint64 addSaturating(qint64 total, qint64 addend) {
    if (addend < 0 || total > kMaxMboxEntryBytes) {
        return kMaxMboxEntryBytes + 1;
    }
    return (addend > kMaxMboxEntryBytes - total) ? kMaxMboxEntryBytes + 1 : total + addend;
}

// Upper bound on the bytes one assembled entry needs, before anything is copied.
qint64 estimatedEntryBytes(const PstItemDetail& item,
                           const QVector<QPair<QString, QByteArray>>& attachments) {
    qint64 total =
        addSaturating(0, static_cast<qint64>(item.body_plain.size()) * kUtf8WorstBytesPerUnit);
    total = addSaturating(total,
                          static_cast<qint64>(item.body_html.size()) * kUtf8WorstBytesPerUnit);
    for (const auto& attachment : attachments) {
        const qint64 encoded = (static_cast<qint64>(attachment.second.size()) /
                                kBase64GrowthDenominator * kBase64GrowthNumerator) +
                               kMimeBase64LineLength;
        total = addSaturating(total, encoded);
    }
    return total;
}

// Fail closed rather than fabricate envelope metadata: the mbox "From_" line and the RFC 5322
// Date are load-bearing, and inventing "unknown@localhost" or the current time would masquerade
// missing data as real (no-fallback rule). A caller that reaches here with a message lacking a
// sender or date must surface that gap.
std::expected<void, error_code> validateEnvelope(const PstItemDetail& item) {
    if (item.sender_email.isEmpty()) {
        logError("MboxWriter: message has no sender address; refusing to fabricate one");
        return std::unexpected(error_code::missing_required_field);
    }
    if (item.sender_email.trimmed().isEmpty() || item.sender_email.contains(QLatin1Char('<')) ||
        item.sender_email.contains(QLatin1Char('>'))) {
        // The addr-spec is emitted between angle brackets and after the "From " envelope keyword;
        // a value carrying its own brackets (or nothing but spaces) would name a different sender
        // than the message did.
        logError("MboxWriter: message sender address is malformed; refusing to rewrite it");
        return std::unexpected(error_code::validation_failed);
    }
    if (!item.date.isValid()) {
        logError("MboxWriter: message has no valid date; refusing to fabricate one");
        return std::unexpected(error_code::missing_required_field);
    }
    return {};
}

// Refuse content this writer cannot carry honestly: a body it cannot decode, or one too large to
// assemble in memory.
std::expected<void, error_code> validateContent(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) {
    if (item.body_plain.isEmpty() && item.body_html.isEmpty() &&
        !item.body_rtf_compressed.isEmpty()) {
        // The compressed RTF (MS-OXRTFCP) is this message's only body and this writer cannot
        // decode it. Writing an empty body would report a message whose content was silently
        // lost, so surface the gap instead.
        logError("MboxWriter: message body is RTF-only; refusing to export an empty body");
        return std::unexpected(error_code::not_implemented);
    }
    if (estimatedEntryBytes(item, attachments) > kMaxMboxEntryBytes) {
        logError("MboxWriter: message exceeds the {} byte assembly limit; refusing to buffer it",
                 std::to_string(kMaxMboxEntryBytes));
        return std::unexpected(error_code::resource_limit_reached);
    }
    return {};
}

// mboxrd: a body line beginning "From " must be escaped, and so must an already-escaped
// ">From " -- a reader strips one '>' on read, so leaving it alone would hand back "From "
// and corrupt content the source message carried.
bool isMboxFromLine(const QByteArray& line) {
    qsizetype i = 0;
    while (i < line.size() && line.at(i) == '>') {
        ++i;
    }
    return line.mid(i).startsWith("From ");
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
    if (m_finalized) {
        // Reopening here would truncate the mailbox this run just finished writing (combined
        // mode) or silently redirect to a suffixed file (per-folder mode); fail closed.
        logError("MboxWriter: write after finalize; refusing to reopen a closed mailbox");
        return std::unexpected(error_code::write_error);
    }
    const QString key = fileKey(folder_path);
    if (m_failed_keys.contains(key)) {
        // An earlier write into this mailbox stopped mid-record: appending more mail would hide
        // the half-written message behind valid-looking records.
        logError("MboxWriter: mailbox left incomplete by an earlier failure: {}",
                 folder_path.toStdString());
        return std::unexpected(error_code::write_error);
    }

    // Build the entry BEFORE the destination is opened: opening truncates, so a message that
    // fails validation must not first cost the previous run's mailbox.
    auto entry = formatMboxEntry(item, attachment_data);
    if (!entry) {
        return std::unexpected(entry.error());
    }

    QFile* file = getOrCreateFile(folder_path);
    if (!file) {
        return std::unexpected(error_code::write_error);
    }
    if (!writeFully(*file, *entry)) {
        // A short write splits one message mid-stream and corrupts the mbox (the
        // next From_ line lands inside the previous body); fail closed.
        logError("MboxWriter: incomplete write for folder: {}", folder_path.toStdString());
        m_failed_keys.insert(key);
        m_failed = true;
        return std::unexpected(error_code::write_error);
    }

    m_bytes_written += entry->size();
    return {};
}

bool MboxWriter::closeOneFile(QFile* file, const QString& key) {
    if (!file) {
        return true;
    }
    bool ok = true;
    if (file->isOpen()) {
        // close() alone discards a deferred error (a full disk only surfaces when the buffered
        // tail is pushed out), which would report a truncated mailbox as a complete one.
        if (!file->flush()) {
            ok = false;
        }
        file->close();
        if (file->error() != QFileDevice::NoError) {
            ok = false;
        }
        if (!ok) {
            logError("MboxWriter: failed to flush/close mailbox for {}: {}",
                     key.toStdString(),
                     file->errorString().toStdString());
        }
    }
    delete file;
    if (!ok) {
        m_failed = true;
    }
    return ok;
}

void MboxWriter::finalize() {
    for (auto it = m_open_files.cbegin(); it != m_open_files.cend(); ++it) {
        if (!closeOneFile(it.value(), it.key())) {
            m_failed_keys.insert(it.key());
        }
    }
    m_open_files.clear();
    m_open_order.clear();
    m_finalized = true;
}

// ============================================================================
// File Management
// ============================================================================

QString MboxWriter::fileKey(const QString& folder_path) const {
    return m_one_per_folder ? folder_path : QStringLiteral("combined");
}

QString MboxWriter::resolveFilePath(const QString& folder_path) const {
    const QString base_name = (m_one_per_folder && !folder_path.isEmpty())
                                  ? sanitizeFolderName(folder_path)
                                  // Namespace the single combined mailbox by source-derived
                                  // basename so distinct jobs sharing one output directory do not
                                  // truncate each other; empty keeps the legacy "mailbox.mbox".
                                  : (m_combined_basename.isEmpty()
                                         ? QStringLiteral("mailbox")
                                         : sanitizeFolderName(m_combined_basename));
    const QString base = m_output_dir + QStringLiteral("/") + base_name;
    // Two distinct folders can sanitize to the same name (e.g. "A_B/C" and "A/B_C" -> "A_B_C"),
    // and "Inbox"/"inbox" ARE one file on Windows; disambiguate with a numeric suffix so their
    // mail does not interleave into one mailbox -- or truncate it twice through two handles.
    QString file_path = base + QStringLiteral(".mbox");
    for (int suffix = kFirstDisambiguationSuffix;
         m_used_file_paths.contains(pathIdentity(file_path));
         ++suffix) {
        file_path = base + QStringLiteral(" (%1).mbox").arg(suffix);
    }
    return file_path;
}

void MboxWriter::evictOldestOpenFiles() {
    while (m_open_files.size() >= kMaxOpenMboxFiles && !m_open_order.isEmpty()) {
        const QString evicted = m_open_order.takeFirst();
        if (!closeOneFile(m_open_files.take(evicted), evicted)) {
            m_failed_keys.insert(evicted);
        }
    }
}

QFile* MboxWriter::openMboxFile(const QString& key, const QString& file_path) {
    const QString parent = QFileInfo(file_path).absolutePath();
    if (!QDir().mkpath(parent)) {
        logError("MboxWriter: cannot create output directory: {}", parent.toStdString());
        return nullptr;
    }

    // Containment: this process runs elevated, so a junction planted at an ancestor -- or a link
    // planted at the mailbox name itself -- must not redirect a truncating write onto a file
    // outside the output directory the operator chose.
    const QString canonical_root = QFileInfo(m_output_dir).canonicalFilePath();
    if (!partition_export::pathWithinRoot(canonical_root, parent)) {
        logError("MboxWriter: output path escapes the output directory: {}",
                 file_path.toStdString());
        return nullptr;
    }
    const QFileInfo leaf(file_path);
    // Checked before exists(): a symlink whose target does not exist yet reports exists()==false,
    // and Truncate would follow it and CREATE that target wherever it points.
    if (leaf.isSymLink() || leaf.isJunction()) {
        logError("MboxWriter: refusing to write through a link at: {}", file_path.toStdString());
        return nullptr;
    }
    if (leaf.exists() && !partition_export::pathWithinRoot(canonical_root, file_path)) {
        logError("MboxWriter: mailbox path escapes the output directory: {}",
                 file_path.toStdString());
        return nullptr;
    }

    // Truncate, not Append: each file is truncated exactly ONCE per run and then written
    // sequentially through the kept-open handle, so within-run messages still accumulate. Append
    // on a first open would instead MERGE this run's output onto a stale file left by a previous
    // run, duplicating old mail (B7-32). A mailbox reopened after handle-cap eviction appends,
    // because this run already truncated it.
    const QString identity = pathIdentity(file_path);
    const QIODevice::OpenMode mode = QIODevice::WriteOnly |
                                     (m_truncated_paths.contains(identity) ? QIODevice::Append
                                                                           : QIODevice::Truncate);
    auto* file = new QFile(file_path);
    if (!file->open(mode)) {
        logError("MboxWriter: failed to open mbox file {}: {}",
                 file_path.toStdString(),
                 file->errorString().toStdString());
        delete file;
        return nullptr;
    }

    // Recorded only now: a path claimed by an open that failed would push a later retry of the
    // same folder onto a different, suffixed mailbox.
    m_truncated_paths.insert(identity);
    m_used_file_paths.insert(identity);
    m_key_paths.insert(key, file_path);
    m_open_files.insert(key, file);
    m_open_order.append(key);
    return file;
}

QFile* MboxWriter::getOrCreateFile(const QString& folder_path) {
    const QString key = fileKey(folder_path);

    if (m_open_files.contains(key)) {
        m_open_order.removeOne(key);
        m_open_order.append(key);
        return m_open_files.value(key);
    }
    if (m_output_dir.trimmed().isEmpty()) {
        // No output directory means no target; resolving against the process working directory
        // would scatter mail somewhere nobody chose.
        logError("MboxWriter: no output directory configured; refusing to guess one");
        return nullptr;
    }
    if (m_failed_keys.contains(key)) {
        logError("MboxWriter: mailbox left incomplete by an earlier failure: {}",
                 folder_path.toStdString());
        return nullptr;
    }

    evictOldestOpenFiles();

    // A mailbox evicted by the handle cap reopens its recorded path; a new one resolves a fresh,
    // collision-free path.
    const QString cached_path = m_key_paths.value(key);
    const QString file_path = cached_path.isEmpty() ? resolveFilePath(folder_path) : cached_path;
    return openMboxFile(key, file_path);
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
        message.append("To: " + encodedHeaderValue(item.display_to) + "\r\n");
    }
    if (!item.display_cc.isEmpty()) {
        message.append("Cc: " + encodedHeaderValue(item.display_cc) + "\r\n");
    }
    // Bcc, threading and importance are part of what the source message carried; dropping them
    // loses recipients and conversation structure the archive exists to preserve.
    if (!item.display_bcc.isEmpty()) {
        message.append("Bcc: " + encodedHeaderValue(item.display_bcc) + "\r\n");
    }
    if (!item.subject.isEmpty()) {
        message.append("Subject: " + encodedHeaderValue(item.subject) + "\r\n");
    }
    message.append("Date: " + date.toString(Qt::RFC2822Date).toUtf8() + "\r\n");
    if (!item.message_id.isEmpty()) {
        message.append("Message-ID: " + sanitizeHeaderValue(item.message_id).toUtf8() + "\r\n");
    }
    if (!item.in_reply_to.isEmpty()) {
        message.append("In-Reply-To: " + sanitizeHeaderValue(item.in_reply_to).toUtf8() + "\r\n");
    }
    appendImportanceHeader(message, item.importance);
}

void MboxWriter::appendMultipartBody(QByteArray& message,
                                     const PstItemDetail& item,
                                     const QString& boundary,
                                     const QString& alt_boundary,
                                     const QVector<QPair<QString, QByteArray>>& attachments) {
    // Plain and HTML are ALTERNATIVES of one another, not two documents, so they go in a
    // multipart/alternative -- nested inside the mixed container when the message also has
    // attachments (RFC 2046). Emitted as siblings of multipart/mixed, clients show the message
    // twice.
    const bool both_bodies = !item.body_plain.isEmpty() && !item.body_html.isEmpty();
    const QByteArray outer_type = (both_bodies && attachments.isEmpty())
                                      ? QByteArrayLiteral("multipart/alternative")
                                      : QByteArrayLiteral("multipart/mixed");

    message.append("MIME-Version: 1.0\r\n");
    message.append("Content-Type: " + outer_type + ";\r\n");
    message.append(" boundary=\"" + boundary.toUtf8() + "\"\r\n");
    message.append("\r\n");

    appendBodyParts(message, item, boundary, alt_boundary);

    for (const auto& [att_name, att_data] : attachments) {
        appendAttachmentPart(message, boundary, item, att_name, att_data);
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
        // Single-part still needs MIME-Version and an explicit transfer encoding: without them a
        // UTF-8 body is read as default 7-bit US-ASCII and mis-decoded.
        message.append("MIME-Version: 1.0\r\n");
        message.append("Content-Type: text/plain; charset=utf-8\r\n");
        message.append("Content-Transfer-Encoding: 8bit\r\n");
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
    QString alt_boundary;
    if (!item.body_plain.isEmpty() && !item.body_html.isEmpty() && !attachments.isEmpty()) {
        alt_boundary = makeUniqueBoundary(item);
        if (alt_boundary.isEmpty() || alt_boundary == boundary) {
            return std::unexpected(error_code::internal_error);
        }
    }
    appendMultipartBody(message, item, boundary, alt_boundary, attachments);
    return message;
}

std::expected<QByteArray, error_code> MboxWriter::formatMboxEntry(
    const PstItemDetail& item, const QVector<QPair<QString, QByteArray>>& attachments) {
    QByteArray entry;

    if (auto envelope = validateEnvelope(item); !envelope) {
        return std::unexpected(envelope.error());
    }
    if (auto content = validateContent(item, attachments); !content) {
        return std::unexpected(content.error());
    }
    const QString sender = item.sender_email;
    const QDateTime date = item.date;

    // asctime format: "Mon Jan 01 00:00:00 2024"
    const QString date_str = date.toUTC().toString(QStringLiteral("ddd MMM dd HH:mm:ss yyyy"));

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

    // Terminate the last body line when the source body did not, then the blank line that
    // separates this record from the next From_ line (without it the next message's From_ is
    // read as part of this body).
    if (!entry.endsWith('\n')) {
        entry.append('\n');
    }
    entry.append('\n');

    return entry;
}

QByteArray MboxWriter::escapeFromLines(const QByteArray& content) {
    QByteArray result;
    result.reserve(content.size());

    // qsizetype throughout: a message larger than INT_MAX would otherwise truncate these indices
    // into negative values and slice the content at the wrong offsets.
    qsizetype pos = 0;
    while (pos < content.size()) {
        const qsizetype newline = content.indexOf('\n', pos);
        if (newline < 0) {
            result.append(content.mid(pos));
            break;
        }

        const QByteArray line = content.mid(pos, newline - pos + 1);
        // Only escape lines starting with "From " (after the first), plus the already-escaped
        // ">From " forms a reader will unescape.
        if (pos > 0 && isMboxFromLine(line)) {
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
    // Windows drops trailing dots and spaces, so "Inbox " and "Inbox" would resolve to ONE file
    // that two folders then truncate in turn; keep them distinct and creatable.
    static const QRegularExpression kTrailing(QStringLiteral("[ .]+$"));
    safe.replace(kTrailing, QStringLiteral("_"));
    if (isReservedDeviceName(safe)) {
        safe.append(QLatin1Char('_'));
    }
    return safe;
}

}  // namespace sak
