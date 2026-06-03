// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_executor.cpp
/// @brief Queued operation execution for Partition Manager.

#include "sak/partition_executor.h"

#include "sak/elevation_broker.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QJsonObject>
#include <QUuid>

#include <algorithm>

namespace sak {

namespace {
constexpr int kDefaultPartitionTaskTimeoutSeconds = 1800;
constexpr int kMaxPartitionTaskOutputBytes = 1024 * 1024;

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
}  // namespace

PartitionExecutor::PartitionExecutor(QObject* parent) : QObject(parent) {}

PartitionExecutionResult PartitionExecutor::execute(const QVector<PartitionOperation>& operations,
                                                    bool dry_run,
                                                    bool use_elevation) {
    m_cancelled.store(false, std::memory_order_relaxed);
    PartitionExecutionResult result;
    result.batch_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.dry_run = dry_run;

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

    result.success = !result.cancelled &&
                     std::all_of(result.steps.begin(), result.steps.end(), [](const auto& step) {
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
    std::lock_guard<std::mutex> lock(m_active_broker_mutex);
    if (m_active_broker) {
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

PartitionExecutionStep PartitionExecutor::executeElevatedScript(const PartitionOperation& operation,
                                                                const PartitionScript& script) {
    auto step = newScriptExecutionStep(operation);
    ElevationBroker broker;
    connect(&broker, &ElevationBroker::progressUpdated, this, &PartitionExecutor::progressUpdated);
    setActiveBroker(&broker);
    QJsonObject payload;
    payload[QStringLiteral("command")] = script.script;
    payload[QStringLiteral("timeout_seconds")] = std::max(script.timeout_seconds,
                                                          kDefaultPartitionTaskTimeoutSeconds);
    payload[QStringLiteral("max_output_bytes")] = kMaxPartitionTaskOutputBytes;
    const auto elevated =
        broker.executeTask(QStringLiteral("RunPowerShell"), operation.summary, payload);
    setActiveBroker(nullptr);
    if (!elevated) {
        step.error_message = QStringLiteral("Elevation helper failed");
        return step;
    }
    step.success = elevated->success &&
                   elevated->data.value(QStringLiteral("exit_code")).toInt(-1) == 0;
    step.stdout_text = elevated->data.value(QStringLiteral("stdout")).toString();
    step.stderr_text = elevated->data.value(QStringLiteral("stderr")).toString();
    step.error_message = elevated->error_message;
    if (!step.success && step.error_message.isEmpty()) {
        step.error_message = elevated->data.value(QStringLiteral("error_message")).toString();
    }
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
    const auto proc = runPowerShell(
        script.script, script.timeout_seconds * kMillisecondsPerSecond, true, true, [this]() {
            return m_cancelled.load(std::memory_order_relaxed);
        });
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

void PartitionExecutor::setActiveBroker(ElevationBroker* broker) {
    std::lock_guard<std::mutex> lock(m_active_broker_mutex);
    m_active_broker = broker;
}

}  // namespace sak
