// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_subagent_runner.h"

#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <QThread>

#include <utility>

namespace sak::ai {

namespace {
constexpr qsizetype kSubagentSummaryMaxChars = 2000;
constexpr qsizetype kSafetyIdentifierDigestChars = 32;

QString safetyIdentifierFromRunSeed(const QString& seed) {
    const QString clean = seed.trimmed();
    if (clean.isEmpty()) {
        return {};
    }
    const QByteArray digest =
        QCryptographicHash::hash(clean.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("sak-run-%1")
        .arg(QString::fromLatin1(digest.left(kSafetyIdentifierDigestChars)));
}

QStringList readStringList(const QJsonValue& value) {
    QStringList out;
    if (!value.isArray()) {
        return out;
    }
    const QJsonArray array = value.toArray();
    for (const auto& entry : array) {
        if (entry.isString()) {
            out.append(entry.toString());
        }
    }
    return out;
}

QJsonArray writeStringList(const QStringList& list) {
    QJsonArray array;
    for (const auto& value : list) {
        array.append(value);
    }
    return array;
}

bool tokenCancelled(const CancellationToken& token) {
    return token.isValid() && token.isCancellationRequested();
}

AiSubagentResult baseResult(const AiSubagentTask& task,
                            AiSubagentStatus status,
                            const QString& error_message) {
    AiSubagentResult result;
    result.task_id = task.task_id;
    result.agent_id = task.agent_id;
    result.status = status;
    result.error_message = error_message;
    return result;
}

CancellationToken agentTokenForTask(const AiSubagentTask& task,
                                    const CancellationToken& parent_token) {
    if (!parent_token.isValid()) {
        return {};
    }
    return parent_token.createChild(
        QStringLiteral("agent_%1").arg(task.task_id.isEmpty() ? task.agent_id : task.task_id));
}

QString subagentContext(const AiSubagentTask& task) {
    QStringList context_parts;
    if (!task.objective.isEmpty()) {
        context_parts << QStringLiteral("Objective: %1").arg(task.objective);
    }
    if (!task.context_refs.isEmpty()) {
        context_parts
            << QStringLiteral("Context refs: %1").arg(task.context_refs.join(QStringLiteral(", ")));
    }
    if (!task.instructions_refs.isEmpty()) {
        context_parts << QStringLiteral("Instructions: %1")
                             .arg(task.instructions_refs.join(QStringLiteral(", ")));
    }
    context_parts << QStringLiteral("Tool policy: %1").arg(toolPolicyToString(task.tool_policy));
    context_parts << QStringLiteral("Token budget: %1").arg(task.token_budget);
    if (!task.expected_output_schema.isEmpty()) {
        context_parts << QStringLiteral(
                             "Output schema label: %1. Use the standard SAK subagent result shape; "
                             "do not invent a different JSON shape.")
                             .arg(task.expected_output_schema);
    }
    context_parts << QStringLiteral(
        "Return only one JSON object with keys: status, summary, findings, artifacts, citations, "
        "actions_taken, risks, questions_for_human, recommended_next_steps, confidence.");
    context_parts << QStringLiteral(
        "Use status 'complete' when you can analyze available evidence, even if evidence is "
        "partial. Put caveats, missing admin data, and uncertainty in risks/questions. Use status "
        "'failed' only when no useful analysis can be produced, and then include error_message.");
    context_parts << QStringLiteral(
        "Each finding object should include severity, title, evidence_refs, and recommendation.");
    return context_parts.join(QLatin1Char('\n'));
}

IAiModelClient::Request modelRequestForTask(const AiSubagentTask& task) {
    IAiModelClient::Request request;
    request.system_instructions = task.system_instructions;
    request.objective = task.objective;
    request.context = subagentContext(task);
    request.expected_output_schema = task.expected_output_schema;
    request.model = task.model;
    request.reasoning_effort = task.reasoning_effort;
    request.api_key = task.api_key;
    request.safety_identifier = safetyIdentifierFromRunSeed(task.run_id);
    request.token_budget = task.token_budget;
    return request;
}

QString fallbackSummaryFromPayload(const QJsonObject& payload) {
    const QStringList summary_keys{
        QStringLiteral("summary"),
        QStringLiteral("analysis"),
        QStringLiteral("diagnosis"),
        QStringLiteral("message"),
        QStringLiteral("result"),
        QStringLiteral("answer"),
    };
    for (const auto& key : summary_keys) {
        const auto value = payload.value(key);
        if (value.isString() && !value.toString().trimmed().isEmpty()) {
            return value.toString().trimmed().left(kSubagentSummaryMaxChars);
        }
        if (value.isObject() || value.isArray()) {
            return QString::fromUtf8(QJsonDocument(value.isObject()
                                                       ? QJsonDocument(value.toObject())
                                                       : QJsonDocument(value.toArray()))
                                         .toJson(QJsonDocument::Compact))
                .left(kSubagentSummaryMaxChars);
        }
    }
    return {};
}

void normalizeParsedSubagentResult(AiSubagentResult* parsed,
                                   const AiSubagentTask& task,
                                   const IAiModelClient::Response& response,
                                   const QJsonObject& payload) {
    parsed->task_id = task.task_id;
    parsed->agent_id = task.agent_id;
    if (parsed->summary.trimmed().isEmpty()) {
        parsed->summary = fallbackSummaryFromPayload(payload);
    }
    if (parsed->usage.total_tokens == 0) {
        parsed->usage = response.usage;
    }
    if (parsed->usage.total_tokens > task.token_budget && task.token_budget > 0) {
        parsed->risks.append(QStringLiteral("Subagent exceeded advisory token budget (%1 > %2)")
                                 .arg(parsed->usage.total_tokens)
                                 .arg(task.token_budget));
    }
}

bool failedWithoutContent(const AiSubagentResult& result) {
    return result.status == AiSubagentStatus::Failed && result.error_message.trimmed().isEmpty() &&
           result.summary.trimmed().isEmpty() && result.findings.isEmpty() &&
           result.actions_taken.isEmpty() && result.risks.isEmpty() &&
           result.questions_for_human.isEmpty() && result.recommended_next_steps.isEmpty();
}

void acceptFailedStatusWithoutContent(AiSubagentResult* result) {
    result->status = AiSubagentStatus::Complete;
    result->summary = QStringLiteral(
        "Subagent returned a failed status without explanation; continuing with available prior "
        "workflow evidence.");
    result->risks.append(
        QStringLiteral("Subagent returned failed status without an error message; "
                       "accepted as degraded output."));
}

AiSubagentResult parseSubagentJsonResult(const AiSubagentTask& task,
                                         const IAiModelClient::Response& response,
                                         bool* retryable) {
    QJsonParseError parse_error{};
    const auto doc = QJsonDocument::fromJson(response.text.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        *retryable = true;
        AiSubagentResult failed = baseResult(
            task,
            AiSubagentStatus::Failed,
            QStringLiteral("Subagent returned non-JSON output: %1").arg(parse_error.errorString()));
        failed.usage = response.usage;
        return failed;
    }

    const QJsonObject payload = doc.object();
    AiSubagentResult parsed = AiSubagentResult::fromJson(payload);
    normalizeParsedSubagentResult(&parsed, task, response, payload);
    if (failedWithoutContent(parsed)) {
        acceptFailedStatusWithoutContent(&parsed);
    }
    *retryable = false;
    return parsed;
}

struct SubagentAttempt {
    AiSubagentResult result;
    bool retryable{false};
};

SubagentAttempt invokeSubagentAttempt(IAiModelClient* model_client,
                                      const AiSubagentTask& task,
                                      const IAiModelClient::Request& request,
                                      const CancellationToken& agent_token) {
    const auto response = model_client->invoke(request, agent_token);
    if (tokenCancelled(agent_token)) {
        AiSubagentResult cancelled =
            baseResult(task, AiSubagentStatus::Cancelled, agent_token.cancelReason());
        cancelled.usage = response.usage;
        return {cancelled, false};
    }
    if (!response.success) {
        AiSubagentResult failed = baseResult(task,
                                             AiSubagentStatus::Failed,
                                             response.error_message.isEmpty()
                                                 ? QStringLiteral("Model invocation failed")
                                                 : response.error_message);
        failed.usage = response.usage;
        return {failed, true};
    }
    bool retryable = false;
    return {parseSubagentJsonResult(task, response, &retryable), retryable};
}

void sleepBeforeRetry(int retry_delay_ms) {
    if (retry_delay_ms > 0) {
        QThread::msleep(static_cast<unsigned long>(retry_delay_ms));
    }
}

AiSubagentResult timeoutResult(const AiSubagentTask& task, int wall_clock_timeout_ms) {
    return baseResult(
        task,
        AiSubagentStatus::TimedOut,
        QStringLiteral("Subagent wall-clock timeout exceeded (%1 ms)").arg(wall_clock_timeout_ms));
}

AiSubagentResult runSubagentAttempts(IAiModelClient* model_client,
                                     const AiSubagentTask& task,
                                     const AiSubagentRunnerOptions& options,
                                     const CancellationToken& agent_token,
                                     const IAiModelClient::Request& request) {
    const int max_attempts = std::max(1, options.max_retries + 1);
    const QDeadlineTimer deadline = options.wall_clock_timeout_ms > 0
                                        ? QDeadlineTimer(options.wall_clock_timeout_ms)
                                        : QDeadlineTimer(QDeadlineTimer::Forever);
    AiSubagentResult last_attempt =
        baseResult(task, AiSubagentStatus::Failed, QStringLiteral("Subagent did not run"));
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (deadline.hasExpired()) {
            return timeoutResult(task, options.wall_clock_timeout_ms);
        }
        if (tokenCancelled(agent_token)) {
            return baseResult(task, AiSubagentStatus::Cancelled, agent_token.cancelReason());
        }

        const SubagentAttempt current =
            invokeSubagentAttempt(model_client, task, request, agent_token);
        last_attempt = current.result;
        if (!current.retryable || attempt >= max_attempts || deadline.hasExpired()) {
            break;
        }
        sleepBeforeRetry(options.retry_delay_ms);
    }
    if (deadline.hasExpired() && last_attempt.status == AiSubagentStatus::Failed) {
        return timeoutResult(task, options.wall_clock_timeout_ms);
    }
    return last_attempt;
}

}  // namespace

QString subagentStatusToString(AiSubagentStatus status) {
    switch (status) {
    case AiSubagentStatus::Complete:
        return QStringLiteral("complete");
    case AiSubagentStatus::Failed:
        return QStringLiteral("failed");
    case AiSubagentStatus::Blocked:
        return QStringLiteral("blocked");
    case AiSubagentStatus::Cancelled:
        return QStringLiteral("cancelled");
    case AiSubagentStatus::TimedOut:
        return QStringLiteral("timed_out");
    }
    return QStringLiteral("failed");
}

AiSubagentStatus subagentStatusFromString(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("complete")) {
        return AiSubagentStatus::Complete;
    }
    if (normalized == QLatin1String("blocked")) {
        return AiSubagentStatus::Blocked;
    }
    if (normalized == QLatin1String("cancelled")) {
        return AiSubagentStatus::Cancelled;
    }
    if (normalized == QLatin1String("timed_out")) {
        return AiSubagentStatus::TimedOut;
    }
    return AiSubagentStatus::Failed;
}

QJsonObject AiSubagentTask::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("task_id")] = task_id;
    obj[QStringLiteral("run_id")] = run_id;
    obj[QStringLiteral("workflow_id")] = workflow_id;
    obj[QStringLiteral("phase_id")] = phase_id;
    obj[QStringLiteral("agent_id")] = agent_id;
    obj[QStringLiteral("objective")] = objective;
    obj[QStringLiteral("system_instructions")] = system_instructions;
    obj[QStringLiteral("context_refs")] = writeStringList(context_refs);
    obj[QStringLiteral("instructions_refs")] = writeStringList(instructions_refs);
    obj[QStringLiteral("tool_policy")] = toolPolicyToString(tool_policy);
    obj[QStringLiteral("timeout_seconds")] = timeout_seconds;
    obj[QStringLiteral("token_budget")] = token_budget;
    obj[QStringLiteral("expected_output_schema")] = expected_output_schema;
    obj[QStringLiteral("model")] = model;
    obj[QStringLiteral("reasoning_effort")] = reasoning_effort;
    return obj;
}

AiSubagentTask AiSubagentTask::fromJson(const QJsonObject& object) {
    AiSubagentTask task;
    task.task_id = object.value(QStringLiteral("task_id")).toString();
    task.run_id = object.value(QStringLiteral("run_id")).toString();
    task.workflow_id = object.value(QStringLiteral("workflow_id")).toString();
    task.phase_id = object.value(QStringLiteral("phase_id")).toString();
    task.agent_id = object.value(QStringLiteral("agent_id")).toString();
    task.objective = object.value(QStringLiteral("objective")).toString();
    task.system_instructions = object.value(QStringLiteral("system_instructions")).toString();
    task.context_refs = readStringList(object.value(QStringLiteral("context_refs")));
    task.instructions_refs = readStringList(object.value(QStringLiteral("instructions_refs")));
    task.tool_policy = toolPolicyFromString(object.value(QStringLiteral("tool_policy")).toString());
    task.timeout_seconds =
        object.value(QStringLiteral("timeout_seconds")).toInt(kDefaultSubagentTimeoutSeconds);
    task.token_budget =
        object.value(QStringLiteral("token_budget")).toInt(kDefaultSubagentTokenBudget);
    task.expected_output_schema = object.value(QStringLiteral("expected_output_schema")).toString();
    task.model = object.value(QStringLiteral("model")).toString();
    task.reasoning_effort = object.value(QStringLiteral("reasoning_effort")).toString();
    return task;
}

QJsonObject AiSubagentFinding::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("severity")] = severity;
    obj[QStringLiteral("title")] = title;
    obj[QStringLiteral("evidence_refs")] = writeStringList(evidence_refs);
    obj[QStringLiteral("recommendation")] = recommendation;
    return obj;
}

AiSubagentFinding AiSubagentFinding::fromJson(const QJsonObject& object) {
    AiSubagentFinding finding;
    finding.severity = object.value(QStringLiteral("severity")).toString();
    finding.title = object.value(QStringLiteral("title")).toString();
    finding.evidence_refs = readStringList(object.value(QStringLiteral("evidence_refs")));
    finding.recommendation = object.value(QStringLiteral("recommendation")).toString();
    return finding;
}

QJsonObject AiSubagentResult::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("task_id")] = task_id;
    obj[QStringLiteral("agent_id")] = agent_id;
    obj[QStringLiteral("status")] = subagentStatusToString(status);
    obj[QStringLiteral("summary")] = summary;
    QJsonArray findings_array;
    for (const auto& finding : findings) {
        findings_array.append(finding.toJson());
    }
    obj[QStringLiteral("findings")] = findings_array;
    obj[QStringLiteral("artifacts")] = writeStringList(artifacts);
    obj[QStringLiteral("citations")] = writeStringList(citations);
    obj[QStringLiteral("actions_taken")] = writeStringList(actions_taken);
    obj[QStringLiteral("risks")] = writeStringList(risks);
    obj[QStringLiteral("questions_for_human")] = writeStringList(questions_for_human);
    obj[QStringLiteral("recommended_next_steps")] = writeStringList(recommended_next_steps);
    obj[QStringLiteral("confidence")] = confidence;
    obj[QStringLiteral("error_message")] = error_message;

    QJsonObject usage_obj;
    usage_obj[QStringLiteral("input_tokens")] = static_cast<double>(usage.input_tokens);
    usage_obj[QStringLiteral("output_tokens")] = static_cast<double>(usage.output_tokens);
    usage_obj[QStringLiteral("total_tokens")] = static_cast<double>(usage.total_tokens);
    obj[QStringLiteral("usage")] = usage_obj;
    return obj;
}

AiSubagentResult AiSubagentResult::fromJson(const QJsonObject& object) {
    AiSubagentResult result;
    result.task_id = object.value(QStringLiteral("task_id")).toString();
    result.agent_id = object.value(QStringLiteral("agent_id")).toString();
    result.status = subagentStatusFromString(object.value(QStringLiteral("status")).toString());
    result.summary = object.value(QStringLiteral("summary")).toString();
    const auto findings_array = object.value(QStringLiteral("findings")).toArray();
    result.findings.reserve(findings_array.size());
    for (const auto& entry : findings_array) {
        if (entry.isObject()) {
            result.findings.append(AiSubagentFinding::fromJson(entry.toObject()));
        }
    }
    result.artifacts = readStringList(object.value(QStringLiteral("artifacts")));
    result.citations = readStringList(object.value(QStringLiteral("citations")));
    result.actions_taken = readStringList(object.value(QStringLiteral("actions_taken")));
    result.risks = readStringList(object.value(QStringLiteral("risks")));
    result.questions_for_human =
        readStringList(object.value(QStringLiteral("questions_for_human")));
    result.recommended_next_steps =
        readStringList(object.value(QStringLiteral("recommended_next_steps")));
    result.confidence = object.value(QStringLiteral("confidence")).toDouble(0.0);
    result.error_message = object.value(QStringLiteral("error_message")).toString();

    const auto usage_obj = object.value(QStringLiteral("usage")).toObject();
    result.usage.input_tokens =
        static_cast<int>(usage_obj.value(QStringLiteral("input_tokens")).toDouble(0.0));
    result.usage.output_tokens =
        static_cast<int>(usage_obj.value(QStringLiteral("output_tokens")).toDouble(0.0));
    result.usage.total_tokens =
        static_cast<int>(usage_obj.value(QStringLiteral("total_tokens")).toDouble(0.0));
    return result;
}

AiSubagentRunner::AiSubagentRunner(IAiModelClient* model_client) : m_model_client(model_client) {}

void AiSubagentRunner::setOptions(const AiSubagentRunnerOptions& options) {
    m_options = options;
    if (m_options.max_retries < 0) {
        m_options.max_retries = 0;
    }
    if (m_options.retry_delay_ms < 0) {
        m_options.retry_delay_ms = 0;
    }
    if (m_options.wall_clock_timeout_ms < 0) {
        m_options.wall_clock_timeout_ms = 0;
    }
}

void AiSubagentRunner::setModelClientFactory(AiModelClientFactory factory) {
    m_model_client_factory = std::move(factory);
}

AiSubagentResult AiSubagentRunner::run(const AiSubagentTask& task,
                                       const CancellationToken& parent_token) const {
    if (tokenCancelled(parent_token)) {
        return baseResult(task, AiSubagentStatus::Cancelled, parent_token.cancelReason());
    }

    std::unique_ptr<IAiModelClient> owned_client;
    IAiModelClient* model_client = m_model_client;
    if (m_model_client_factory) {
        owned_client = m_model_client_factory();
        model_client = owned_client.get();
    }

    if (!model_client) {
        return baseResult(task,
                          AiSubagentStatus::Failed,
                          QStringLiteral("No model client configured"));
    }

    const CancellationToken agent_token = agentTokenForTask(task, parent_token);
    const IAiModelClient::Request request = modelRequestForTask(task);
    return runSubagentAttempts(model_client, task, m_options, agent_token, request);
}

}  // namespace sak::ai
