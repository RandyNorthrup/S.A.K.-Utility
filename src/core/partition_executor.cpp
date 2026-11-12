// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_executor.cpp
/// @brief Queued operation execution for Partition Manager.

#include "sak/partition_executor.h"

#include "sak/app_paths.h"
#include "sak/elevation_broker.h"
#include "sak/layout_constants.h"
#include "sak/no_follow_fs.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QUuid>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace sak {

static_assert(kPartitionLongTaskTimeoutSeconds <= kElevatedTaskMaxTimeoutSeconds,
              "a long partition task must not be cut short by the helper's own limit");

namespace {
constexpr int kDefaultPartitionTaskTimeoutSeconds = 1800;
constexpr int kMaxPartitionTaskOutputBytes = 1024 * 1024;
// Removing a link, a shadow copy by its ID and cipher's folder takes seconds; a cleanup that hangs
// must not hold the batch's end, or an exit, for long.
constexpr int kStopCleanupTimeoutSeconds = 120;

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

// A batch whose cancel stopped a step Windows goes on with is not "cancelled": it names that step
// as possibly still running in Windows.
bool nameWorkLeftToWindows(PartitionExecutionResult* result) {
    const auto left = std::ranges::find_if(result->steps, [](const PartitionExecutionStep& step) {
        return step.may_still_run_in_windows;
    });
    if (left != result->steps.end()) {
        result->message =
            QStringLiteral("Partition operation batch stopped. %1").arg(left->error_message);
        return true;
    }
    return false;
}

void markCancelled(PartitionExecutionResult* result) {
    result->cancelled = true;
    result->message = QStringLiteral("Partition operation batch cancelled");
    if (nameWorkLeftToWindows(result)) {
        return;  // That step's message carries its cleanup too.
    }
    // What the cleanup after the stop removed, and what it could not, is part of the result.
    for (const PartitionExecutionStep& step : result->steps) {
        const QString cleanup = stopCleanupSummary(
            {.removed = step.stop_cleanup_removed, .not_removed = step.stop_cleanup_not_removed});
        if (!cleanup.isEmpty()) {
            result->message += QStringLiteral(". ") + cleanup;
        }
    }
}

// @p sentence after @p message, as the next sentence.
QString withSentence(const QString& message, const QString& sentence) {
    if (message.trimmed().isEmpty()) {
        return sentence;
    }
    return message.trimmed().endsWith(QLatin1Char('.'))
               ? message.trimmed() + QLatin1Char(' ') + sentence
               : message.trimmed() + QStringLiteral(". ") + sentence;
}

// The cleanup after a stopped elevated step runs in the same helper, which is still there: the
// step's task ended, the helper did not. Nothing is relaunched for it.
std::expected<QString, QString> runElevatedStopCleanup(ElevationBroker* broker,
                                                       const QString& script);

// Runs a cleanup through @p broker -- unless its helper did not answer the step's own task, in
// which case it is gone and is not relaunched for a cleanup.
std::function<std::expected<QString, QString>(const QString&)> elevatedStopCleanupRunner(
    const std::shared_ptr<ElevationBroker>& broker, bool helper_answered) {
    return [broker, helper_answered](const QString& cleanup) -> std::expected<QString, QString> {
        if (!helper_answered) {
            return std::unexpected(QStringLiteral("the elevated helper was no longer running"));
        }
        return runElevatedStopCleanup(broker.get(), cleanup);
    };
}

std::expected<QString, QString> runElevatedStopCleanup(ElevationBroker* broker,
                                                       const QString& script) {
    QJsonObject payload;
    payload[QStringLiteral("command")] = script;
    payload[QStringLiteral("timeout_seconds")] = kStopCleanupTimeoutSeconds;
    payload[QStringLiteral("max_output_bytes")] = kMaxPartitionTaskOutputBytes;
    const auto result = broker->executeTask(QStringLiteral("RunPowerShell"),
                                            QStringLiteral("Clean up after a stopped step"),
                                            payload);
    if (!result) {
        return std::unexpected(QStringLiteral("the elevated helper did not run the cleanup"));
    }
    return result->data.value(QStringLiteral("stdout")).toString();
}

}  // namespace

// Security brief 4a (owner ruling 2026-11-11): each secret is written CREATE_NEW into this run's
// own temporary folder under the program's data folder, never anywhere else on the PC, and held
// read-only (no writer, no delete) until the operation is over, so nothing can change the password
// between this write and the writer reading it, on NTFS or FAT. The path, not the secret, goes into
// the script and so onto no command line. Afterwards each file is overwritten with zeros and
// removed. A no-op (the script is returned verbatim) when the script carries no credentials.
StagedScriptCredentials::StagedScriptCredentials(const PartitionScript& script)
    : m_script(script.script) {
    for (const auto& credential : script.credential_files) {
        const QByteArray secret = credential.secret.toUtf8();
        auto staged = app_paths::writeHeldSessionTempFile(
            QStringLiteral("sak-apfs-cred-%1.tmp").arg(no_follow::randomToken()), secret);
        if (!staged) {
            m_ok = false;
            m_error = QStringLiteral("Unable to stage encryption credential file: %1")
                          .arg(staged.error());
            return;
        }
        // The placeholder is embedded inside a single-quoted PowerShell literal
        // (e.g. -VolumePasswordFile '<placeholder>'). Only the apostrophe is special there, so
        // double it: a data folder under a name such as O'Brien would otherwise close the literal
        // and inject into the elevated RunPowerShell command.
        const QString staged_path = QDir::toNativeSeparators(staged->second)
                                        .replace(QLatin1Char('\''), QStringLiteral("''"));
        m_script.replace(credential.placeholder, staged_path);
        m_files.push_back(
            StagedFile{std::move(staged->first), staged->second, static_cast<int>(secret.size())});
    }
}

StagedScriptCredentials::~StagedScriptCredentials() {
    for (auto& file : m_files) {
        file.hold.reset();
        QFile overwrite(file.path);
        if (overwrite.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
            overwrite.write(QByteArray(file.size, '\0'));
            overwrite.flush();
            overwrite.close();
        }
        QFile::remove(file.path);
    }
}

std::vector<QString> StagedScriptCredentials::stagedPaths() const {
    std::vector<QString> paths;
    paths.reserve(m_files.size());
    for (const auto& file : m_files) {
        paths.push_back(file.path);
    }
    return paths;
}

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
    ElevationBroker* broker, const std::shared_ptr<StepOutputState>& state) {
    // The script's own output goes to the log a line at a time. Passed on as progress it reached
    // the status bar as raw "STDOUT|..." text with the bar stuck at 50%, and the log stayed empty.
    // Direct: the broker emits on the thread running this step, so the step's state is read and
    // written on that one thread, and a Windows-continued phase is known the moment its marker
    // arrives rather than whenever the GUI thread next runs a queued call. The signals this
    // emits reach the controller queued, as any cross-thread signal does.
    return connect(
        broker,
        &ElevationBroker::progressUpdated,
        this,
        [this, state](int percent, const QString& status) {
            bool is_output = false;
            const QStringList lines = completedOutputLines(status, &state->pending, &is_output);
            if (!is_output) {
                Q_EMIT progressUpdated(percent, status);
                return;
            }
            for (const QString& line : lines) {
                relayOutputLine(state.get(), line);
            }
        },
        Qt::DirectConnection);
}

void PartitionExecutor::relayOutputLine(StepOutputState* state, const QString& line) {
    if (state->leftovers.observe(line)) {
        return;  // What the script made and removed is followed, not logged.
    }
    const auto marker = parseWindowsContinuedMarker(line);
    if (!marker) {
        // A line a script writes for the application -- a cleanup's result, or a marker that did
        // not parse -- is never a line for the Log window.
        if (!isScriptMarkerLine(line)) {
            Q_EMIT logMessage(line);
        }
        return;
    }
    if (marker->begin) {
        state->open_phase = *marker;
        const WindowsContinuedPhase phase{.step_summary = state->step_summary,
                                          .marker = *marker,
                                          .started_at = QDateTime::currentDateTime()};
        Q_EMIT logMessage(windowsContinuedPhaseNotice(phase));
        Q_EMIT windowsPhaseStarted(phase);
        return;
    }
    if (state->open_phase) {
        Q_EMIT logMessage(
            QStringLiteral("%1: Windows is no longer carrying out %2.")
                .arg(state->step_summary, windowsContinuedWorkDescription(*state->open_phase)));
        state->open_phase.reset();
        Q_EMIT windowsPhaseEnded();
    }
}

void PartitionExecutor::flushHelperOutput(StepOutputState* state) {
    // A last line the script wrote without a newline still reaches the log.
    const QString last = std::exchange(state->pending, {}).trimmed();
    if (!last.isEmpty()) {
        relayOutputLine(state, last);
    }
}

void PartitionExecutor::noteWorkLeftToWindows(
    PartitionExecutionStep* step, const std::optional<WindowsContinuedMarker>& streamed_phase) {
    if (step->success) {
        return;
    }
    // The End marker is written from a finally block, so an open phase means the script was
    // killed -- cancelled, timed out, or its helper gone -- while Windows had the work.
    const auto phase = streamed_phase ? streamed_phase
                                      : openWindowsContinuedPhase(step->stdout_text);
    if (!phase) {
        return;
    }
    step->may_still_run_in_windows = true;
    step->error_message = windowsContinuedStepMessage(step->summary, *phase);
}

PartitionExecutionStep PartitionExecutor::stepFromElevatedTask(const PartitionOperation& operation,
                                                               bool task_success,
                                                               const QJsonObject& data,
                                                               const QString& task_error,
                                                               bool cancel_requested) {
    PartitionExecutionStep step = newScriptExecutionStep(operation);
    step.success = task_success && data.value(QStringLiteral("exit_code")).toInt(-1) == 0;
    step.stdout_text = data.value(QStringLiteral("stdout")).toString();
    step.stderr_text = data.value(QStringLiteral("stderr")).toString();
    // On failure, surface the real reason so an elevated operation never reports an
    // empty error (matches the local path).
    step.error_message =
        step.success
            ? task_error
            : elevatedFailureMessage(task_error,
                                     data.value(QStringLiteral("error_message")).toString(),
                                     step.stderr_text,
                                     data.value(QStringLiteral("exit_code")).toInt(-1));
    if (data.value(QStringLiteral("cancelled")).toBool(false) || cancel_requested) {
        step.success = false;
        step.cancelled = true;
        step.error_message = QStringLiteral("Command cancelled");
    }
    return step;
}

void PartitionExecutor::cleanUpAfterStop(PartitionExecutionStep* step,
                                         const StopLeftoverTracker& leftovers,
                                         const StopCleanupRunner& run) {
    // A script that ran to its end removed what it made in its own finally blocks.
    if (step->success || leftovers.outstanding().isEmpty()) {
        return;
    }
    const QVector<StopLeftover>& left = leftovers.outstanding();
    const auto output = run(stopCleanupScript(left));
    const StopCleanupReport report = output ? stopCleanupReport(left, *output)
                                            : stopCleanupNotRun(left, output.error());
    step->stop_cleanup_removed = report.removed;
    step->stop_cleanup_not_removed = report.not_removed;
    if (!report.isEmpty()) {
        step->error_message = withSentence(step->error_message, stopCleanupSummary(report));
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
    auto output = std::make_shared<StepOutputState>();
    output->step_summary = operation.summary;
    const QMetaObject::Connection progress = relayHelperProgress(broker.get(), output);
    const auto clear_broker = qScopeGuard([this, broker, progress, output]() {
        QObject::disconnect(progress);
        flushHelperOutput(output.get());
        // The step is over whatever happened to it; the phase it left open is in its result.
        if (output->open_phase) {
            Q_EMIT windowsPhaseEnded();
        }
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
        step.cancelled = true;  // and nothing was sent
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
    // Any line the script wrote without a newline is read before the phase is judged.
    flushHelperOutput(output.get());
    if (!elevated) {
        step.error_message = QStringLiteral("Elevation helper failed");
    } else {
        step = stepFromElevatedTask(operation,
                                    elevated->success,
                                    elevated->data,
                                    elevated->error_message,
                                    m_cancelled.load(std::memory_order_relaxed));
    }
    noteWorkLeftToWindows(&step, output->open_phase);
    // A second cancel must not stop the cleanup of what the first one left behind.
    clearActiveBroker(broker);
    cleanUpAfterStop(&step,
                     output->leftovers,
                     elevatedStopCleanupRunner(broker, elevated.has_value()));
    // Everything that reads the markers has read them; the output is now what a person reads.
    step.stdout_text = withoutScriptMarkers(step.stdout_text);
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
        step.cancelled = proc.cancelled;
        step.error_message = proc.cancelled   ? QStringLiteral("Command cancelled")
                             : proc.timed_out ? QStringLiteral("Command timed out")
                                              : proc.std_err.trimmed();
    }
    // This path does not stream, so only the output it returns can show a phase left open, or
    // what the script made and did not remove.
    noteWorkLeftToWindows(&step, std::nullopt);
    StopLeftoverTracker leftovers;
    leftovers.observeAll(proc.std_out);
    cleanUpAfterStop(
        &step, leftovers, [](const QString& cleanup) -> std::expected<QString, QString> {
            return runPowerShell(
                       cleanup, kStopCleanupTimeoutSeconds * kMillisecondsPerSecond, true, true)
                .std_out;
        });
    // Read for its markers above; what is kept is what a person reads.
    step.stdout_text = withoutScriptMarkers(step.stdout_text);
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
