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
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace sak::ai {

namespace {

constexpr int kMinimumRequestTimeoutMs = sak::kMillisecondsPerSecond;
constexpr int kMcpStdioErrorPreviewChars = 2000;
// Abort a response read once this many bytes buffer without a newline, so a server
// streaming a newline-free byte stream cannot grow the QProcess buffer until OOM.
constexpr qint64 kMaxStdioReadBufferBytes = 8 * 1024 * 1024;  // 8 MiB
constexpr qsizetype kMaxStderrTailBytes = 64 * 1024;          // 64 KiB

int boundedTimeout(int requested_ms) {
    return qMax(requested_ms, kMinimumRequestTimeoutMs);
}

#ifdef Q_OS_WIN
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
#endif

}  // namespace

// Owns the server QProcess and performs all I/O. Lives on the session's dedicated
// worker thread; every method here runs on that thread (invoked via a blocking
// queued connection from the public API), so QProcess is only ever touched from
// one thread and requests are naturally serialized -- at most one is outstanding.
class AiMcpStdioSession::Worker : public QObject {
public:
    ~Worker() override {
#ifdef Q_OS_WIN
        closeJob();  // Safety net: reap the tree if teardown did not already close the job.
#endif
    }

    bool doOpen(const AiMcpSessionConfig& config, QString* error_message) {
        m_timeoutMs = boundedTimeout(config.request_timeout_ms);
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
        m_process->start();
        if (!m_process->waitForStarted(m_timeoutMs)) {
            setError(error_message,
                     QStringLiteral("Could not start MCP stdio server: %1")
                         .arg(m_process->errorString()));
            stopProcess();
            return false;
        }
#ifdef Q_OS_WIN
        // Place the live server in a kill-on-close Job Object now (while it is running) so its
        // whole descendant tree is reaped at teardown -- even when the server later exits on its
        // OWN, where a parent-PID snapshot walk would find nothing (children reparented).
        // Best-effort: stopProcess falls back to the per-process terminate/kill path if the job
        // cannot be established, so cleanup is never weaker than before.
        assignProcessToJob();
#endif

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
        // Fail closed if the server did not negotiate a protocol version: an initialize result
        // with no protocolVersion is not a usable MCP session, so do not mark it open.
        if (initialize_message.value(QStringLiteral("result"))
                .toObject()
                .value(QStringLiteral("protocolVersion"))
                .toString()
                .trimmed()
                .isEmpty()) {
            setError(error_message,
                     QStringLiteral("MCP initialize result missing protocolVersion"));
            stopProcess();
            return false;
        }
        // "initialized" is a notification: no response is expected.
        if (!writeMessage(mcp::initializedNotification(), error_message)) {
            stopProcess();
            return false;
        }
        return true;
    }

    QJsonObject doListTools(QString* error_message) {
        const int id = nextId();
        const QJsonObject message = request(mcp::toolsListPayload(id), id, error_message);
        const QJsonObject result = message.value(QStringLiteral("result")).toObject();
        // Fail closed on pagination: this transport issues a single tools/list and cannot follow
        // a cursor, so a non-empty nextCursor would mean silently reporting a partial tool set as
        // complete discovery. Refuse rather than under-report.
        if (!result.value(QStringLiteral("nextCursor")).toString().trimmed().isEmpty()) {
            setError(error_message,
                     QStringLiteral("MCP tools/list returned a pagination cursor; paginated "
                                    "discovery is not supported"));
            return {};
        }
        return result;
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
    int nextId() { return ++m_nextId; }

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
            setError(error_message, QStringLiteral("MCP stdio server is not running"));
            return false;
        }
        const QByteArray bytes = mcp::jsonLine(object);
        if (m_process->write(bytes) != bytes.size() ||
            !m_process->waitForBytesWritten(m_timeoutMs)) {
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
            const int remaining = m_timeoutMs - static_cast<int>(timer.elapsed());
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
                setError(error_message,
                         message.value(QStringLiteral("error"))
                             .toObject()
                             .value(QStringLiteral("message"))
                             .toString(QStringLiteral("MCP JSON-RPC error")));
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
        m_stderrTail.append(m_process->readAllStandardError());
        if (m_stderrTail.size() > kMaxStderrTailBytes) {
            m_stderrTail = m_stderrTail.right(kMaxStderrTailBytes);
        }
    }

    QString stderrText() {
        drainStderr();
        return QString::fromUtf8(m_stderrTail).trimmed().left(kMcpStdioErrorPreviewChars);
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
#ifdef Q_OS_WIN
        if (m_jobHandle != nullptr) {
            // The kill-on-close Job Object reaps the entire descendant tree -- including a
            // server that already exited on its own and left orphaned descendants, which the
            // NotRunning early-return below would otherwise skip. Supersedes the per-process
            // terminate/kill and avoids a stale-PID walk.
            closeJob();
            return;
        }
#endif
        if ((m_process == nullptr) || m_process->state() == QProcess::NotRunning) {
            return;
        }
        const qint64 pid = m_process->processId();
        m_process->closeWriteChannel();
#ifdef Q_OS_WIN
        terminateProcessTree(static_cast<DWORD>(pid));
#else
        Q_UNUSED(pid);
#endif
        m_process->kill();
        m_process->waitForFinished(kMinimumRequestTimeoutMs);
    }

#ifdef Q_OS_WIN
    void assignProcessToJob() {
        if ((m_process == nullptr) || m_process->processId() <= 0) {
            return;
        }
        m_jobHandle = ::CreateJobObjectW(nullptr, nullptr);
        if (m_jobHandle == nullptr) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (::SetInformationJobObject(
                m_jobHandle, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) == 0) {
            closeJob();  // Without KILL_ON_JOB_CLOSE the job cannot guarantee cleanup; drop it.
            return;
        }
        const HANDLE proc = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                          FALSE,
                                          static_cast<DWORD>(m_process->processId()));
        if (proc == nullptr) {
            closeJob();
            return;
        }
        if (::AssignProcessToJobObject(m_jobHandle, proc) == 0) {
            closeJob();  // Not in the job -> closing it would reap nothing; fall back.
        }
        ::CloseHandle(proc);
    }

    void closeJob() {
        if (m_jobHandle != nullptr) {
            ::CloseHandle(m_jobHandle);  // KILL_ON_JOB_CLOSE reaps any surviving descendants.
            m_jobHandle = nullptr;
        }
    }
#endif

    static void setError(QString* error_message, const QString& message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
    }

    QProcess* m_process{nullptr};
    QByteArray m_stderrTail;
    int m_nextId{0};
    int m_timeoutMs{kMinimumRequestTimeoutMs};
#ifdef Q_OS_WIN
    HANDLE m_jobHandle{nullptr};
#endif
};

AiMcpStdioSession::AiMcpStdioSession() = default;

AiMcpStdioSession::~AiMcpStdioSession() {
    close();
}

bool AiMcpStdioSession::open(const AiMcpSessionConfig& config, QString* error_message) {
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
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Could not start MCP session worker thread");
        }
        return false;
    }

    m_worker = new Worker;
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
    if (error_message != nullptr) {
        *error_message = error;
    }
    m_open = ok;
    if (!ok) {
        close();
    }
    return ok;
}

bool AiMcpStdioSession::isOpen() const {
    return m_open;
}

QVector<AiMcpToolDescriptor> AiMcpStdioSession::listTools(QString* error_message) {
    if (!m_open || (m_worker == nullptr)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("MCP session is not open");
        }
        return {};
    }
    QJsonObject result;
    QString error;
    const bool invoked = QMetaObject::invokeMethod(
        m_worker,
        [this, &result, &error]() { result = m_worker->doListTools(&error); },
        Qt::BlockingQueuedConnection);
    if (!invoked) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Could not dispatch tools/list to the MCP session worker");
        }
        return {};
    }
    if (error_message != nullptr) {
        *error_message = error;
    }
    if (!error.isEmpty()) {
        return {};
    }
    return parseToolsListResultForTesting(result);
}

QJsonObject AiMcpStdioSession::callTool(const QString& tool_name,
                                        const QJsonObject& arguments,
                                        QString* error_message) {
    if (!m_open || (m_worker == nullptr)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("MCP session is not open");
        }
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
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Could not dispatch tools/call to the MCP session worker");
        }
        return {};
    }
    if (error_message != nullptr) {
        *error_message = error;
    }
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

QJsonObject AiMcpStdioSession::toolsListPayloadForTesting(int id) {
    return mcp::toolsListPayload(id);
}

QVector<AiMcpToolDescriptor> AiMcpStdioSession::parseToolsListResultForTesting(
    const QJsonObject& result) {
    QVector<AiMcpToolDescriptor> tools;
    const QJsonArray entries = result.value(QStringLiteral("tools")).toArray();
    tools.reserve(entries.size());
    for (const auto& entry : entries) {
        const QJsonObject obj = entry.toObject();
        const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;  // a tool with no name is unusable; skip it
        }
        tools.append({.name = name,
                      .description = obj.value(QStringLiteral("description")).toString(),
                      .input_schema = obj.value(QStringLiteral("inputSchema")).toObject()});
    }
    return tools;
}

}  // namespace sak::ai
