// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file mbox_parser.h
/// @brief MBOX file parser per RFC 4155
///
/// Parses MBOX files (Thunderbird, Apple Mail, Linux mail clients).
/// Supports indexing, header-only loading, and full MIME parsing
/// with attachment extraction.

#pragma once

#include "sak/email_types.h"
#include "sak/strong_index.h"

#include <QFile>
#include <QMap>
#include <QObject>
#include <QRecursiveMutex>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <expected>
#include <optional>

namespace sak {
enum class error_code;

/// R5-G14-19: a message index and an attachment index are both plain ints, so
/// readAttachmentData(message, attachment) could be called with the two arguments
/// swapped and still compile -- silently returning the wrong bytes. These tags make
/// that swap a hard compile error. Both keep a SIGNED underlying because a negative
/// index is a legitimate "invalid" value the reads reject explicitly.
struct MboxMessageIndexTag {};
struct MboxAttachmentIndexTag {};
using MboxMessageIndex = StrongIndex<MboxMessageIndexTag, int>;
using MboxAttachmentIndex = StrongIndex<MboxAttachmentIndexTag, int>;
}  // namespace sak

/// One attachment with its decoded content, produced by MboxParser::readAllAttachments in a
/// SINGLE recursive MIME pass. filename and data come from the same enumeration, so they always
/// correspond -- readMessageDetail and readAttachmentData both enumerate recursively and are keyed
/// on this same index.
struct MboxAttachmentPayload {
    QString filename;
    QByteArray data;
};

class MboxParser : public QObject {
    Q_OBJECT

public:
    explicit MboxParser(QObject* parent = nullptr);
    ~MboxParser() override;

    MboxParser(const MboxParser&) = delete;
    MboxParser& operator=(const MboxParser&) = delete;
    MboxParser(MboxParser&&) = delete;
    MboxParser& operator=(MboxParser&&) = delete;

    /// Open an MBOX file
    void open(const QString& file_path);

    /// Close the currently opened file
    void close();

    /// Whether a file is currently open
    [[nodiscard]] bool isOpen() const;

    /// Index all messages (scan for "From " lines) without loading bodies
    void indexMessages();

    /// Load summary list (headers only) for a range of messages
    void loadMessages(int offset, int limit);

    /// Load full detail for a specific message
    void loadMessageDetail(int message_index);

    /// Cancel current operation
    void cancel();

    /// Get the path of the currently opened file
    [[nodiscard]] QString filePath() const;

    /// Get total indexed message count
    [[nodiscard]] int messageCount() const;

    // Synchronous API for worker threads
    [[nodiscard]] std::expected<QVector<sak::MboxMessage>, sak::error_code> readMessages(int offset,
                                                                                         int limit);

    [[nodiscard]] std::expected<sak::MboxMessageDetail, sak::error_code> readMessageDetail(
        int message_index);

    /// Read decoded attachment bytes for a specific message and attachment index. The index is the
    /// same recursive (DFS) enumeration as readMessageDetail's attachments list and
    /// readAllAttachments (so name[i] and bytes[i] always correspond, including inside nested
    /// multiparts).
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readAttachmentData(
        sak::MboxMessageIndex message_index, sak::MboxAttachmentIndex attachment_index);

    /// Read ALL attachments (name + decoded bytes) of a message in ONE recursive pass.
    /// Names and bytes are index-aligned by construction, and the raw message is read +
    /// MIME-parsed + base64-decoded only once (not once per attachment).
    [[nodiscard]] std::expected<QVector<MboxAttachmentPayload>, sak::error_code> readAllAttachments(
        int message_index);

Q_SIGNALS:
    void fileOpened(QString path, int estimated_count);
    void indexingComplete(int total_messages);
    void messagesLoaded(QVector<sak::MboxMessage> messages, int total_count);
    void messageDetailLoaded(sak::MboxMessageDetail detail);
    void progressUpdated(int percent, QString status);
    void errorOccurred(QString error);

private:
    // Serializes ALL access to the shared m_file cursor / m_message_offsets / m_attachment_sink.
    // The GUI thread (open/close/loadMessages/loadMessageDetail) and worker threads (readMessages/
    // readMessageDetail/readAttachmentData from search/export/attachment tasks) all touch the same
    // QFile seek position; without this they raced and returned mixed/corrupt data. Recursive so a
    // public entry point (readAttachmentData) may call another (readAllAttachments) without
    // self-deadlock. cancel() intentionally does NOT take it -- it only sets the atomic flag.
    mutable QRecursiveMutex m_file_mutex;
    QFile m_file;
    QVector<qint64> m_message_offsets;
    bool m_is_open = false;
    bool m_is_indexed = false;
    std::atomic<bool> m_cancelled{false};

    /// Non-null only for the duration of a readAllAttachments() parse: appendAttachment pushes each
    /// attachment's decoded bytes here so names (detail.attachments) and content stay index-aligned
    /// from ONE recursive pass. Never set for the GUI's readMessageDetail path (stays nullptr).
    QVector<QByteArray>* m_attachment_sink = nullptr;

    /// Set true by decodeTransferEncoding when a strict (AbortOnBase64DecodingErrors) base64 decode
    /// fails during a parseMimeMessage() pass. Reset at the start of every parse. The public detail
    /// / attachment reads inspect it afterward and fail closed rather than hand back partial bytes
    /// that Qt's permissive decoder would otherwise silently produce.
    bool m_mime_decode_failed = false;

    /// After a parseMimeMessage() pass, return the fail-closed error (cancellation or a strict
    /// base64 decode failure) that must abort the read, or nullopt when the parse was clean.
    [[nodiscard]] std::optional<sak::error_code> mimeParseFailure() const;

    /// Scan file for all "From " line boundaries
    void buildMessageIndex();

    /// Read raw message bytes for a given index
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readRawMessage(int message_index);

    /// Parse RFC 5322 headers from raw bytes
    [[nodiscard]] QMap<QString, QString> parseHeaders(const QByteArray& raw_message);

    /// Parse MIME structure for body and attachments. If m_attachment_sink is set (see
    /// readAllAttachments), each attachment's decoded content is appended to it in lock-step with
    /// detail.attachments, letting a caller recover the bytes from the SAME recursive enumeration.
    void parseMimeMessage(const QByteArray& raw_message, sak::MboxMessageDetail& detail);

    /// Handle a non-multipart message body
    void parseSinglePart(const QByteArray& body,
                         const QString& content_type,
                         const QString& transfer_encoding,
                         const QString& charset,
                         sak::MboxMessageDetail& detail);

    /// Process one MIME part (text or attachment)
    void processMimePart(const QByteArray& part,
                         sak::MboxMessageDetail& detail,
                         int& attachment_idx,
                         int depth = 0);
    void recurseMultipartPart(const QString& part_content_type,
                              const QByteArray& part_body,
                              sak::MboxMessageDetail& detail,
                              int& attachment_idx,
                              int depth);

    /// MIME part metadata for attachment processing
    struct MimePartInfo {
        QByteArray body;
        QString content_type;
        QString encoding;
        QString disposition;
        QMap<QString, QString> headers;
    };

    /// Build and append an attachment entry to detail. If m_attachment_sink is set, the decoded
    /// content (computed here anyway to size the attachment) is also appended to it, so it stays
    /// index-aligned with detail.attachments.
    void appendAttachment(const MimePartInfo& part,
                          sak::MboxMessageDetail& detail,
                          int& attachment_idx);

    /// Decode transfer encoding (base64, quoted-printable)
    [[nodiscard]] QByteArray decodeTransferEncoding(const QByteArray& data,
                                                    const QString& encoding);

    /// Decode character set to QString
    [[nodiscard]] QString decodeCharset(const QByteArray& data, const QString& charset);

    /// Parse a date string from email headers
    [[nodiscard]] QDateTime parseEmailDate(const QString& date_str);

    /// Check if a line is an MBOX "From " separator
    [[nodiscard]] static bool isFromLine(const QByteArray& line);
};
