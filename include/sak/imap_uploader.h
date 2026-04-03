// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file imap_uploader.h
/// @brief IMAP client for uploading converted emails to a mail server

#pragma once

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/ost_converter_types.h"

#include <QObject>

#include <atomic>
#include <expected>

class QSslSocket;

namespace sak {

/// @brief Uploads messages to an IMAP server via APPEND
///
/// Supports PLAIN, LOGIN, and XOAUTH2 authentication.
/// Creates folder hierarchy on the server and uploads messages
/// with correct flags and internal dates.
class ImapUploader : public QObject {
    Q_OBJECT

public:
    explicit ImapUploader(QObject* parent = nullptr);
    ~ImapUploader() override;

    // Non-copyable, non-movable
    ImapUploader(const ImapUploader&) = delete;
    ImapUploader& operator=(const ImapUploader&) = delete;
    ImapUploader(ImapUploader&&) = delete;
    ImapUploader& operator=(ImapUploader&&) = delete;

    /// Test the connection without uploading
    [[nodiscard]] std::expected<void, error_code> testConnection(const ImapServerConfig& config);

    /// Upload messages for a single folder
    [[nodiscard]] std::expected<int, error_code> uploadFolder(
        const ImapServerConfig& config,
        const QString& target_folder,
        const QVector<QByteArray>& eml_contents,
        const QVector<QStringList>& flags_list,
        const QVector<QDateTime>& dates);

    /// Cancel the upload
    void cancel();

Q_SIGNALS:
    void uploadStarted(int total_items);
    void uploadProgress(int items_done, int total_items, qint64 bytes_sent);
    void folderCreated(QString folder_name);
    void uploadComplete(int items_uploaded, int items_failed);
    void errorOccurred(QString error);

private:
    /// Connect and authenticate
    [[nodiscard]] std::expected<void, error_code> connectAndAuth(const ImapServerConfig& config);

    /// Send an IMAP command and wait for the tagged response
    [[nodiscard]] std::expected<QString, error_code> sendCommand(const QString& command,
                                                                 int timeout_ms);

    /// Authenticate with the server
    [[nodiscard]] std::expected<void, error_code> authenticate(const ImapServerConfig& config);

    /// Validate an authentication command response
    [[nodiscard]] static std::expected<void, error_code> validateAuthResponse(
        const std::expected<QString, error_code>& result);

    /// Create a folder on the server (IMAP CREATE)
    [[nodiscard]] std::expected<void, error_code> createFolder(const QString& folder_path);

    /// Upload a single message (IMAP APPEND with flags and date)
    [[nodiscard]] std::expected<void, error_code> appendMessage(const QString& folder,
                                                                const QByteArray& eml_content,
                                                                const QStringList& flags,
                                                                const QDateTime& internal_date);

    /// Disconnect and clean up
    void disconnectFromServer();

    /// Wait for a tagged IMAP response containing OK
    [[nodiscard]] std::expected<QString, error_code> awaitTaggedResponse(const QString& tag);

    /// Map PST flags to IMAP flags
    [[nodiscard]] static QStringList mapFlags(const PstItemDetail& item);

    /// Format a date for IMAP APPEND internal date
    [[nodiscard]] static QString formatImapDate(const QDateTime& date);

    QSslSocket* m_socket = nullptr;
    int m_tag_counter = 0;
    std::atomic<bool> m_cancelled{false};
    qint64 m_bytes_sent = 0;
};

}  // namespace sak
