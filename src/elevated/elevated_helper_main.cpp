// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevated_helper_main.cpp
/// @brief Entry point for the elevated helper process (sak_elevated_helper.exe)
///
/// This is a headless process launched via UAC by the main S.A.K. Utility.
/// It creates a named pipe, waits for IPC commands, and executes elevated
/// tasks on behalf of the parent process. Auto-exits after inactivity timeout.

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/app_paths.h"
#include "sak/elevated_pipe_protocol.h"
#include "sak/elevated_pipe_server.h"
#include "sak/elevated_task_dispatcher.h"
#include "sak/error_codes.h"
#include "sak/logger.h"
#include "sak/permission_manager.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// ======================================================================
// Command-Line Parsing
// ======================================================================

struct HelperArgs {
    QString pipe_name;
    qint64 parent_pid{0};
};

HelperArgs parseArgs(int argc, char* argv[]) {
    HelperArgs args;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--pipe" && i + 1 < argc) {
            args.pipe_name = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--parent-pid" && i + 1 < argc) {
            args.parent_pid = QString::fromLocal8Bit(argv[++i]).toLongLong();
        }
    }
    return args;
}

void configurePortableRuntimeDirs() {
    const QString temp_dir = sak::app_paths::tempDirectory();
    if (!sak::app_paths::ensureDirectory(temp_dir)) {
        return;
    }
    const QByteArray native_temp = QDir::toNativeSeparators(temp_dir).toLocal8Bit();
    qputenv("TMP", native_temp);
    qputenv("TEMP", native_temp);
}

// ======================================================================
// Quick Action Task Handler
// ======================================================================

/// @brief Generic handler that instantiates a QuickAction, runs it, and
///        relays progress back through the pipe.
template <typename ActionT, typename... CtorArgs>
sak::TaskHandlerResult runQuickAction(const QJsonObject& payload,
                                      sak::ProgressCallback progress,
                                      sak::CancelCheck is_cancelled,
                                      CtorArgs&&... ctor_args) {
    (void)payload;

    auto action = std::make_unique<ActionT>(std::forward<CtorArgs>(ctor_args)...);

    // Connect action progress to the progress callback
    QObject::connect(action.get(),
                     &sak::QuickAction::executionProgress,
                     [&progress](const QString& msg, int pct) { progress(pct, msg); });

    // Check cancellation periodically via the action's own mechanism
    QTimer cancel_timer;
    cancel_timer.setInterval(500);
    QObject::connect(&cancel_timer, &QTimer::timeout, [&]() {
        if (is_cancelled()) {
            action->cancel();
        }
    });
    cancel_timer.start();

    // Execute the action synchronously
    action->execute();
    cancel_timer.stop();

    sak::TaskHandlerResult result;
    auto exec_result = action->lastExecutionResult();
    result.success = exec_result.success;

    QJsonObject data;
    data["message"] = exec_result.message;
    data["log"] = exec_result.log;
    data["status"] = static_cast<int>(action->status());
    result.data = data;

    if (!result.success) {
        result.error_message = exec_result.message;
    }

    return result;
}

// ======================================================================
// Task Registration
// ======================================================================

sak::TaskHandlerResult runPermissionTask(
    const QJsonObject& payload,
    sak::ProgressCallback progress,
    std::function<std::expected<void, sak::error_code>(
        sak::PermissionManager&, const QString&, const QString&)> operation,
    const QString& action_label,
    bool needs_user_sid) {
    QString path = payload["path"].toString();
    if (path.isEmpty()) {
        return {false, {}, "Missing path in payload"};
    }
    QString user_sid = needs_user_sid ? payload["user_sid"].toString() : QString();
    if (needs_user_sid && user_sid.isEmpty()) {
        return {false, {}, "Missing user_sid in payload"};
    }
    progress(0, action_label + path);
    sak::PermissionManager pm;
    auto result = operation(pm, path, user_sid);
    if (!result) {
        return {false, {}, pm.getLastError()};
    }
    progress(100, action_label + QStringLiteral("done"));
    return {true, {}, {}};
}

QString cappedTaskOutput(const QString& value, int max_bytes) {
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

#ifdef _WIN32
struct ElevatedJobGuard {
    HANDLE job{nullptr};
    ~ElevatedJobGuard() {
        if (job) {
            ::CloseHandle(job);  // KILL_ON_JOB_CLOSE reaps survivors
        }
    }
    void assign(qint64 pid) {
        if (!job) {
            job = ::CreateJobObjectW(nullptr, nullptr);
            if (!job) {
                return;
            }
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(
                job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        if (pid <= 0) {
            return;
        }
        const HANDLE handle =
            ::OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (!handle) {
            return;
        }
        ::AssignProcessToJobObject(job, handle);
        ::CloseHandle(handle);
    }
    void terminate() const {
        if (job) {
            ::TerminateJobObject(job, 1);
        }
    }
};
#endif

QJsonObject baseElevatedPowerShellData() {
    QJsonObject data;
    data[QStringLiteral("started")] = false;
    data[QStringLiteral("cancelled")] = false;
    data[QStringLiteral("timed_out")] = false;
    data[QStringLiteral("elevated")] = true;
    data[QStringLiteral("exit_code")] = -1;
    data[QStringLiteral("exit_status")] = 0;
    data[QStringLiteral("duration_ms")] = 0.0;
    return data;
}

sak::TaskHandlerResult elevatedPowerShellFailure(QJsonObject data,
                                                 const QString& message,
                                                 qint64 duration_ms = 0) {
    sak::TaskHandlerResult task;
    task.success = false;
    task.error_message = message;
    data[QStringLiteral("duration_ms")] = static_cast<double>(duration_ms);
    data[QStringLiteral("error_message")] = message;
    task.data = data;
    return task;
}

void drainPowerShellOutput(QProcess& ps,
                           sak::ProgressCallback progress,
                           QString& stdout_accumulator,
                           QString& stderr_accumulator) {
    const QByteArray new_stdout = ps.readAllStandardOutput();
    const QByteArray new_stderr = ps.readAllStandardError();
    if (!new_stdout.isEmpty()) {
        const QString chunk = QString::fromUtf8(new_stdout);
        stdout_accumulator.append(chunk);
        progress(50, QStringLiteral("STDOUT|") + chunk);
    }
    if (!new_stderr.isEmpty()) {
        const QString chunk = QString::fromUtf8(new_stderr);
        stderr_accumulator.append(chunk);
        progress(50, QStringLiteral("STDERR|") + chunk);
    }
}

void stopElevatedPowerShell(QProcess& ps
#ifdef _WIN32
                            ,
                            const ElevatedJobGuard& job
#endif
) {
#ifdef _WIN32
    job.terminate();
#endif
    ps.kill();
    ps.waitForFinished(2000);
}

struct ElevatedPowerShellLoopResult {
    bool cancelled{false};
    bool timed_out{false};
};

struct ElevatedPowerShellLoopContext {
    QProcess* ps{nullptr};
    const QElapsedTimer* timer{nullptr};
    int timeout_ms{0};
    sak::ProgressCallback progress;
    sak::CancelCheck is_cancelled;
    QString* stdout_accumulator{nullptr};
    QString* stderr_accumulator{nullptr};
#ifdef _WIN32
    const ElevatedJobGuard* job{nullptr};
#endif
};

ElevatedPowerShellLoopResult waitForElevatedPowerShell(
    const ElevatedPowerShellLoopContext& context) {
    ElevatedPowerShellLoopResult result;
    while (true) {
        const bool finished = context.ps->waitForFinished(250);
        drainPowerShellOutput(*context.ps,
                              context.progress,
                              *context.stdout_accumulator,
                              *context.stderr_accumulator);
        if (finished) {
            break;
        }
        if (context.is_cancelled && context.is_cancelled()) {
            result.cancelled = true;
            stopElevatedPowerShell(*context.ps
#ifdef _WIN32
                                   ,
                                   *context.job
#endif
            );
            drainPowerShellOutput(*context.ps,
                                  context.progress,
                                  *context.stdout_accumulator,
                                  *context.stderr_accumulator);
            break;
        }
        if (context.timer->elapsed() >= context.timeout_ms) {
            result.timed_out = true;
            stopElevatedPowerShell(*context.ps
#ifdef _WIN32
                                   ,
                                   *context.job
#endif
            );
            drainPowerShellOutput(*context.ps,
                                  context.progress,
                                  *context.stdout_accumulator,
                                  *context.stderr_accumulator);
            break;
        }
    }
    return result;
}

sak::TaskHandlerResult runElevatedPowerShellTask(const QJsonObject& payload,
                                                 sak::ProgressCallback progress,
                                                 sak::CancelCheck is_cancelled) {
    QJsonObject data = baseElevatedPowerShellData();

    const QString command = payload[QStringLiteral("command")].toString();
    if (command.trimmed().isEmpty()) {
        return elevatedPowerShellFailure(data, QStringLiteral("PowerShell command is empty"));
    }

    const int timeout_ms =
        std::clamp(payload[QStringLiteral("timeout_seconds")].toInt(120), 5, 3600) * 1000;
    const int output_cap = std::clamp(payload[QStringLiteral("max_output_bytes")].toInt(262'144),
                                      4096,
                                      4 * 1024 * 1024);
    const int half_cap = std::max(output_cap / 2, 1024);

    QProcess ps;
    ps.setProcessChannelMode(QProcess::SeparateChannels);
    QElapsedTimer timer;
    timer.start();

    progress(0, QStringLiteral("Starting elevated PowerShell"));
    ps.start(QStringLiteral("powershell.exe"),
             QStringList{QStringLiteral("-NoProfile"),
                         QStringLiteral("-ExecutionPolicy"),
                         QStringLiteral("Bypass"),
                         QStringLiteral("-Command"),
                         command});

    if (!ps.waitForStarted(5000)) {
        return elevatedPowerShellFailure(
            data,
            QStringLiteral("Failed to start elevated PowerShell: %1").arg(ps.errorString()),
            timer.elapsed());
    }

#ifdef _WIN32
    ElevatedJobGuard job;
    job.assign(ps.processId());
#endif

    data[QStringLiteral("started")] = true;
    progress(5, QStringLiteral("Elevated PowerShell running"));

    QString stdout_accumulator;
    QString stderr_accumulator;
    ElevatedPowerShellLoopContext loop_context;
    loop_context.ps = &ps;
    loop_context.timer = &timer;
    loop_context.timeout_ms = timeout_ms;
    loop_context.progress = progress;
    loop_context.is_cancelled = is_cancelled;
    loop_context.stdout_accumulator = &stdout_accumulator;
    loop_context.stderr_accumulator = &stderr_accumulator;
#ifdef _WIN32
    loop_context.job = &job;
#endif
    const ElevatedPowerShellLoopResult loop_result = waitForElevatedPowerShell(loop_context);

    data[QStringLiteral("cancelled")] = loop_result.cancelled;
    data[QStringLiteral("timed_out")] = loop_result.timed_out;
    data[QStringLiteral("exit_code")] = ps.exitCode();
    data[QStringLiteral("exit_status")] = static_cast<int>(ps.exitStatus());
    data[QStringLiteral("duration_ms")] = static_cast<double>(timer.elapsed());
    data[QStringLiteral("stdout")] = cappedTaskOutput(stdout_accumulator, half_cap);
    data[QStringLiteral("stderr")] = cappedTaskOutput(stderr_accumulator, half_cap);
    data[QStringLiteral("stdout_full_bytes")] =
        static_cast<double>(stdout_accumulator.toUtf8().size());
    data[QStringLiteral("stderr_full_bytes")] =
        static_cast<double>(stderr_accumulator.toUtf8().size());
    if (loop_result.timed_out) {
        data[QStringLiteral("error_message")] = QStringLiteral("Command timed out");
    } else if (loop_result.cancelled) {
        data[QStringLiteral("error_message")] = QStringLiteral("Command cancelled");
    } else {
        data[QStringLiteral("error_message")] = QString();
    }

    progress(100, QStringLiteral("Elevated PowerShell finished"));
    sak::TaskHandlerResult task;
    task.success = true;
    task.data = data;
    return task;
}

void registerQuickActionTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(QStringLiteral("Verify System Files"),
                               [](const QJsonObject& payload,
                                  sak::ProgressCallback progress,
                                  sak::CancelCheck is_cancelled) {
                                   return runQuickAction<sak::VerifySystemFilesAction>(
                                       payload, std::move(progress), std::move(is_cancelled));
                               });

    dispatcher.registerHandler(QStringLiteral("Check Disk Errors"),
                               [](const QJsonObject& payload,
                                  sak::ProgressCallback progress,
                                  sak::CancelCheck is_cancelled) {
                                   return runQuickAction<sak::CheckDiskErrorsAction>(
                                       payload, std::move(progress), std::move(is_cancelled));
                               });

    dispatcher.registerHandler(QStringLiteral("Reset Network"),
                               [](const QJsonObject& payload,
                                  sak::ProgressCallback progress,
                                  sak::CancelCheck is_cancelled) {
                                   return runQuickAction<sak::ResetNetworkAction>(
                                       payload, std::move(progress), std::move(is_cancelled));
                               });

    dispatcher.registerHandler(
        QStringLiteral("Backup BitLocker Keys"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck is_cancelled) {
            QString backup_location = payload["backup_location"].toString("C:/SAK_Backups");
            return runQuickAction<sak::BackupBitlockerKeysAction>(
                payload, std::move(progress), std::move(is_cancelled), backup_location);
        });
}

void registerPermissionTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(
        QStringLiteral("TakeOwnership"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck /*is_cancelled*/) -> sak::TaskHandlerResult {
            return runPermissionTask(
                payload,
                std::move(progress),
                [](sak::PermissionManager& pm, const QString& path, const QString& sid) {
                    return pm.tryTakeOwnership(path, sid);
                },
                QStringLiteral("Taking ownership of: "),
                true);
        });

    dispatcher.registerHandler(
        QStringLiteral("StripPermissions"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck /*is_cancelled*/) -> sak::TaskHandlerResult {
            return runPermissionTask(
                payload,
                std::move(progress),
                [](sak::PermissionManager& pm, const QString& path, const QString& /*sid*/) {
                    return pm.tryStripPermissions(path);
                },
                QStringLiteral("Stripping permissions: "),
                false);
        });

    dispatcher.registerHandler(
        QStringLiteral("SetStandardPermissions"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck /*is_cancelled*/) -> sak::TaskHandlerResult {
            return runPermissionTask(
                payload,
                std::move(progress),
                [](sak::PermissionManager& pm, const QString& path, const QString& sid) {
                    return pm.trySetStandardUserPermissions(path, sid);
                },
                QStringLiteral("Setting permissions: "),
                true);
        });
}

void registerFileTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(
        QStringLiteral("BackupFile"),
        [](const QJsonObject& payload,
           sak::ProgressCallback progress,
           sak::CancelCheck /*is_cancelled*/) -> sak::TaskHandlerResult {
            QString source = payload["source"].toString();
            QString destination = payload["destination"].toString();
            if (source.isEmpty() || destination.isEmpty()) {
                return {false, {}, "Missing source or destination in payload"};
            }
            progress(0, QStringLiteral("Copying: ") + source);
            if (!QFile::copy(source, destination)) {
                return {false, {}, QStringLiteral("Failed to copy: ") + source};
            }
            progress(100, QStringLiteral("File copied"));
            return {true, {}, {}};
        });

    dispatcher.registerHandler(
        QStringLiteral("ReadThermalData"),
        [](const QJsonObject& /*payload*/,
           sak::ProgressCallback progress,
           sak::CancelCheck /*is_cancelled*/) -> sak::TaskHandlerResult {
            progress(0, QStringLiteral("Querying thermal sensors..."));

            QProcess ps;
            ps.setProcessChannelMode(QProcess::MergedChannels);
            QString script = QStringLiteral(
                "try{"
                "$t=Get-CimInstance -Namespace root/WMI "
                "-ClassName MSAcpi_ThermalZoneTemperature "
                "-ErrorAction Stop|Select-Object -First 1;"
                "if($t.CurrentTemperature -gt 0){"
                "Write-Output ([math]::Round(($t.CurrentTemperature/10)-273.15,1))"
                "}else{Write-Output '-1'}"
                "}catch{Write-Output '-1'}");
            ps.start("powershell.exe", {"-NoProfile", "-NoLogo", "-Command", script});

            if (!ps.waitForStarted(5000) || !ps.waitForFinished(10'000)) {
                ps.kill();
                ps.waitForFinished(2000);
                return {false, {}, "Thermal query timed out"};
            }

            QString output = QString::fromUtf8(ps.readAllStandardOutput()).trimmed();
            bool ok = false;
            double temp = output.toDouble(&ok);
            if (!ok || temp <= 0) {
                return {false, {}, "No thermal data available"};
            }

            QJsonObject data;
            data["cpu_temperature"] = temp;
            progress(100, QStringLiteral("Thermal data retrieved"));
            return {true, data, {}};
        });
}

void registerAiTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    dispatcher.registerHandler(QStringLiteral("RunPowerShell"), runElevatedPowerShellTask);
}

void registerAllTasks(sak::ElevatedTaskDispatcher& dispatcher) {
    registerQuickActionTasks(dispatcher);
    registerPermissionTasks(dispatcher);
    registerFileTasks(dispatcher);
    registerAiTasks(dispatcher);
}

// ======================================================================
// Main Event Loop
// ======================================================================

int runHelper(sak::ElevatedPipeServer& server, sak::ElevatedTaskDispatcher& dispatcher) {
    bool cancel_requested = false;

    server.sendReady();
    sak::logInfo("ElevatedHelper: sent Ready, entering task loop");

    while (true) {
        auto msg = server.readMessage();
        if (!msg) {
            sak::logInfo("ElevatedHelper: client disconnected — shutting down");
            break;
        }

        switch (msg->type) {
        case sak::PipeMessageType::Shutdown: {
            sak::logInfo("ElevatedHelper: received Shutdown");
            return 0;
        }
        case sak::PipeMessageType::CancelRequest: {
            cancel_requested = true;
            sak::logInfo("ElevatedHelper: cancel requested");
            break;
        }
        case sak::PipeMessageType::TaskRequest: {
            QString task_id = msg->json["task"].toString();
            QJsonObject payload = msg->json["payload"].toObject();
            cancel_requested = false;

            sak::logInfo("ElevatedHelper: executing task '{}'", task_id.toStdString());

            auto progress_cb = [&server](int pct, const QString& status) {
                server.sendProgress(pct, status);
            };
            auto cancel_cb = [&cancel_requested]() {
                return cancel_requested;
            };

            auto result = dispatcher.dispatch(task_id, payload, progress_cb, cancel_cb);

            if (result) {
                server.sendResult(result->success, result->data);
            } else {
                server.sendError(static_cast<int>(result.error()),
                                 QString::fromStdString(
                                     std::string(sak::to_string(result.error()))));
            }
            break;
        }
        default:
            sak::logWarning("ElevatedHelper: unexpected message type {}",
                            static_cast<int>(msg->type));
            break;
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("SAK Elevated Helper");
    configurePortableRuntimeDirs();

    // Initialize logger
    const QString log_path = sak::app_paths::logsDirectory();
    auto log_dir = std::filesystem::path(log_path.toStdWString());
    auto& logger = sak::logger::instance();
    if (auto result = logger.initialize(log_dir); !result) {
        return 1;
    }

    sak::logInfo("========================================");
    sak::logInfo("SAK Elevated Helper starting");
    sak::logInfo("========================================");

    HelperArgs args = parseArgs(argc, argv);
    if (args.pipe_name.isEmpty()) {
        sak::logError("ElevatedHelper: --pipe argument required");
        return 1;
    }

    sak::logInfo("ElevatedHelper: pipe='{}', parent_pid={}",
                 args.pipe_name.toStdString(),
                 args.parent_pid);

    // Register task handlers (compile-time allowlist)
    sak::ElevatedTaskDispatcher dispatcher;
    registerAllTasks(dispatcher);
    sak::logInfo("ElevatedHelper: registered {} task handlers", dispatcher.handlerCount());

    // Start pipe server and wait for connection
    sak::ElevatedPipeServer server(args.pipe_name, args.parent_pid);
    if (!server.start()) {
        sak::logError("ElevatedHelper: failed to start pipe server");
        return 2;
    }

    int exit_code = runHelper(server, dispatcher);

    server.stop();
    sak::logInfo("ElevatedHelper: exiting with code {}", exit_code);
    logger.flush();

    return exit_code;
}
