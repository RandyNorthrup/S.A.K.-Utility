// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_mcp_stdio_client.h"

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

[[nodiscard]] QJsonObject initializePayload() {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), kInitializeId},
        {QStringLiteral("method"), QStringLiteral("initialize")},
        {QStringLiteral("params"),
         QJsonObject{
             {QStringLiteral("protocolVersion"), QStringLiteral("2024-11-05")},
             {QStringLiteral("capabilities"), QJsonObject{}},
             {QStringLiteral("clientInfo"),
              QJsonObject{{QStringLiteral("name"), QStringLiteral("sak-utility")},
                          {QStringLiteral("version"), QStringLiteral("1")}}},
         }},
    };
}

[[nodiscard]] QJsonObject initializedNotification() {
    return QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                       {QStringLiteral("method"), QStringLiteral("notifications/initialized")},
                       {QStringLiteral("params"), QJsonObject{}}};
}

[[nodiscard]] QJsonObject toolCallPayload(int id,
                                          const QString& tool_name,
                                          const QJsonObject& arguments) {
    return QJsonObject{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), QStringLiteral("tools/call")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("name"), tool_name},
                     {QStringLiteral("arguments"), arguments}}},
    };
}

[[nodiscard]] QByteArray jsonLine(const QJsonObject& object) {
    QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    bytes.append('\n');
    return bytes;
}

[[nodiscard]] QJsonObject parseJsonLine(const QByteArray& line, QString* error_message) {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = QStringLiteral("Invalid MCP stdio JSON response: %1")
                                 .arg(parse_error.errorString());
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return doc.object();
}

enum class StdioPhase {
    Starting,
    AwaitInitialize,
    AwaitTool,
};

struct StdioCallState {
    QJsonObject response;
    QString error_message;
};

QString processStderr(QProcess* process) {
    return QString::fromUtf8(process ? process->readAllStandardError() : QByteArray()).trimmed();
}

QString timeoutMessage(QProcess* process) {
    const QString stderr_text = processStderr(process);
    return stderr_text.isEmpty() ? QStringLiteral("MCP stdio request timed out")
                                 : QStringLiteral("MCP stdio request timed out: %1")
                                       .arg(stderr_text.left(kMcpStdioErrorPreviewChars));
}

QString exitedMessage(QProcess* process) {
    const QString stderr_text = processStderr(process);
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
                             fail(exitedMessage(m_process), false);
                         });
        QObject::connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
            handleReadyRead();
        });
        QObject::connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
            fail(timeoutMessage(m_process));
        });
    }

    void onStarted() {
        m_phase = StdioPhase::AwaitInitialize;
        (void)write(initializePayload());
    }

    void handleReadyRead() {
        while (m_process->canReadLine()) {
            if (!processLine(m_process->readLine().trimmed())) {
                return;
            }
        }
    }

    bool processLine(const QByteArray& line) {
        if (line.isEmpty()) {
            return true;
        }
        QString parse_error;
        const QJsonObject message = parseJsonLine(line, &parse_error);
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
        if (m_phase == StdioPhase::AwaitInitialize) {
            return sendToolCall();
        }
        finish({}, message, true);
        return false;
    }

    bool sendToolCall() {
        m_phase = StdioPhase::AwaitTool;
        if (!write(initializedNotification())) {
            return false;
        }
        return write(toolCallPayload(kToolCallId, m_request.tool_name, m_request.arguments));
    }

    bool write(const QJsonObject& object) {
        const QByteArray bytes = jsonLine(object);
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
    StdioPhase m_phase{StdioPhase::Starting};
    std::atomic_bool m_completed{false};
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
    done.acquire();
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
    return initializePayload();
}

QJsonObject AiMcpStdioClient::toolCallPayloadForTesting(int id,
                                                        const QString& tool_name,
                                                        const QJsonObject& arguments) {
    return toolCallPayload(id, tool_name, arguments);
}

}  // namespace sak::ai
