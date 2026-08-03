// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_parser.cpp
/// @brief MBOX file parser implementation (RFC 4155 / RFC 5322 / MIME)

#include "sak/mbox_parser.h"

#include "sak/email_constants.h"
#include "sak/error_codes.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QMutex>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStringDecoder>
#include <QTimeZone>

#include <algorithm>
#include <optional>

using sak::error_code;

namespace {

constexpr qint64 kFromLineProbeBytes = 1024;
constexpr int kIndexProgressStepPercent = 5;
constexpr int kCrLfHeaderBoundaryBytes = 4;
constexpr int kLfHeaderBoundaryBytes = 2;
constexpr int kQuotedPrintableHexDigitCount = 2;
constexpr int kQuotedPrintableFirstHexOffset = 1;
constexpr int kQuotedPrintableSecondHexOffset = 2;
constexpr int kQuotedPrintableNibbleShift = 4;
constexpr int kFromLineMinimumLength = 6;
constexpr int kFromLineSenderOffset = 5;

/// Header-level attachment heuristic for the summary list. Mirrors the classification
/// parseMimeMessage applies to full bodies: any multipart/* may carry parts, an explicit
/// attachment disposition, or a single non-text part all count as "has attachments". The old
/// check only matched multipart/mixed and so under-reported (multipart/related, a lone PDF, etc.).
bool summaryHasAttachments(const QString& content_type, const QString& disposition) {
    if (content_type.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive)) {
        return true;
    }
    if (disposition.startsWith(QLatin1String("attachment"), Qt::CaseInsensitive)) {
        return true;
    }
    return !content_type.isEmpty() &&
           !content_type.startsWith(QLatin1String("text/"), Qt::CaseInsensitive);
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

MboxParser::MboxParser(QObject* parent) : QObject(parent) {}

MboxParser::~MboxParser() {
    close();
}

// ============================================================================
// Public API
// ============================================================================

void MboxParser::open(const QString& file_path) {
    const QMutexLocker locker(&m_file_mutex);
    close();
    m_cancelled.store(false, std::memory_order_relaxed);

    m_file.setFileName(file_path);
    if (!m_file.open(QIODevice::ReadOnly)) {
        const auto message = QStringLiteral("Cannot open MBOX file: %1").arg(file_path);
        sak::logError("MboxParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        return;
    }

    // Quick validation — first line should start with "From "
    QByteArray first_line = m_file.readLine(kFromLineProbeBytes);
    if (!isFromLine(first_line)) {
        const auto message =
            QStringLiteral("Not a valid MBOX file (missing 'From ' header): %1").arg(file_path);
        sak::logError("MboxParser: {}", message.toStdString());
        Q_EMIT errorOccurred(message);
        m_file.close();
        return;
    }

    m_file.seek(0);
    m_is_open = true;

    // Estimate message count from file size (rough average ~5KB per message)
    constexpr qint64 kAvgMessageSize = 5120;
    int estimated = static_cast<int>(std::min(m_file.size() / kAvgMessageSize,
                                              static_cast<qint64>(sak::email::kMaxSearchResults)));

    Q_EMIT fileOpened(file_path, estimated);
}

void MboxParser::close() {
    const QMutexLocker locker(&m_file_mutex);
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_is_open = false;
    m_is_indexed = false;
    m_message_offsets.clear();
}

bool MboxParser::isOpen() const {
    const QMutexLocker locker(&m_file_mutex);
    return m_is_open;
}

void MboxParser::indexMessages() {
    const QMutexLocker locker(&m_file_mutex);
    if (!m_is_open) {
        Q_EMIT errorOccurred(QStringLiteral("No file is open"));
        return;
    }

    m_cancelled.store(false, std::memory_order_relaxed);
    Q_EMIT progressUpdated(0, QStringLiteral("Indexing messages..."));

    buildMessageIndex();

    if (m_cancelled.load(std::memory_order_relaxed)) {
        Q_EMIT errorOccurred(QStringLiteral("Indexing cancelled"));
        return;
    }

    m_is_indexed = true;
    Q_EMIT progressUpdated(sak::kPercentMax, QStringLiteral("Indexing complete"));
    Q_EMIT indexingComplete(m_message_offsets.size());
}

void MboxParser::loadMessages(int offset, int limit) {
    const QMutexLocker locker(&m_file_mutex);
    auto result = readMessages(offset, limit);
    if (result) {
        Q_EMIT messagesLoaded(std::move(*result), m_message_offsets.size());
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load messages: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void MboxParser::loadMessageDetail(int message_index) {
    const QMutexLocker locker(&m_file_mutex);
    auto result = readMessageDetail(message_index);
    if (result) {
        Q_EMIT messageDetailLoaded(std::move(*result));
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load message detail: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void MboxParser::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

int MboxParser::messageCount() const {
    const QMutexLocker locker(&m_file_mutex);
    return m_message_offsets.size();
}

QString MboxParser::filePath() const {
    const QMutexLocker locker(&m_file_mutex);
    return m_file.fileName();
}

// ============================================================================
// Synchronous API
// ============================================================================

std::expected<QVector<sak::MboxMessage>, error_code> MboxParser::readMessages(int offset,
                                                                              int limit) {
    const QMutexLocker locker(&m_file_mutex);
    // A new read starts uncancelled: a cancel from a previous, already-finished
    // operation must not permanently block later reads until reopen (B7-24).
    m_cancelled.store(false, std::memory_order_relaxed);
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }
    if (!m_is_indexed) {
        buildMessageIndex();
        m_is_indexed = true;
    }

    int total = m_message_offsets.size();
    int start = std::min(offset, total);
    int end_idx = (limit > 0) ? std::min(start + limit, total) : total;

    QVector<sak::MboxMessage> messages;
    messages.reserve(end_idx - start);

    for (int msg_idx = start; msg_idx < end_idx; ++msg_idx) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return std::unexpected(error_code::operation_cancelled);
        }

        auto raw = readRawMessage(msg_idx);
        if (!raw) {
            // Do not drop it silently: the message stays counted by messageCount()
            // so skipping it without a trace hides a list/count mismatch.
            sak::logWarning("MboxParser: skipping unreadable message {} (error {})",
                            std::to_string(msg_idx),
                            std::to_string(static_cast<int>(raw.error())));
            continue;
        }

        auto headers = parseHeaders(*raw);

        sak::MboxMessage msg;
        msg.message_index = msg_idx;
        msg.file_offset = m_message_offsets[msg_idx];
        msg.message_size = raw->size();
        msg.subject = headers.value(QStringLiteral("subject"));
        msg.from = headers.value(QStringLiteral("from"));
        msg.to = headers.value(QStringLiteral("to"));
        msg.cc = headers.value(QStringLiteral("cc"));
        msg.date = parseEmailDate(headers.value(QStringLiteral("date")));

        // Check for attachment indicators from the message headers (broadened to match
        // what parseMimeMessage would actually surface as attachments, not just mixed).
        msg.has_attachments =
            summaryHasAttachments(headers.value(QStringLiteral("content-type")),
                                  headers.value(QStringLiteral("content-disposition")));

        messages.append(std::move(msg));
    }

    return messages;
}

std::expected<sak::MboxMessageDetail, error_code> MboxParser::readMessageDetail(int message_index) {
    const QMutexLocker locker(&m_file_mutex);
    // Clear a stale cancel from a prior operation so this read is not blocked (B7-24).
    m_cancelled.store(false, std::memory_order_relaxed);
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }
    if (!m_is_indexed) {
        buildMessageIndex();
        m_is_indexed = true;
    }
    if (message_index < 0 || message_index >= m_message_offsets.size()) {
        return std::unexpected(error_code::invalid_argument);
    }

    auto raw = readRawMessage(message_index);
    if (!raw) {
        return std::unexpected(raw.error());
    }

    auto headers = parseHeaders(*raw);

    sak::MboxMessageDetail detail;
    detail.message_index = message_index;
    detail.subject = headers.value(QStringLiteral("subject"));
    detail.from = headers.value(QStringLiteral("from"));
    detail.to = headers.value(QStringLiteral("to"));
    detail.cc = headers.value(QStringLiteral("cc"));
    detail.bcc = headers.value(QStringLiteral("bcc"));
    detail.date = parseEmailDate(headers.value(QStringLiteral("date")));
    detail.message_id = headers.value(QStringLiteral("message-id"));

    // Build raw headers string
    int header_end = raw->indexOf("\r\n\r\n");
    if (header_end < 0) {
        header_end = raw->indexOf("\n\n");
    }
    if (header_end >= 0) {
        detail.raw_headers = QString::fromUtf8(raw->left(header_end));
    }

    // Parse MIME body and attachments
    parseMimeMessage(*raw, detail);

    // Fail closed: a cancellation that landed mid-parse, or a strict base64 decode failure, must
    // not surface a truncated/partial message as a success.
    if (const auto fail = mimeParseFailure()) {
        return std::unexpected(*fail);
    }

    return detail;
}

std::expected<QByteArray, error_code> MboxParser::readAttachmentData(int message_index,
                                                                     int attachment_index) {
    const QMutexLocker locker(&m_file_mutex);
    // Clear a stale cancel so a prior abort does not block this extraction (B7-24).
    // readAllAttachments (called below) intentionally does NOT reset, so a cancel
    // that arrives during THIS call still interrupts it.
    m_cancelled.store(false, std::memory_order_relaxed);
    if (attachment_index < 0) {
        return std::unexpected(error_code::invalid_argument);
    }
    // Route through readAllAttachments so an attachment index means the SAME thing here as in
    // readMessageDetail: both enumerate attachments RECURSIVELY (DFS through nested multiparts).
    // The old bespoke extractor counted only TOP-LEVEL parts, so any nested multipart (e.g. an
    // inline image in multipart/related plus a top-level attachment) mis-paired names and bytes for
    // every caller. As a bonus this parses the message once, not twice.
    const auto all = readAllAttachments(message_index);
    if (!all) {
        return std::unexpected(all.error());
    }
    if (attachment_index >= all->size()) {
        return std::unexpected(error_code::invalid_argument);
    }
    return (*all)[attachment_index].data;
}

// ============================================================================
// Internal — Message Index Building
// ============================================================================

std::optional<error_code> MboxParser::mimeParseFailure() const {
    if (m_cancelled.load(std::memory_order_relaxed)) {
        return error_code::operation_cancelled;
    }
    if (m_mime_decode_failed) {
        return error_code::mbox_message_parse_error;
    }
    return std::nullopt;
}

void MboxParser::buildMessageIndex() {
    m_message_offsets.clear();
    m_file.seek(0);

    qint64 file_size = m_file.size();
    qint64 bytes_read = 0;
    int last_progress = 0;

    while (!m_file.atEnd()) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return;
        }

        qint64 line_offset = m_file.pos();
        QByteArray line = m_file.readLine(sak::email::kMboxMaxMessageSize);

        if (isFromLine(line)) {
            m_message_offsets.append(line_offset);
        }

        bytes_read = m_file.pos();
        if (file_size > 0) {
            int progress = static_cast<int>((bytes_read * sak::kPercentMax) / file_size);
            if (progress > last_progress &&
                (progress - last_progress) >= kIndexProgressStepPercent) {
                last_progress = progress;
                Q_EMIT progressUpdated(
                    progress,
                    QStringLiteral("Indexed %1 messages...").arg(m_message_offsets.size()));
            }
        }
    }
}

// ============================================================================
// Internal — Raw Message Reading
// ============================================================================

std::expected<QByteArray, error_code> MboxParser::readRawMessage(int message_index) {
    if (message_index < 0 || message_index >= m_message_offsets.size()) {
        return std::unexpected(error_code::invalid_argument);
    }

    qint64 start_offset = m_message_offsets[message_index];
    qint64 end_offset;

    if (message_index + 1 < m_message_offsets.size()) {
        end_offset = m_message_offsets[message_index + 1];
    } else {
        end_offset = m_file.size();
    }

    qint64 message_size = end_offset - start_offset;
    if (message_size > sak::email::kMboxMaxMessageSize) {
        return std::unexpected(error_code::file_too_large);
    }
    if (message_size <= 0) {
        return std::unexpected(error_code::read_error);
    }

    if (!m_file.seek(start_offset)) {
        return std::unexpected(error_code::seek_error);
    }

    // Skip the "From " line
    QByteArray from_line = m_file.readLine(kFromLineProbeBytes);
    Q_UNUSED(from_line);

    qint64 remaining = end_offset - m_file.pos();
    if (remaining <= 0) {
        return QByteArray{};
    }

    QByteArray data = m_file.read(remaining);
    if (data.isEmpty()) {
        return std::unexpected(error_code::read_error);
    }

    // Un-escape "From " lines: ">From " → "From "
    data.replace("\n>From ", "\nFrom ");

    return data;
}

// ============================================================================
// Internal — Header Parsing (RFC 5322)
// ============================================================================

QMap<QString, QString> MboxParser::parseHeaders(const QByteArray& raw_message) {
    QMap<QString, QString> headers;

    // Find the header/body boundary
    int header_end = raw_message.indexOf("\r\n\r\n");
    if (header_end < 0) {
        header_end = raw_message.indexOf("\n\n");
    }

    QByteArray header_block = (header_end >= 0) ? raw_message.left(header_end) : raw_message;

    // Split into lines and handle continuation (folded headers)
    QList<QByteArray> lines = header_block.split('\n');

    QString current_name;
    QString current_value;

    auto flushHeader = [&]() {
        if (!current_name.isEmpty()) {
            headers.insert(current_name.toLower().trimmed(), current_value.trimmed());
        }
        current_name.clear();
        current_value.clear();
    };

    for (const auto& raw_line : lines) {
        QByteArray line = raw_line;
        if (line.endsWith('\r')) {
            line.chop(1);
        }

        if (line.isEmpty()) {
            break;
        }

        // Continuation line (starts with whitespace)
        if (line.startsWith(' ') || line.startsWith('\t')) {
            current_value += QLatin1Char(' ') + QString::fromUtf8(line).trimmed();
            continue;
        }

        // New header
        flushHeader();
        int colon = line.indexOf(':');
        if (colon > 0) {
            current_name = QString::fromUtf8(line.left(colon));
            current_value = QString::fromUtf8(line.mid(colon + 1)).trimmed();
        }
    }
    flushHeader();

    return headers;
}

// ============================================================================
// File-scope MIME Helpers
// ============================================================================

namespace {

/// Find header/body boundary (\r\n\r\n or \n\n)
std::pair<int, int> findBodyBoundary(const QByteArray& data) {
    int header_end = data.indexOf("\r\n\r\n");
    int body_start = header_end + kCrLfHeaderBoundaryBytes;
    if (header_end < 0) {
        header_end = data.indexOf("\n\n");
        body_start = header_end + kLfHeaderBoundaryBytes;
    }
    return {header_end, body_start};
}

/// True only for a real MIME boundary delimiter line (RFC 2046 §5.1.1): the line
/// is exactly "--<boundary>" (opening) or "--<boundary>--" (closing), allowing
/// only trailing linear whitespace. A body line that merely SHARES the delimiter
/// prefix (e.g. delimiter "--sep" and content line "--separator") is not a
/// boundary, so it must not split the message (B7-33). `line` is already stripped
/// of a trailing CR.
bool isBoundaryDelimiterLine(const QByteArray& line, const QByteArray& delimiter) {
    if (!line.startsWith(delimiter)) {
        return false;
    }
    const QByteArray rest = line.mid(delimiter.size()).trimmed();
    return rest.isEmpty() || rest == QByteArrayLiteral("--");
}

/// Upper bound on the number of MIME parts one body may yield. A crafted body that is nothing but
/// boundary delimiters could otherwise force an unbounded QVector; cap it and stop scanning.
constexpr int kMaxMimeParts = 100'000;

/// Fold one raw line (no trailing '\n') into the running split state. Returns false only when the
/// part cap is reached and scanning must stop.
bool consumeMimeLine(const QByteArray& line,
                     const QByteArray& delimiter,
                     QVector<QByteArray>& parts,
                     QByteArray& current,
                     bool& in_part) {
    QByteArray trimmed = line;
    if (trimmed.endsWith('\r')) {
        trimmed.chop(1);
    }
    if (isBoundaryDelimiterLine(trimmed, delimiter)) {
        if (in_part && !current.isEmpty()) {
            if (parts.size() >= kMaxMimeParts) {
                return false;
            }
            parts.append(std::move(current));
            current.clear();
        }
        // A closing delimiter ("--<boundary>--") ends the multipart; an opening
        // one ("--<boundary>") starts the next part.
        in_part = !trimmed.trimmed().endsWith(QByteArrayLiteral("--"));
        return true;
    }
    if (in_part) {
        current.append(line);
        current.append('\n');
    }
    return true;
}

/// Split a multipart body into MIME parts using the delimiter. Scans line boundaries in place
/// rather than materializing a per-line QList of the (up to kMboxMaxMessageSize) body, so a huge
/// body does not amplify into millions of small allocations.
QVector<QByteArray> splitMimeParts(const QByteArray& body, const QByteArray& delimiter) {
    QVector<QByteArray> mime_parts;
    QByteArray current_part;
    bool in_part = false;

    int pos = 0;
    const int size = body.size();
    while (pos < size) {
        const int newline = body.indexOf('\n', pos);
        const int line_end = (newline < 0) ? size : newline;
        if (!consumeMimeLine(
                body.mid(pos, line_end - pos), delimiter, mime_parts, current_part, in_part)) {
            return mime_parts;
        }
        if (newline < 0) {
            break;
        }
        pos = newline + 1;
    }
    if (!current_part.isEmpty() && mime_parts.size() < kMaxMimeParts) {
        mime_parts.append(std::move(current_part));
    }
    return mime_parts;
}

/// Decode quoted-printable encoded data
QByteArray decodeQuotedPrintable(const QByteArray& data) {
    QByteArray result;
    result.reserve(data.size());

    for (int idx = 0; idx < data.size(); ++idx) {
        const char current_char = data[idx];
        if (current_char != '=') {
            result.append(current_char);
            continue;
        }
        if (idx + kQuotedPrintableHexDigitCount >= data.size()) {
            result.append(current_char);
            continue;
        }
        const char hex1 = data[idx + kQuotedPrintableFirstHexOffset];
        const char hex2 = data[idx + kQuotedPrintableSecondHexOffset];

        // Soft line break
        if (hex1 == '\r' || hex1 == '\n') {
            idx += (hex1 == '\r' && hex2 == '\n') ? kQuotedPrintableHexDigitCount
                                                  : kQuotedPrintableFirstHexOffset;
            continue;
        }
        // Hex-encoded byte
        bool ok_first = false;
        bool ok_second = false;
        int val1 = QByteArray(1, hex1).toInt(&ok_first, sak::kHexBase);
        int val2 = QByteArray(1, hex2).toInt(&ok_second, sak::kHexBase);
        if (ok_first && ok_second) {
            result.append(static_cast<char>((val1 << kQuotedPrintableNibbleShift) | val2));
            idx += kQuotedPrintableHexDigitCount;
            continue;
        }
        result.append(current_char);
    }
    return result;
}

/// Extract charset value from a Content-Type header
QString extractCharset(const QString& content_type) {
    static const QRegularExpression charset_regex(QStringLiteral(
                                                      R"re(charset\s*=\s*"?([^"\;\s]+)"?)re"),
                                                  QRegularExpression::CaseInsensitiveOption);
    auto match = charset_regex.match(content_type);
    return match.hasMatch() ? match.captured(1) : QString();
}

/// Extract MIME boundary from a Content-Type header
QString extractBoundary(const QString& content_type) {
    static const QRegularExpression boundary_regex(QStringLiteral(
                                                       R"re(boundary\s*=\s*"?([^"\;\s]+)"?)re"),
                                                   QRegularExpression::CaseInsensitiveOption);
    auto match = boundary_regex.match(content_type);
    return match.hasMatch() ? match.captured(1) : QString();
}

}  // anonymous namespace

// ============================================================================
// Internal — MIME Parsing
// ============================================================================

void MboxParser::parseSinglePart(const QByteArray& body,
                                 const QString& content_type,
                                 const QString& transfer_encoding,
                                 const QString& charset,
                                 sak::MboxMessageDetail& detail) {
    QByteArray decoded = decodeTransferEncoding(body, transfer_encoding);
    QString text = decodeCharset(decoded, charset);

    if (content_type.startsWith(QLatin1String("text/html"), Qt::CaseInsensitive)) {
        detail.body_html = text;
    } else {
        detail.body_plain = text;
    }
}

void MboxParser::appendAttachment(const MimePartInfo& part,
                                  sak::MboxMessageDetail& detail,
                                  int& attachment_idx) {
    sak::PstAttachmentInfo att;
    att.index = attachment_idx++;
    att.mime_type = part.content_type.section(QLatin1Char(';'), 0, 0).trimmed();
    att.attach_method = sak::email::kAttachByValue;

    // A quoted value may legitimately contain spaces (filename="my report.pdf"); capture the whole
    // quoted string in group 1, and fall back to an unquoted token (group 2) for bare values.
    static const QRegularExpression filename_regex(
        QStringLiteral(R"re(filename\s*=\s*(?:"([^"]*)"|([^";\s]+)))re"),
        QRegularExpression::CaseInsensitiveOption);
    auto fn_match = filename_regex.match(part.disposition);
    if (!fn_match.hasMatch()) {
        fn_match = filename_regex.match(part.content_type);
    }
    if (fn_match.hasMatch()) {
        att.long_filename = fn_match.captured(1).isNull() ? fn_match.captured(2)
                                                          : fn_match.captured(1);
        att.filename = att.long_filename;
    }

    QByteArray decoded = decodeTransferEncoding(part.body, part.encoding);
    att.size_bytes = decoded.size();
    att.content_id = part.headers.value(QStringLiteral("content-id"));

    // Hand the decoded bytes to a collecting caller (readAllAttachments) in lock-step with the
    // attachment entry, so name (detail.attachments) and content (m_attachment_sink) share one
    // enumeration index.
    if (m_attachment_sink != nullptr) {
        m_attachment_sink->append(decoded);
    }
    detail.attachments.append(std::move(att));
}

void MboxParser::recurseMultipartPart(const QString& part_content_type,
                                      const QByteArray& part_body,
                                      sak::MboxMessageDetail& detail,
                                      int& attachment_idx,
                                      int depth) {
    constexpr int kMaxMimeDepth = 20;
    if (depth >= kMaxMimeDepth) {
        return;
    }
    const QString sub_boundary = extractBoundary(part_content_type);
    if (sub_boundary.isEmpty()) {
        return;
    }
    const QByteArray delimiter = QByteArrayLiteral("--") + sub_boundary.toUtf8();
    for (const auto& sub_part : splitMimeParts(part_body, delimiter)) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        processMimePart(sub_part, detail, attachment_idx, depth + 1);
    }
}

void MboxParser::processMimePart(const QByteArray& part,
                                 sak::MboxMessageDetail& detail,
                                 int& attachment_idx,
                                 int depth) {
    auto part_headers = parseHeaders(part);
    QString part_ct = part_headers.value(QStringLiteral("content-type"),
                                         QStringLiteral("text/plain"));
    QString part_enc = part_headers.value(QStringLiteral("content-transfer-encoding"));
    QString part_disp = part_headers.value(QStringLiteral("content-disposition"));

    auto [part_hdr_end, part_body_start] = findBodyBoundary(part);
    if (part_hdr_end < 0) {
        return;
    }
    QByteArray part_body = part.mid(part_body_start);

    // Nested multipart/* part: recurse into its sub-parts. Without this, an inner
    // multipart yields nothing (it is neither text nor an attachment) and its
    // contents -- body and attachments -- are lost.
    if (part_ct.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive)) {
        recurseMultipartPart(part_ct, part_body, detail, attachment_idx, depth);
        return;
    }

    bool is_attachment = part_disp.startsWith(QLatin1String("attachment"), Qt::CaseInsensitive);
    bool is_non_text = !part_ct.startsWith(QLatin1String("text/"), Qt::CaseInsensitive) &&
                       !part_ct.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive);

    if (is_attachment || is_non_text) {
        MimePartInfo mime_part{part_body, part_ct, part_enc, part_disp, part_headers};
        appendAttachment(mime_part, detail, attachment_idx);
        return;
    }

    // Text part
    QString part_charset = extractCharset(part_ct);
    QByteArray decoded = decodeTransferEncoding(part_body, part_enc);
    QString text = decodeCharset(decoded, part_charset);

    if (part_ct.startsWith(QLatin1String("text/html"), Qt::CaseInsensitive)) {
        detail.body_html = text;
    } else if (part_ct.startsWith(QLatin1String("text/plain"), Qt::CaseInsensitive)) {
        detail.body_plain = text;
    }
}

void MboxParser::parseMimeMessage(const QByteArray& raw_message, sak::MboxMessageDetail& detail) {
    // Fresh parse: clear any decode-failure sticky flag from a previous message so the
    // public reads only ever observe THIS message's outcome.
    m_mime_decode_failed = false;

    auto [header_end, body_start] = findBodyBoundary(raw_message);
    if (header_end < 0) {
        return;
    }

    QByteArray body = raw_message.mid(body_start);
    auto headers = parseHeaders(raw_message);
    QString content_type = headers.value(QStringLiteral("content-type"),
                                         QStringLiteral("text/plain"));
    QString transfer_enc = headers.value(QStringLiteral("content-transfer-encoding"));
    QString charset = extractCharset(content_type);

    if (!content_type.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive)) {
        const QString disposition = headers.value(QStringLiteral("content-disposition"));
        const bool is_attachment = disposition.startsWith(QLatin1String("attachment"),
                                                          Qt::CaseInsensitive);
        const bool is_non_text = !content_type.startsWith(QLatin1String("text/"),
                                                          Qt::CaseInsensitive);
        if (is_attachment || is_non_text) {
            // A single-part message that is itself a file (e.g. a lone PDF, or one
            // marked Content-Disposition: attachment) must be exposed as an
            // attachment, not decoded into garbled body text (B7-33).
            MimePartInfo mime_part{body, content_type, transfer_enc, disposition, headers};
            int attachment_idx = 0;
            appendAttachment(mime_part, detail, attachment_idx);
            return;
        }
        parseSinglePart(body, content_type, transfer_enc, charset, detail);
        return;
    }

    QString boundary = extractBoundary(content_type);
    if (boundary.isEmpty()) {
        detail.body_plain = QString::fromUtf8(body);
        return;
    }

    QByteArray delimiter = QByteArrayLiteral("--") + boundary.toUtf8();
    QVector<QByteArray> mime_parts = splitMimeParts(body, delimiter);

    int attachment_idx = 0;
    for (const auto& part : mime_parts) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return;
        }
        processMimePart(part, detail, attachment_idx);
    }
}

std::expected<QVector<MboxAttachmentPayload>, error_code> MboxParser::readAllAttachments(
    int message_index) {
    const QMutexLocker locker(&m_file_mutex);
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }
    if (!m_is_indexed) {
        buildMessageIndex();
        m_is_indexed = true;
    }
    if (message_index < 0 || message_index >= m_message_offsets.size()) {
        return std::unexpected(error_code::invalid_argument);
    }

    auto raw = readRawMessage(message_index);
    if (!raw) {
        return std::unexpected(raw.error());
    }

    // One recursive pass: names land in detail.attachments, bytes in payload_bytes, index-aligned.
    // m_attachment_sink routes appendAttachment's decoded bytes into payload_bytes for the duration
    // of this parse only. A scope guard clears it on EVERY exit (including a thrown exception), so
    // the pointer can never dangle past this call and the GUI's readMessageDetail path (which never
    // sets it) stays unaffected.
    sak::MboxMessageDetail detail;
    QVector<QByteArray> payload_bytes;
    m_attachment_sink = &payload_bytes;
    const auto sink_guard = qScopeGuard([this] { m_attachment_sink = nullptr; });
    parseMimeMessage(*raw, detail);

    // Fail closed rather than return attachments with silently truncated bytes.
    if (const auto fail = mimeParseFailure()) {
        return std::unexpected(*fail);
    }

    QVector<MboxAttachmentPayload> out;
    out.reserve(detail.attachments.size());
    for (int i = 0; i < detail.attachments.size(); ++i) {
        const sak::PstAttachmentInfo& att = detail.attachments[i];
        const QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        // Defensive: the two lists are appended in lock-step, so sizes match; guard anyway so a
        // future divergence yields empty bytes rather than an out-of-range read.
        out.append({name, i < payload_bytes.size() ? payload_bytes[i] : QByteArray()});
    }
    return out;
}

// ============================================================================
// Internal — Decoding
// ============================================================================

QByteArray MboxParser::decodeTransferEncoding(const QByteArray& data, const QString& encoding) {
    if (encoding.compare(QLatin1String("base64"), Qt::CaseInsensitive) == 0) {
        // Strip the line-wrapping whitespace MIME inserts, then decode strictly: Qt's default
        // decoder silently yields partial bytes for malformed input, so any remaining invalid
        // character fails the decode (m_mime_decode_failed) instead of producing garbage.
        QByteArray compact = data;
        compact.removeIf(
            [](char byte) { return byte == '\r' || byte == '\n' || byte == ' ' || byte == '\t'; });
        const auto decoded = QByteArray::fromBase64Encoding(
            compact, QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            m_mime_decode_failed = true;
            return {};
        }
        return decoded.decoded;
    }
    if (encoding.compare(QLatin1String("quoted-printable"), Qt::CaseInsensitive) == 0) {
        return decodeQuotedPrintable(data);
    }
    // 7bit, 8bit, binary — no decoding needed
    return data;
}

QString MboxParser::decodeCharset(const QByteArray& data, const QString& charset) {
    if (charset.isEmpty() || charset.compare(QLatin1String("utf-8"), Qt::CaseInsensitive) == 0 ||
        charset.compare(QLatin1String("us-ascii"), Qt::CaseInsensitive) == 0) {
        return QString::fromUtf8(data);
    }

    auto decoder = QStringDecoder(charset.toLatin1().constData());
    if (decoder.isValid()) {
        return decoder(data);
    }

    // Fallback to UTF-8
    sak::logWarning("MboxParser: Unknown charset '{}', falling back to UTF-8",
                    charset.toStdString());
    return QString::fromUtf8(data);
}

// ============================================================================
// Internal — Date Parsing
// ============================================================================

QDateTime MboxParser::parseEmailDate(const QString& date_str) {
    if (date_str.isEmpty()) {
        return {};
    }

    // Try RFC 2822 format: "Thu, 13 Feb 2020 12:30:00 +0000"
    QDateTime dt = QDateTime::fromString(date_str.trimmed(), Qt::RFC2822Date);
    if (dt.isValid()) {
        return dt;
    }

    // Try ISO 8601
    dt = QDateTime::fromString(date_str.trimmed(), Qt::ISODate);
    if (dt.isValid()) {
        return dt;
    }

    // Try common variations
    static const QStringList kDateFormats = {
        QStringLiteral("ddd, d MMM yyyy HH:mm:ss"),
        QStringLiteral("d MMM yyyy HH:mm:ss"),
        QStringLiteral("ddd MMM d HH:mm:ss yyyy"),
        QStringLiteral("yyyy-MM-dd HH:mm:ss"),
    };

    for (const auto& fmt : kDateFormats) {
        dt = QDateTime::fromString(date_str.trimmed(), fmt);
        if (dt.isValid()) {
            return dt;
        }
    }

    return {};
}

// ============================================================================
// Internal — MBOX "From " Detection
// ============================================================================

bool MboxParser::isFromLine(const QByteArray& line) {
    // RFC 4155: "From " followed by sender and an asctime date. Cheap gate first (this runs per
    // line while indexing), then require a clock time and a 4-digit year on the line so ordinary
    // body prose beginning "From " is not mistaken for a separator. Best-effort: exotic date
    // spellings may be missed, but that is preferable to splitting a message on stray text.
    if (line.size() < kFromLineMinimumLength) {
        return false;
    }
    if (!line.startsWith("From ") || line[kFromLineSenderOffset] == ' ') {
        return false;
    }
    static const QRegularExpression from_line_shape(
        QStringLiteral(R"re(^From \S+ .*\d{1,2}:\d{2}(:\d{2})?.*\d{4})re"));
    return from_line_shape.match(QString::fromLatin1(line)).hasMatch();
}
