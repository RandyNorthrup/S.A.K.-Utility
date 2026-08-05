// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file imap_uploader.cpp
/// @brief IMAP client for uploading messages via APPEND

#include "sak/imap_uploader.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QSemaphore>
#include <QSslSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <functional>
#include <tuple>
#include <utility>

namespace sak {

namespace {
constexpr int kImapDefaultTimeoutMs = 30'000;
constexpr qint64 kImapMaxTimeoutMs = 24LL * 60 * 60 * 1000;  // 24h ceiling
constexpr int kImapMaxMessageSize = 25 * static_cast<int>(kBytesPerMB);
constexpr int kImapDateTimeFieldWidth = kTimeFieldWidth;
constexpr int kImapDateTimeBase = kDecimalBase;
constexpr int kImapTagWidth = 4;
constexpr int kServerGreetingPreviewChars = 200;
constexpr int kPstHighImportanceValue = 2;

struct ImapSessionResult {
    bool success{false};
    int uploaded{0};
    int failed{0};
    error_code error{error_code::success};
    QString error_message;
};

/// Wrap a value as an RFC 3501 quoted string: backslash-escape `"` and `\`, and
/// strip CR/LF (which are illegal in a quoted string and would otherwise let a
/// crafted username/password or PST-derived folder name inject IMAP commands).
QString imapQuote(const QString& raw) {
    QString escaped = raw;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    escaped.remove(QLatin1Char('\r'));
    escaped.remove(QLatin1Char('\n'));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

// Thin file-local aliases so the session worker below reads cleanly; the
// definitions live on ImapUploader (exposed as testable static seams).
bool hasCompleteLineWithPrefix(const QString& buf, const QString& prefix) {
    return ImapUploader::hasCompleteLineWithPrefix(buf, prefix);
}
bool taggedLineIsOk(const QString& buf, const QString& tag) {
    return ImapUploader::taggedLineIsOk(buf, tag);
}
bool isValidImapFlag(const QString& flag) {
    return ImapUploader::isValidImapFlag(flag);
}

QString formatImapDateForAppend(const QDateTime& date) {
    static const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    const QDateTime utc = date.toUTC();
    return QStringLiteral("%1-%2-%3 %4:%5:%6 +0000")
        .arg(utc.date().day(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'))
        .arg(QLatin1String(kMonths[utc.date().month() - 1]))
        .arg(utc.date().year())
        .arg(utc.time().hour(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'))
        .arg(utc.time().minute(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'))
        .arg(utc.time().second(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'));
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
        if (!m_request.config->use_ssl) {
            // STARTTLS is not implemented, so use_ssl=false is pure plaintext: it
            // would send credentials and message bodies in the clear. Refuse the
            // session rather than leak them (B9-04).
            m_sinks.result->success = false;
            m_sinks.result->error = error_code::connection_failed;
            m_sinks.result->error_message =
                QStringLiteral("Refusing plaintext IMAP; enable SSL/TLS.");
            m_sinks.owner_thread->quit();
            m_sinks.done->release();
            return;
        }
        m_socket = new QSslSocket(this);
        m_timeoutTimer = new QTimer(this);
        m_timeoutTimer->setSingleShot(true);
        m_cancelTimer = new QTimer(this);
        m_cancelTimer->setInterval(kTimerPollingFastMs);
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
        if (config().timeout_seconds <= 0) {
            return kImapDefaultTimeoutMs;
        }
        // Compute in 64-bit and clamp: a large timeout_seconds * 1000 would overflow
        // a 32-bit int and could wrap to a tiny/negative timer interval.
        const qint64 ms = static_cast<qint64>(config().timeout_seconds) * kMillisecondsPerSecond;
        return static_cast<int>(std::min<qint64>(ms, kImapMaxTimeoutMs));
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
        m_currentTag =
            QStringLiteral("A%1").arg(m_tagCounter, kImapTagWidth, kDecimalBase, QChar('0'));
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
            // Success only when every message was appended. A CREATE/APPEND reject
            // or an oversized skip incremented `failed`; a migration that silently
            // dropped messages must not report success (B9-02).
            if (m_sinks.result->failed > 0) {
                finish(false,
                       error_code::partial_failure,
                       QStringLiteral("%1 of %2 message(s) failed to upload")
                           .arg(m_sinks.result->failed)
                           .arg(messages().size()));
            } else {
                finish(true);
            }
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
        m_currentTag =
            QStringLiteral("A%1").arg(m_tagCounter, kImapTagWidth, kDecimalBase, QChar('0'));
        const QString command = m_currentTag + QStringLiteral(" APPEND %1 %2%3{%4}\r\n")
                                                   .arg(imapQuote(m_request.target_folder),
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
        // Drop any flag that is not a valid IMAP atom before joining it into the
        // APPEND flag-list, so a crafted flag cannot break out of the parens or
        // inject a command (B9-03).
        QStringList valid;
        for (const QString& flag : flags().at(m_currentIndex)) {
            if (isValidImapFlag(flag)) {
                valid.append(flag);
            }
        }
        if (valid.isEmpty()) {
            return {};
        }
        return QStringLiteral("(") + valid.join(QStringLiteral(" ")) + QStringLiteral(") ");
    }

    QString dateForCurrentMessage() const {
        if (!dates().at(m_currentIndex).isValid()) {
            return {};
        }
        return QStringLiteral("\"") + formatImapDateForAppend(dates().at(m_currentIndex)) +
               QStringLiteral("\" ");
    }

    void handleAppendResponse(const QString& response) {
        if (taggedLineIsOk(response, m_currentTag)) {
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
            sendCommand(QStringLiteral("LOGIN %1 %2")
                            .arg(imapQuote(config().username), imapQuote(config().password)),
                        auth_done);
            break;
        case ImapAuthMethod::XOAuth2:
            sendCommand(xoauth2AuthCommand(), auth_done);
            break;
        default:
            // An out-of-range auth enum would otherwise send nothing and hang until
            // the timeout aborts the session. Fail closed immediately instead.
            finish(false,
                   error_code::invalid_argument,
                   QStringLiteral("Unsupported IMAP authentication method"));
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
        // Require the TAGGED OK: an untagged "* OK" preceding a tagged "Axxx NO"
        // would otherwise be read as a successful authentication.
        if (!taggedLineIsOk(response, m_currentTag)) {
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
        sendCommand(QStringLiteral("CREATE %1").arg(imapQuote(m_request.target_folder)),
                    [this](const QString& response) { handleCreateResponse(response); });
    }

    void handleNoopResponse(const QString& response) {
        if (!taggedLineIsOk(response, m_currentTag)) {
            failConnection(QStringLiteral("NOOP failed"));
            return;
        }
        finish(true);
    }

    void handleCreateResponse(const QString& response) {
        // Only signal a real creation on the tagged OK; a NO (folder exists) still
        // proceeds to append but must not fire folder_created.
        if (taggedLineIsOk(response, m_currentTag) && m_request.folder_created) {
            m_request.folder_created(m_request.target_folder);
        }
        appendNext();
    }

    void handleReadable() {
        const QByteArray chunk = m_socket->readAll();
        // The server is an UNTRUSTED network peer and every reader below waits for a
        // complete CRLF line, so a peer that streams bytes with no line terminator would
        // grow m_buffer without bound (memory exhaustion). Refuse the session once the
        // ceiling would be crossed -- the buffer is never truncated and parsed on, which
        // would decide a command's outcome from a half-read response.
        if (ImapUploader::responseBufferWouldOverflow(m_buffer.size(), chunk.size())) {
            failConnection(
                QStringLiteral("Server response exceeded the %1-character limit without a "
                               "complete line")
                    .arg(static_cast<qlonglong>(ImapUploader::kMaxResponseBufferChars)));
            return;
        }
        m_buffer += QString::fromUtf8(chunk);
        if (handleGreeting()) {
            return;
        }
        if (handleContinuation()) {
            return;
        }
        handleTaggedResponse();
    }

    /// True when the current tag's response line has arrived complete (the tag is
    /// present and a CRLF follows it), so a partial read is not decided early.
    bool bufferHasCompleteTaggedLine() const {
        if (m_currentTag.isEmpty()) {
            return false;
        }
        return hasCompleteLineWithPrefix(m_buffer, m_currentTag + QStringLiteral(" "));
    }

    bool handleGreeting() {
        if (!m_waitingGreeting) {
            return false;
        }
        // Wait for a complete greeting line before judging it (a partial read may
        // not yet contain the full first line).
        if (!m_buffer.contains(QStringLiteral("\r\n"))) {
            return true;
        }
        // Require a real untagged success greeting ("* OK"/"* PREAUTH"). A "* BYE"
        // or "* NO" greeting -- even one that happens to contain "OK" somewhere --
        // must not proceed to send credentials (loose-parse fix).
        if (!ImapUploader::isValidImapGreeting(m_buffer)) {
            failConnection(QStringLiteral("Bad server greeting: ") +
                           m_buffer.left(kServerGreetingPreviewChars));
            return true;
        }
        m_waitingGreeting = false;
        m_buffer.clear();
        authenticate();
        return true;
    }

    bool handleContinuation() {
        if (!m_waitingContinuation) {
            return false;
        }
        // A continuation request is a line that STARTS with '+' -- matching '+'
        // anywhere (e.g. inside an untagged line) would send the literal early
        // (B9-05).
        if (hasCompleteLineWithPrefix(m_buffer, QStringLiteral("+"))) {
            m_waitingContinuation = false;
            m_buffer.clear();
            if (m_socket->write(m_pendingLiteral) < 0) {
                failConnection(m_socket->errorString());
            } else {
                resetTimeout();
            }
            return true;
        }
        // The server may reject the APPEND outright with a tagged NO/BAD instead of
        // a '+' continuation; stop waiting and let handleTaggedResponse run rather
        // than hanging until the timeout aborts the whole upload.
        if (bufferHasCompleteTaggedLine()) {
            m_waitingContinuation = false;
            return false;
        }
        return true;  // continuation not yet complete
    }

    void handleTaggedResponse() {
        if (!m_taggedCallback || !bufferHasCompleteTaggedLine()) {
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
    // SAK-ALLOW-BLOCKING: `thread` is a stack local destroyed on return, and ~QThread on
    // a live thread aborts the process, so this join is not optional. The worker has
    // already released `done`, so the quit-less exec() has nothing left to run.
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
    m_cancelled.store(false);
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
    // The worker walks all three vectors by the same index; if the caller passes
    // mismatched lengths the release build (where the asserts compile out) would
    // drive an out-of-range QVector::at() -> OOB read. Fail closed instead (B9-01).
    if (eml_contents.size() != flags_list.size() || eml_contents.size() != dates.size()) {
        Q_EMIT errorOccurred(QStringLiteral("Message, flag, and date counts must match."));
        return std::unexpected(error_code::invalid_argument);
    }
    m_cancelled.store(false);

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
    if (result.error == error_code::operation_cancelled) {
        // A cancelled upload is not a success: surface it so the caller does not
        // treat the partial uploaded count as a completed transfer.
        return std::unexpected(error_code::operation_cancelled);
    }
    if (!result.success) {
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
    m_cancelled.store(false);
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
    int timeout = config.timeout_seconds * kMillisecondsPerSecond;
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

bool ImapUploader::hasCompleteLineWithPrefix(const QString& buf, const QString& prefix) {
    int start = 0;
    while (true) {
        const int nl = buf.indexOf(QStringLiteral("\r\n"), start);
        if (nl < 0) {
            return false;  // no complete line remains
        }
        if (QStringView(buf).sliced(start, nl - start).startsWith(prefix)) {
            return true;
        }
        start = nl + 2;
    }
}

bool ImapUploader::taggedLineIsOk(const QString& buf, const QString& tag) {
    return hasCompleteLineWithPrefix(buf, tag + QStringLiteral(" OK"));
}

bool ImapUploader::isValidImapGreeting(const QString& buf) {
    return hasCompleteLineWithPrefix(buf, QStringLiteral("* OK")) ||
           hasCompleteLineWithPrefix(buf, QStringLiteral("* PREAUTH"));
}

bool ImapUploader::responseBufferWouldOverflow(qsizetype buffered_chars, qsizetype incoming_bytes) {
    if (buffered_chars < 0 || incoming_bytes < 0) {
        return true;  // a nonsensical size fails closed
    }
    // incoming_bytes counts BYTES and buffered_chars counts decoded characters; UTF-8 never
    // decodes to more characters than it has bytes, so comparing the sum is conservative.
    return buffered_chars + incoming_bytes > kMaxResponseBufferChars;
}

bool ImapUploader::isValidImapFlag(const QString& flag) {
    if (flag.isEmpty()) {
        return false;
    }
    int i = 0;
    if (flag.at(0) == QLatin1Char('\\')) {
        if (flag.size() == 1) {
            return false;  // a bare backslash is not a flag
        }
        i = 1;
    }
    for (; i < flag.size(); ++i) {
        const QChar ch = flag.at(i);
        if (ch < QChar(0x21) || ch > QChar(0x7E) || QStringLiteral("(){ %*\"\\").contains(ch)) {
            return false;
        }
    }
    return true;
}

QStringList ImapUploader::mapFlags(const PstItemDetail& item) {
    QStringList flags;

    // Check if PstItemDetail has read status
    // PR_MESSAGE_FLAGS & MSGFLAG_READ -> \Seen
    // Importance high -> \Flagged
    if (item.importance == kPstHighImportanceValue) {
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
        .arg(utc.date().day(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'))
        .arg(QLatin1String(kMonths[utc.date().month() - 1]))
        .arg(utc.date().year())
        .arg(utc.time().hour(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'))
        .arg(utc.time().minute(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'))
        .arg(utc.time().second(), kImapDateTimeFieldWidth, kImapDateTimeBase, QChar('0'));
}

}  // namespace sak
