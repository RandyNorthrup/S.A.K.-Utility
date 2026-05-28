// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_orchestrator.h"

#include "sak/ai/ai_recovery_policy.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <future>
#include <vector>

namespace sak::ai {

namespace {

constexpr int kDefaultContextStringLimit = 1200;
constexpr int kDefaultContextArrayLimit = 50;
constexpr int kMaxContextJsonDepth = 8;
constexpr int kToolScalarContextLimit = 900;
constexpr int kToolStdoutContextLimit = 2400;
constexpr int kToolStderrContextLimit = 1400;
constexpr int kPackagesContextStringLimit = 600;
constexpr int kPackagesContextArrayLimit = 12;
constexpr int kFilesContextStringLimit = 500;
constexpr int kFilesContextArrayLimit = 20;
constexpr int kPhaseSkipReasonLimit = 800;
constexpr int kPhaseErrorMessageLimit = 1200;
constexpr int kMetadataSummaryLimit = 1200;
constexpr int kUserMessageContextLimit = 1600;
constexpr int kInputValuesContextStringLimit = 900;
constexpr int kInputValuesContextArrayLimit = 20;

bool isDelegatePhase(const WorkflowPhase& phase) {
    return phase.type.compare(QStringLiteral("delegate"), Qt::CaseInsensitive) == 0;
}

bool isToolPhase(const WorkflowPhase& phase) {
    return phase.type.compare(QStringLiteral("tool_action"), Qt::CaseInsensitive) == 0 ||
           phase.type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) == 0;
}

bool isOverseerPhase(const WorkflowPhase& phase) {
    return phase.type.compare(QStringLiteral("overseer"), Qt::CaseInsensitive) == 0;
}

bool conditionSatisfied(const WorkflowPhase& phase, const QSet<QString>& flags) {
    const QString condition = phase.condition.trimmed();
    if (condition.isEmpty()) {
        return true;
    }
    return flags.contains(condition);
}

QJsonObject phaseExecutionToJson(const AiPhaseExecution& execution) {
    QJsonObject obj;
    obj[QStringLiteral("phase_id")] = execution.phase_id;
    obj[QStringLiteral("phase_type")] = execution.phase_type;
    obj[QStringLiteral("agent_id")] = execution.agent_id;
    obj[QStringLiteral("subagent_status")] = subagentStatusToString(execution.subagent_status);
    obj[QStringLiteral("ran")] = execution.ran;
    obj[QStringLiteral("skipped")] = execution.skipped;
    obj[QStringLiteral("success")] = execution.success;
    obj[QStringLiteral("skip_reason")] = execution.skip_reason;
    obj[QStringLiteral("error_message")] = execution.error_message;
    obj[QStringLiteral("duration_ms")] = static_cast<double>(execution.duration_ms);
    if (!execution.tool_result.isEmpty()) {
        obj[QStringLiteral("tool_result")] = execution.tool_result;
    }
    if (!execution.metadata.isEmpty()) {
        obj[QStringLiteral("metadata")] = execution.metadata;
    }
    return obj;
}

QString cappedContextString(const QString& value, int limit) {
    if (limit <= 0 || value.size() <= limit) {
        return value;
    }
    return value.left(limit) +
           QStringLiteral("\n...[truncated %1 chars]").arg(value.size() - limit);
}

QJsonValue truncatedWorkflowContextValue(const QJsonValue& value,
                                         int string_limit = kDefaultContextStringLimit,
                                         int array_limit = kDefaultContextArrayLimit,
                                         int depth = 0) {
    if (depth > kMaxContextJsonDepth) {
        return QStringLiteral("[truncated: maximum depth reached]");
    }
    if (value.isString()) {
        return cappedContextString(value.toString(), string_limit);
    }
    if (value.isArray()) {
        QJsonArray out;
        const auto array = value.toArray();
        const int limit = std::min(static_cast<int>(array.size()), array_limit);
        for (int i = 0; i < limit; ++i) {
            out.append(
                truncatedWorkflowContextValue(array.at(i), string_limit, array_limit, depth + 1));
        }
        if (array.size() > limit) {
            out.append(QStringLiteral("[truncated %1 array items]").arg(array.size() - limit));
        }
        return out;
    }
    if (value.isObject()) {
        QJsonObject out;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            out.insert(
                it.key(),
                truncatedWorkflowContextValue(it.value(), string_limit, array_limit, depth + 1));
        }
        return out;
    }
    return value;
}

QJsonObject compactToolResultForDelegate(const QJsonObject& tool_result) {
    QJsonObject compact;
    const QStringList bool_keys{
        QStringLiteral("success"),
        QStringLiteral("cancelled"),
        QStringLiteral("timed_out"),
        QStringLiteral("requires_admin"),
    };
    for (const auto& key : bool_keys) {
        if (tool_result.contains(key)) {
            compact.insert(key, tool_result.value(key).toBool(false));
        }
    }
    const QStringList scalar_keys{
        QStringLiteral("command_id"),
        QStringLiteral("exit_code"),
        QStringLiteral("duration_ms"),
        QStringLiteral("error_message"),
        QStringLiteral("preview"),
        QStringLiteral("stdout_path"),
        QStringLiteral("stderr_path"),
        QStringLiteral("output_dir"),
        QStringLiteral("manifest_path"),
        QStringLiteral("file_count"),
        QStringLiteral("package_count"),
    };
    for (const auto& key : scalar_keys) {
        if (tool_result.contains(key)) {
            compact.insert(key,
                           truncatedWorkflowContextValue(tool_result.value(key),
                                                         kToolScalarContextLimit));
        }
    }
    if (tool_result.contains(QStringLiteral("stdout"))) {
        compact.insert(QStringLiteral("stdout"),
                       cappedContextString(tool_result.value(QStringLiteral("stdout")).toString(),
                                           kToolStdoutContextLimit));
    }
    if (tool_result.contains(QStringLiteral("stderr"))) {
        compact.insert(QStringLiteral("stderr"),
                       cappedContextString(tool_result.value(QStringLiteral("stderr")).toString(),
                                           kToolStderrContextLimit));
    }
    if (tool_result.contains(QStringLiteral("packages"))) {
        compact.insert(QStringLiteral("packages"),
                       truncatedWorkflowContextValue(tool_result.value(QStringLiteral("packages")),
                                                     kPackagesContextStringLimit,
                                                     kPackagesContextArrayLimit));
    }
    if (tool_result.contains(QStringLiteral("files"))) {
        compact.insert(QStringLiteral("files"),
                       truncatedWorkflowContextValue(tool_result.value(QStringLiteral("files")),
                                                     kFilesContextStringLimit,
                                                     kFilesContextArrayLimit));
    }
    return compact;
}

QJsonObject compactPhaseForDelegate(const AiPhaseExecution& phase) {
    QJsonObject compact;
    compact[QStringLiteral("phase_id")] = phase.phase_id;
    compact[QStringLiteral("phase_type")] = phase.phase_type;
    compact[QStringLiteral("agent_id")] = phase.agent_id;
    compact[QStringLiteral("ran")] = phase.ran;
    compact[QStringLiteral("skipped")] = phase.skipped;
    compact[QStringLiteral("success")] = phase.success;
    compact[QStringLiteral("duration_ms")] = static_cast<double>(phase.duration_ms);
    if (!phase.skip_reason.isEmpty()) {
        compact[QStringLiteral("skip_reason")] = cappedContextString(phase.skip_reason,
                                                                     kPhaseSkipReasonLimit);
    }
    if (!phase.error_message.isEmpty()) {
        compact[QStringLiteral("error_message")] = cappedContextString(phase.error_message,
                                                                       kPhaseErrorMessageLimit);
    }
    if (!phase.tool_result.isEmpty()) {
        compact[QStringLiteral("tool_result")] = compactToolResultForDelegate(phase.tool_result);
    }
    if (!phase.metadata.isEmpty()) {
        QJsonObject metadata;
        const QString summary = phase.metadata.value(QStringLiteral("summary")).toString();
        if (!summary.isEmpty()) {
            metadata[QStringLiteral("summary")] = cappedContextString(summary,
                                                                      kMetadataSummaryLimit);
        }
        if (phase.metadata.contains(QStringLiteral("status"))) {
            metadata[QStringLiteral("status")] = phase.metadata.value(QStringLiteral("status"));
        }
        if (phase.metadata.contains(QStringLiteral("tokens"))) {
            metadata[QStringLiteral("tokens")] = phase.metadata.value(QStringLiteral("tokens"));
        }
        if (phase.metadata.contains(QStringLiteral("recovery_decision"))) {
            metadata[QStringLiteral("recovery_decision")] =
                phase.metadata.value(QStringLiteral("recovery_decision"));
        }
        if (!metadata.isEmpty()) {
            compact[QStringLiteral("metadata")] = metadata;
        }
    }
    return compact;
}

QString workflowContextForDelegate(const AiWorkflowPhaseContext& context) {
    QJsonObject root;
    root[QStringLiteral("run_id")] = context.run_id;
    root[QStringLiteral("workflow_id")] = context.workflow_id;
    root[QStringLiteral("user_message")] = cappedContextString(context.user_message,
                                                               kUserMessageContextLimit);
    root[QStringLiteral("input_values")] =
        truncatedWorkflowContextValue(context.input_values,
                                      kInputValuesContextStringLimit,
                                      kInputValuesContextArrayLimit)
            .toObject();
    QJsonArray flags;
    for (const auto& flag : context.flags) {
        flags.append(flag);
    }
    root[QStringLiteral("flags")] = flags;
    QJsonArray prior;
    for (const auto& phase : context.prior_phases) {
        prior.append(compactPhaseForDelegate(phase));
    }
    root[QStringLiteral("prior_phases")] = prior;

    QString text = QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
    constexpr int kMaxWorkflowContextChars = 18'000;
    if (text.size() > kMaxWorkflowContextChars) {
        text = text.left(kMaxWorkflowContextChars) +
               QStringLiteral("\n...[workflow context truncated]");
    }
    return text;
}

}  // namespace

QJsonObject AiOrchestratorResult::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("run_id")] = run_id;
    obj[QStringLiteral("workflow_id")] = workflow_id;
    obj[QStringLiteral("status")] = runStatusToString(status);
    obj[QStringLiteral("error_message")] = error_message;
    obj[QStringLiteral("parallel_groups_executed")] = parallel_groups_executed;
    QJsonArray phase_array;
    for (const auto& phase : phases) {
        phase_array.append(phaseExecutionToJson(phase));
    }
    obj[QStringLiteral("phases")] = phase_array;
    QJsonArray flag_array;
    for (const auto& flag : flags) {
        flag_array.append(flag);
    }
    obj[QStringLiteral("flags")] = flag_array;
    return obj;
}

AiOrchestrator::AiOrchestrator(AiSubagentRunner* subagent_runner, IAiToolExecutor* tool_executor)
    : m_subagent_runner(subagent_runner), m_tool_executor(tool_executor) {}

void AiOrchestrator::setOptions(const AiOrchestrationOptions& options) {
    m_options = options;
    if (m_options.max_parallel_subagents < 1) {
        m_options.max_parallel_subagents = 1;
    }
    if (m_options.resume_start_phase_index < 0) {
        m_options.resume_start_phase_index = 0;
    }
}

AiOrchestrationOptions AiOrchestrator::options() const {
    return m_options;
}

void AiOrchestrator::setOverseerHandler(OverseerPhaseHandler handler) {
    m_overseer_handler = std::move(handler);
}

void AiOrchestrator::setPhaseStartedCallback(PhaseStartedCallback callback) {
    m_phase_started_callback = std::move(callback);
}

void AiOrchestrator::setPhaseCompletedCallback(PhaseCompletedCallback callback) {
    m_phase_completed_callback = std::move(callback);
}

bool AiOrchestrator::isReadOnlyPolicy(AiToolPolicy policy) {
    return policy == AiToolPolicy::NoLocalExecution || policy == AiToolPolicy::ReadOnlyPc;
}

AiToolPolicy AiOrchestrator::policyForAgent(const WorkflowTemplate& workflow,
                                            const QString& agent_id) const {
    for (const auto& agent : workflow.agents) {
        if (agent.id == agent_id) {
            return toolPolicyFromString(agent.tool_policy);
        }
    }
    return AiToolPolicy::NoLocalExecution;
}

QVector<AiOrchestrator::PhaseGroup> AiOrchestrator::planGroups(
    const WorkflowTemplate& workflow, const QSet<QString>& /*flags*/) const {
    QVector<PhaseGroup> groups;
    const int max_parallel = std::max(1, m_options.max_parallel_subagents);

    PhaseGroup current_group;
    current_group.parallel = true;

    const auto flush = [&]() {
        if (!current_group.phase_indices.isEmpty()) {
            groups.append(current_group);
            current_group = PhaseGroup{};
            current_group.parallel = true;
        }
    };

    for (int i = 0; i < workflow.phases.size(); ++i) {
        const auto& phase = workflow.phases.at(i);
        const bool has_condition = !phase.condition.trimmed().isEmpty();
        const bool parallelizable = !has_condition && isDelegatePhase(phase) &&
                                    isReadOnlyPolicy(policyForAgent(workflow, phase.agent));
        if (!parallelizable) {
            flush();
            PhaseGroup serial_group;
            serial_group.phase_indices.append(i);
            serial_group.parallel = false;
            groups.append(serial_group);
            continue;
        }

        if (current_group.phase_indices.size() >= max_parallel) {
            flush();
        }
        current_group.phase_indices.append(i);
    }
    flush();
    return groups;
}

AiPhaseExecution AiOrchestrator::executeOverseerPhase(const WorkflowPhase& phase,
                                                      const CancellationToken& parent_token) const {
    AiPhaseExecution execution;
    execution.phase_id = phase.id;
    execution.phase_type = phase.type;
    execution.ran = true;
    if (m_overseer_handler) {
        QElapsedTimer timer;
        timer.start();
        const auto result = m_overseer_handler(phase, parent_token);
        execution.duration_ms = timer.elapsed();
        execution.metadata = result;
        execution.success = !result.value(QStringLiteral("failed")).toBool(false) &&
                            !parent_token.isCancellationRequested();
        if (!execution.success) {
            execution.error_message = result.value(QStringLiteral("error_message")).toString();
        }
    } else {
        execution.success = !parent_token.isCancellationRequested();
    }
    if (parent_token.isCancellationRequested()) {
        execution.success = false;
        execution.error_message = parent_token.cancelReason();
    }
    return execution;
}

AiPhaseExecution AiOrchestrator::executeToolPhase(const WorkflowPhase& phase,
                                                  AiToolPolicy policy,
                                                  const AiWorkflowPhaseContext& context,
                                                  const CancellationToken& parent_token) const {
    AiPhaseExecution execution;
    execution.phase_id = phase.id;
    execution.phase_type = phase.type;
    execution.agent_id = phase.agent;
    execution.ran = true;

    if (!m_tool_executor) {
        execution.success = false;
        execution.error_message = QStringLiteral("No tool executor configured");
        return execution;
    }

    CancellationToken phase_token =
        parent_token.isValid() ? parent_token.createChild(QStringLiteral("phase_%1").arg(phase.id))
                               : CancellationToken{};
    QElapsedTimer timer;
    timer.start();
    execution.tool_result = m_tool_executor->runToolPhase(phase, policy, context, phase_token);
    execution.duration_ms = timer.elapsed();

    if (phase_token.isCancellationRequested()) {
        execution.success = false;
        execution.error_message = phase_token.cancelReason();
        return execution;
    }
    execution.success = execution.tool_result.value(QStringLiteral("success")).toBool(false);
    if (!execution.success) {
        execution.error_message =
            execution.tool_result.value(QStringLiteral("error_message")).toString();
    }
    return execution;
}

AiPhaseExecution AiOrchestrator::executeDelegatePhase(const WorkflowTemplate& workflow,
                                                      const WorkflowPhase& phase,
                                                      const QString& run_id,
                                                      const AiWorkflowPhaseContext& context,
                                                      const CancellationToken& parent_token) const {
    AiPhaseExecution execution;
    execution.phase_id = phase.id;
    execution.phase_type = phase.type;
    execution.agent_id = phase.agent;
    execution.ran = true;

    if (!m_subagent_runner) {
        execution.success = false;
        execution.error_message = QStringLiteral("No subagent runner configured");
        return execution;
    }

    AiSubagentTask task;
    task.task_id = QStringLiteral("task_%1").arg(phase.id);
    task.run_id = run_id;
    task.workflow_id = workflow.id;
    task.phase_id = phase.id;
    task.agent_id = phase.agent;
    task.objective = phase.prompt;
    task.tool_policy = policyForAgent(workflow, phase.agent);
    task.expected_output_schema = phase.expected_output;
    task.model = m_options.default_model;
    task.reasoning_effort = m_options.default_reasoning_effort;
    task.api_key = m_options.api_key;
    task.system_instructions =
        QStringLiteral(
            "You are a SAK Utility workflow subagent. Use the workflow context JSON "
            "below as shared working memory, including prior tool outputs, artifacts, "
            "flags, and human inputs. Do not claim evidence you cannot see. Return "
            "only the requested JSON object. Keep summary/findings concise; do not "
            "copy full command output.\n\nWorkflow context JSON:\n%1")
            .arg(workflowContextForDelegate(context));
    for (const auto& agent : workflow.agents) {
        if (agent.id == phase.agent) {
            if (agent.token_budget > 0) {
                task.token_budget = agent.token_budget;
            }
            break;
        }
    }

    QElapsedTimer timer;
    timer.start();
    const auto result = m_subagent_runner->run(task, parent_token);
    execution.duration_ms = timer.elapsed();
    execution.subagent_status = result.status;
    execution.error_message = result.error_message;
    QJsonObject metadata;
    metadata[QStringLiteral("status")] = subagentStatusToString(result.status);
    metadata[QStringLiteral("summary")] = result.summary;
    metadata[QStringLiteral("confidence")] = result.confidence;
    metadata[QStringLiteral("findings")] = result.findings.size();
    metadata[QStringLiteral("tokens")] = static_cast<double>(result.usage.total_tokens);
    metadata[QStringLiteral("result")] = result.toJson();
    execution.metadata = metadata;
    execution.success = result.status == AiSubagentStatus::Complete;
    return execution;
}

void AiOrchestrator::initializeRunState(const WorkflowTemplate& workflow,
                                        const QString& run_id,
                                        RunState* state) const {
    state->result.run_id = run_id;
    state->result.workflow_id = workflow.id;
    state->result.status = AiRunStatus::Running;
    state->context.run_id = run_id;
    state->context.workflow_id = workflow.id;
    state->context.user_message = m_options.user_message;
    state->context.input_values = m_options.input_values;
    if (m_options.resume_enabled) {
        applyResumeState(workflow, state);
    }
}

void AiOrchestrator::applyResumeState(const WorkflowTemplate& workflow, RunState* state) const {
    state->result.phases = m_options.resume_prior_phases;
    state->result.flags = m_options.resume_flags;
    state->context.phase_results = m_options.resume_phase_results;
    state->context.prior_phases = m_options.resume_prior_phases;
    state->context.flags = state->result.flags;
    for (const auto& prior : m_options.resume_prior_phases) {
        if (prior.phase_id.isEmpty()) {
            continue;
        }
        if (!state->context.phase_results.contains(prior.phase_id)) {
            state->context.phase_results.insert(prior.phase_id, phaseExecutionToJson(prior));
        }
        for (int phase_index = 0; phase_index < workflow.phases.size(); ++phase_index) {
            if (workflow.phases.at(phase_index).id == prior.phase_id) {
                state->executed_indices.insert(phase_index);
                break;
            }
        }
        if (prior.ran && !prior.skipped) {
            state->result.flags.insert(QStringLiteral("%1_%2").arg(
                prior.phase_id,
                prior.success ? QStringLiteral("succeeded") : QStringLiteral("failed")));
        }
    }
    state->context.flags = state->result.flags;
}

AiPhaseExecution AiOrchestrator::executeWorkflowPhase(const WorkflowTemplate& workflow,
                                                      const WorkflowPhase& phase,
                                                      const RunState& state,
                                                      const CancellationToken& root_token) const {
    if (m_phase_started_callback) {
        m_phase_started_callback(phase);
    }
    if (isOverseerPhase(phase)) {
        return executeOverseerPhase(phase, root_token);
    }
    if (isToolPhase(phase)) {
        return executeToolPhase(
            phase, policyForAgent(workflow, phase.agent), state.context, root_token);
    }
    if (isDelegatePhase(phase)) {
        return executeDelegatePhase(
            workflow, phase, state.result.run_id, state.context, root_token);
    }
    AiPhaseExecution execution;
    execution.phase_id = phase.id;
    execution.phase_type = phase.type;
    execution.success = false;
    execution.error_message = QStringLiteral("Unsupported phase type: %1").arg(phase.type);
    return execution;
}

AiRecoveryDecision AiOrchestrator::recoveryDecisionFor(const WorkflowTemplate& workflow,
                                                       const WorkflowPhase& phase,
                                                       const AiPhaseExecution& execution) const {
    AiFailureContext failure_context;
    failure_context.workflow_id = workflow.id;
    failure_context.phase_id = execution.phase_id;
    failure_context.phase_type = execution.phase_type;
    failure_context.agent_id = execution.agent_id;
    failure_context.tool_name = phase.tool;
    failure_context.risk = phase.risk;
    failure_context.error_message = execution.error_message;
    failure_context.user_cancelled = execution.subagent_status == AiSubagentStatus::Cancelled;
    return AiRecoveryPolicy::classifyFailure(failure_context);
}

void AiOrchestrator::attachRecoveryDecision(AiPhaseExecution* execution,
                                            const AiRecoveryDecision& decision) const {
    if (execution) {
        execution->metadata.insert(QStringLiteral("recovery_decision"), decision.toJson());
    }
}

AiPhaseExecution AiOrchestrator::executeWithRecovery(const WorkflowTemplate& workflow,
                                                     const WorkflowPhase& phase,
                                                     AiPhaseExecution execution,
                                                     const RunState& state,
                                                     const CancellationToken& root_token) const {
    if (execution.success || execution.skipped) {
        return execution;
    }
    AiRecoveryDecision decision = recoveryDecisionFor(workflow, phase, execution);
    attachRecoveryDecision(&execution, decision);
    if (decision.action == AiRecoveryAction::Reassign) {
        return executeReassignmentRecovery(
            workflow, phase, {std::move(execution), decision}, state, root_token);
    }
    if (decision.action != AiRecoveryAction::Retry || !decision.retry_allowed ||
        (root_token.isValid() && root_token.isCancellationRequested())) {
        return execution;
    }
    return executeRetryRecovery(workflow, phase, {execution, decision}, state, root_token);
}

AiPhaseExecution AiOrchestrator::executeReassignmentRecovery(
    const WorkflowTemplate& workflow,
    const WorkflowPhase& phase,
    RecoveryRequest request,
    const RunState& state,
    const CancellationToken& root_token) const {
    WorkflowPhase reassignment_phase;
    reassignment_phase.id = QStringLiteral("%1_reassignment").arg(phase.id);
    reassignment_phase.type = QStringLiteral("overseer");
    reassignment_phase.agent = request.decision.suggested_agent.isEmpty()
                                   ? QStringLiteral("overseer")
                                   : request.decision.suggested_agent;
    reassignment_phase.prompt =
        QStringLiteral(
            "Review failed phase '%1' from workflow '%2'. Decide whether work can "
            "continue, needs human input, or must be aborted. Failure: %3")
            .arg(phase.id, workflow.id, request.execution.error_message);
    const AiPhaseExecution reassignment =
        executeWorkflowPhase(workflow, reassignment_phase, state, root_token);
    request.execution.metadata.insert(QStringLiteral("reassigned_to"), reassignment_phase.agent);
    request.execution.metadata.insert(QStringLiteral("reassignment"),
                                      phaseExecutionToJson(reassignment));
    if (!reassignment.success) {
        request.decision.action = AiRecoveryAction::AskHuman;
        request.decision.reason = QStringLiteral("Reassignment failed; human decision needed.");
        request.decision.requires_human = true;
        request.decision.retry_allowed = false;
        request.decision.safe_to_continue = false;
        attachRecoveryDecision(&request.execution, request.decision);
    }
    return request.execution;
}

AiPhaseExecution AiOrchestrator::executeRetryRecovery(const WorkflowTemplate& workflow,
                                                      const WorkflowPhase& phase,
                                                      const RecoveryRequest& request,
                                                      const RunState& state,
                                                      const CancellationToken& root_token) const {
    AiPhaseExecution retry = executeWorkflowPhase(workflow, phase, state, root_token);
    retry.metadata.insert(QStringLiteral("retry_count"), 1);
    retry.metadata.insert(QStringLiteral("first_failure"), phaseExecutionToJson(request.execution));
    attachRecoveryDecision(&retry, request.decision);
    if (retry.success && !retry.skipped) {
        retry.metadata.insert(QStringLiteral("recovered"), true);
    }
    if (!retry.success && !retry.skipped) {
        AiRecoveryDecision retry_decision = recoveryDecisionFor(workflow, phase, retry);
        if (retry_decision.action == AiRecoveryAction::Retry) {
            retry_decision.action = AiRecoveryAction::Abort;
            retry_decision.retry_allowed = false;
            retry_decision.safe_to_continue = false;
            retry_decision.reason = QStringLiteral("Retry exhausted after transient failure.");
        }
        attachRecoveryDecision(&retry, retry_decision);
    }
    return retry;
}

QVector<int> AiOrchestrator::runnableGroupPositions(
    const WorkflowTemplate& workflow,
    const PhaseGroup& group,
    RunState* state,
    QVector<AiPhaseExecution>* group_executions) const {
    QVector<int> runnable_positions;
    runnable_positions.reserve(group.phase_indices.size());
    for (int slot = 0; slot < group.phase_indices.size(); ++slot) {
        const int phase_index = group.phase_indices.at(slot);
        if (m_options.resume_enabled && phase_index < m_options.resume_start_phase_index) {
            state->executed_indices.insert(phase_index);
            continue;
        }
        const auto& phase = workflow.phases.at(phase_index);
        if (!conditionSatisfied(phase, state->result.flags)) {
            AiPhaseExecution skipped;
            skipped.phase_id = phase.id;
            skipped.phase_type = phase.type;
            skipped.agent_id = phase.agent;
            skipped.skipped = true;
            skipped.skip_reason =
                QStringLiteral("Condition not satisfied: %1").arg(phase.condition);
            (*group_executions)[slot] = std::move(skipped);
            continue;
        }
        runnable_positions.append(slot);
    }
    return runnable_positions;
}

void AiOrchestrator::executeParallelGroup(const WorkflowTemplate& workflow,
                                          RunState* state,
                                          GroupWork* work,
                                          const CancellationToken& root_token) const {
    ++state->result.parallel_groups_executed;
    std::vector<std::future<AiPhaseExecution>> futures;
    futures.reserve(static_cast<size_t>(work->runnable_positions.size()));
    for (int slot : work->runnable_positions) {
        const int phase_index = work->group->phase_indices.at(slot);
        const WorkflowPhase phase = workflow.phases.at(phase_index);
        futures.emplace_back(
            std::async(std::launch::async, [this, &workflow, phase, state, &root_token]() {
                if (m_phase_started_callback) {
                    m_phase_started_callback(phase);
                }
                return executeDelegatePhase(
                    workflow, phase, state->result.run_id, state->context, root_token);
            }));
    }
    for (int i = 0; i < work->runnable_positions.size(); ++i) {
        const int slot = work->runnable_positions.at(i);
        const int phase_index = work->group->phase_indices.at(slot);
        const WorkflowPhase phase = workflow.phases.at(phase_index);
        work->executions[slot] =
            executeWithRecovery(workflow, phase, futures[i].get(), *state, root_token);
    }
}

void AiOrchestrator::executeSerialGroup(const WorkflowTemplate& workflow,
                                        const RunState& state,
                                        GroupWork* work,
                                        const CancellationToken& root_token) const {
    for (int slot : work->runnable_positions) {
        const int phase_index = work->group->phase_indices.at(slot);
        const auto& phase = workflow.phases.at(phase_index);
        if (root_token.isValid() && root_token.isCancellationRequested()) {
            AiPhaseExecution cancelled;
            cancelled.phase_id = phase.id;
            cancelled.phase_type = phase.type;
            cancelled.agent_id = phase.agent;
            cancelled.skipped = true;
            cancelled.skip_reason = root_token.cancelReason();
            work->executions[slot] = std::move(cancelled);
            continue;
        }
        work->executions[slot] =
            executeWithRecovery(workflow,
                                phase,
                                executeWorkflowPhase(workflow, phase, state, root_token),
                                state,
                                root_token);
        if (root_token.isValid() && root_token.isCancellationRequested()) {
            break;
        }
    }
}

QVector<AiPhaseExecution> AiOrchestrator::executePhaseGroup(
    const WorkflowTemplate& workflow,
    const PhaseGroup& group,
    RunState* state,
    const CancellationToken& root_token) const {
    GroupWork work;
    work.group = &group;
    work.executions.resize(group.phase_indices.size());
    work.runnable_positions = runnableGroupPositions(workflow, group, state, &work.executions);
    const bool parallel_group = group.parallel && work.runnable_positions.size() > 1;
    if (parallel_group) {
        executeParallelGroup(workflow, state, &work, root_token);
    } else {
        executeSerialGroup(workflow, *state, &work, root_token);
    }
    return work.executions;
}

void AiOrchestrator::recordCleanupFailureIfNeeded(const AiPhaseExecution& execution,
                                                  RunState* state) const {
    if (execution.phase_type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) == 0) {
        state->result.cleanup_failures.append(execution.phase_id);
    }
}

void AiOrchestrator::updateOutcomeFromRecoveryDecision(const AiPhaseExecution& execution,
                                                       const AiRecoveryDecision& recovery,
                                                       GroupOutcome* outcome) const {
    if (execution.subagent_status == AiSubagentStatus::Cancelled && !outcome->any_cancelled) {
        outcome->any_cancelled = true;
        outcome->first_cancel_msg = execution.error_message;
        return;
    }
    if ((recovery.requires_human || recovery.action == AiRecoveryAction::AskHuman) &&
        !outcome->any_waiting_human) {
        outcome->any_waiting_human = true;
        outcome->first_failure_id = execution.phase_id;
        outcome->first_failure_msg = recovery.reason.isEmpty() ? execution.error_message
                                                               : recovery.reason;
        return;
    }
    if (!recovery.safe_to_continue && !outcome->any_failed) {
        outcome->any_failed = true;
        outcome->first_failure_id = execution.phase_id;
        outcome->first_failure_msg = execution.error_message;
    }
}

void AiOrchestrator::updateOutcomeForFailure(const WorkflowTemplate& workflow,
                                             const WorkflowPhase& phase,
                                             AiPhaseExecution* execution,
                                             GroupOutcome* outcome,
                                             RunState* state) const {
    state->result.flags.insert(QStringLiteral("%1_failed").arg(execution->phase_id));
    if (!execution->metadata.contains(QStringLiteral("recovery_decision"))) {
        attachRecoveryDecision(execution, recoveryDecisionFor(workflow, phase, *execution));
    }
    const auto recovery = AiRecoveryDecision::fromJson(
        execution->metadata.value(QStringLiteral("recovery_decision")).toObject());
    recordCleanupFailureIfNeeded(*execution, state);
    updateOutcomeFromRecoveryDecision(*execution, recovery, outcome);
}

AiOrchestrator::GroupOutcome AiOrchestrator::appendGroupExecutions(
    const WorkflowTemplate& workflow,
    const PhaseGroup& group,
    QVector<AiPhaseExecution>* group_executions,
    RunState* state) const {
    GroupOutcome outcome;
    for (int slot = 0; slot < group_executions->size(); ++slot) {
        auto& execution = (*group_executions)[slot];
        if (execution.phase_id.isEmpty()) {
            continue;
        }
        const int phase_index = group.phase_indices.at(slot);
        state->executed_indices.insert(phase_index);
        if (execution.ran && !execution.skipped && !execution.success) {
            updateOutcomeForFailure(
                workflow, workflow.phases.at(phase_index), &execution, &outcome, state);
        } else if (execution.ran && !execution.skipped && execution.success) {
            state->result.flags.insert(QStringLiteral("%1_succeeded").arg(execution.phase_id));
        }
        state->result.phases.append(std::move(execution));
        state->context.phase_results.insert(state->result.phases.last().phase_id,
                                            phaseExecutionToJson(state->result.phases.last()));
        state->context.prior_phases.append(state->result.phases.last());
        state->context.flags = state->result.flags;
        if (m_phase_completed_callback) {
            m_phase_completed_callback(state->result.phases.last());
        }
    }
    return outcome;
}

void AiOrchestrator::updateRunStatusFromGroupOutcome(const GroupOutcome& outcome,
                                                     RunState* state) const {
    if (outcome.any_cancelled) {
        state->result.status = AiRunStatus::Cancelled;
        state->result.error_message = outcome.first_cancel_msg.isEmpty()
                                          ? QStringLiteral("Phase cancelled")
                                          : outcome.first_cancel_msg;
        return;
    }
    if (outcome.any_waiting_human) {
        state->result.status = AiRunStatus::WaitingForHuman;
        state->result.error_message = QStringLiteral("Phase %1 needs human input: %2")
                                          .arg(outcome.first_failure_id, outcome.first_failure_msg);
        return;
    }
    if (outcome.any_failed && m_options.stop_on_phase_failure) {
        state->aborted_for_failure = true;
        state->abort_reason = QStringLiteral("Phase %1 failed: %2")
                                  .arg(outcome.first_failure_id, outcome.first_failure_msg);
    }
}

void AiOrchestrator::runAlwaysRunRecoveryPhases(const WorkflowTemplate& workflow,
                                                RunState* state,
                                                const CancellationToken& root_token) const {
    for (int phase_index = 0; phase_index < workflow.phases.size(); ++phase_index) {
        if (state->executed_indices.contains(phase_index)) {
            continue;
        }
        const auto& phase = workflow.phases.at(phase_index);
        if (!phase.always_run) {
            continue;
        }
        AiPhaseExecution recovery = executeWorkflowPhase(workflow, phase, *state, root_token);
        recovery.metadata.insert(QStringLiteral("recovery_pass"), true);
        if (recovery.ran && !recovery.skipped) {
            state->result.flags.insert(QStringLiteral("%1_%2").arg(
                recovery.phase_id,
                recovery.success ? QStringLiteral("succeeded") : QStringLiteral("failed")));
            if (!recovery.success) {
                attachRecoveryDecision(&recovery, recoveryDecisionFor(workflow, phase, recovery));
                if (recovery.phase_type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) ==
                    0) {
                    state->result.cleanup_failures.append(recovery.phase_id);
                }
            }
        }
        state->executed_indices.insert(phase_index);
        state->result.phases.append(std::move(recovery));
        state->context.phase_results.insert(state->result.phases.last().phase_id,
                                            phaseExecutionToJson(state->result.phases.last()));
        state->context.prior_phases.append(state->result.phases.last());
        state->context.flags = state->result.flags;
        if (m_phase_completed_callback) {
            m_phase_completed_callback(state->result.phases.last());
        }
    }
}

void AiOrchestrator::runPlannedGroups(const WorkflowTemplate& workflow,
                                      const QVector<PhaseGroup>& groups,
                                      RunState* state,
                                      const CancellationToken& root_token) const {
    for (const auto& group : groups) {
        if (state->aborted_for_failure) {
            break;
        }
        if (root_token.isValid() && root_token.isCancellationRequested()) {
            state->result.status = AiRunStatus::Cancelled;
            state->result.error_message = root_token.cancelReason();
            break;
        }

        QVector<AiPhaseExecution> group_executions =
            executePhaseGroup(workflow, group, state, root_token);
        const GroupOutcome outcome =
            appendGroupExecutions(workflow, group, &group_executions, state);

        if (root_token.isValid() && root_token.isCancellationRequested()) {
            state->result.status = AiRunStatus::Cancelled;
            state->result.error_message = root_token.cancelReason();
        } else {
            updateRunStatusFromGroupOutcome(outcome, state);
        }
        if (state->result.status == AiRunStatus::Cancelled ||
            state->result.status == AiRunStatus::WaitingForHuman) {
            break;
        }
    }
}

AiOrchestratorResult AiOrchestrator::run(const WorkflowTemplate& workflow,
                                         const QString& run_id,
                                         const CancellationToken& root_token) const {
    RunState state;
    initializeRunState(workflow, run_id, &state);
    if (workflow.phases.isEmpty()) {
        state.result.status = AiRunStatus::Failed;
        state.result.error_message = QStringLiteral("Workflow has no phases");
        return state.result;
    }

    const auto groups = planGroups(workflow, state.result.flags);
    runPlannedGroups(workflow, groups, &state, root_token);

    if (state.aborted_for_failure) {
        runAlwaysRunRecoveryPhases(workflow, &state, root_token);
        state.result.status = AiRunStatus::Failed;
        state.result.error_message = state.abort_reason;
        return state.result;
    }

    if (state.result.status == AiRunStatus::WaitingForHuman ||
        state.result.status == AiRunStatus::Cancelled) {
        return state.result;
    }
    if (root_token.isValid() && root_token.isCancellationRequested()) {
        state.result.status = AiRunStatus::Cancelled;
        state.result.error_message = root_token.cancelReason();
    } else {
        state.result.status = AiRunStatus::Completed;
    }
    return state.result;
}

}  // namespace sak::ai
