// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_stdio_session.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/layout_constants.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaObject>
#include <QProcess>
#include <QSet>
#include <QThread>

#include <windows.h>

#include <tlhelp32.h>

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

// Kill the server and any children it spawned. A bare QProcess::kill leaks
// grandchildren; an MCP server that shells out would otherwise strand them.
void terminateProcessTree(DWORD process_id) {
    if (process_id == 0) {
        return;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry) != 0) {
            do {
                if (entry.th32ParentProcessID == process_id) {
                    terminateProcessTree(entry.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &entry) != 0);
        }
        CloseHandle(snapshot);
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, process_id);
    if (process != nullptr) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}

}  // namespace

// Owns the server QProcess and performs all I/O. Lives on the session's dedicated
// worker thread; every method here runs on that thread (invoked via a blocking
// queued connection from the public API), so QProcess is only ever touched from
// one thread and requests are naturally serialized -- at most one is outstanding.
class AiMcpStdioSession::Worker : public QObject {
public:
    explicit Worker(std::atomic_bool* open_state) : m_open_state(open_state) {}

    ~Worker() override {
        m_open_state->store(false, std::memory_order_release);
        closeJob();  // Safety net: reap the tree if teardown did not already close the job.
    }

    bool doOpen(const AiMcpSessionConfig& config, QString* error_message) {
        m_open_state->store(false, std::memory_order_release);
        m_timeout_ms = AiMcpStdioSession::effectiveRequestTimeoutMs(config.request_timeout_ms);
        const QString command = config.command.trimmed();
        if (command.isEmpty() || !QFileInfo::exists(command)) {
            setError(error_message, QStringLiteral("MCP stdio command missing: %1").arg(command));
            return false;
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
        m_process->start();
        if (!m_process->waitForStarted(m_timeout_ms)) {
            setError(error_message,
                     QStringLiteral("Could not start MCP stdio server: %1")
                         .arg(m_process->errorString()));
            stopProcess();
            return false;
        }
        // Place the live server in a kill-on-close Job Object now (while it is running) so its
        // whole descendant tree is reaped at teardown -- even when the server later exits on its
        // OWN, where a parent-PID snapshot walk would find nothing (children reparented).
        if (!assignProcessToJob()) {
            setError(error_message,
                     QStringLiteral("Could not secure MCP stdio server process tree"));
            stopProcess();
            return false;
        }

        QString handshake_error;
        const int initialize_id = nextId();
        const QJsonObject initialize_message =
            request(mcp::initializePayload(initialize_id), initialize_id, &handshake_error);
        if (!handshake_error.isEmpty()) {
            setError(error_message,
                     QStringLiteral("MCP initialize failed: %1").arg(handshake_error));
            stopProcess();
            return false;
        }
        const QString initialize_error = mcp::initializeResponseError(initialize_message);
        if (!initialize_error.isEmpty()) {
            setError(error_message, initialize_error);
            stopProcess();
            return false;
        }
        // "initialized" is a notification: no response is expected.
        if (!writeMessage(mcp::initializedNotification(), error_message)) {
            stopProcess();
            return false;
        }
        m_open_state->store(true, std::memory_order_release);
        return true;
    }

    QVector<AiMcpToolDescriptor> doListTools(QString* error_message) {
        QVector<AiMcpToolDescriptor> tools;
        QSet<QString> seen_cursors;
        QString cursor;
        for (int page_number = 0; page_number < kMaxToolsListPages; ++page_number) {
            const int id = nextId();
            const QJsonObject message =
                request(mcp::toolsListPayload(id, cursor), id, error_message);
            if ((error_message != nullptr) && !error_message->isEmpty()) {
                return {};
            }
            const auto page = AiMcpStdioSession::parseToolsListPage(
                message.value(QStringLiteral("result")).toObject());
            if (!page.has_value()) {
                setError(error_message, page.error());
                return {};
            }
            if (page->tools.size() > kMaxDiscoveredTools - tools.size()) {
                setError(error_message,
                         QStringLiteral("MCP tools/list exceeded the %1-tool ceiling")
                             .arg(kMaxDiscoveredTools));
                return {};
            }
            tools.append(page->tools);
            if (page->next_cursor.isEmpty()) {
                if (error_message != nullptr) {
                    error_message->clear();
                }
                return tools;
            }
            if (seen_cursors.contains(page->next_cursor)) {
                setError(error_message, QStringLiteral("MCP tools/list returned a cursor cycle"));
                return {};
            }
            seen_cursors.insert(page->next_cursor);
            cursor = page->next_cursor;
        }
        setError(
            error_message,
            QStringLiteral("MCP tools/list exceeded the %1-page ceiling").arg(kMaxToolsListPages));
        return {};
    }

    QJsonObject doCallTool(const QString& tool_name,
                           const QJsonObject& arguments,
                           QString* error_message) {
        if (tool_name.trimmed().isEmpty()) {
            setError(error_message, QStringLiteral("MCP tool name is empty"));
            return {};
        }
        const int id = nextId();
        const QJsonObject message =
            request(mcp::toolCallPayload(id, tool_name, arguments), id, error_message);
        if ((error_message != nullptr) && !error_message->isEmpty()) {
            return {};
        }
        const QJsonObject result = message.value(QStringLiteral("result")).toObject();
        // Fail closed on a malformed tools/call result: MCP requires a content array (an error
        // result still carries content plus isError:true). Without this, a result missing content
        // would be handed back to the caller as a successful call.
        if (!result.value(QStringLiteral("content")).isArray()) {
            setError(error_message,
                     QStringLiteral("MCP tools/call result missing a content array"));
            return {};
        }
        return result;
    }

    void doClose() { stopProcess(); }

private:
    int nextId() { return ++m_next_id; }

    // Writes a request and blocks (synchronously, on this worker thread) until the
    // matching-id response arrives. Returns the full JSON-RPC message; sets
    // *error_message and returns {} on a JSON-RPC error, timeout, or server exit.
    QJsonObject request(const QJsonObject& payload, int id, QString* error_message) {
        if (!writeMessage(payload, error_message)) {
            return {};
        }
        return awaitResponse(id, error_message);
    }

    bool writeMessage(const QJsonObject& object, QString* error_message) {
        if ((m_process == nullptr) || m_process->state() == QProcess::NotRunning) {
            m_open_state->store(false, std::memory_order_release);
            setError(error_message, QStringLiteral("MCP stdio server is not running"));
            return false;
        }
        const QByteArray bytes = mcp::jsonLine(object);
        if (m_process->write(bytes) != bytes.size() ||
            !m_process->waitForBytesWritten(m_timeout_ms)) {
            setError(error_message, QStringLiteral("Could not write MCP request to stdio server"));
            return false;
        }
        return true;
    }

    QJsonObject awaitResponse(int id, QString* error_message) {
        QElapsedTimer timer;
        timer.start();
        for (;;) {
            const QJsonObject matched = drainMatchingLines(id, error_message);
            if (!matched.isEmpty() || ((error_message != nullptr) && !error_message->isEmpty())) {
                return matched;
            }
            if (m_process->bytesAvailable() > kMaxStdioReadBufferBytes) {
                setError(error_message,
                         QStringLiteral("MCP stdio response exceeded %1 bytes without a newline")
                             .arg(kMaxStdioReadBufferBytes));
                return {};
            }
            const int remaining = m_timeout_ms - static_cast<int>(timer.elapsed());
            if (remaining <= 0) {
                setError(error_message, timeoutMessage());
                return {};
            }
            if (!m_process->waitForReadyRead(remaining)) {
                setError(error_message,
                         m_process->state() == QProcess::NotRunning ? exitedMessage()
                                                                    : timeoutMessage());
                return {};
            }
            drainStderr();
        }
    }

    // Reads every complete line currently buffered, returning the first whose id
    // matches. Non-matching ids (stale responses) and id-less notifications are
    // skipped; non-JSON stdout lines (stray server logs) are tolerated and skipped.
    // A JSON-RPC error object for our id sets *error_message and returns {}.
    QJsonObject drainMatchingLines(int id, QString* error_message) {
        // Enforce the byte cap BEFORE reading any line: a server can buffer a single
        // newline-terminated line larger than the cap, and canReadLine()/readLine() would
        // allocate the whole oversized line before the post-drain guard in awaitResponse runs.
        // Fail closed up front so the cap bounds a single huge line and newline-free
        // accumulation alike.
        if (m_process->bytesAvailable() > kMaxStdioReadBufferBytes) {
            setError(error_message,
                     QStringLiteral("MCP stdio response exceeded %1 bytes without a newline")
                         .arg(kMaxStdioReadBufferBytes));
            return {};
        }
        while (m_process->canReadLine()) {
            const QByteArray line = m_process->readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            QString parse_error;
            const QJsonObject message = mcp::parseJsonLine(line, &parse_error);
            if (message.isEmpty() || message.value(QStringLiteral("id")).toInt(-1) != id) {
                continue;
            }
            if (message.contains(QStringLiteral("error"))) {
                setError(error_message, mcp::jsonRpcErrorMessage(message));
                return {};
            }
            // Fail closed on a malformed id-matching response: require a JSON-RPC result
            // object (initialize/tools/list/tools/call all return objects). Without this an
            // absent/scalar result would be read as an empty {} success by doCallTool.
            if (!message.value(QStringLiteral("result")).isObject()) {
                setError(error_message,
                         QStringLiteral("MCP stdio response missing a result object"));
                return {};
            }
            if (error_message != nullptr) {
                error_message->clear();
            }
            return message;
        }
        return {};
    }

    void drainStderr() {
        if (m_process == nullptr) {
            return;
        }
        m_stderr_tail.append(m_process->readAllStandardError());
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

    void stopProcess() {
        m_open_state->store(false, std::memory_order_release);
        if (m_job_handle != nullptr) {
            // The kill-on-close Job Object reaps the entire descendant tree -- including a
            // server that already exited on its own and left orphaned descendants, which the
            // NotRunning early-return below would otherwise skip. Supersedes the per-process
            // terminate/kill and avoids a stale-PID walk.
            closeJob();
            return;
        }
        if ((m_process == nullptr) || m_process->state() == QProcess::NotRunning) {
            return;
        }
        const qint64 pid = m_process->processId();
        m_process->closeWriteChannel();
        terminateProcessTree(static_cast<DWORD>(pid));
        m_process->kill();
        m_process->waitForFinished(kMinimumRequestTimeoutMs);
    }

    bool assignProcessToJob() {
        if ((m_process == nullptr) || m_process->processId() <= 0) {
            return false;
        }
        m_job_handle = ::CreateJobObjectW(nullptr, nullptr);
        if (m_job_handle == nullptr) {
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(
                m_job_handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
            closeJob();  // Without KILL_ON_JOB_CLOSE the job cannot guarantee cleanup; drop it.
            return false;
        }
        const HANDLE proc = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                          FALSE,
                                          static_cast<DWORD>(m_process->processId()));
        if (proc == nullptr) {
            closeJob();
            return false;
        }
        const bool assigned = ::AssignProcessToJobObject(m_job_handle, proc) != 0;
        ::CloseHandle(proc);
        if (!assigned) {
            closeJob();
        }
        return assigned;
    }

    void closeJob() {
        if (m_job_handle != nullptr) {
            ::CloseHandle(m_job_handle);  // KILL_ON_JOB_CLOSE reaps any surviving descendants.
            m_job_handle = nullptr;
        }
    }

    static void setError(QString* error_message, const QString& message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
    }

    QProcess* m_process{nullptr};
    std::atomic_bool* m_open_state{nullptr};
    QByteArray m_stderr_tail;
    int m_next_id{0};
    int m_timeout_ms{kMinimumRequestTimeoutMs};
    HANDLE m_job_handle{nullptr};
};

AiMcpStdioSession::AiMcpStdioSession() = default;

AiMcpStdioSession::~AiMcpStdioSession() {
    close();
}

bool AiMcpStdioSession::open(const AiMcpSessionConfig& config, QString& error_message) {
    error_message.clear();
    close();
    m_thread = new QThread;
    // Start the thread BEFORE creating/moving the worker. If the OS cannot spawn
    // the thread (thread exhaustion), the blocking queued call below would post an
    // event no event loop ever runs and hang the caller forever. Bailing here keeps
    // that path a returned error, and means the worker only ever exists alongside a
    // running event loop.
    m_thread->start();
    if (!m_thread->isRunning()) {
        delete m_thread;
        m_thread = nullptr;
        error_message = QStringLiteral("Could not start MCP session worker thread");
        return false;
    }

    m_worker = new Worker(&m_open);
    m_worker->moveToThread(m_thread);
    QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    bool ok = false;
    QString error;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &config, &error, &ok]() { ok = m_worker->doOpen(config, &error); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        // The blocking call could not be delivered to the worker's event loop; fail closed
        // rather than reporting a silent, error-less non-open.
        error = QStringLiteral("Could not dispatch open to the MCP session worker");
        ok = false;
    }
    error_message = error;
    if (!ok) {
        close();
    }
    return ok;
}

bool AiMcpStdioSession::isOpen() const {
    return m_open.load(std::memory_order_acquire);
}

int AiMcpStdioSession::effectiveRequestTimeoutMs(int requested_ms) {
    return qMax(requested_ms, kMinimumRequestTimeoutMs);
}

QVector<AiMcpToolDescriptor> AiMcpStdioSession::listTools(QString& error_message) {
    error_message.clear();
    if (!m_open || (m_worker == nullptr)) {
        error_message = QStringLiteral("MCP session is not open");
        return {};
    }
    QVector<AiMcpToolDescriptor> result;
    QString error;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &result, &error]() { result = m_worker->doListTools(&error); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        error_message = QStringLiteral("Could not dispatch tools/list to the MCP session worker");
        return {};
    }
    error_message = error;
    if (!error.isEmpty()) {
        return {};
    }
    return result;
}

QJsonObject AiMcpStdioSession::callTool(const QString& tool_name,
                                        const QJsonObject& arguments,
                                        QString& error_message) {
    error_message.clear();
    if (!m_open || (m_worker == nullptr)) {
        error_message = QStringLiteral("MCP session is not open");
        return {};
    }
    QJsonObject result;
    QString error;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &tool_name, &arguments, &result, &error]() {
            result = m_worker->doCallTool(tool_name, arguments, &error);
        },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        error_message = QStringLiteral("Could not dispatch tools/call to the MCP session worker");
        return {};
    }
    error_message = error;
    if (!error.isEmpty()) {
        return {};
    }
    return result;
}

void AiMcpStdioSession::close() {
    if (m_thread != nullptr) {
        // Only issue the blocking call while the worker's event loop is actually
        // running (it always is once open() succeeds); guarding this keeps close()
        // safe even if the thread were ever stopped, so it cannot hang like the
        // pre-guard open() start-failure path could.
        if ((m_worker != nullptr) && m_thread->isRunning()) {
            QMetaObject::invokeMethod(
                m_worker, [this]() { m_worker->doClose(); }, Qt::BlockingQueuedConnection);
        }
        m_thread->quit();
        // SAK-ALLOW-BLOCKING: `delete m_thread` follows, and deleting a live QThread aborts
        // the process. doClose() above already ran to completion on that thread via a
        // blocking queued call, so the event loop has nothing left but to exit.
        m_thread->wait();
        delete m_thread;  // m_worker was deleteLater'd on QThread::finished
        m_thread = nullptr;
        m_worker = nullptr;
    }
    m_open = false;
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
