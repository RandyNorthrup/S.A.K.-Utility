// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file imap_uploader.cpp
/// @brief IMAP client for uploading messages via APPEND

#include "sak/imap_uploader.h"

#include "sak/logger.h"

#include <QSslSocket>

#include <tuple>

namespace sak {

namespace {
constexpr int kImapDefaultTimeoutMs = 30'000;
constexpr int kImapReadBufferSize = 8192;
constexpr int kImapMaxMessageSize = 25 * 1024 * 1024;  // 25 MB
}  // namespace

// ======================================================================
// Construction / Destruction
// ======================================================================

ImapUploader::ImapUploader(QObject* parent) : QObject(parent) {}

ImapUploader::~ImapUploader() {
    disconnectFromServer();
}

// ======================================================================
// Public API
// ======================================================================

std::expected<void, error_code> ImapUploader::testConnection(const ImapServerConfig& config) {
    auto connect_result = connectAndAuth(config);
    if (!connect_result.has_value()) {
        return std::unexpected(connect_result.error());
    }

    // Send NOOP to verify
    auto noop_result = sendCommand(QStringLiteral("NOOP"), config.timeout_seconds * 1000);
    disconnectFromServer();

    if (!noop_result.has_value()) {
        return std::unexpected(noop_result.error());
    }
    return {};
}

std::expected<int, error_code> ImapUploader::uploadFolder(const ImapServerConfig& config,
                                                          const QString& target_folder,
                                                          const QVector<QByteArray>& eml_contents,
                                                          const QVector<QStringList>& flags_list,
                                                          const QVector<QDateTime>& dates) {
    Q_ASSERT(eml_contents.size() == flags_list.size());
    Q_ASSERT(eml_contents.size() == dates.size());

    auto connect_result = connectAndAuth(config);
    if (!connect_result.has_value()) {
        return std::unexpected(connect_result.error());
    }

    // Create the target folder (ignore error if already exists)
    std::ignore = createFolder(target_folder);

    int uploaded = 0;
    int failed = 0;
    int total = eml_contents.size();

    Q_EMIT uploadStarted(total);

    for (int i = 0; i < total; ++i) {
        if (m_cancelled.load()) {
            break;
        }

        if (eml_contents[i].size() > kImapMaxMessageSize) {
            logWarning("IMAP: message {} exceeds size limit, skipping", std::to_string(i));
            ++failed;
            continue;
        }

        auto append_result = appendMessage(target_folder, eml_contents[i], flags_list[i], dates[i]);

        if (append_result.has_value()) {
            ++uploaded;
        } else {
            ++failed;
            logWarning("IMAP: failed to upload message {}", std::to_string(i));
        }

        Q_EMIT uploadProgress(uploaded + failed, total, m_bytes_sent);
    }

    disconnectFromServer();
    Q_EMIT uploadComplete(uploaded, failed);

    return uploaded;
}

void ImapUploader::cancel() {
    m_cancelled.store(true);
}

// ======================================================================
// Connection
// ======================================================================

std::expected<void, error_code> ImapUploader::connectAndAuth(const ImapServerConfig& config) {
    disconnectFromServer();

    m_socket = new QSslSocket(this);
    m_tag_counter = 0;
    m_bytes_sent = 0;

    int timeout = config.timeout_seconds * 1000;
    if (timeout <= 0) {
        timeout = kImapDefaultTimeoutMs;
    }

    if (config.use_ssl) {
        m_socket->connectToHostEncrypted(config.host, config.port);
        if (!m_socket->waitForEncrypted(timeout)) {
            QString err = QStringLiteral("SSL handshake failed: ") + m_socket->errorString();
            logError("IMAP: {}", err.toStdString());
            disconnectFromServer();
            return std::unexpected(error_code::connection_failed);
        }
    } else {
        m_socket->connectToHost(config.host, config.port);
        if (!m_socket->waitForConnected(timeout)) {
            QString err = QStringLiteral("Connection failed: ") + m_socket->errorString();
            logError("IMAP: {}", err.toStdString());
            disconnectFromServer();
            return std::unexpected(error_code::connection_failed);
        }
    }

    // Read server greeting
    if (!m_socket->waitForReadyRead(timeout)) {
        disconnectFromServer();
        return std::unexpected(error_code::connection_failed);
    }
    QByteArray greeting = m_socket->readAll();
    if (!greeting.contains("OK")) {
        logError("IMAP: bad greeting: {}", greeting.toStdString());
        disconnectFromServer();
        return std::unexpected(error_code::connection_failed);
    }

    return authenticate(config);
}

std::expected<void, error_code> ImapUploader::validateAuthResponse(
    const std::expected<QString, error_code>& result) {
    if (!result.has_value()) {
        return std::unexpected(error_code::authentication_failed);
    }
    if (!result.value().contains(QStringLiteral("OK"))) {
        return std::unexpected(error_code::authentication_failed);
    }
    return {};
}

std::expected<void, error_code> ImapUploader::authenticate(const ImapServerConfig& config) {
    int timeout = config.timeout_seconds * 1000;
    if (timeout <= 0) {
        timeout = kImapDefaultTimeoutMs;
    }

    switch (config.auth_method) {
    case ImapAuthMethod::Plain: {
        QByteArray plain_data;
        plain_data.append('\0');
        plain_data.append(config.username.toUtf8());
        plain_data.append('\0');
        plain_data.append(config.password.toUtf8());

        QString cmd = QStringLiteral("AUTHENTICATE PLAIN ") +
                      QString::fromUtf8(plain_data.toBase64());
        auto check = validateAuthResponse(sendCommand(cmd, timeout));
        if (!check.has_value()) {
            return check;
        }
        break;
    }
    case ImapAuthMethod::Login: {
        QString cmd = QStringLiteral("LOGIN \"%1\" \"%2\"").arg(config.username, config.password);
        auto check = validateAuthResponse(sendCommand(cmd, timeout));
        if (!check.has_value()) {
            return check;
        }
        break;
    }
    case ImapAuthMethod::XOAuth2: {
        QByteArray xoauth2;
        xoauth2.append("user=");
        xoauth2.append(config.username.toUtf8());
        xoauth2.append('\x01');
        xoauth2.append("auth=Bearer ");
        xoauth2.append(config.password.toUtf8());
        xoauth2.append('\x01');
        xoauth2.append('\x01');

        QString cmd = QStringLiteral("AUTHENTICATE XOAUTH2 ") +
                      QString::fromUtf8(xoauth2.toBase64());
        auto check = validateAuthResponse(sendCommand(cmd, timeout));
        if (!check.has_value()) {
            return check;
        }
        break;
    }
    }

    logInfo("IMAP: authenticated as {}", config.username.toStdString());
    return {};
}

// ======================================================================
// IMAP Commands
// ======================================================================

std::expected<QString, error_code> ImapUploader::sendCommand(const QString& command,
                                                             int timeout_ms) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return std::unexpected(error_code::connection_failed);
    }

    ++m_tag_counter;
    QString tag = QStringLiteral("A%1").arg(m_tag_counter, 4, 10, QChar('0'));
    QString full_cmd = tag + QStringLiteral(" ") + command + QStringLiteral("\r\n");

    m_socket->write(full_cmd.toUtf8());
    if (!m_socket->waitForBytesWritten(timeout_ms)) {
        return std::unexpected(error_code::connection_failed);
    }

    // Read response until we get the tagged response line
    QString response;
    QByteArray tag_bytes = tag.toUtf8();
    while (m_socket->waitForReadyRead(timeout_ms)) {
        QByteArray data = m_socket->readAll();
        response += QString::fromUtf8(data);
        if (response.contains(tag)) {
            break;
        }
    }

    if (!response.contains(tag)) {
        return std::unexpected(error_code::connection_failed);
    }

    return response;
}

std::expected<void, error_code> ImapUploader::createFolder(const QString& folder_path) {
    QString cmd = QStringLiteral("CREATE \"%1\"").arg(folder_path);
    auto result = sendCommand(cmd, kImapDefaultTimeoutMs);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    // CREATE may fail with NO if folder exists â€” that's ok
    if (result.value().contains(QStringLiteral("OK"))) {
        Q_EMIT folderCreated(folder_path);
    }

    return {};
}

std::expected<void, error_code> ImapUploader::appendMessage(const QString& folder,
                                                            const QByteArray& eml_content,
                                                            const QStringList& flags,
                                                            const QDateTime& internal_date) {
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return std::unexpected(error_code::connection_failed);
    }

    ++m_tag_counter;
    QString tag = QStringLiteral("A%1").arg(m_tag_counter, 4, 10, QChar('0'));

    // Build APPEND command
    QString flags_str;
    if (!flags.isEmpty()) {
        flags_str = QStringLiteral("(") + flags.join(QStringLiteral(" ")) + QStringLiteral(") ");
    }

    QString date_str;
    if (internal_date.isValid()) {
        date_str = QStringLiteral("\"") + formatImapDate(internal_date) + QStringLiteral("\" ");
    }

    QString cmd = tag + QStringLiteral(" APPEND \"%1\" %2%3{%4}\r\n")
                            .arg(folder)
                            .arg(flags_str)
                            .arg(date_str)
                            .arg(eml_content.size());

    // Send the command (server responds with + continuation)
    m_socket->write(cmd.toUtf8());
    if (!m_socket->waitForBytesWritten(kImapDefaultTimeoutMs)) {
        return std::unexpected(error_code::connection_failed);
    }

    // Wait for continuation response (+)
    if (!m_socket->waitForReadyRead(kImapDefaultTimeoutMs)) {
        return std::unexpected(error_code::connection_failed);
    }
    QByteArray continuation = m_socket->readAll();
    if (!continuation.contains("+")) {
        return std::unexpected(error_code::connection_failed);
    }

    // Send the literal message data
    m_socket->write(eml_content);
    m_socket->write("\r\n");
    if (!m_socket->waitForBytesWritten(kImapDefaultTimeoutMs)) {
        return std::unexpected(error_code::connection_failed);
    }

    m_bytes_sent += eml_content.size();

    auto response_result = awaitTaggedResponse(tag);
    if (!response_result.has_value()) {
        return std::unexpected(error_code::connection_failed);
    }

    return {};
}

std::expected<QString, error_code> ImapUploader::awaitTaggedResponse(const QString& tag) {
    QString response;
    while (m_socket->waitForReadyRead(kImapDefaultTimeoutMs)) {
        QByteArray data = m_socket->readAll();
        response += QString::fromUtf8(data);
        if (response.contains(tag)) {
            break;
        }
    }

    if (!response.contains(tag + QStringLiteral(" OK"))) {
        return std::unexpected(error_code::connection_failed);
    }

    return response;
}

void ImapUploader::disconnectFromServer() {
    if (m_socket) {
        if (m_socket->state() == QAbstractSocket::ConnectedState) {
            // Send LOGOUT (best-effort)
            m_socket->write("A9999 LOGOUT\r\n");
            m_socket->waitForBytesWritten(2000);
        }
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

// ======================================================================
// Static helpers
// ======================================================================

QStringList ImapUploader::mapFlags(const PstItemDetail& item) {
    QStringList flags;

    // Check if PstItemDetail has read status
    // PR_MESSAGE_FLAGS & MSGFLAG_READ -> \Seen
    // Importance high -> \Flagged
    if (item.importance == 2) {
        flags.append(QStringLiteral("\\Flagged"));
    }

    // Default: mark as \Seen (most migrated mail is already read)
    flags.append(QStringLiteral("\\Seen"));

    return flags;
}

QString ImapUploader::formatImapDate(const QDateTime& date) {
    // Format: "25-Mar-2026 10:30:00 +0000"
    static const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    QDateTime utc = date.toUTC();
    return QStringLiteral("%1-%2-%3 %4:%5:%6 +0000")
        .arg(utc.date().day(), 2, 10, QChar('0'))
        .arg(QLatin1String(kMonths[utc.date().month() - 1]))
        .arg(utc.date().year())
        .arg(utc.time().hour(), 2, 10, QChar('0'))
        .arg(utc.time().minute(), 2, 10, QChar('0'))
        .arg(utc.time().second(), 2, 10, QChar('0'));
}

}  // namespace sak
