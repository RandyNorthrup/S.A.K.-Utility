// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_stdio_session.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/contained_process_quarantine.h"
#include "sak/layout_constants.h"
#include "sak/windows_process_job.h"

#include <QDeadlineTimer>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QMetaObject>
#include <QProcess>
#include <QSemaphore>
#include <QSet>
#include <QThread>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

#include <private/qobject_p.h>
#include <private/qprocess_p.h>
#include <private/qwindowspipereader_p.h>

namespace sak::ai {

namespace {

constexpr int kMinimumRequestTimeoutMs = sak::kMillisecondsPerSecond;
constexpr int kMcpStdioErrorPreviewChars = 2000;
// Abort a response read once this many bytes buffer without a newline, so a server
// streaming a newline-free byte stream cannot grow the QProcess buffer until OOM.
constexpr qint64 kMaxStdioReadBufferBytes = 8 * 1024 * 1024;  // 8 MiB
constexpr qsizetype kMaxStderrTailBytes = 64 * 1024;          // 64 KiB
constexpr int kMaxToolsListPages = 128;
constexpr qsizetype kMaxDiscoveredTools = 4096;
constexpr int kProcessCleanupTimeoutMs = 5000;
constexpr qint64 kProcessCleanupPollMs = 25;

class BoundedPipeDevice final : public QIODevice {
public:
    explicit BoundedPipeDevice(QWindowsPipeReader* reader) : m_reader(reader) {
        open(QIODevice::ReadOnly);
    }

    [[nodiscard]] bool isSequential() const override { return true; }
    [[nodiscard]] qint64 bytesAvailable() const override {
        return QIODevice::bytesAvailable() + (m_reader ? m_reader->bytesAvailable() : 0);
    }
    [[nodiscard]] bool canReadLine() const override {
        return QIODevice::canReadLine() || (m_reader && m_reader->canReadLine());
    }

protected:
    qint64 readData(char* data, qint64 max_length) override {
        return m_reader ? m_reader->read(data, max_length) : -1;
    }
    qint64 readLineData(char* data, qint64 max_length) override {
        if (!m_reader || max_length <= 0 || max_length > kMaxStdioReadBufferBytes) {
            return -1;
        }
        // QWindowsPipeReader reserves one byte for its terminating NUL, while
        // QIODevice::readLineData's max_length counts payload bytes. Supply
        // that extra byte so Qt's incremental long-line reader sees a full
        // chunk and continues until the actual newline.
        QByteArray chunk(static_cast<qsizetype>(max_length + 1), '\0');
        const qint64 read = m_reader->readLine(chunk.data(), chunk.size());
        if (read > 0) {
            std::memcpy(data, chunk.constData(), static_cast<size_t>(read));
        }
        return read;
    }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QPointer<QWindowsPipeReader> m_reader;
};

void startSessionThread(QThread& thread) {
    thread.start();
}

std::expected<AiMcpToolDescriptor, QString> parseToolDescriptor(const QJsonValue& entry) {
    if (!entry.isObject()) {
        return std::unexpected(QStringLiteral("MCP tools/list contains a non-object tool"));
    }
    const QJsonObject object = entry.toObject();
    const QString name = object.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        return std::unexpected(QStringLiteral("MCP tools/list contains a tool with no name"));
    }
    const QJsonValue description = object.value(QStringLiteral("description"));
    if (!description.isUndefined() && !description.isString()) {
        return std::unexpected(QStringLiteral("MCP tools/list contains a non-string description"));
    }
    const QJsonValue input_schema = object.value(QStringLiteral("inputSchema"));
    if (!input_schema.isUndefined() && !input_schema.isObject()) {
        return std::unexpected(QStringLiteral("MCP tools/list contains a non-object inputSchema"));
    }
    return AiMcpToolDescriptor{.name = name,
                               .description = description.toString(),
                               .input_schema = input_schema.toObject()};
}

std::expected<std::optional<QJsonObject>, QString> parseMatchingLine(const QByteArray& line,
                                                                     int id) {
    if (line.isEmpty()) {
        return {};
    }
    const auto message = mcp::parseJsonLine(line);
    if (!message || message->value(QStringLiteral("id")).toInt(-1) != id) {
        return {};
    }
    if (message->contains(QStringLiteral("error"))) {
        return std::unexpected(mcp::jsonRpcErrorMessage(*message));
    }
    if (!message->value(QStringLiteral("result")).isObject()) {
        return std::unexpected(QStringLiteral("MCP stdio response missing a result object"));
    }
    return std::optional<QJsonObject>{*message};
}

std::expected<QByteArray, QString> readCompleteLine(QIODevice& input, qint64 buffered) {
    // canReadLine() established a newline in these already-buffered bytes. A
    // bounded read avoids extending allocation as a producer refills the pipe.
    const QByteArray line = input.readLine(buffered + 1);
    if (!line.endsWith('\n')) {
        return std::unexpected(QStringLiteral("MCP stdio response line became incomplete"));
    }
    return line.trimmed();
}

std::expected<std::optional<QJsonObject>, QString> parseLineWithinDeadline(
    QIODevice& input,
    qint64 buffered,
    int id,
    const QDeadlineTimer& deadline,
    const std::function<QString()>& timeout_message) {
    const auto line = readCompleteLine(input, buffered);
    if (!line) {
        return std::unexpected(line.error());
    }
    if (deadline.hasExpired()) {
        return std::unexpected(timeout_message());
    }
    const auto matched = parseMatchingLine(*line, id);
    if (deadline.hasExpired()) {
        return std::unexpected(timeout_message());
    }
    return matched;
}

}  // namespace

// Owns the server QProcess and performs all I/O. Lives on the session's dedicated
// worker thread; public requests use blocking queued calls and close uses an
// owned, deadline-bound queued completion. QProcess is touched on one thread,
// and public calls remain sequential -- at most one request is outstanding.
struct AiMcpStdioSession::PendingCleanup {
    QSemaphore completed;
    std::expected<void, QString> result;
};

// Check each buffered line against one request deadline. Non-matching replies,
// notifications, and server log lines are noise, never a reason to extend time.
std::expected<std::optional<QJsonObject>, QString> AiMcpStdioSession::drainMatchingLines(
    QIODevice& input,
    int id,
    const QDeadlineTimer& deadline,
    const std::function<QString()>& timeout_message) {
    for (;;) {
        if (deadline.hasExpired()) {
            return std::unexpected(timeout_message());
        }
        const qint64 buffered = input.bytesAvailable();
        if (buffered > kMaxStdioReadBufferBytes ||
            (buffered == kMaxStdioReadBufferBytes && !input.canReadLine())) {
            return std::unexpected(
                QStringLiteral("MCP stdio response exceeded %1 bytes without a newline")
                    .arg(kMaxStdioReadBufferBytes));
        }
        if (!input.canReadLine()) {
            return {};
        }
        const auto matched =
            parseLineWithinDeadline(input, buffered, id, deadline, timeout_message);
        if (!matched || matched->has_value()) {
            return matched;
        }
    }
}

class AiMcpStdioSession::Worker : public QObject {
public:
    Worker(std::shared_ptr<std::atomic_bool> open_state,
           int initial_request_id,
           JobFactory job_factory)
        : m_open_state(std::move(open_state))
        , m_next_id(initial_request_id)
        , m_job_factory(std::move(job_factory)) {}

    ~Worker() override {
        m_open_state->store(false, std::memory_order_release);
        // Invariant, not a cleanup timeout: this Worker is deleted only after its controller
        // finishes, and the session quits that controller only after doClose() proved the Job
        // empty. An unproven cleanup keeps the controller running in custody instead, so
        // reaching this means a live QProcess or non-empty Job is about to be destroyed.
        if (m_job || (m_process != nullptr && m_process->state() != QProcess::NotRunning)) {
            qFatal("MCP worker destruction refused: process cleanup is unproven");
        }
    }

    std::expected<void, QString> doOpen(const AiMcpSessionConfig& config) {
        m_open_state->store(false, std::memory_order_release);
        m_timeout_ms = AiMcpStdioSession::effectiveRequestTimeoutMs(config.request_timeout_ms);
        const QString command = config.command.trimmed();
        if (command.isEmpty() || !QFileInfo::exists(command)) {
            return std::unexpected(QStringLiteral("MCP stdio command missing: %1").arg(command));
        }
        const auto initialize_id = nextId();
        if (!initialize_id) {
            return std::unexpected(initialize_id.error());
        }
        const auto started = startContainedProcess(config, command);
        if (!started) {
            return std::unexpected(started.error());
        }
        const auto initialized = initializeProtocol(*initialize_id);
        if (!initialized) {
            return std::unexpected(initialized.error());
        }
        m_open_state->store(m_process->state() == QProcess::Running, std::memory_order_release);
        return {};
    }

    std::expected<void, QString> startContainedProcess(const AiMcpSessionConfig& config,
                                                       const QString& command) {
        auto job = m_job_factory();
        if (!job) {
            return std::unexpected(job.error());
        }
        m_job = std::move(*job);
        if (!m_job) {
            return std::unexpected(QStringLiteral("MCP containment factory returned no Job"));
        }
        m_process = new QProcess(this);
        m_process->setProgram(command);
        m_process->setProcessEnvironment(config.environment);
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        m_process->setReadChannel(QProcess::StandardOutput);
        QObject::connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            m_open_state->store(false, std::memory_order_release);
        });
        QObject::connect(m_process,
                         QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         this,
                         [this](int, QProcess::ExitStatus) {
                             m_open_state->store(false, std::memory_order_release);
                         });
        const auto prepared =
            m_job->installOn(*m_process,
                             sak::WindowsJobPipeLimits{.stdout_bytes = kMaxStdioReadBufferBytes,
                                                       .stderr_bytes = kMaxStderrTailBytes});
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        m_process->start();
        const bool started = m_process->waitForStarted(m_timeout_ms);
        const auto completed = m_job->completeLaunch(*m_process, started);
        if (!completed) {
            return std::unexpected(completed.error());
        }
        if (!started) {
            return std::unexpected(QStringLiteral("Could not start MCP stdio server: %1")
                                       .arg(m_process->errorString()));
        }
        auto* const state = static_cast<QProcessPrivate*>(QObjectPrivate::get(m_process));
        if (state->stdoutChannel.reader == nullptr || state->stderrChannel.reader == nullptr) {
            return std::unexpected(QStringLiteral("MCP bounded stdio readers are unavailable"));
        }
        m_stdout_device = std::make_unique<BoundedPipeDevice>(state->stdoutChannel.reader);
        m_stderr_device = std::make_unique<BoundedPipeDevice>(state->stderrChannel.reader);
        return {};
    }

    std::expected<void, QString> initializeProtocol(int initialize_id) {
        const auto initialize_message = request(mcp::initializePayload(initialize_id),
                                                initialize_id);
        if (!initialize_message) {
            return std::unexpected(
                QStringLiteral("MCP initialize failed: %1").arg(initialize_message.error()));
        }
        const QString initialize_error = mcp::initializeResponseError(*initialize_message);
        if (!initialize_error.isEmpty()) {
            return std::unexpected(initialize_error);
        }
        // "initialized" is a notification: no response is expected.
        return writeMessage(mcp::initializedNotification(),
                            QDeadlineTimer(m_timeout_ms, Qt::PreciseTimer));
    }

    std::expected<QVector<AiMcpToolDescriptor>, QString> doListTools() {
        QVector<AiMcpToolDescriptor> tools;
        QSet<QString> seen_cursors;
        QString cursor;
        for (int page_number = 0; page_number < kMaxToolsListPages; ++page_number) {
            const auto id = nextId();
            if (!id) {
                return std::unexpected(id.error());
            }
            const auto message = request(mcp::toolsListPayload(*id, cursor), *id);
            if (!message) {
                return std::unexpected(message.error());
            }
            const auto page = AiMcpStdioSession::parseToolsListPage(
                message->value(QStringLiteral("result")).toObject());
            if (!page.has_value()) {
                return std::unexpected(page.error());
            }
            if (page->tools.size() > kMaxDiscoveredTools - tools.size()) {
                return std::unexpected(QStringLiteral("MCP tools/list exceeded the %1-tool ceiling")
                                           .arg(kMaxDiscoveredTools));
            }
            tools.append(page->tools);
            if (page->next_cursor.isEmpty()) {
                return tools;
            }
            if (seen_cursors.contains(page->next_cursor)) {
                return std::unexpected(QStringLiteral("MCP tools/list returned a cursor cycle"));
            }
            seen_cursors.insert(page->next_cursor);
            cursor = page->next_cursor;
        }
        return std::unexpected(
            QStringLiteral("MCP tools/list exceeded the %1-page ceiling").arg(kMaxToolsListPages));
    }

    std::expected<QJsonObject, QString> doCallTool(const QString& tool_name,
                                                   const QJsonObject& arguments) {
        if (tool_name.trimmed().isEmpty()) {
            return std::unexpected(QStringLiteral("MCP tool name is empty"));
        }
        const auto id = nextId();
        if (!id) {
            return std::unexpected(id.error());
        }
        const auto message = request(mcp::toolCallPayload(*id, tool_name, arguments), *id);
        if (!message) {
            return std::unexpected(message.error());
        }
        const QJsonObject result = message->value(QStringLiteral("result")).toObject();
        // Fail closed on a malformed tools/call result: MCP requires a content array (an error
        // result still carries content plus isError:true). Without this, a result missing content
        // would be handed back to the caller as a successful call.
        if (!result.value(QStringLiteral("content")).isArray()) {
            return std::unexpected(QStringLiteral("MCP tools/call result missing a content array"));
        }
        return result;
    }

    std::expected<void, QString> doClose(const QDeadlineTimer& deadline) {
        return stopProcess(deadline);
    }

private:
    std::expected<int, QString> nextId() {
        if (m_next_id == std::numeric_limits<int>::max()) {
            return std::unexpected(QStringLiteral("MCP request ID space exhausted"));
        }
        return ++m_next_id;
    }

    // Writes a request and blocks (synchronously, on this worker thread) until the
    // matching-id response arrives, or returns the specific write/response failure.
    std::expected<QJsonObject, QString> request(const QJsonObject& payload, int id) {
        const QDeadlineTimer deadline(m_timeout_ms, Qt::PreciseTimer);
        const auto written = writeMessage(payload, deadline);
        if (!written) {
            return std::unexpected(written.error());
        }
        return awaitResponse(id, deadline);
    }

    std::expected<void, QString> writeMessage(const QJsonObject& object,
                                              const QDeadlineTimer& deadline) {
        if ((m_process == nullptr) || m_process->state() == QProcess::NotRunning) {
            m_open_state->store(false, std::memory_order_release);
            return std::unexpected(QStringLiteral("MCP stdio server is not running"));
        }
        if (deadline.hasExpired()) {
            return std::unexpected(timeoutMessage());
        }
        const QByteArray bytes = mcp::jsonLine(object);
        if (m_process->write(bytes) != bytes.size()) {
            return std::unexpected(QStringLiteral("Could not write MCP request to stdio server"));
        }
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) {
            return std::unexpected(timeoutMessage());
        }
        const int wait_ms =
            static_cast<int>(std::min<qint64>(remaining, std::numeric_limits<int>::max()));
        if (!m_process->waitForBytesWritten(wait_ms)) {
            return std::unexpected(
                deadline.hasExpired()
                    ? timeoutMessage()
                    : QStringLiteral("Could not write MCP request to stdio server"));
        }
        return {};
    }

    std::expected<QJsonObject, QString> awaitResponse(int id, const QDeadlineTimer& deadline) {
        for (;;) {
            const auto matched = AiMcpStdioSession::drainMatchingLines(
                *m_stdout_device, id, deadline, [this]() { return timeoutMessage(); });
            if (!matched) {
                return std::unexpected(matched.error());
            }
            if (matched->has_value()) {
                return **matched;
            }
            const qint64 remaining = deadline.remainingTime();
            if (remaining <= 0) {
                return std::unexpected(timeoutMessage());
            }
            const int wait_ms =
                static_cast<int>(std::min<qint64>(remaining, std::numeric_limits<int>::max()));
            if (!m_process->waitForReadyRead(wait_ms)) {
                return std::unexpected(m_process->state() == QProcess::NotRunning
                                           ? exitedMessage()
                                           : timeoutMessage());
            }
            drainStderr();
        }
    }

    void drainStderr() {
        if (m_stderr_device == nullptr) {
            return;
        }
        m_stderr_tail.append(m_stderr_device->read(kMaxStderrTailBytes));
        if (m_stderr_tail.size() > kMaxStderrTailBytes) {
            m_stderr_tail = m_stderr_tail.right(kMaxStderrTailBytes);
        }
    }

    QString stderrText() {
        drainStderr();
        return QString::fromUtf8(m_stderr_tail).trimmed().left(kMcpStdioErrorPreviewChars);
    }

    QString timeoutMessage() {
        const QString tail = stderrText();
        return tail.isEmpty() ? QStringLiteral("MCP stdio request timed out")
                              : QStringLiteral("MCP stdio request timed out: %1").arg(tail);
    }

    QString exitedMessage() {
        const QString tail = stderrText();
        return tail.isEmpty()
                   ? QStringLiteral("MCP stdio server exited before response")
                   : QStringLiteral("MCP stdio server exited before response: %1").arg(tail);
    }

    std::expected<void, QString> waitForPrimaryExit(const QDeadlineTimer& deadline) {
        if (m_process == nullptr || m_process->state() == QProcess::NotRunning) {
            return {};
        }
        const qint64 remaining = deadline.remainingTime();
        if (remaining <= 0) {
            return std::unexpected(QStringLiteral("MCP primary cleanup deadline expired"));
        }
        const int wait_ms =
            static_cast<int>(qMin<qint64>(remaining, std::numeric_limits<int>::max()));
        if (!m_process->waitForFinished(wait_ms)) {
            return std::unexpected(
                QStringLiteral("MCP primary cleanup failed: %1").arg(m_process->errorString()));
        }
        return {};
    }

    std::expected<void, QString> waitForEmptyJob(const QDeadlineTimer& deadline) {
        for (;;) {
            const auto accounting = m_job->accounting();
            if (!accounting) {
                return std::unexpected(accounting.error());
            }
            if (accounting->active_processes == 0) {
                return {};
            }
            const qint64 remaining = deadline.remainingTime();
            if (remaining <= 0) {
                return std::unexpected(QStringLiteral("MCP descendants exceeded cleanup deadline"));
            }
            QThread::msleep(static_cast<unsigned long>(qMin(kProcessCleanupPollMs, remaining)));
        }
    }

    std::expected<void, QString> stopProcess(const QDeadlineTimer& deadline) {
        m_open_state->store(false, std::memory_order_release);
        if (!m_job) {
            return waitForPrimaryExit(deadline);
        }
        const auto accounting = m_job->accounting();
        if (!accounting) {
            return std::unexpected(accounting.error());
        }
        if (accounting->active_processes != 0) {
            const auto terminated = m_job->terminate();
            if (!terminated) {
                return std::unexpected(terminated.error());
            }
        }
        const auto primary = waitForPrimaryExit(deadline);
        if (!primary) {
            return std::unexpected(primary.error());
        }
        const auto empty = waitForEmptyJob(deadline);
        if (!empty) {
            return std::unexpected(empty.error());
        }
        const auto closed = m_job->closeEmpty();
        if (!closed) {
            return std::unexpected(closed.error());
        }
        m_job.reset();
        return {};
    }

    QProcess* m_process{nullptr};
    std::unique_ptr<BoundedPipeDevice> m_stdout_device;
    std::unique_ptr<BoundedPipeDevice> m_stderr_device;
    std::shared_ptr<std::atomic_bool> m_open_state;
    QByteArray m_stderr_tail;
    int m_next_id{0};
    int m_timeout_ms{kMinimumRequestTimeoutMs};
    JobFactory m_job_factory;
    std::unique_ptr<sak::WindowsProcessJob> m_job;
};

AiMcpStdioSession::AiMcpStdioSession()
    : m_thread_starter(startSessionThread), m_job_factory(sak::WindowsProcessJob::create) {}

AiMcpStdioSession::~AiMcpStdioSession() {
    const auto closed = close();
    if (!closed) {
        // Report + contain (owner decision 2026-10-24). This was a qFatal, so a child that merely
        // outlived the cleanup deadline under machine load took the whole application down. The
        // live Worker, QProcess and kill-on-close Job are neither destroyed nor abandoned: custody
        // owns them until a retry proves cleanup or the process exits and the OS closes the Job.
        quarantine(takeRetainedOwnership(),
                   QStringLiteral("MCP session destruction could not prove cleanup: %1")
                       .arg(closed.error()));
    }
}

namespace {

// Custody adapter: one cleanup retry is one ordinary bounded close() of the retained session.
// It is destroyed only after that close succeeded, so ~AiMcpStdioSession then has nothing left.
class QuarantinedSession final : public sak::ContainedProcessQuarantine::Owner {
public:
    explicit QuarantinedSession(std::unique_ptr<AiMcpStdioSession> session)
        : m_session(std::move(session)) {}

    [[nodiscard]] std::expected<void, QString> retryCleanup() override {
        return m_session->close();
    }

private:
    std::unique_ptr<AiMcpStdioSession> m_session;
};

}  // namespace

std::unique_ptr<AiMcpStdioSession> AiMcpStdioSession::takeRetainedOwnership() {
    auto retained = std::make_unique<AiMcpStdioSession>();
    retained->m_worker = std::exchange(m_worker, nullptr);
    retained->m_worker_liveness = m_worker_liveness;
    m_worker_liveness.clear();
    retained->m_thread = std::exchange(m_thread, nullptr);
    retained->m_join_requested = std::exchange(m_join_requested, false);
    retained->m_pending_cleanup = std::exchange(m_pending_cleanup, nullptr);
    // The Worker keeps writing liveness through this exact flag from its own thread.
    retained->m_open = std::exchange(m_open, std::make_shared<std::atomic_bool>(false));
    return retained;
}

void AiMcpStdioSession::quarantine(std::unique_ptr<AiMcpStdioSession> session,
                                   const QString& reason) {
    sak::ContainedProcessQuarantine::retain(
        std::make_unique<QuarantinedSession>(std::move(session)), reason);
}

void AiMcpStdioSession::setThreadStarterForTesting(std::function<void(QThread&)> starter) {
    m_thread_starter = starter ? std::move(starter) : startSessionThread;
}

std::expected<void, QString> AiMcpStdioSession::startWorker() {
    m_thread = new QThread;
    // Detach on the allocating caller. A later sequential caller can then adopt the
    // controller after joining, without relying on the creator having an event loop.
    m_thread->moveToThread(nullptr);
    if (m_thread->thread() != nullptr) {
        return std::unexpected(QStringLiteral("Could not detach MCP session thread controller"));
    }
    // Start before creating the worker: a failed launch must never reach a blocking
    // queued invocation whose event loop cannot run.
    m_thread_starter(*m_thread);
    if (!m_thread->isRunning()) {
        return std::unexpected(QStringLiteral("Could not start MCP session worker thread"));
    }
    m_worker = new Worker(m_open, m_initial_request_id_for_testing, m_job_factory);
    m_worker_liveness = m_worker;
    m_worker->moveToThread(m_thread);
    // Check affinity instead of moveToThread's return type, which was void in Qt 6.5.
    if (m_worker->thread() != m_thread) {
        delete m_worker;  // A refused move leaves this newly created worker on the caller.
        m_worker = nullptr;
        return std::unexpected(QStringLiteral("Could not move MCP session worker to its thread"));
    }
    QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    return {};
}

std::expected<void, QString> AiMcpStdioSession::open(const AiMcpSessionConfig& config) {
    const auto previous = close();
    if (!previous) {
        return std::unexpected(previous.error());
    }
    const auto started = startWorker();
    if (!started) {
        return std::unexpected(closeAfterFailure(started.error()));
    }
    std::expected<void, QString> result;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &config, &result]() { result = m_worker->doOpen(config); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        // The blocking call could not be delivered to the worker's event loop; fail closed
        // rather than reporting a silent, error-less non-open.
        result =
            std::unexpected(QStringLiteral("Could not dispatch open to the MCP session worker"));
    }
    if (!result) {
        return std::unexpected(closeAfterFailure(result.error()));
    }
    return result;
}

bool AiMcpStdioSession::isOpen() const {
    return m_open->load(std::memory_order_acquire);
}

int AiMcpStdioSession::effectiveRequestTimeoutMs(int requested_ms) {
    return qMax(requested_ms, kMinimumRequestTimeoutMs);
}

std::expected<QVector<AiMcpToolDescriptor>, QString> AiMcpStdioSession::listTools() {
    if (!isOpen() || (m_worker == nullptr)) {
        return std::unexpected(QStringLiteral("MCP session is not open"));
    }
    std::expected<QVector<AiMcpToolDescriptor>, QString> result;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &result]() { result = m_worker->doListTools(); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        return std::unexpected(
            QStringLiteral("Could not dispatch tools/list to the MCP session worker"));
    }
    return result;
}

std::expected<QJsonObject, QString> AiMcpStdioSession::callTool(const QString& tool_name,
                                                                const QJsonObject& arguments) {
    if (!isOpen() || (m_worker == nullptr)) {
        return std::unexpected(QStringLiteral("MCP session is not open"));
    }
    std::expected<QJsonObject, QString> result;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &tool_name, &arguments, &result]() {
            result = m_worker->doCallTool(tool_name, arguments);
        },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        return std::unexpected(
            QStringLiteral("Could not dispatch tools/call to the MCP session worker"));
    }
    return result;
}

std::expected<void, QString> AiMcpStdioSession::close() {
    return closeWithDeadline(QDeadlineTimer(kProcessCleanupTimeoutMs));
}

QString AiMcpStdioSession::closeAfterFailure(const QString& primary_error) {
    const auto closed = close();
    return closed ? primary_error
                  : QStringLiteral("%1; MCP cleanup failed: %2").arg(primary_error, closed.error());
}

bool AiMcpStdioSession::isClosed() const {
    return m_thread == nullptr;
}

QObject* AiMcpStdioSession::workerObjectForTesting() const {
    return m_worker;
}

std::expected<void, QString> AiMcpStdioSession::cleanupWorkerBeforeJoin(
    const QDeadlineTimer& deadline) {
    if (m_worker == nullptr) {
        return {};
    }
    if (!m_thread->isRunning()) {
        if (!m_thread->wait(deadline)) {
            return std::unexpected(QStringLiteral("MCP worker thread exceeded cleanup deadline"));
        }
        if (m_worker_liveness) {
            return std::unexpected(
                QStringLiteral("MCP cleanup unavailable: worker thread is not running"));
        }
        // Worker destructor already proved its process/Job clean. A queued event may have
        // been dropped when its event loop stopped; no callback can still reach this state.
        m_pending_cleanup.reset();
        return {};
    }
    // A timed-out caller retains this exact request. A later close waits for the same Worker
    // completion instead of posting an unbounded queue of duplicate cleanup operations.
    if (!m_pending_cleanup) {
        m_pending_cleanup = std::make_shared<PendingCleanup>();
        Worker* const worker = m_worker;
        const auto request = m_pending_cleanup;
        const bool invoked = QMetaObject::invokeMethod(
            worker,
            [worker, request, deadline]() {
                request->result = deadline.hasExpired()
                                      ? std::unexpected(QStringLiteral(
                                            "MCP worker cleanup dispatch exceeded deadline"))
                                      : worker->doClose(deadline);
                request->completed.release();
            },
            Qt::QueuedConnection);
        if (!invoked) {
            m_pending_cleanup.reset();
            return std::unexpected(
                QStringLiteral("Could not dispatch close to the MCP session worker"));
        }
    }
    if (!m_pending_cleanup->completed.tryAcquire(1, deadline)) {
        return std::unexpected(QStringLiteral("MCP worker cleanup dispatch exceeded deadline"));
    }
    auto cleanup = std::move(m_pending_cleanup->result);
    m_pending_cleanup.reset();
    return cleanup;
}

std::expected<void, QString> AiMcpStdioSession::closeWithDeadline(const QDeadlineTimer& deadline) {
    m_open->store(false, std::memory_order_release);
    if (m_thread != nullptr) {
        if (m_thread == QThread::currentThread()) {
            return std::unexpected(
                QStringLiteral("MCP session close cannot run on its own managed thread"));
        }
        if (!m_join_requested) {
            const auto cleanup = cleanupWorkerBeforeJoin(deadline);
            if (!cleanup) {
                return std::unexpected(cleanup.error());
            }
            m_join_requested = true;
            m_thread->quit();
        }
        // A timeout retains this session/controller. Cleanup is already proved, so retry joins
        // without dispatching to a Worker whose event loop may have stopped or been deleted.
        if (!m_thread->wait(deadline)) {
            return std::unexpected(QStringLiteral("MCP worker thread exceeded cleanup deadline"));
        }
        m_thread->moveToThread(QThread::currentThread());
        if (m_thread->thread() != QThread::currentThread()) {
            qFatal("Could not adopt joined MCP session thread controller for destruction");
        }
        delete m_thread;  // m_worker was deleteLater'd on QThread::finished
        m_thread = nullptr;
        m_worker = nullptr;
        m_join_requested = false;
    }
    m_open->store(false, std::memory_order_release);
    return {};
}

std::expected<AiMcpToolsListPage, QString> AiMcpStdioSession::parseToolsListPage(
    const QJsonObject& result) {
    const QJsonValue tools_value = result.value(QStringLiteral("tools"));
    if (!tools_value.isArray()) {
        return std::unexpected(QStringLiteral("MCP tools/list result is missing a tools array"));
    }
    const QJsonValue cursor_value = result.value(QStringLiteral("nextCursor"));
    if (!cursor_value.isUndefined() && !cursor_value.isString()) {
        return std::unexpected(QStringLiteral("MCP tools/list nextCursor is not a string"));
    }

    AiMcpToolsListPage page;
    const QJsonArray entries = tools_value.toArray();
    page.tools.reserve(entries.size());
    for (const QJsonValue& entry : entries) {
        const auto descriptor = parseToolDescriptor(entry);
        if (!descriptor.has_value()) {
            return std::unexpected(descriptor.error());
        }
        page.tools.append(*descriptor);
    }
    page.next_cursor = cursor_value.toString().trimmed();
    return page;
}

}  // namespace sak::ai
