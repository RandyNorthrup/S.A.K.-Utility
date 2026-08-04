// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_stdio_client.h"

#include "sak/ai/ai_mcp_jsonrpc.h"
#include "sak/layout_constants.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
#endif

#include <atomic>
#include <utility>

namespace sak::ai {

namespace {

constexpr int kInitializeId = 1;
constexpr int kToolCallId = 2;
constexpr int kMcpStdioErrorPreviewChars = 2000;
constexpr int kMinimumRequestTimeoutMs = sak::kMillisecondsPerSecond;
// Grace added to the semaphore wait beyond the request timeout so a worker thread that failed to
// START (finish() never runs) can never deadlock the caller on an unconditional acquire.
constexpr int kSemaphoreWaitGraceMs = 30'000;
// Abort once this many stdout bytes buffer without a newline, so a server that streams
// a newline-free byte stream cannot grow the QProcess buffer until the process OOMs.
constexpr qint64 kMaxStdioReadBufferBytes = 8 * 1024 * 1024;  // 8 MiB
// Only the tail of stderr is retained for the error preview; drop the rest.
constexpr qsizetype kMaxStderrTailBytes = 64 * 1024;  // 64 KiB

enum class StdioPhase {
    Starting,
    AwaitInitialize,
    AwaitTool,
};

struct StdioCallState {
    QJsonObject response;
    QString error_message;
};

QString timeoutMessage(const QString& stderr_text) {
    return stderr_text.isEmpty() ? QStringLiteral("MCP stdio request timed out")
                                 : QStringLiteral("MCP stdio request timed out: %1")
                                       .arg(stderr_text.left(kMcpStdioErrorPreviewChars));
}

QString exitedMessage(const QString& stderr_text) {
    return stderr_text.isEmpty() ? QStringLiteral("MCP stdio server exited before response")
                                 : QStringLiteral("MCP stdio server exited before response: %1")
                                       .arg(stderr_text.left(kMcpStdioErrorPreviewChars));
}

#ifdef Q_OS_WIN
void terminateProcessTree(DWORD process_id) {
    if (process_id == 0) {
        return;
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ParentProcessID == process_id) {
                    terminateProcessTree(entry.th32ProcessID);
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, process_id);
    if (process) {
        TerminateProcess(process, 1);
        CloseHandle(process);
    }
}
#endif

class StdioToolCallWorker final : public QObject {
public:
    StdioToolCallWorker(AiMcpStdioCallRequest request,
                        StdioCallState* state,
                        QSemaphore* done,
                        QThread* owner_thread)
        : m_request(std::move(request))
        , m_state(state)
        , m_done(done)
        , m_ownerThread(owner_thread) {}

    ~StdioToolCallWorker() override {
#ifdef Q_OS_WIN
        closeJob();  // Safety net: reap the tree if teardown did not already close the job.
#endif
    }

    void start() {
        m_process = new QProcess(this);
        m_timeoutTimer = new QTimer(this);
        m_timeoutTimer->setSingleShot(true);
        m_process->setProgram(m_request.command);
        m_process->setProcessEnvironment(m_request.environment);
        m_process->setProcessChannelMode(QProcess::SeparateChannels);

        connectProcessSignals();
        m_timeoutTimer->start(qMax(m_request.timeout_ms, kMinimumRequestTimeoutMs));
        m_process->start();
    }

private:
    void connectProcessSignals() {
        QObject::connect(m_process, &QProcess::started, this, [this]() { onStarted(); });
        QObject::connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
            fail(QStringLiteral("Could not start MCP stdio server: %1")
                     .arg(m_process->errorString()));
        });
        QObject::connect(m_process,
                         QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         this,
                         [this](int, QProcess::ExitStatus) {
                             fail(exitedMessage(currentStderr()), false);
                         });
        QObject::connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
            handleReadyRead();
        });
        QObject::connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
            drainStderr();
        });
        QObject::connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
            fail(timeoutMessage(currentStderr()));
        });
    }

    // Drain stderr as it arrives, retaining only the trailing bytes for diagnostics so an
    // unbounded stderr stream cannot accumulate in the QProcess buffer.
    void drainStderr() {
        if (!m_process) {
            return;
        }
        m_stderrTail.append(m_process->readAllStandardError());
        if (m_stderrTail.size() > kMaxStderrTailBytes) {
            m_stderrTail = m_stderrTail.right(kMaxStderrTailBytes);
        }
    }

    QString currentStderr() {
        drainStderr();
        return QString::fromUtf8(m_stderrTail).trimmed();
    }

    void onStarted() {
        m_phase = StdioPhase::AwaitInitialize;
        // Place the live server in a kill-on-close Job Object NOW (while it is running) so its
        // whole descendant tree is reaped at teardown -- even when the server later exits on its
        // OWN, where stopProcess sees NotRunning and a parent-PID snapshot walk would find nothing
        // (children reparented). Best-effort: if the job cannot be set up, stopProcess falls back
        // to the per-process terminate/kill path so cleanup is never weaker than before.
        assignProcessToJob();
        (void)write(mcp::initializePayload(kInitializeId));
    }

#ifdef Q_OS_WIN
    void assignProcessToJob() {
        if (!m_process || m_process->processId() <= 0) {
            return;
        }
        m_jobHandle = ::CreateJobObjectW(nullptr, nullptr);
        if (!m_jobHandle) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(
                m_jobHandle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            closeJob();  // Without KILL_ON_JOB_CLOSE the job cannot guarantee cleanup; drop it.
            return;
        }
        const HANDLE proc = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                          FALSE,
                                          static_cast<DWORD>(m_process->processId()));
        if (!proc) {
            closeJob();
            return;
        }
        if (!::AssignProcessToJobObject(m_jobHandle, proc)) {
            closeJob();  // Not in the job -> closing it would reap nothing; fall back.
        }
        ::CloseHandle(proc);
    }

    void closeJob() {
        if (m_jobHandle) {
            ::CloseHandle(m_jobHandle);  // KILL_ON_JOB_CLOSE reaps any surviving descendants.
            m_jobHandle = nullptr;
        }
    }
#endif

    void handleReadyRead() {
        // Enforce the byte cap BEFORE reading any line: a fast server can buffer a single
        // newline-terminated line larger than the cap, and canReadLine()/readLine() would
        // allocate the whole oversized line before the post-loop guard runs. Fail closed up
        // front so the cap bounds a single huge line and newline-free accumulation alike.
        if (m_process->bytesAvailable() > kMaxStdioReadBufferBytes) {
            fail(QStringLiteral("MCP stdio response exceeded %1 bytes without a newline")
                     .arg(kMaxStdioReadBufferBytes));
            return;
        }
        while (m_process->canReadLine()) {
            if (!processLine(m_process->readLine().trimmed())) {
                return;
            }
        }
        // No complete line yet, but bytes keep buffering: a server streaming newline-free
        // output. Abort before the QProcess buffer can grow large enough to OOM the app.
        if (m_process->bytesAvailable() > kMaxStdioReadBufferBytes) {
            fail(QStringLiteral("MCP stdio response exceeded %1 bytes without a newline")
                     .arg(kMaxStdioReadBufferBytes));
        }
    }

    bool processLine(const QByteArray& line) {
        if (line.isEmpty()) {
            return true;
        }
        QString parse_error;
        const QJsonObject message = mcp::parseJsonLine(line, &parse_error);
        if (message.isEmpty()) {
            fail(parse_error);
            return false;
        }
        if (message.value(QStringLiteral("id")).toInt(-1) != expectedMessageId()) {
            return true;
        }
        return handleMatchedMessage(message);
    }

    int expectedMessageId() const {
        return m_phase == StdioPhase::AwaitInitialize ? kInitializeId : kToolCallId;
    }

    bool handleMatchedMessage(const QJsonObject& message) {
        if (message.contains(QStringLiteral("error"))) {
            const QJsonObject error = message.value(QStringLiteral("error")).toObject();
            fail(error.value(QStringLiteral("message"))
                     .toString(QStringLiteral("MCP JSON-RPC error")));
            return false;
        }
        // Fail closed on a malformed id-matching response: a JSON-RPC result MUST be present
        // and be an object (initialize + tools/call both return objects). Without this an
        // absent/scalar result would be accepted and later read as an empty {} success.
        if (!message.value(QStringLiteral("result")).isObject()) {
            fail(QStringLiteral("MCP stdio response missing a result object"));
            return false;
        }
        if (m_phase == StdioPhase::AwaitInitialize) {
            return sendToolCall();
        }
        finish({}, message, true);
        return false;
    }

    bool sendToolCall() {
        m_phase = StdioPhase::AwaitTool;
        if (!write(mcp::initializedNotification())) {
            return false;
        }
        return write(mcp::toolCallPayload(kToolCallId, m_request.tool_name, m_request.arguments));
    }

    bool write(const QJsonObject& object) {
        const QByteArray bytes = mcp::jsonLine(object);
        if (m_process->write(bytes) == bytes.size()) {
            return true;
        }
        fail(QStringLiteral("Could not write MCP request to stdio server"));
        return false;
    }

    void fail(const QString& error, bool force_kill = true) { finish(error, {}, force_kill); }

    void finish(const QString& error, const QJsonObject& response, bool force_kill) {
        bool expected = false;
        if (!m_completed.compare_exchange_strong(expected, true)) {
            return;
        }
        if (m_timeoutTimer->isActive()) {
            m_timeoutTimer->stop();
        }
        m_state->error_message = error;
        m_state->response = response;
        stopProcess(force_kill);
        m_done->release();
        m_ownerThread->quit();
    }

    void stopProcess(bool force_kill) {
#ifdef Q_OS_WIN
        if (m_jobHandle) {
            // The Job Object reaps the entire tree via KILL_ON_JOB_CLOSE -- including a server
            // that already exited on its own and left orphaned descendants (the NotRunning
            // early-return below would otherwise skip cleanup entirely). Supersedes the
            // per-process terminate/kill.
            closeJob();
            return;
        }
#endif
        // Fallback path when no Job Object could be established: best-effort per-process teardown.
        if (m_process->state() == QProcess::NotRunning) {
            return;
        }
        const qint64 pid = m_process->processId();
        m_process->closeWriteChannel();
        if (!force_kill) {
            m_process->terminate();
            return;
        }
#ifdef Q_OS_WIN
        terminateProcessTree(static_cast<DWORD>(pid));
#endif
        m_process->kill();
    }

    AiMcpStdioCallRequest m_request;
    StdioCallState* m_state{nullptr};
    QSemaphore* m_done{nullptr};
    QThread* m_ownerThread{nullptr};
    QProcess* m_process{nullptr};
    QTimer* m_timeoutTimer{nullptr};
    QByteArray m_stderrTail;
    StdioPhase m_phase{StdioPhase::Starting};
    std::atomic_bool m_completed{false};
#ifdef Q_OS_WIN
    HANDLE m_jobHandle{nullptr};
#endif
};

bool validateStdioToolCall(const AiMcpStdioCallRequest& request, QString* error_message) {
    const QString command = request.command.trimmed();
    if (command.isEmpty() || !QFileInfo::exists(command)) {
        if (error_message) {
            *error_message = QStringLiteral("MCP stdio command missing: %1").arg(request.command);
        }
        return false;
    }
    if (request.tool_name.trimmed().isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("MCP tool name is empty");
        }
        return false;
    }
    return true;
}

StdioCallState performStdioToolCall(const AiMcpStdioCallRequest& request) {
    StdioCallState state;
    QSemaphore done;
    QThread stdio_thread;
    auto* worker = new StdioToolCallWorker(request, &state, &done, &stdio_thread);
    worker->moveToThread(&stdio_thread);
    QObject::connect(&stdio_thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&stdio_thread, &QThread::started, worker, [worker]() { worker->start(); });
    stdio_thread.start();
    // Timed acquire so a worker thread that failed to start cannot hang the caller forever; the
    // worker releases within its own request timeout on every normal path, so a grace past it
    // only fires on a genuine start failure.
    const int acquire_timeout_ms = qMax(request.timeout_ms, kMinimumRequestTimeoutMs) +
                                   kSemaphoreWaitGraceMs;
    if (!done.tryAcquire(1, acquire_timeout_ms)) {
        state.error_message = QStringLiteral("MCP stdio worker thread did not start");
    }
    stdio_thread.quit();
    stdio_thread.wait();
    return state;
}

}  // namespace

QJsonObject AiMcpStdioClient::callTool(const AiMcpStdioCallRequest& request,
                                       QString* error_message) {
    if (!validateStdioToolCall(request, error_message)) {
        return {};
    }
    const StdioCallState state = performStdioToolCall(request);
    if (error_message) {
        *error_message = state.error_message;
    }
    if (!state.error_message.isEmpty()) {
        return {};
    }
    return state.response;
}

QJsonObject AiMcpStdioClient::initializePayloadForTesting() {
    return mcp::initializePayload(kInitializeId);
}

QJsonObject AiMcpStdioClient::toolCallPayloadForTesting(int id,
                                                        const QString& tool_name,
                                                        const QJsonObject& arguments) {
    return mcp::toolCallPayload(id, tool_name, arguments);
}

}  // namespace sak::ai
