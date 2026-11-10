// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_recovery_policy.h"
#include "sak/ai/ai_run_state.h"
#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/ai_workflow_template.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <expected>
#include <functional>

namespace sak::ai {

struct AiPhaseExecution {
    QString phase_id;
    QString phase_type;
    QString agent_id;
    AiSubagentStatus subagent_status{AiSubagentStatus::Failed};
    QJsonObject tool_result;
    QJsonObject metadata;
    bool ran{false};
    bool skipped{false};
    bool success{false};
    QString skip_reason;
    QString error_message;
    qint64 duration_ms{0};

    /// @brief Canonical phase record used by run results and recovery snapshots.
    /// Delegate-only status is omitted from non-delegate phases.
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::expected<AiPhaseExecution, QString> fromJson(
        const QJsonObject& object);
};

/// Hard resource ceiling independent of workflow/session input.
inline constexpr int kMaxParallelSubagents = 8;

struct AiOrchestrationOptions {
    int max_parallel_subagents{1};
    bool stop_on_phase_failure{true};
    // The session's tool-policy ceiling. Every per-agent workflow policy is clamped to this so a
    // catalog agent can never grant a workflow more capability than the session that launched it
    // (e.g. a Chat/Research session must not gain package/mutating tools via a workflow agent).
    // Defaults to the most restrictive policy so an unset ceiling fails closed.
    AiToolPolicy session_policy_ceiling{AiToolPolicy::NoLocalExecution};
    QString default_model;
    QString api_key;
    QString user_message;
    QJsonObject input_values;
    bool resume_enabled{false};
    int resume_start_phase_index{0};
    QVector<AiPhaseExecution> resume_prior_phases;
    // Resume identity is mandatory when resume_enabled is true. Both ids must be non-empty and
    // exactly match the workflow/run. Flags and phase-results are derived from validated history;
    // callers cannot inject either block independently.
    QString resume_workflow_id;
    QString resume_run_id;
};

struct AiOrchestratorResult {
    QString run_id;
    QString workflow_id;
    AiRunStatus status{AiRunStatus::Idle};
    QVector<AiPhaseExecution> phases;
    QSet<QString> flags;
    QStringList cleanup_failures;
    QString error_message;
    int parallel_groups_executed{0};

    [[nodiscard]] QJsonObject toJson() const;
};

struct AiWorkflowPhaseContext {
    QString run_id;
    QString workflow_id;
    QString user_message;
    QJsonObject input_values;
    QJsonObject phase_results;
    QSet<QString> flags;
    QVector<AiPhaseExecution> prior_phases;
    QString workflow_guidance;
};

/// @brief Field-aware bounded workflow state for an untrusted user-role model message.
[[nodiscard]] std::expected<QString, QString> untrustedWorkflowContextMessage(
    const AiWorkflowPhaseContext& context);
[[nodiscard]] std::expected<QString, QString> completeWorkflowGuidanceText(const QByteArray& bytes,
                                                                           const QString& reference,
                                                                           qsizetype max_bytes);

/// @brief Executes workflow `tool_action` and `cleanup` phases on behalf of
/// the orchestrator. Injected so tests can simulate success/failure/cancel
/// without touching real local commands.
class IAiToolExecutor {
public:
    virtual ~IAiToolExecutor() = default;
    [[nodiscard]] virtual QJsonObject runToolPhase(const WorkflowPhase& phase,
                                                   AiToolPolicy policy,
                                                   const AiWorkflowPhaseContext& context,
                                                   const CancellationToken& token) = 0;
};

/// @brief Required callback when a workflow declares `overseer` phases.
/// Missing handlers and results without an explicit boolean success fail closed.
using OverseerPhaseHandler = std::function<QJsonObject(const WorkflowPhase& phase,
                                                       const AiWorkflowPhaseContext& context,
                                                       const CancellationToken& token)>;

/// @brief Optional callback invoked after a declared workflow phase reaches a
/// terminal record. Each callback is paired with exactly one started callback.
using PhaseCompletedCallback = std::function<void(const AiPhaseExecution& execution)>;

/// @brief Optional callback invoked before a declared workflow phase is evaluated.
///
/// Both lifecycle callbacks run sequentially on the thread that called run(),
/// including when delegate work runs in parallel. Retry/reassignment attempts
/// remain metadata of the declared phase and do not create unpaired lifecycle
/// events. UI callers should marshal to the main thread inside the callback.
using PhaseStartedCallback = std::function<void(const WorkflowPhase& phase)>;

/// @brief Resolves a workflow instruction/skill reference (e.g.
/// "skills/package-selection.md") to its full guidance text. Injected so the
/// orchestrator stays I/O-free and testable; the panel wires it to the bundled
/// resources / skill store. An error or empty successful value fails preflight.
using WorkflowGuidanceResolution = std::expected<QString, QString>;
using WorkflowGuidanceResolver = std::function<WorkflowGuidanceResolution(const QString& ref)>;

/// @brief Reports whether a workflow's declared required-software entry is
/// actually available in the current environment. Injected so the orchestrator
/// stays I/O-free; the panel wires it to the real tool registry / system probe.
/// A false value means known-but-unavailable. An unexpected value means the kind
/// or probe is unsupported. Both fail preflight, as does a missing resolver.
using SoftwareAvailabilityResolution = std::expected<bool, QString>;
using SoftwareAvailabilityResolver =
    std::function<SoftwareAvailabilityResolution(const WorkflowRequirement& requirement)>;

/// @brief Runs a workflow's phases end-to-end. Sequential by default;
/// consecutive `delegate` phases whose agent tool_policy is read-only are
/// grouped into a single parallel batch up to `max_parallel_subagents`.
/// Mutating phases serialize. Cancel via the root token propagates to every
/// in-flight phase token.
class AiOrchestrator {
public:
    AiOrchestrator(AiSubagentRunner* subagent_runner, IAiToolExecutor* tool_executor);

    void setOptions(const AiOrchestrationOptions& options);
    [[nodiscard]] const AiOrchestrationOptions& options() const;

    void setOverseerHandler(OverseerPhaseHandler handler);
    void setPhaseStartedCallback(PhaseStartedCallback callback);
    void setPhaseCompletedCallback(PhaseCompletedCallback callback);
    void setGuidanceResolver(WorkflowGuidanceResolver resolver);
    void setSoftwareResolver(SoftwareAvailabilityResolver resolver);

    [[nodiscard]] AiOrchestratorResult run(const WorkflowTemplate& workflow,
                                           const QString& run_id,
                                           const CancellationToken& root_token) const;

    [[nodiscard]] static bool isReadOnlyPolicy(AiToolPolicy policy);
    /// @brief Validate an untrusted resume snapshot after canonical JSON decoding.
    [[nodiscard]] static QString validateResumeState(const WorkflowTemplate& workflow,
                                                     const QString& run_id,
                                                     const AiOrchestrationOptions& options);

private:
    struct PhaseGroup {
        QVector<int> phase_indices;
        bool parallel{false};
    };

    struct RunState {
        AiOrchestratorResult result;
        AiWorkflowPhaseContext context;
        QSet<int> executed_indices;
        bool aborted_for_failure{false};
        QString abort_reason;
    };

    struct GroupOutcome {
        bool any_failed{false};
        bool any_cancelled{false};
        bool any_waiting_human{false};
        QString first_failure_id;
        QString first_failure_msg;
        QString first_cancel_msg;
    };

    struct GroupWork {
        const PhaseGroup* group{nullptr};
        QVector<int> runnable_positions;
        QVector<AiPhaseExecution> executions;
    };

    struct RecoveryRequest {
        AiPhaseExecution execution;
        AiRecoveryDecision decision;
    };

    [[nodiscard]] QVector<PhaseGroup> planGroups(const WorkflowTemplate& workflow,
                                                 const QSet<QString>& flags) const;
    void initializeRunState(const WorkflowTemplate& workflow,
                            const QString& run_id,
                            RunState* state) const;
    [[nodiscard]] QString applyResumeState(const WorkflowTemplate& workflow, RunState* state) const;
    static void seedDerivedResumeState(const AiPhaseExecution& prior,
                                       int phase_index,
                                       RunState* state);
    [[nodiscard]] AiPhaseExecution executeWorkflowPhase(const WorkflowTemplate& workflow,
                                                        const WorkflowPhase& phase,
                                                        const RunState& state,
                                                        const CancellationToken& root_token) const;
    [[nodiscard]] static AiRecoveryDecision recoveryDecisionFor(const WorkflowTemplate& workflow,
                                                                const WorkflowPhase& phase,
                                                                const AiPhaseExecution& execution);
    static void attachRecoveryDecision(AiPhaseExecution* execution,
                                       const AiRecoveryDecision& decision);
    [[nodiscard]] AiPhaseExecution executeWithRecovery(const WorkflowTemplate& workflow,
                                                       const WorkflowPhase& phase,
                                                       AiPhaseExecution execution,
                                                       const RunState& state,
                                                       const CancellationToken& root_token) const;
    [[nodiscard]] AiPhaseExecution executeReassignmentRecovery(
        const WorkflowTemplate& workflow,
        const WorkflowPhase& phase,
        RecoveryRequest request,
        const RunState& state,
        const CancellationToken& root_token) const;
    [[nodiscard]] AiPhaseExecution executeRetryRecovery(const WorkflowTemplate& workflow,
                                                        const WorkflowPhase& phase,
                                                        const RecoveryRequest& request,
                                                        const RunState& state,
                                                        const CancellationToken& root_token) const;
    [[nodiscard]] QVector<AiPhaseExecution> executePhaseGroup(
        const WorkflowTemplate& workflow,
        const PhaseGroup& group,
        RunState* state,
        const CancellationToken& root_token) const;
    [[nodiscard]] QVector<int> runnableGroupPositions(
        const WorkflowTemplate& workflow,
        const PhaseGroup& group,
        RunState* state,
        QVector<AiPhaseExecution>* group_executions) const;
    void executeParallelGroup(const WorkflowTemplate& workflow,
                              RunState* state,
                              GroupWork* work,
                              const CancellationToken& root_token) const;
    void executeSerialGroup(const WorkflowTemplate& workflow,
                            const RunState& state,
                            GroupWork* work,
                            const CancellationToken& root_token) const;
    [[nodiscard]] GroupOutcome appendGroupExecutions(const WorkflowTemplate& workflow,
                                                     const PhaseGroup& group,
                                                     QVector<AiPhaseExecution>* group_executions,
                                                     RunState* state) const;
    static void updateOutcomeForFailure(const WorkflowTemplate& workflow,
                                        const WorkflowPhase& phase,
                                        AiPhaseExecution* execution,
                                        GroupOutcome* outcome,
                                        RunState* state);
    static void recordCleanupFailureIfNeeded(const AiPhaseExecution& execution, RunState* state);
    static void recordRecoveryCallbackFailure(const QString& callback_error, RunState* state);
    static void updateOutcomeFromRecoveryDecision(const AiPhaseExecution& execution,
                                                  const AiRecoveryDecision& recovery,
                                                  GroupOutcome* outcome);
    void updateRunStatusFromGroupOutcome(const GroupOutcome& outcome, RunState* state) const;
    void runPlannedGroups(const WorkflowTemplate& workflow,
                          const QVector<PhaseGroup>& groups,
                          RunState* state,
                          const CancellationToken& root_token) const;
    void runAlwaysRunRecoveryPhases(const WorkflowTemplate& workflow,
                                    RunState* state,
                                    const CancellationToken& root_token) const;
    [[nodiscard]] AiPhaseExecution executeDelegatePhase(
        const WorkflowTemplate& workflow,
        const WorkflowPhase& phase,
        const QString& run_id,
        const AiWorkflowPhaseContext& context,
        const CancellationToken& parent_token) const;
    [[nodiscard]] std::expected<AiSubagentTask, QString> buildDelegateTask(
        const WorkflowTemplate& workflow,
        const WorkflowPhase& phase,
        const QString& run_id,
        const AiWorkflowPhaseContext& context) const;
    [[nodiscard]] AiPhaseExecution executeToolPhase(const WorkflowPhase& phase,
                                                    AiToolPolicy policy,
                                                    const AiWorkflowPhaseContext& context,
                                                    const CancellationToken& parent_token) const;
    [[nodiscard]] AiPhaseExecution executeOverseerPhase(
        const WorkflowPhase& phase,
        const AiWorkflowPhaseContext& context,
        const CancellationToken& parent_token) const;
    [[nodiscard]] AiToolPolicy policyForAgent(const WorkflowTemplate& workflow,
                                              const QString& agent_id) const;
    [[nodiscard]] WorkflowGuidanceResolution resolveWorkflowGuidance(
        const WorkflowTemplate& workflow) const;
    // Empty means all required probes succeeded and reported available.
    [[nodiscard]] QString requiredSoftwareError(const WorkflowTemplate& workflow) const;
    [[nodiscard]] QString prepareRunState(const WorkflowTemplate& workflow, RunState* state) const;

    AiSubagentRunner* m_subagent_runner{nullptr};
    IAiToolExecutor* m_tool_executor{nullptr};
    AiOrchestrationOptions m_options;
    OverseerPhaseHandler m_overseer_handler;
    PhaseStartedCallback m_phase_started_callback;
    PhaseCompletedCallback m_phase_completed_callback;
    WorkflowGuidanceResolver m_guidance_resolver;
    SoftwareAvailabilityResolver m_software_resolver;
};

}  // namespace sak::ai
