// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_execution_broker.h"

#include "sak/ai/ai_credential_store.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <utility>

namespace sak::ai {

namespace {

constexpr int kTimeoutTickMs = 200;
constexpr int kMinTimeoutSeconds = 5;
constexpr int kMaxTimeoutSeconds = 3600;
constexpr int kMinOutputCap = 1024;

[[nodiscard]] QString cappedTail(const QString& value, int max_bytes) {
    if (max_bytes <= 0) {
        return {};
    }
    const QByteArray bytes = value.toUtf8();
    if (bytes.size() <= max_bytes) {
        return value;
    }
    return QString::fromUtf8(bytes.left(max_bytes)) +
           QStringLiteral("\n...[output truncated to %1 bytes]").arg(max_bytes);
}

}  // namespace

QJsonObject AiCommandResult::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("started")] = started;
    obj[QStringLiteral("cancelled")] = cancelled;
    obj[QStringLiteral("timed_out")] = timed_out;
    obj[QStringLiteral("elevated")] = elevated;
    obj[QStringLiteral("exit_code")] = exit_code;
    obj[QStringLiteral("exit_status")] = exit_status;
    obj[QStringLiteral("duration_ms")] = static_cast<double>(duration_ms);
    obj[QStringLiteral("stdout")] = CredentialStore::redactSecrets(stdout_text);
    obj[QStringLiteral("stderr")] = CredentialStore::redactSecrets(stderr_text);
    obj[QStringLiteral("error_message")] = CredentialStore::redactSecrets(error_message);
    return obj;
}

QString AiCommandResult::toJsonString() const {
    return QString::fromUtf8(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
}

ExecutionBroker::ExecutionBroker(QObject* parent) : QObject(parent) {}

ExecutionBroker::~ExecutionBroker() {
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
#ifdef _WIN32
            terminateProcessTree();
#endif
            m_process->kill();
        }
    }
#ifdef _WIN32
    closeJobHandle();
#endif
}

#ifdef _WIN32
void ExecutionBroker::assignProcessToJob() {
    if (!m_process || m_process->processId() <= 0) {
        return;
    }
    if (!m_job_handle) {
        m_job_handle = ::CreateJobObjectW(nullptr, nullptr);
        if (!m_job_handle) {
            return;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(
            m_job_handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    const HANDLE process = ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                         FALSE,
                                         static_cast<DWORD>(m_process->processId()));
    if (!process) {
        return;
    }
    ::AssignProcessToJobObject(m_job_handle, process);
    ::CloseHandle(process);
}

void ExecutionBroker::terminateProcessTree() {
    if (m_job_handle) {
        ::TerminateJobObject(m_job_handle, 1);
    }
}

void ExecutionBroker::closeJobHandle() {
    if (m_job_handle) {
        ::CloseHandle(m_job_handle);  // KILL_ON_JOB_CLOSE reaps any survivors
        m_job_handle = nullptr;
    }
}
#endif

void ExecutionBroker::setElevatedRunner(ElevatedRunner runner) {
    m_elevated_runner = std::move(runner);
}

void ExecutionBroker::setElevatedCancel(ElevatedCancel cancel) {
    m_elevated_cancel = std::move(cancel);
}

bool ExecutionBroker::isRunning() const noexcept {
    return m_running;
}

QString ExecutionBroker::runningCommandId() const {
    return m_running ? m_command_id : QString{};
}

bool ExecutionBroker::startPowerShell(const AiCommandRequest& request, const QString& command_id) {
    if (m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(command_id, std::move(fail));
        return false;
    }
    m_command_id = command_id;
    m_cancel_requested = false;
    m_finished_emitted = false;
    if (request.command.trimmed().isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("PowerShell command is empty");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.requires_admin) {
        if (!m_elevated_runner) {
            AiCommandResult result;
            result.elevated = true;
            result.error_message = QStringLiteral("Elevated AI command execution is not connected");
            emitDeferredFinish(std::move(result));
            return false;
        }
        m_running = true;
        m_elevated_in_flight = true;
        Q_EMIT started(m_command_id);
        QElapsedTimer timer;
        timer.start();
        // The runner uses ElevationBroker::executeTask which processes events
        // periodically so the UI can update and Stop can route cancel.
        AiCommandResult elevated_result = m_elevated_runner(request);
        if (elevated_result.duration_ms == 0) {
            elevated_result.duration_ms = timer.elapsed();
        }
        elevated_result.elevated = true;
        m_elevated_in_flight = false;
        completeWith(std::move(elevated_result));
        return true;
    }
    QStringList args;
    args << QStringLiteral("-NoProfile") << QStringLiteral("-ExecutionPolicy")
         << QStringLiteral("Bypass") << QStringLiteral("-Command") << request.command;
    return launchProcess({QStringLiteral("powershell.exe"),
                          args,
                          request.timeout_seconds,
                          request.max_output_bytes,
                          command_id,
                          false});
}

bool ExecutionBroker::startCmd(const AiCommandRequest& request, const QString& command_id) {
    if (m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(command_id, std::move(fail));
        return false;
    }
    m_command_id = command_id;
    m_cancel_requested = false;
    m_finished_emitted = false;
    if (request.command.trimmed().isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("CMD command is empty");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.requires_admin) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
            "Elevated cmd.exe launch is not supported; use run_powershell for admin tasks.");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    QStringList args;
    args << QStringLiteral("/c") << request.command;
    return launchProcess({QStringLiteral("cmd.exe"),
                          args,
                          request.timeout_seconds,
                          request.max_output_bytes,
                          command_id,
                          false});
}

bool ExecutionBroker::startProcess(const AiCommandRequest& request, const QString& command_id) {
    if (m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(command_id, std::move(fail));
        return false;
    }
    m_command_id = command_id;
    m_cancel_requested = false;
    m_finished_emitted = false;
    if (request.program.trimmed().isEmpty()) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Program path is empty");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    if (request.requires_admin) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral(
            "Elevated direct-process launch is not supported; use run_powershell for admin tasks.");
        emitDeferredFinish(std::move(fail));
        return false;
    }
    return launchProcess({request.program,
                          request.arguments,
                          request.timeout_seconds,
                          request.max_output_bytes,
                          command_id,
                          false});
}

bool ExecutionBroker::launchProcess(const ProcessLaunchRequest& request) {
    if (request.already_running_check && m_running) {
        AiCommandResult fail;
        fail.error_message = QStringLiteral("Broker is already running a command");
        emitDeferredStandaloneFinish(request.command_id, std::move(fail));
        return false;
    }

    m_command_id = request.command_id;
    m_max_output_bytes = std::max(request.max_output_bytes, kMinOutputCap);
    m_stdout_buffer.clear();
    m_stderr_buffer.clear();
    m_cancel_requested = false;
    m_finished_emitted = false;

    const int clamped = std::clamp(request.timeout_seconds, kMinTimeoutSeconds, kMaxTimeoutSeconds);
    m_timeout_ms = clamped * kMillisecondsPerSecond;

    m_process = std::make_unique<QProcess>(this);
    connect(m_process.get(),
            &QProcess::readyReadStandardOutput,
            this,
            &ExecutionBroker::onReadyReadStdout);
    connect(m_process.get(),
            &QProcess::readyReadStandardError,
            this,
            &ExecutionBroker::onReadyReadStderr);
    connect(m_process.get(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus status) {
                onProcessFinished(code, static_cast<int>(status));
            });
    connect(m_process.get(), &QProcess::started, this, [this]() {
#ifdef _WIN32
        assignProcessToJob();
#endif
        Q_EMIT started(m_command_id);
        if (m_timeout_ms > 0) {
            QTimer::singleShot(kTimeoutTickMs, this, &ExecutionBroker::onTimeoutTick);
        }
    });
    connect(m_process.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        onProcessError(static_cast<int>(error));
    });

    m_running = true;
    m_timer.start();
    m_process->start(request.program, request.arguments);
    return true;
}

void ExecutionBroker::cancel() {
    if (!m_running || m_cancel_requested) {
        return;
    }
    m_cancel_requested = true;
    if (m_elevated_in_flight && m_elevated_cancel) {
        m_elevated_cancel();
        return;
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
#ifdef _WIN32
        terminateProcessTree();
#endif
        m_process->kill();
    }
}

void ExecutionBroker::onReadyReadStdout() {
    if (!m_process) {
        return;
    }
    const QString chunk = QString::fromUtf8(m_process->readAllStandardOutput());
    if (chunk.isEmpty()) {
        return;
    }
    appendCapped(m_stdout_buffer, chunk);
    Q_EMIT stdoutChunk(m_command_id, chunk);
}

void ExecutionBroker::onReadyReadStderr() {
    if (!m_process) {
        return;
    }
    const QString chunk = QString::fromUtf8(m_process->readAllStandardError());
    if (chunk.isEmpty()) {
        return;
    }
    appendCapped(m_stderr_buffer, chunk);
    Q_EMIT stderrChunk(m_command_id, chunk);
}

void ExecutionBroker::onProcessFinished(int exit_code, int exit_status) {
    if (!m_process) {
        return;
    }
    onReadyReadStdout();
    onReadyReadStderr();

    AiCommandResult result;
    result.started = true;
    result.cancelled = m_cancel_requested;
    result.exit_code = exit_code;
    result.exit_status = exit_status;
    result.duration_ms = m_timer.elapsed();
    const int half_cap = std::max(m_max_output_bytes / 2, kMinOutputCap);
    result.stdout_text = cappedTail(m_stdout_buffer, half_cap);
    result.stderr_text = cappedTail(m_stderr_buffer, half_cap);
    if (m_cancel_requested) {
        result.error_message = QStringLiteral("Command cancelled");
    }
    completeWith(std::move(result));
}

void ExecutionBroker::onProcessError(int error) {
    if (!m_process || m_finished_emitted) {
        return;
    }
    if (static_cast<QProcess::ProcessError>(error) == QProcess::FailedToStart) {
        AiCommandResult result;
        result.error_message =
            QStringLiteral("Process start error: %1").arg(m_process->errorString());
        completeWith(std::move(result));
    }
}

void ExecutionBroker::onTimeoutTick() {
    if (!m_running || m_finished_emitted || !m_process) {
        return;
    }
    if (m_process->state() == QProcess::NotRunning) {
        return;
    }
    if (m_timeout_ms > 0 && m_timer.elapsed() >= m_timeout_ms) {
        m_cancel_requested = false;
        if (m_process) {
#ifdef _WIN32
            terminateProcessTree();
#endif
            m_process->kill();
        }
        onReadyReadStdout();
        onReadyReadStderr();
        AiCommandResult result;
        result.started = true;
        result.timed_out = true;
        result.exit_code = -1;
        result.duration_ms = m_timer.elapsed();
        const int half_cap = std::max(m_max_output_bytes / 2, kMinOutputCap);
        result.stdout_text = cappedTail(m_stdout_buffer, half_cap);
        result.stderr_text = cappedTail(m_stderr_buffer, half_cap);
        result.error_message = QStringLiteral("Command timed out");
        completeWith(std::move(result));
        return;
    }
    QTimer::singleShot(kTimeoutTickMs, this, &ExecutionBroker::onTimeoutTick);
}

void ExecutionBroker::completeWith(AiCommandResult result) {
    if (m_finished_emitted) {
        return;
    }
    m_finished_emitted = true;
    m_running = false;
    const QString id = m_command_id;
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
#ifdef _WIN32
            terminateProcessTree();
#endif
            m_process->kill();
        }
        m_process.reset();
    }
#ifdef _WIN32
    closeJobHandle();
#endif
    Q_EMIT finished(id, result);
}

void ExecutionBroker::emitDeferredFinish(AiCommandResult result) {
    const QString id = m_command_id;
    QTimer::singleShot(0, this, [this, id, result]() {
        if (m_finished_emitted) {
            return;
        }
        m_finished_emitted = true;
        m_running = false;
        Q_EMIT finished(id, result);
    });
}

void ExecutionBroker::emitDeferredStandaloneFinish(const QString& command_id,
                                                   AiCommandResult result) {
    QTimer::singleShot(0, this, [this, command_id, result]() {
        Q_EMIT this->finished(command_id, result);
    });
}

void ExecutionBroker::appendCapped(QString& target, const QString& chunk) const {
    target.append(chunk);
    const QByteArray bytes = target.toUtf8();
    if (bytes.size() > m_max_output_bytes) {
        target = QString::fromUtf8(bytes.right(m_max_output_bytes));
    }
}

AiCommandRequest ExecutionBroker::requestFromJson(const QJsonObject& args) {
    AiCommandRequest request;
    request.command = args.value(QStringLiteral("command")).toString();
    request.timeout_seconds = std::clamp(
        args.value(QStringLiteral("timeout_seconds")).toInt(kAiCommandDefaultTimeoutSeconds),
        kMinTimeoutSeconds,
        kMaxTimeoutSeconds);
    request.requires_admin = args.value(QStringLiteral("requires_admin")).toBool(false);
    return request;
}

AiCommandRequest ExecutionBroker::processRequestFromJson(const QJsonObject& args) {
    AiCommandRequest request;
    request.program = args.value(QStringLiteral("program")).toString();
    const auto arg_array = args.value(QStringLiteral("arguments")).toArray();
    for (const auto& value : arg_array) {
        request.arguments.append(value.toString());
    }
    request.timeout_seconds = std::clamp(
        args.value(QStringLiteral("timeout_seconds")).toInt(kAiCommandDefaultTimeoutSeconds),
        kMinTimeoutSeconds,
        kMaxTimeoutSeconds);
    request.requires_admin = args.value(QStringLiteral("requires_admin")).toBool(false);
    return request;
}

}  // namespace sak::ai
