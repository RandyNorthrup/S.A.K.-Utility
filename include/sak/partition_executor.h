// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_executor.h
/// @brief Applies queued Partition Manager operations.

#pragma once

#include "sak/partition_manager_types.h"
#include "sak/partition_script_builder.h"
#include "sak/partition_stop_cleanup.h"
#include "sak/windows_continued_work.h"

#include <QJsonObject>
#include <QObject>

#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

class PartitionManagerCoreTests;

namespace sak {

class ElevationBroker;

class PartitionExecutor : public QObject {
    Q_OBJECT

public:
    explicit PartitionExecutor(QObject* parent = nullptr);

    [[nodiscard]] PartitionExecutionResult execute(const QVector<PartitionOperation>& operations,
                                                   bool dry_run,
                                                   bool use_elevation);
    void cancel();

    /// What a failed script said about itself: PowerShell's error text, without the lines after it
    /// that say where in the script it was raised. Empty when there is none.
    [[nodiscard]] static QString scriptFailureReason(const QString& std_err);
    /// A non-empty reason for a failed elevated operation, and never just "it exited".
    [[nodiscard]] static QString elevatedFailureMessage(const QString& helper_error,
                                                        const QString& structured_error,
                                                        const QString& std_err,
                                                        int exit_code);
    /// The helper streams a script's output as progress frames "STDOUT|<chunk>" and
    /// "STDERR|<chunk>". Appends @p chunk's text to @p pending and returns the lines it
    /// completed, leaving a partial last line in @p pending. Empty for any other frame, and
    /// @p is_output says which it was.
    [[nodiscard]] static QStringList completedOutputLines(const QString& frame,
                                                          QString* pending,
                                                          bool* is_output);

#ifdef SAK_PARTITION_FS_TEST_HOOKS
    /// @brief Test-only: called with each broker right after an elevated operation registers
    ///        it, before anything is sent. Static so the class layout is the same in test and
    ///        product builds. Compiled only into tests that define SAK_PARTITION_FS_TEST_HOOKS.
    static void setBrokerRegisteredObserverForTesting(
        std::function<void(const std::shared_ptr<ElevationBroker>&)> observer);
#endif

Q_SIGNALS:
    void progressUpdated(int percent, const QString& status);
    void logMessage(const QString& message);
    /// A step's script said Windows has taken its work over: from here Windows finishes it even
    /// if the script is killed, so a cancel cannot stop it. Emitted from the thread running the
    /// step, as the helper's output arrives.
    void windowsPhaseStarted(const sak::WindowsContinuedPhase& phase);
    /// That phase is over: its cmdlet returned, or the step ended.
    void windowsPhaseEnded();

private:
    friend class ::PartitionManagerCoreTests;

    /// One elevated step's streamed output, as it arrives from the helper.
    struct StepOutputState {
        QString step_summary;
        QString pending;                ///< A partial last line, until its newline arrives.
        std::optional<WindowsContinuedMarker> open_phase;
        StopLeftoverTracker leftovers;  ///< What the script made and has not removed yet.
    };

    /// Runs a cleanup script and returns its output, or why it could not run.
    using StopCleanupRunner = std::function<std::expected<QString, QString>(const QString&)>;

    PartitionScriptBuilder m_script_builder;
    std::atomic_bool m_cancelled{false};
    mutable std::mutex m_active_broker_mutex;
    std::shared_ptr<ElevationBroker> m_active_broker;

    /// Pass the helper's progress on, and the script output it carries to logMessage.
    void flushHelperOutput(StepOutputState* state);
    [[nodiscard]] QMetaObject::Connection relayHelperProgress(
        ElevationBroker* broker, const std::shared_ptr<StepOutputState>& state);
    /// One line of a step's output: a Windows-continued marker moves the step's phase, any other
    /// line goes to the log.
    void relayOutputLine(StepOutputState* state, const QString& line);
    /// A step whose script ended without success while its Windows-continued phase was open
    /// (@p streamed_phase, or a Begin marker with no End in its output) may still be running in
    /// Windows, and says so instead of "cancelled".
    static void noteWorkLeftToWindows(PartitionExecutionStep* step,
                                      const std::optional<WindowsContinuedMarker>& streamed_phase);
    /// A step whose script was stopped can leave what its finally would have removed: its
    /// shadow copy and that copy's link, or cipher's folder. @p run removes exactly those
    /// @p leftovers named, and the step's result says what was removed and what could not be.
    /// The step a finished elevated task describes, before any Windows-continued phase or
    /// cleanup is considered: a task the helper or this executor cancelled is a cancelled step.
    [[nodiscard]] static PartitionExecutionStep stepFromElevatedTask(
        const PartitionOperation& operation,
        bool task_success,
        const QJsonObject& data,
        const QString& task_error,
        bool cancel_requested);
    static void cleanUpAfterStop(PartitionExecutionStep* step,
                                 const StopLeftoverTracker& leftovers,
                                 const StopCleanupRunner& run);
    [[nodiscard]] PartitionExecutionStep executeOperation(const PartitionOperation& operation,
                                                          bool dry_run,
                                                          bool use_elevation);
    [[nodiscard]] PartitionExecutionStep executeScript(const PartitionOperation& operation,
                                                       const PartitionScript& script,
                                                       bool use_elevation);
    [[nodiscard]] PartitionExecutionStep executeElevatedScript(const PartitionOperation& operation,
                                                               const PartitionScript& script);
    [[nodiscard]] PartitionExecutionStep executeLocalScript(const PartitionOperation& operation,
                                                            const PartitionScript& script);
    void setActiveBroker(std::shared_ptr<ElevationBroker> broker);
    void clearActiveBroker(const std::shared_ptr<ElevationBroker>& broker);
};

}  // namespace sak
