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

#include <QFile>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <expected>

namespace sak {
enum class error_code;
}

/// One attachment with its decoded content, produced by MboxParser::readAllAttachments in a
/// SINGLE recursive MIME pass. filename and data come from the same enumeration, so they always
/// correspond (unlike pairing readMessageDetail's recursive index with the non-recursive
/// readAttachmentData extractor).
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

    /// Read decoded attachment bytes for a specific message and attachment index
    [[nodiscard]] std::expected<QByteArray, sak::error_code> readAttachmentData(
        int message_index, int attachment_index);

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
    QFile m_file;
    QVector<qint64> m_message_offsets;
    bool m_is_open = false;
    bool m_is_indexed = false;
    std::atomic<bool> m_cancelled{false};

    /// Non-null only for the duration of a readAllAttachments() parse: appendAttachment pushes each
    /// attachment's decoded bytes here so names (detail.attachments) and content stay index-aligned
    /// from ONE recursive pass. Never set for the GUI's readMessageDetail path (stays nullptr).
    QVector<QByteArray>* m_attachment_sink = nullptr;

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

    /// Extract decoded bytes for a specific attachment index from raw MIME
    [[nodiscard]] std::expected<QByteArray, sak::error_code> extractAttachmentBytes(
        const QByteArray& raw_message, int attachment_index);

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
