// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file imap_uploader.cpp
/// @brief IMAP client for uploading messages via APPEND

#include "sak/imap_uploader.h"

#include "sak/logger.h"

#include <QSemaphore>
#include <QSslSocket>
#include <QThread>
#include <QTimer>

#include <functional>
#include <tuple>
#include <utility>

namespace sak {

namespace {
constexpr int kImapDefaultTimeoutMs = 30'000;
constexpr int kImapReadBufferSize = 8192;
constexpr int kImapMaxMessageSize = 25 * 1024 * 1024;  // 25 MB

struct ImapSessionResult {
    bool success{false};
    int uploaded{0};
    int failed{0};
    error_code error{error_code::success};
    QString error_message;
};

QString formatImapDateForAppend(const QDateTime& date) {
    static const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const QDateTime utc = date.toUTC();
    return QStringLiteral("%1-%2-%3 %4:%5:%6 +0000")
        .arg(utc.date().day(), 2, 10, QChar('0'))
        .arg(QLatin1String(kMonths[utc.date().month() - 1]))
        .arg(utc.date().year())
        .arg(utc.time().hour(), 2, 10, QChar('0'))
        .arg(utc.time().minute(), 2, 10, QChar('0'))
        .arg(utc.time().second(), 2, 10, QChar('0'));
}

struct ImapSessionRequest {
    const ImapServerConfig* config{nullptr};
    bool upload_mode{false};
    QString target_folder;
    const QVector<QByteArray>* eml_contents{nullptr};
    const QVector<QStringList>* flags_list{nullptr};
    const QVector<QDateTime>* dates{nullptr};
    const std::atomic<bool>* cancelled{nullptr};
    std::function<void(int)> upload_started;
    std::function<void(int, int, qint64)> upload_progress;
    std::function<void(const QString&)> folder_created;
};

struct ImapSessionSinks {
    ImapSessionResult* result{nullptr};
    QSemaphore* done{nullptr};
    QThread* owner_thread{nullptr};
};

class ImapSessionWorker final : public QObject {
public:
    ImapSessionWorker(ImapSessionRequest request, ImapSessionSinks sinks)
        : m_request(std::move(request)), m_sinks(sinks) {}

    void start() {
        m_socket = new QSslSocket(this);
        m_timeoutTimer = new QTimer(this);
        m_timeoutTimer->setSingleShot(true);
        m_cancelTimer = new QTimer(this);
        m_cancelTimer->setInterval(100);
        connectSignals();
        m_cancelTimer->start();
        resetTimeout();
        startSocket();
    }

private:
    const ImapServerConfig& config() const { return *m_request.config; }
    const QVector<QByteArray>& messages() const { return *m_request.eml_contents; }
    const QVector<QStringList>& flags() const { return *m_request.flags_list; }
    const QVector<QDateTime>& dates() const { return *m_request.dates; }

    int timeoutMs() const {
        return config().timeout_seconds > 0 ? config().timeout_seconds * 1000
                                            : kImapDefaultTimeoutMs;
    }

    void finish(bool success, error_code code = error_code::success, const QString& message = {}) {
        if (m_finished) {
            return;
        }
        m_finished = true;
        m_timeoutTimer->stop();
        m_cancelTimer->stop();
        m_sinks.result->success = success;
        m_sinks.result->error = code;
        m_sinks.result->error_message = message;
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_sinks.owner_thread->quit();
        m_sinks.done->release();
    }

    void failConnection(const QString& message) {
        logError("IMAP: {}", message.toStdString());
        finish(false, error_code::connection_failed, message);
    }

    void resetTimeout() { m_timeoutTimer->start(timeoutMs()); }

    void sendCommand(const QString& command, std::function<void(const QString&)> callback) {
        ++m_tagCounter;
        m_currentTag = QStringLiteral("A%1").arg(m_tagCounter, 4, 10, QChar('0'));
        m_taggedCallback = std::move(callback);
        m_waitingContinuation = false;
        m_buffer.clear();
        const QString full = m_currentTag + QStringLiteral(" ") + command + QStringLiteral("\r\n");
        if (m_socket->write(full.toUtf8()) < 0) {
            failConnection(m_socket->errorString());
            return;
        }
        resetTimeout();
    }

    void appendNext() {
        if (m_request.cancelled->load()) {
            finish(false, error_code::operation_cancelled, QStringLiteral("Upload cancelled"));
            return;
        }
        if (m_currentIndex >= messages().size()) {
            finish(true);
            return;
        }
        if (skipOversizedMessage()) {
            appendNext();
            return;
        }
        sendAppendCommand();
    }

    bool skipOversizedMessage() {
        if (messages().at(m_currentIndex).size() <= kImapMaxMessageSize) {
            return false;
        }
        logWarning("IMAP: message {} exceeds size limit, skipping", std::to_string(m_currentIndex));
        ++m_sinks.result->failed;
        ++m_currentIndex;
        notifyUploadProgress();
        return true;
    }

    void sendAppendCommand() {
        ++m_tagCounter;
        m_currentTag = QStringLiteral("A%1").arg(m_tagCounter, 4, 10, QChar('0'));
        const QString command = m_currentTag + QStringLiteral(" APPEND \"%1\" %2%3{%4}\r\n")
                                                   .arg(m_request.target_folder,
                                                        flagsForCurrentMessage(),
                                                        dateForCurrentMessage())
                                                   .arg(messages().at(m_currentIndex).size());
        m_pendingLiteral = messages().at(m_currentIndex) + "\r\n";
        m_buffer.clear();
        m_waitingContinuation = true;
        m_taggedCallback = [this](const QString& response) {
            handleAppendResponse(response);
        };
        if (m_socket->write(command.toUtf8()) < 0) {
            failConnection(m_socket->errorString());
            return;
        }
        resetTimeout();
    }

    QString flagsForCurrentMessage() const {
        if (flags().at(m_currentIndex).isEmpty()) {
            return {};
        }
        return QStringLiteral("(") + flags().at(m_currentIndex).join(QStringLiteral(" ")) +
               QStringLiteral(") ");
    }

    QString dateForCurrentMessage() const {
        if (!dates().at(m_currentIndex).isValid()) {
            return {};
        }
        return QStringLiteral("\"") + formatImapDateForAppend(dates().at(m_currentIndex)) +
               QStringLiteral("\" ");
    }

    void handleAppendResponse(const QString& response) {
        if (response.contains(m_currentTag + QStringLiteral(" OK"))) {
            ++m_sinks.result->uploaded;
            m_bytesSent += messages().at(m_currentIndex).size();
        } else {
            ++m_sinks.result->failed;
            logWarning("IMAP: failed to upload message {}", std::to_string(m_currentIndex));
        }
        ++m_currentIndex;
        notifyUploadProgress();
        appendNext();
    }

    void notifyUploadProgress() {
        if (m_request.upload_progress) {
            m_request.upload_progress(m_sinks.result->uploaded + m_sinks.result->failed,
                                      messages().size(),
                                      m_bytesSent);
        }
    }

    void authenticate() {
        const auto auth_done = [this](const QString& response) {
            handleAuthResponse(response);
        };
        switch (config().auth_method) {
        case ImapAuthMethod::Plain:
            sendCommand(plainAuthCommand(), auth_done);
            break;
        case ImapAuthMethod::Login:
            sendCommand(
                QStringLiteral("LOGIN \"%1\" \"%2\"").arg(config().username, config().password),
                auth_done);
            break;
        case ImapAuthMethod::XOAuth2:
            sendCommand(xoauth2AuthCommand(), auth_done);
            break;
        }
    }

    QString plainAuthCommand() const {
        QByteArray plain_data;
        plain_data.append('\0');
        plain_data.append(config().username.toUtf8());
        plain_data.append('\0');
        plain_data.append(config().password.toUtf8());
        return QStringLiteral("AUTHENTICATE PLAIN ") + QString::fromUtf8(plain_data.toBase64());
    }

    QString xoauth2AuthCommand() const {
        QByteArray xoauth2;
        xoauth2.append("user=");
        xoauth2.append(config().username.toUtf8());
        xoauth2.append('\x01');
        xoauth2.append("auth=Bearer ");
        xoauth2.append(config().password.toUtf8());
        xoauth2.append('\x01');
        xoauth2.append('\x01');
        return QStringLiteral("AUTHENTICATE XOAUTH2 ") + QString::fromUtf8(xoauth2.toBase64());
    }

    void handleAuthResponse(const QString& response) {
        if (!response.contains(QStringLiteral("OK"))) {
            finish(false,
                   error_code::authentication_failed,
                   QStringLiteral("Authentication failed"));
            return;
        }
        logInfo("IMAP: authenticated as {}", config().username.toStdString());
        continueAfterAuth();
    }

    void continueAfterAuth() {
        if (!m_request.upload_mode) {
            sendCommand(QStringLiteral("NOOP"),
                        [this](const QString& response) { handleNoopResponse(response); });
            return;
        }
        if (m_request.upload_started) {
            m_request.upload_started(messages().size());
        }
        sendCommand(QStringLiteral("CREATE \"%1\"").arg(m_request.target_folder),
                    [this](const QString& response) { handleCreateResponse(response); });
    }

    void handleNoopResponse(const QString& response) {
        if (!response.contains(QStringLiteral("OK"))) {
            failConnection(QStringLiteral("NOOP failed"));
            return;
        }
        finish(true);
    }

    void handleCreateResponse(const QString& response) {
        if (response.contains(QStringLiteral("OK")) && m_request.folder_created) {
            m_request.folder_created(m_request.target_folder);
        }
        appendNext();
    }

    void handleReadable() {
        m_buffer += QString::fromUtf8(m_socket->readAll());
        if (handleGreeting()) {
            return;
        }
        if (handleContinuation()) {
            return;
        }
        handleTaggedResponse();
    }

    bool handleGreeting() {
        if (!m_waitingGreeting) {
            return false;
        }
        if (!m_buffer.contains(QStringLiteral("OK"))) {
            failConnection(QStringLiteral("Bad server greeting: ") + m_buffer.left(200));
            return true;
        }
        m_waitingGreeting = false;
        m_buffer.clear();
        authenticate();
        return true;
    }

    bool handleContinuation() {
        if (!m_waitingContinuation || !m_buffer.contains(QStringLiteral("+"))) {
            return m_waitingContinuation;
        }
        m_waitingContinuation = false;
        m_buffer.clear();
        if (m_socket->write(m_pendingLiteral) < 0) {
            failConnection(m_socket->errorString());
            return true;
        }
        resetTimeout();
        return true;
    }

    void handleTaggedResponse() {
        if (m_currentTag.isEmpty() || !m_taggedCallback || !m_buffer.contains(m_currentTag)) {
            return;
        }
        const QString response = m_buffer;
        m_buffer.clear();
        auto callback = std::move(m_taggedCallback);
        m_taggedCallback = {};
        m_timeoutTimer->stop();
        callback(response);
    }

    void connectSignals() {
        QObject::connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
            failConnection(QStringLiteral("IMAP operation timed out"));
        });
        QObject::connect(m_cancelTimer, &QTimer::timeout, this, [this]() {
            if (m_request.cancelled->load()) {
                finish(false, error_code::operation_cancelled, QStringLiteral("Upload cancelled"));
            }
        });
        QObject::connect(m_socket, &QSslSocket::encrypted, this, [this]() { beginGreetingWait(); });
        QObject::connect(m_socket, &QTcpSocket::connected, this, [this]() {
            if (!config().use_ssl) {
                beginGreetingWait();
            }
        });
        QObject::connect(m_socket, &QTcpSocket::readyRead, this, [this]() { handleReadable(); });
        QObject::connect(m_socket,
                         &QTcpSocket::errorOccurred,
                         this,
                         [this](QAbstractSocket::SocketError error) { handleSocketError(error); });
    }

    void beginGreetingWait() {
        m_waitingGreeting = true;
        resetTimeout();
    }

    void handleSocketError(QAbstractSocket::SocketError error) {
        if (m_finished || error == QAbstractSocket::RemoteHostClosedError) {
            return;
        }
        failConnection(m_socket->errorString());
    }

    void startSocket() {
        if (config().use_ssl) {
            m_socket->connectToHostEncrypted(config().host, config().port);
        } else {
            m_socket->connectToHost(config().host, config().port);
        }
    }

    ImapSessionRequest m_request;
    ImapSessionSinks m_sinks;
    QSslSocket* m_socket{nullptr};
    QTimer* m_timeoutTimer{nullptr};
    QTimer* m_cancelTimer{nullptr};
    int m_tagCounter{0};
    int m_currentIndex{0};
    qint64 m_bytesSent{0};
    QString m_currentTag;
    QString m_buffer;
    bool m_finished{false};
    bool m_waitingGreeting{false};
    bool m_waitingContinuation{false};
    std::function<void(const QString&)> m_taggedCallback;
    QByteArray m_pendingLiteral;
};

ImapSessionResult runImapSession(const ImapSessionRequest& request) {
    ImapSessionResult result;
    QThread thread;
    QSemaphore done;
    auto* worker =
        new ImapSessionWorker(request, {.result = &result, .done = &done, .owner_thread = &thread});
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&thread, &QThread::started, worker, [worker]() { worker->start(); });
    thread.start();
    done.acquire();
    thread.wait();
    return result;
}
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
    const QVector<QByteArray> messages;
    const QVector<QStringList> flags;
    const QVector<QDateTime> dates;
    const ImapSessionResult result = runImapSession({.config = &config,
                                                     .upload_mode = false,
                                                     .eml_contents = &messages,
                                                     .flags_list = &flags,
                                                     .dates = &dates,
                                                     .cancelled = &m_cancelled});
    if (!result.success) {
        Q_EMIT errorOccurred(result.error_message);
        return std::unexpected(result.error);
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

    const ImapSessionResult result = runImapSession(
        {.config = &config,
         .upload_mode = true,
         .target_folder = target_folder,
         .eml_contents = &eml_contents,
         .flags_list = &flags_list,
         .dates = &dates,
         .cancelled = &m_cancelled,
         .upload_started = [this](int total) { Q_EMIT uploadStarted(total); },
         .upload_progress =
             [this](int done, int total, qint64 bytes_sent) {
                 m_bytes_sent = bytes_sent;
                 Q_EMIT uploadProgress(done, total, bytes_sent);
             },
         .folder_created = [this](const QString& folder) { Q_EMIT folderCreated(folder); }});

    Q_EMIT uploadComplete(result.uploaded, result.failed);
    if (!result.success && result.error != error_code::operation_cancelled) {
        Q_EMIT errorOccurred(result.error_message);
        return std::unexpected(result.error);
    }

    return result.uploaded;
}

void ImapUploader::cancel() {
    m_cancelled.store(true);
}

// ======================================================================
// Connection
// ======================================================================

std::expected<void, error_code> ImapUploader::connectAndAuth(const ImapServerConfig& config) {
    const QVector<QByteArray> messages;
    const QVector<QStringList> flags;
    const QVector<QDateTime> dates;
    const ImapSessionResult result = runImapSession({.config = &config,
                                                     .upload_mode = false,
                                                     .eml_contents = &messages,
                                                     .flags_list = &flags,
                                                     .dates = &dates,
                                                     .cancelled = &m_cancelled});
    if (!result.success) {
        return std::unexpected(result.error);
    }
    return {};
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
    Q_UNUSED(command)
    Q_UNUSED(timeout_ms)
    return std::unexpected(error_code::connection_failed);
}

std::expected<void, error_code> ImapUploader::createFolder(const QString& folder_path) {
    QString cmd = QStringLiteral("CREATE \"%1\"").arg(folder_path);
    auto result = sendCommand(cmd, kImapDefaultTimeoutMs);
    if (!result.has_value()) {
        return std::unexpected(result.error());
    }

    // CREATE may fail with NO if folder exists -- that's ok
    if (result.value().contains(QStringLiteral("OK"))) {
        Q_EMIT folderCreated(folder_path);
    }

    return {};
}

std::expected<void, error_code> ImapUploader::appendMessage(const QString& folder,
                                                            const QByteArray& eml_content,
                                                            const QStringList& flags,
                                                            const QDateTime& internal_date) {
    Q_UNUSED(folder)
    Q_UNUSED(eml_content)
    Q_UNUSED(flags)
    Q_UNUSED(internal_date)
    return std::unexpected(error_code::connection_failed);
}

std::expected<QString, error_code> ImapUploader::awaitTaggedResponse(const QString& tag) {
    Q_UNUSED(tag)
    return std::unexpected(error_code::connection_failed);
}

void ImapUploader::disconnectFromServer() {
    if (m_socket) {
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
