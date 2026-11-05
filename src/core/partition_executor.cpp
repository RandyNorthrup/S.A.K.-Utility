// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_executor.cpp
/// @brief Queued operation execution for Partition Manager.

#include "sak/partition_executor.h"

#include "sak/elevation_broker.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <vector>

namespace sak {

static_assert(kPartitionLongTaskTimeoutSeconds <= kElevatedTaskMaxTimeoutSeconds,
              "a long partition task must not be cut short by the helper's own limit");

namespace {
constexpr int kDefaultPartitionTaskTimeoutSeconds = 1800;
constexpr int kMaxPartitionTaskOutputBytes = 1024 * 1024;

#ifdef SAK_PARTITION_FS_TEST_HOOKS
std::function<void(const std::shared_ptr<ElevationBroker>&)>& brokerRegisteredObserver() {
    static std::function<void(const std::shared_ptr<ElevationBroker>&)> observer;
    return observer;
}
#endif

PartitionExecutionStep blockedExecutionStep(const PartitionOperation& operation,
                                            const QStringList& blockers) {
    PartitionExecutionStep step;
    step.operation_id = operation.id;
    step.summary = operation.summary;
    step.error_message = blockers.join(QStringLiteral("; "));
    return step;
}

PartitionExecutionStep dryRunExecutionStep(const PartitionOperation& operation,
                                           const PartitionScript& script) {
    PartitionExecutionStep step;
    step.operation_id = operation.id;
    step.summary = operation.summary;
    step.success = true;
    step.skipped = true;
    step.stdout_text = script.preview + QStringLiteral("\n\n") +
                       (script.dry_run_script.isEmpty() ? script.script : script.dry_run_script);
    return step;
}

PartitionExecutionStep newScriptExecutionStep(const PartitionOperation& operation) {
    PartitionExecutionStep step;
    step.operation_id = operation.id;
    step.summary = operation.summary;
    return step;
}

void markCancelled(PartitionExecutionResult* result) {
    result->cancelled = true;
    result->message = QStringLiteral("Partition operation batch cancelled");
}

// Materializes a script's PartitionScriptCredential secrets into locked temp files for the
// lifetime of one execution, substituting each placeholder in the script with its temp path.
// On destruction it overwrites each file with zeros and removes it, so a credential (e.g. a
// FileVault format password) never persists on disk and never appears in the script text or
// any child-process command line. A no-op (the script is returned verbatim) when the script
// carries no credentials, so every existing operation is byte-for-byte unaffected.
class StagedScriptCredentials {
public:
    explicit StagedScriptCredentials(const PartitionScript& script) : m_script(script.script) {
        for (const auto& credential : script.credential_files) {
            auto file = std::make_unique<QTemporaryFile>(
                QDir::tempPath() + QStringLiteral("/sak-apfs-cred-XXXXXX.tmp"));
            file->setAutoRemove(true);
            if (!file->open()) {
                m_ok = false;
                m_error = QStringLiteral("Unable to stage encryption credential file");
                return;
            }
            // Best-effort owner-only permissions; %TEMP% is already user-scoped on Windows.
            file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            const QByteArray secret = credential.secret.toUtf8();
            // Fail closed on a short write / flush failure (e.g. a full TEMP): a 0-byte or
            // partial credential file would otherwise silently change the encryption password
            // (e.g. to empty) instead of aborting the operation.
            if (file->write(secret) != secret.size() || !file->flush()) {
                m_ok = false;
                m_error = QStringLiteral(
                    "Unable to write encryption credential file (disk full or "
                    "I/O error)");
                return;
            }
            // Release the parent handle so the writer child can open the file; the
            // QTemporaryFile object keeps the path alive until this guard is destroyed.
            file->close();
            // The placeholder is embedded inside a single-quoted PowerShell literal
            // (e.g. -VolumePasswordFile '<placeholder>'). Only the apostrophe is
            // special there, so double it: a temp path under a username such as
            // O'Brien, or an attacker-crafted TEMP directory, would otherwise close
            // the literal and inject into the elevated RunPowerShell command.
            const QString stagedPath = QString(QDir::toNativeSeparators(file->fileName()))
                                           .replace(QLatin1Char('\''), QStringLiteral("''"));
            m_script.replace(credential.placeholder, stagedPath);
            m_secret_sizes.push_back(static_cast<int>(secret.size()));
            m_files.push_back(std::move(file));
        }
    }

    StagedScriptCredentials(const StagedScriptCredentials&) = delete;
    StagedScriptCredentials& operator=(const StagedScriptCredentials&) = delete;

    ~StagedScriptCredentials() {
        for (std::size_t i = 0; i < m_files.size(); ++i) {
            if (m_files[i]->open()) {
                m_files[i]->write(QByteArray(m_secret_sizes[i], '\0'));
                m_files[i]->flush();
                m_files[i]->close();
            }
        }
        // QTemporaryFile destructors remove the files.
    }

    [[nodiscard]] bool ok() const noexcept { return m_ok; }
    [[nodiscard]] const QString& error() const noexcept { return m_error; }
    [[nodiscard]] const QString& script() const noexcept { return m_script; }

private:
    QString m_script;
    std::vector<std::unique_ptr<QTemporaryFile>> m_files;
    std::vector<int> m_secret_sizes;
    bool m_ok{true};
    QString m_error;
};
}  // namespace

PartitionExecutor::PartitionExecutor(QObject* parent) : QObject(parent) {}

// What the script said about itself -> structured helper error -> structured data error -> script
// stderr -> exit code. The helper's own line for a script that stopped is only "Command exited with
// code 1", and a person reading that cannot tell a refusal that wrote nothing from a copy that
// failed halfway.
QString PartitionExecutor::elevatedFailureMessage(const QString& helper_error,
                                                  const QString& structured_error,
                                                  const QString& std_err,
                                                  int exit_code) {
    if (const QString reason = scriptFailureReason(std_err); exit_code != 0 && !reason.isEmpty()) {
        return reason;
    }
    if (!helper_error.isEmpty()) {
        return helper_error;
    }
    if (!structured_error.isEmpty()) {
        return structured_error;
    }
    const QString trimmed = std_err.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed;
    }
    return QStringLiteral("Operation failed (exit code %1) with no error output").arg(exit_code);
}

QString PartitionExecutor::scriptFailureReason(const QString& std_err) {
    QStringList reason;
    for (const auto& line : std_err.split(QLatin1Char('\n'))) {
        const QString text = line.trimmed();
        if (text.startsWith(QStringLiteral("At line:")) ||
            text.startsWith(QStringLiteral("At C:")) || text.startsWith(QStringLiteral("+ "))) {
            break;
        }
        if (!text.isEmpty()) {
            reason.append(text);
        }
    }
    const QString text = reason.join(QLatin1Char(' '));
    // A failure in the script's own .NET code arrives wrapped in PowerShell's account of the call.
    static const QRegularExpression kWrappedCall(
        QStringLiteral("^Exception calling \"[^\"]*\" with \"\\d+\" argument\\(s\\): \"(.*)\"$"));
    const auto wrapped = kWrappedCall.match(text);
    return wrapped.hasMatch() ? wrapped.captured(1) : text;
}

PartitionExecutionResult PartitionExecutor::execute(const QVector<PartitionOperation>& operations,
                                                    bool dry_run,
                                                    bool use_elevation) {
    PartitionExecutionResult result;
    result.batch_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.dry_run = dry_run;

    // Honor a cancel that arrived after this batch was dispatched but before the worker
    // thread began running it. The executor is constructed fresh per apply (m_cancelled
    // defaults false), so a flag set here can only be a genuine pre-dispatch cancel --
    // clearing it and running the destructive batch anyway would ignore the user.
    if (m_cancelled.load(std::memory_order_relaxed)) {
        markCancelled(&result);
        return result;
    }

    if (operations.isEmpty()) {
        result.message = QStringLiteral("No partition operations queued");
        return result;
    }

    for (int i = 0; i < operations.size(); ++i) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            markCancelled(&result);
            break;
        }

        const auto& operation = operations.at(i);
        Q_EMIT progressUpdated(static_cast<int>((i * kPercentMax) / operations.size()),
                               operation.summary);
        const auto step = executeOperation(operation, dry_run, use_elevation);
        result.steps.append(step);
        if (m_cancelled.load(std::memory_order_relaxed)) {
            markCancelled(&result);
            break;
        }
        if (!step.success) {
            result.message = step.error_message;
            break;
        }
    }

    result.success = !result.cancelled && std::ranges::all_of(result.steps, [](const auto& step) {
        return step.success;
    });
    if (result.success) {
        result.message = dry_run ? QStringLiteral("Dry run complete")
                                 : QStringLiteral("Partition operation batch complete");
    }
    Q_EMIT progressUpdated(kPercentMax, result.message);
    return result;
}

void PartitionExecutor::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(m_active_broker_mutex);
    if (m_active_broker != nullptr) {
        m_active_broker->cancelCurrentTask();
    }
}

PartitionExecutionStep PartitionExecutor::executeOperation(const PartitionOperation& operation,
                                                           bool dry_run,
                                                           bool use_elevation) {
    const auto script = m_script_builder.buildScript(operation);
    if (!script.valid()) {
        return blockedExecutionStep(operation, script.blockers);
    }
    return dry_run ? dryRunExecutionStep(operation, script)
                   : executeScript(operation, script, use_elevation);
}

PartitionExecutionStep PartitionExecutor::executeScript(const PartitionOperation& operation,
                                                        const PartitionScript& script,
                                                        bool use_elevation) {
    return use_elevation ? executeElevatedScript(operation, script)
                         : executeLocalScript(operation, script);
}

QMetaObject::Connection PartitionExecutor::relayHelperProgress(
    ElevationBroker* broker, const std::shared_ptr<QString>& pending_output) {
    // The script's own output goes to the log a line at a time. Passed on as progress it reached
    // the status bar as raw "STDOUT|..." text with the bar stuck at 50%, and the log stayed empty.
    return connect(broker,
                   &ElevationBroker::progressUpdated,
                   this,
                   [this, pending_output](int percent, const QString& status) {
                       bool is_output = false;
                       const QStringList lines =
                           completedOutputLines(status, pending_output.get(), &is_output);
                       if (!is_output) {
                           Q_EMIT progressUpdated(percent, status);
                           return;
                       }
                       for (const QString& line : lines) {
                           Q_EMIT logMessage(line);
                       }
                   });
}

void PartitionExecutor::flushHelperOutput(const QString& pending_output) {
    // A last line the script wrote without a newline still reaches the log.
    if (!pending_output.trimmed().isEmpty()) {
        Q_EMIT logMessage(pending_output.trimmed());
    }
}

QStringList PartitionExecutor::completedOutputLines(const QString& frame,
                                                    QString* pending,
                                                    bool* is_output) {
    static const QString kStdout = QStringLiteral("STDOUT|");
    static const QString kStderr = QStringLiteral("STDERR|");
    const bool out = frame.startsWith(kStdout);
    const bool err = frame.startsWith(kStderr);
    *is_output = out || err;
    if (!*is_output) {
        return {};
    }
    pending->append(frame.mid(kStdout.size()));
    QStringList lines;
    qsizetype newline = pending->indexOf(QLatin1Char('\n'));
    while (newline >= 0) {
        const QString line = pending->left(newline).trimmed();
        if (!line.isEmpty()) {
            lines.append(err ? QStringLiteral("[stderr] ") + line : line);
        }
        pending->remove(0, newline + 1);
        newline = pending->indexOf(QLatin1Char('\n'));
    }
    return lines;
}

PartitionExecutionStep PartitionExecutor::executeElevatedScript(const PartitionOperation& operation,
                                                                const PartitionScript& script) {
    auto step = newScriptExecutionStep(operation);
    const StagedScriptCredentials credentials(script);
    if (!credentials.ok()) {
        step.error_message = credentials.error();
        return step;
    }
    // The session's one helper: every queued operation (and every inventory scan) uses it, so
    // a batch costs one UAC prompt instead of one per operation. Its progress connection is
    // per operation and is dropped again below, since the broker outlives this executor.
    const auto broker = ElevationBroker::sessionBroker();
    auto pending_output = std::make_shared<QString>();
    const QMetaObject::Connection progress = relayHelperProgress(broker.get(), pending_output);
    const auto clear_broker = qScopeGuard([this, broker, progress, pending_output]() {
        QObject::disconnect(progress);
        flushHelperOutput(*pending_output);
        clearActiveBroker(broker);
    });
    setActiveBroker(broker);
#ifdef SAK_PARTITION_FS_TEST_HOOKS
    if (const auto& observer = brokerRegisteredObserver()) {
        observer(broker);
    }
#endif
    if (m_cancelled.load(std::memory_order_relaxed)) {
        // A cancel that raced in before the broker was registered would have found
        // m_active_broker null and been unable to stop this task. Now that the broker is
        // registered, re-check so a cancelled batch never launches the elevated
        // (destructive) task through the gap.
        step.error_message = QStringLiteral("Command cancelled");
        return step;
    }
    QJsonObject payload;
    payload[QStringLiteral("command")] = credentials.script();
    payload[QStringLiteral("timeout_seconds")] = std::max(script.timeout_seconds,
                                                          kDefaultPartitionTaskTimeoutSeconds);
    payload[QStringLiteral("max_output_bytes")] = kMaxPartitionTaskOutputBytes;
    const auto elevated =
        broker->executeTask(QStringLiteral("RunPowerShell"), operation.summary, payload);
    if (!elevated) {
        step.error_message = QStringLiteral("Elevation helper failed");
        return step;
    }
    step.success = elevated->success &&
                   elevated->data.value(QStringLiteral("exit_code")).toInt(-1) == 0;
    step.stdout_text = elevated->data.value(QStringLiteral("stdout")).toString();
    step.stderr_text = elevated->data.value(QStringLiteral("stderr")).toString();
    // On failure, surface the real reason so an elevated operation never reports an
    // empty error (matches the local path).
    step.error_message = step.success
                             ? elevated->error_message
                             : elevatedFailureMessage(
                                   elevated->error_message,
                                   elevated->data.value(QStringLiteral("error_message")).toString(),
                                   step.stderr_text,
                                   elevated->data.value(QStringLiteral("exit_code")).toInt(-1));
    if (elevated->data.value(QStringLiteral("cancelled")).toBool(false) ||
        m_cancelled.load(std::memory_order_relaxed)) {
        step.success = false;
        step.error_message = QStringLiteral("Command cancelled");
    }
    return step;
}

PartitionExecutionStep PartitionExecutor::executeLocalScript(const PartitionOperation& operation,
                                                             const PartitionScript& script) {
    auto step = newScriptExecutionStep(operation);
    const StagedScriptCredentials credentials(script);
    if (!credentials.ok()) {
        step.error_message = credentials.error();
        return step;
    }
    const auto proc =
        runPowerShell(credentials.script(),
                      script.timeout_seconds * kMillisecondsPerSecond,
                      true,
                      true,
                      [this]() { return m_cancelled.load(std::memory_order_relaxed); });
    step.success = proc.succeeded();
    step.stdout_text = proc.std_out;
    step.stderr_text = proc.std_err;
    if (!step.success) {
        step.error_message = proc.cancelled   ? QStringLiteral("Command cancelled")
                             : proc.timed_out ? QStringLiteral("Command timed out")
                                              : proc.std_err.trimmed();
    }
    return step;
}

#ifdef SAK_PARTITION_FS_TEST_HOOKS
void PartitionExecutor::setBrokerRegisteredObserverForTesting(
    std::function<void(const std::shared_ptr<ElevationBroker>&)> observer) {
    brokerRegisteredObserver() = std::move(observer);
}
#endif

void PartitionExecutor::setActiveBroker(std::shared_ptr<ElevationBroker> broker) {
    const std::lock_guard<std::mutex> lock(m_active_broker_mutex);
    m_active_broker = std::move(broker);
}

void PartitionExecutor::clearActiveBroker(const std::shared_ptr<ElevationBroker>& broker) {
    const std::lock_guard<std::mutex> lock(m_active_broker_mutex);
    if (m_active_broker == broker) {
        m_active_broker.reset();
    }
}

}  // namespace sak
