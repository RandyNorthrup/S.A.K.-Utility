// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_parser.cpp
/// @brief MBOX file parser implementation (RFC 4155 / RFC 5322 / MIME)

#include "sak/mbox_parser.h"

#include "sak/email_constants.h"
#include "sak/error_codes.h"
#include "sak/logger.h"

#include <QRegularExpression>
#include <QStringDecoder>
#include <QTimeZone>

#include <algorithm>

using sak::error_code;

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
    QByteArray first_line = m_file.readLine(1024);
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
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_is_open = false;
    m_is_indexed = false;
    m_message_offsets.clear();
}

bool MboxParser::isOpen() const {
    return m_is_open;
}

void MboxParser::indexMessages() {
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
    Q_EMIT progressUpdated(100, QStringLiteral("Indexing complete"));
    Q_EMIT indexingComplete(m_message_offsets.size());
}

void MboxParser::loadMessages(int offset, int limit) {
    auto result = readMessages(offset, limit);
    if (result) {
        Q_EMIT messagesLoaded(std::move(*result), m_message_offsets.size());
    } else {
        Q_EMIT errorOccurred(QStringLiteral("Failed to load messages: %1")
                                 .arg(QString::fromUtf8(sak::to_string(result.error()))));
    }
}

void MboxParser::loadMessageDetail(int message_index) {
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
    return m_message_offsets.size();
}

QString MboxParser::filePath() const {
    return m_file.fileName();
}

// ============================================================================
// Synchronous API
// ============================================================================

std::expected<QVector<sak::MboxMessage>, error_code> MboxParser::readMessages(int offset,
                                                                              int limit) {
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

        // Check for attachment indicators in Content-Type
        QString content_type = headers.value(QStringLiteral("content-type"));
        msg.has_attachments = content_type.contains(QLatin1String("multipart/mixed"),
                                                    Qt::CaseInsensitive);

        messages.append(std::move(msg));
    }

    return messages;
}

std::expected<sak::MboxMessageDetail, error_code> MboxParser::readMessageDetail(int message_index) {
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

    return detail;
}

std::expected<QByteArray, error_code> MboxParser::readAttachmentData(int message_index,
                                                                     int attachment_index) {
    if (!m_is_open) {
        return std::unexpected(error_code::invalid_operation);
    }
    if (attachment_index < 0) {
        return std::unexpected(error_code::invalid_argument);
    }

    auto raw = readRawMessage(message_index);
    if (!raw) {
        return std::unexpected(raw.error());
    }

    // Re-parse MIME to extract attachment bytes at the given index
    sak::MboxMessageDetail detail;
    parseMimeMessage(*raw, detail);

    if (attachment_index >= detail.attachments.size()) {
        return std::unexpected(error_code::invalid_argument);
    }

    // Re-parse extracting actual bytes this time
    return extractAttachmentBytes(*raw, attachment_index);
}

// ============================================================================
// Internal — Message Index Building
// ============================================================================

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
            int progress = static_cast<int>((bytes_read * 100) / file_size);
            if (progress > last_progress && (progress - last_progress) >= 5) {
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
    QByteArray from_line = m_file.readLine(1024);
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
    int body_start_skip = 4;
    if (header_end < 0) {
        header_end = raw_message.indexOf("\n\n");
        body_start_skip = 2;
    }
    Q_UNUSED(body_start_skip);

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
    int body_start = header_end + 4;
    if (header_end < 0) {
        header_end = data.indexOf("\n\n");
        body_start = header_end + 2;
    }
    return {header_end, body_start};
}

/// Split a multipart body into MIME parts using the delimiter
QVector<QByteArray> splitMimeParts(const QByteArray& body, const QByteArray& delimiter) {
    QVector<QByteArray> mime_parts;
    QByteArray current_part;
    bool in_part = false;

    const QList<QByteArray> lines = body.split('\n');
    for (const auto& line : lines) {
        QByteArray trimmed = line;
        if (trimmed.endsWith('\r')) {
            trimmed.chop(1);
        }
        if (trimmed.startsWith(delimiter)) {
            if (in_part && !current_part.isEmpty()) {
                mime_parts.append(current_part);
                current_part.clear();
            }
            in_part = !trimmed.endsWith(QByteArrayLiteral("--"));
            continue;
        }
        if (!in_part) {
            continue;
        }
        current_part.append(line);
        if (!line.endsWith('\n')) {
            current_part.append('\n');
        }
    }
    if (!current_part.isEmpty()) {
        mime_parts.append(current_part);
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
        if (idx + 2 >= data.size()) {
            result.append(current_char);
            continue;
        }
        const char hex1 = data[idx + 1];
        const char hex2 = data[idx + 2];

        // Soft line break
        if (hex1 == '\r' || hex1 == '\n') {
            idx += (hex1 == '\r' && hex2 == '\n') ? 2 : 1;
            continue;
        }
        // Hex-encoded byte
        bool ok_first = false;
        bool ok_second = false;
        int val1 = QByteArray(1, hex1).toInt(&ok_first, 16);
        int val2 = QByteArray(1, hex2).toInt(&ok_second, 16);
        if (ok_first && ok_second) {
            result.append(static_cast<char>((val1 << 4) | val2));
            idx += 2;
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

    static const QRegularExpression filename_regex(QStringLiteral(
                                                       R"re(filename\s*=\s*"?([^"\;\s]+)"?)re"),
                                                   QRegularExpression::CaseInsensitiveOption);
    auto fn_match = filename_regex.match(part.disposition);
    if (!fn_match.hasMatch()) {
        fn_match = filename_regex.match(part.content_type);
    }
    if (fn_match.hasMatch()) {
        att.long_filename = fn_match.captured(1);
        att.filename = att.long_filename;
    }

    QByteArray decoded = decodeTransferEncoding(part.body, part.encoding);
    att.size_bytes = decoded.size();
    att.content_id = part.headers.value(QStringLiteral("content-id"));

    detail.attachments.append(std::move(att));
}

void MboxParser::processMimePart(const QByteArray& part,
                                 sak::MboxMessageDetail& detail,
                                 int& attachment_idx) {
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
        processMimePart(part, detail, attachment_idx);
    }
}

std::expected<QByteArray, error_code> MboxParser::extractAttachmentBytes(
    const QByteArray& raw_message, int attachment_index) {
    auto [header_end, body_start] = findBodyBoundary(raw_message);
    if (header_end < 0) {
        return std::unexpected(error_code::invalid_argument);
    }

    QByteArray body = raw_message.mid(body_start);
    auto headers = parseHeaders(raw_message);
    QString content_type = headers.value(QStringLiteral("content-type"),
                                         QStringLiteral("text/plain"));

    if (!content_type.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive)) {
        return std::unexpected(error_code::invalid_argument);
    }

    QString boundary = extractBoundary(content_type);
    if (boundary.isEmpty()) {
        return std::unexpected(error_code::invalid_argument);
    }

    QByteArray delimiter = QByteArrayLiteral("--") + boundary.toUtf8();
    QVector<QByteArray> mime_parts = splitMimeParts(body, delimiter);

    int att_idx = 0;
    for (const auto& part : mime_parts) {
        auto part_hdrs = parseHeaders(part);
        QString part_ct = part_hdrs.value(QStringLiteral("content-type"),
                                          QStringLiteral("text/plain"));
        QString part_disp = part_hdrs.value(QStringLiteral("content-disposition"));

        bool is_att = part_disp.startsWith(QLatin1String("attachment"), Qt::CaseInsensitive);
        bool is_non_text = !part_ct.startsWith(QLatin1String("text/"), Qt::CaseInsensitive) &&
                           !part_ct.startsWith(QLatin1String("multipart/"), Qt::CaseInsensitive);

        if (!is_att && !is_non_text) {
            continue;
        }
        if (att_idx != attachment_index) {
            ++att_idx;
            continue;
        }

        auto [phdr_end, pbody_start] = findBodyBoundary(part);
        if (phdr_end < 0) {
            return std::unexpected(error_code::invalid_argument);
        }
        QString part_enc = part_hdrs.value(QStringLiteral("content-transfer-encoding"));
        QByteArray part_body = part.mid(pbody_start);
        return decodeTransferEncoding(part_body, part_enc);
    }
    return std::unexpected(error_code::invalid_argument);
}

// ============================================================================
// Internal — Decoding
// ============================================================================

QByteArray MboxParser::decodeTransferEncoding(const QByteArray& data, const QString& encoding) {
    if (encoding.compare(QLatin1String("base64"), Qt::CaseInsensitive) == 0) {
        return QByteArray::fromBase64(data);
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
    // RFC 4155: "From " followed by email/sender and date
    // Practical check: starts with "From " and is followed by non-whitespace
    if (line.size() < 6) {
        return false;
    }
    return line.startsWith("From ") && line[5] != ' ';
}
