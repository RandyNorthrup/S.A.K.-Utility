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

#include <algorithm>
#include <limits>
#include <utility>

namespace sak::ai {

namespace {
constexpr qsizetype kSubagentSummaryMaxChars = 2000;
constexpr qsizetype kSafetyIdentifierDigestChars = 32;
// The iteration cap counts model<->tool ROUND TRIPS, so without a per-turn cap a
// single response could ask the runner to execute arbitrarily many calls. A batch
// past this bound is a hostile or broken response and fails closed.
constexpr qsizetype kMaxToolCallsPerTurn = 32;
// Hard bound on the model output the runner will parse as a result. Unbounded
// provider text would otherwise be converted and parsed with no ceiling.
constexpr qsizetype kMaxSubagentResponseBytes = 4 * 1024 * 1024;

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

// Strict shape check for a model-supplied string list. readStringList() is the
// lenient reader used once a payload has been validated; this predicate is what
// the schema validator uses so a wrong-typed field surfaces as an error instead
// of silently becoming an empty list.
bool isStringArray(const QJsonValue& value) {
    if (!value.isArray()) {
        return false;
    }
    const QJsonArray array = value.toArray();
    for (const auto& entry : array) {
        if (!entry.isString()) {
            return false;
        }
    }
    return true;
}

// Token counters are model-supplied and feed budget accounting. A wrong-typed or
// negative count is never valid, and a negative would SUBTRACT from the running
// totals a budget check reads, so it is refused here instead of propagated.
qint64 readTokenCount(const QJsonObject& object, const QString& key) {
    const qint64 value = object.value(key).toInteger(0);
    return value > 0 ? value : 0;
}

// Task budgets arrive as untrusted JSON. A present-but-wrong-typed or non-positive
// budget is malformed input, not a request for "unbounded": the mandatory default
// is kept so a hostile task cannot disable the timeout or the token budget by
// sending "timeout_seconds": 0 or "timeout_seconds": "forever".
int readBoundedTaskInt(const QJsonObject& object, const QString& key, int default_value) {
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return default_value;
    }
    const int parsed = value.toInt(default_value);
    return parsed > 0 ? parsed : default_value;
}

// Token totals are summed across tool round trips and retry attempts; signed
// qint64 overflow is undefined behavior, so the sum saturates instead.
qint64 addSaturatingTokens(qint64 accumulated, qint64 addition) {
    if (addition > 0 && accumulated > std::numeric_limits<qint64>::max() - addition) {
        return std::numeric_limits<qint64>::max();
    }
    if (addition < 0 && accumulated < std::numeric_limits<qint64>::min() - addition) {
        return std::numeric_limits<qint64>::min();
    }
    return accumulated + addition;
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

// The runner's ground-truth marker for a tool it actually dispatched.
QString executedToolMarker() {
    return QStringLiteral("executed tool: ");
}

QString executedToolAction(const QString& tool_name) {
    return executedToolMarker() + tool_name;
}

// The model's self-reported actions_taken shares one list with the runner's
// ground-truth records, so a model could fabricate an entry indistinguishable
// from a tool the runner really dispatched. Relabel (never drop) any model claim
// that impersonates the marker so ground truth stays identifiable.
void relabelForgedToolClaims(AiSubagentResult* result) {
    const QString marker = executedToolMarker();
    for (auto& action : result->actions_taken) {
        if (action.startsWith(marker)) {
            action = QStringLiteral("model-reported: %1").arg(action);
        }
    }
}

// Salvages a summary when the model omitted the schema's own key. The key it was
// salvaged from is reported back so the schema deviation is recorded as a risk
// instead of the wrong-shaped payload passing as a well-formed one.
QString fallbackSummaryFromPayload(const QJsonObject& payload, QString* source_key) {
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
            *source_key = key;
            return value.toString().trimmed().left(kSubagentSummaryMaxChars);
        }
        if (value.isObject() || value.isArray()) {
            *source_key = key;
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
    relabelForgedToolClaims(parsed);
    if (parsed->summary.trimmed().isEmpty()) {
        QString source_key;
        parsed->summary = fallbackSummaryFromPayload(payload, &source_key);
        if (!source_key.isEmpty()) {
            parsed->risks.append(
                QStringLiteral("Subagent result carried no usable 'summary' string; the summary "
                               "shown was salvaged from the '%1' key.")
                    .arg(source_key));
        }
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
           result.questions_for_human.isEmpty() && result.recommended_next_steps.isEmpty() &&
           result.artifacts.isEmpty() && result.citations.isEmpty();
}

void treatContentlessFailureAsDegraded(AiSubagentResult* result) {
    // A subagent that reports "failed" with no error, summary, findings, or any
    // other content produced nothing usable. Do NOT launder it into Complete
    // (which would report a clean success and bypass the recovery machinery).
    // Record it honestly as Degraded: the orchestrator continues on prior
    // workflow evidence, but the status and risk make the degradation auditable.
    result->status = AiSubagentStatus::Degraded;
    result->summary = QStringLiteral(
        "Subagent returned a failed status without explanation or content; recording this phase "
        "as degraded and continuing with available prior workflow evidence.");
    result->risks.append(
        QStringLiteral("Subagent returned a failed status without an error message or content; "
                       "treated as degraded output, not a clean success."));
}

// Every field of a subagent payload is model-controlled. "Root is an object" is
// not validation: without the checks below, {"status":"complete"} reports a clean
// success with no evidence, an unknown status silently becomes Failed, malformed
// findings entries vanish, and a wrong-typed list becomes an empty one. A schema
// violation is surfaced as an error instead of being coerced into a default.
QString statusFieldError(const QJsonObject& payload) {
    const QJsonValue status_value = payload.value(QStringLiteral("status"));
    if (!status_value.isString()) {
        return QStringLiteral("Subagent result has no 'status' string");
    }
    const QString normalized = status_value.toString().trimmed().toLower();
    if (subagentStatusToString(subagentStatusFromString(normalized)) != normalized) {
        return QStringLiteral("Subagent result has an unknown status '%1'")
            .arg(status_value.toString());
    }
    return {};
}

QString findingsFieldError(const QJsonObject& payload) {
    if (!payload.contains(QStringLiteral("findings"))) {
        return {};
    }
    const QJsonValue findings_value = payload.value(QStringLiteral("findings"));
    if (!findings_value.isArray()) {
        return QStringLiteral("Subagent result field 'findings' is not an array");
    }
    const QJsonArray findings = findings_value.toArray();
    for (const auto& entry : findings) {
        if (!entry.isObject()) {
            return QStringLiteral("Subagent result 'findings' contains a non-object entry");
        }
    }
    return {};
}

QString stringListFieldsError(const QJsonObject& payload) {
    const QStringList list_keys{
        QStringLiteral("artifacts"),
        QStringLiteral("citations"),
        QStringLiteral("actions_taken"),
        QStringLiteral("risks"),
        QStringLiteral("questions_for_human"),
        QStringLiteral("recommended_next_steps"),
    };
    for (const auto& key : list_keys) {
        if (payload.contains(key) && !isStringArray(payload.value(key))) {
            return QStringLiteral("Subagent result field '%1' is not an array of strings").arg(key);
        }
    }
    return {};
}

// Confidence is read by callers that weight a subagent's output, so a wrong-typed
// or out-of-range value is a schema violation rather than something to clamp.
QString confidenceFieldError(const QJsonObject& payload) {
    const QJsonValue confidence = payload.value(QStringLiteral("confidence"));
    if (payload.contains(QStringLiteral("confidence")) &&
        (!confidence.isDouble() || confidence.toDouble() < 0.0 || confidence.toDouble() > 1.0)) {
        return QStringLiteral("Subagent result 'confidence' is not a number in [0,1]");
    }
    return {};
}

// The usage block feeds budget accounting, so a wrong-typed or negative counter is
// refused here instead of being coerced to 0 by the lenient reader downstream.
QString usageFieldError(const QJsonObject& payload) {
    if (!payload.contains(QStringLiteral("usage"))) {
        return {};
    }
    if (!payload.value(QStringLiteral("usage")).isObject()) {
        return QStringLiteral("Subagent result field 'usage' is not an object");
    }
    const QJsonObject usage_obj = payload.value(QStringLiteral("usage")).toObject();
    const QStringList usage_keys{
        QStringLiteral("input_tokens"),
        QStringLiteral("cached_input_tokens"),
        QStringLiteral("output_tokens"),
        QStringLiteral("reasoning_tokens"),
        QStringLiteral("total_tokens"),
    };
    for (const auto& key : usage_keys) {
        const QJsonValue value = usage_obj.value(key);
        if (usage_obj.contains(key) && (!value.isDouble() || value.toDouble() < 0.0)) {
            return QStringLiteral("Subagent result usage.%1 is not a non-negative number").arg(key);
        }
    }
    return {};
}

QString scalarFieldsError(const QJsonObject& payload) {
    const QString confidence_error = confidenceFieldError(payload);
    if (!confidence_error.isEmpty()) {
        return confidence_error;
    }
    if (payload.contains(QStringLiteral("summary")) &&
        !payload.value(QStringLiteral("summary")).isString() &&
        !payload.value(QStringLiteral("summary")).isObject() &&
        !payload.value(QStringLiteral("summary")).isArray()) {
        return QStringLiteral("Subagent result 'summary' is not a string");
    }
    if (payload.contains(QStringLiteral("error_message")) &&
        !payload.value(QStringLiteral("error_message")).isString()) {
        return QStringLiteral("Subagent result 'error_message' is not a string");
    }
    return usageFieldError(payload);
}

QString subagentPayloadSchemaError(const QJsonObject& payload) {
    QString error = statusFieldError(payload);
    if (error.isEmpty()) {
        error = findingsFieldError(payload);
    }
    if (error.isEmpty()) {
        error = stringListFieldsError(payload);
    }
    if (error.isEmpty()) {
        error = scalarFieldsError(payload);
    }
    return error;
}

AiSubagentResult parseSubagentJsonResult(const AiSubagentTask& task,
                                         const IAiModelClient::Response& response,
                                         bool* retryable) {
    const QByteArray raw = response.text.toUtf8();
    if (raw.size() > kMaxSubagentResponseBytes) {
        // Bounded before conversion to JSON: an unbounded provider response must
        // not be parsed just because it arrived.
        *retryable = false;
        AiSubagentResult failed =
            baseResult(task,
                       AiSubagentStatus::Failed,
                       QStringLiteral("Subagent response is %1 bytes, over the %2-byte parse cap")
                           .arg(raw.size())
                           .arg(kMaxSubagentResponseBytes));
        failed.usage = response.usage;
        return failed;
    }
    QJsonParseError parse_error{};
    const auto doc = QJsonDocument::fromJson(raw, &parse_error);
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
    const QString schema_error = subagentPayloadSchemaError(payload);
    if (!schema_error.isEmpty()) {
        *retryable = true;
        AiSubagentResult failed = baseResult(task, AiSubagentStatus::Failed, schema_error);
        failed.usage = response.usage;
        return failed;
    }
    AiSubagentResult parsed = AiSubagentResult::fromJson(payload);
    normalizeParsedSubagentResult(&parsed, task, response, payload);
    if (failedWithoutContent(parsed)) {
        treatContentlessFailureAsDegraded(&parsed);
    }
    *retryable = false;
    return parsed;
}

struct SubagentAttempt {
    AiSubagentResult result;
    bool retryable{false};
};

// Records the tools the runner ACTUALLY executed, distinct from the model's
// self-reported actions_taken, so the result reflects ground truth.
void recordExecutedTools(AiSubagentResult* result, const QStringList& executed_tools) {
    for (const auto& tool : executed_tools) {
        result->actions_taken.append(executedToolAction(tool));
    }
}

// Sums a turn's usage into a running accumulator so an acting subagent reports
// tokens across ALL of its model<->tool round trips, not just the last turn.
void addUsage(TokenUsage* accumulated, const TokenUsage& turn) {
    accumulated->input_tokens = addSaturatingTokens(accumulated->input_tokens, turn.input_tokens);
    accumulated->cached_input_tokens = addSaturatingTokens(accumulated->cached_input_tokens,
                                                           turn.cached_input_tokens);
    accumulated->output_tokens = addSaturatingTokens(accumulated->output_tokens,
                                                     turn.output_tokens);
    accumulated->reasoning_tokens = addSaturatingTokens(accumulated->reasoning_tokens,
                                                        turn.reasoning_tokens);
    accumulated->total_tokens = addSaturatingTokens(accumulated->total_tokens, turn.total_tokens);
}

// Honest terminal result when the wall-clock deadline expires mid tool-loop.
// TimedOut (not the Degraded cap result) so a real timeout is not mislabeled as
// an iteration-cap stop and is not silently continued as a success.
AiSubagentResult toolLoopTimeoutResult(const AiSubagentTask& task,
                                       const QStringList& executed_tools,
                                       const TokenUsage& usage) {
    AiSubagentResult result =
        baseResult(task,
                   AiSubagentStatus::TimedOut,
                   QStringLiteral("Subagent wall-clock timeout expired during tool execution"));
    result.usage = usage;
    recordExecutedTools(&result, executed_tools);
    return result;
}

// Honest terminal result when a subagent keeps requesting tools past its cap.
// Degraded (not Complete/Failed) so the run continues non-fatally while the
// unfinished work is auditable (mirrors the W4c contentless-failure handling).
AiSubagentResult toolIterationCapResult(const AiSubagentTask& task,
                                        int cap,
                                        const QStringList& executed_tools,
                                        const TokenUsage& usage) {
    AiSubagentResult result = baseResult(
        task,
        AiSubagentStatus::Degraded,
        QStringLiteral("Subagent reached its tool-call iteration cap (%1) before returning a final "
                       "answer")
            .arg(cap));
    result.usage = usage;
    result.summary =
        QStringLiteral(
            "Subagent stopped after %1 tool iterations without a final answer; returning "
            "a degraded partial result.")
            .arg(cap);
    result.risks.append(
        QStringLiteral("Tool-call iteration cap (%1) reached; the subagent may not have completed "
                       "its work.")
            .arg(cap));
    recordExecutedTools(&result, executed_tools);
    return result;
}

struct ToolExecutionContext {
    IAiSubagentToolExecutor* executor{nullptr};
    int max_iterations{kDefaultSubagentToolIterationCap};
};

// Immutable per-attempt inputs, bundled to keep the acting-loop helpers within
// the argument-count budget.
struct AttemptContext {
    IAiModelClient* model_client{nullptr};
    const AiSubagentTask* task{nullptr};
    const IAiModelClient::Request* request{nullptr};
    ToolExecutionContext tools;
};

struct ActingLoopOutcome {
    IAiModelClient::Response response;
    QStringList executed_tools;
    bool early_return{false};       ///< Set when the loop terminated with a final attempt.
    SubagentAttempt early_attempt;  ///< Valid only when early_return is true.
};

// Terminates the batch: `result` becomes the attempt's final outcome.
bool stopBatch(ActingLoopOutcome* outcome, AiSubagentResult result) {
    outcome->early_return = true;
    outcome->early_attempt = {std::move(result), false};
    return false;
}

AiSubagentResult cancelledToolResult(const AiSubagentTask& task,
                                     const CancellationToken& agent_token,
                                     const QStringList& executed_tools,
                                     const TokenUsage& usage) {
    AiSubagentResult cancelled =
        baseResult(task, AiSubagentStatus::Cancelled, agent_token.cancelReason());
    cancelled.usage = usage;
    recordExecutedTools(&cancelled, executed_tools);
    return cancelled;
}

// Terminal result when the model's tool batch, or the executor's output for it,
// violates the runner's contract. Never retryable: the machine may already have
// been touched by an earlier call in the same attempt.
AiSubagentResult toolContractFailureResult(const AiSubagentTask& task,
                                           const QString& error_message,
                                           const QStringList& executed_tools,
                                           const TokenUsage& usage) {
    AiSubagentResult result = baseResult(task, AiSubagentStatus::Failed, error_message);
    result.usage = usage;
    recordExecutedTools(&result, executed_tools);
    return result;
}

// The tool-call batch is model-controlled input, so it is validated BEFORE any
// call runs: an empty or duplicate call id, a nameless tool, malformed arguments,
// a missing continuation handle, or an over-cap batch all fail closed here rather
// than being handed to the executor (same rules the panel's turn validator
// applies to the main-chat path).
QString toolCallBatchError(const IAiModelClient::Response& response) {
    if (response.response_id.trimmed().isEmpty()) {
        return QStringLiteral("Model requested tool calls without a continuation handle");
    }
    if (response.tool_calls.size() > kMaxToolCallsPerTurn) {
        return QStringLiteral("Model requested %1 tool calls in one turn (cap %2)")
            .arg(response.tool_calls.size())
            .arg(kMaxToolCallsPerTurn);
    }
    QStringList seen_ids;
    for (const auto& call : response.tool_calls) {
        if (call.call_id.trimmed().isEmpty()) {
            return QStringLiteral("Model returned a tool call with no call id");
        }
        if (seen_ids.contains(call.call_id)) {
            return QStringLiteral("Duplicate tool call id %1 in one batch").arg(call.call_id);
        }
        seen_ids.append(call.call_id);
        if (call.name.trimmed().isEmpty()) {
            return QStringLiteral("Tool call %1 has no tool name").arg(call.call_id);
        }
        const QString arguments = call.arguments_json.trimmed();
        if (arguments.isEmpty()) {
            continue;  // A no-argument call is legitimate; a wrong-shaped one is not.
        }
        QJsonParseError parse_error{};
        const auto doc = QJsonDocument::fromJson(arguments.toUtf8(), &parse_error);
        if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            return QStringLiteral("Tool call %1 has malformed arguments: %2")
                .arg(call.call_id, parse_error.errorString());
        }
    }
    return {};
}

// The executor's output is forwarded straight back to the model, so its shape is
// verified before it is trusted: an output keyed to a different (or empty) call
// id, or output that is not JSON, is a broken execution, not a result.
QString toolOutputError(const AiSubagentToolCall& call, const AiSubagentToolOutput& output) {
    if (output.call_id != call.call_id) {
        return QStringLiteral("Tool %1 returned output for call id '%2', expected '%3'")
            .arg(call.name, output.call_id, call.call_id);
    }
    QJsonParseError parse_error{};
    const auto doc = QJsonDocument::fromJson(output.output_json.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !(doc.isObject() || doc.isArray())) {
        return QStringLiteral("Tool %1 returned malformed output JSON: %2")
            .arg(call.name, parse_error.errorString());
    }
    return {};
}

// A user cancel and the wall clock each end a tool turn wherever they fire, and
// each has its own honest terminal result. Returns false when the turn must stop,
// with outcome->early_attempt holding that result.
bool toolTurnMayProceed(const AttemptContext& ctx,
                        const CancellationToken& agent_token,
                        const QDeadlineTimer& deadline,
                        const TokenUsage& accumulated,
                        ActingLoopOutcome* outcome) {
    if (tokenCancelled(agent_token)) {
        return stopBatch(
            outcome,
            cancelledToolResult(*ctx.task, agent_token, outcome->executed_tools, accumulated));
    }
    if (deadline.hasExpired()) {
        return stopBatch(outcome,
                         toolLoopTimeoutResult(*ctx.task, outcome->executed_tools, accumulated));
    }
    return true;
}

// Executes one response's tool_calls, rechecking cancellation and the wall-clock
// deadline BEFORE each individual call -- not just once per response -- and again
// after the last call, before the model is continued. A user cancel or deadline
// that fires part-way through a multi-call batch stops the remaining calls
// immediately (populating outcome->early_attempt) instead of running the whole batch.
// Returns false when it terminated early; true when the batch completed and the model
// was continued with the collected outputs.
bool executeToolCallsForTurn(const AttemptContext& ctx,
                             const CancellationToken& agent_token,
                             const QDeadlineTimer& deadline,
                             const TokenUsage& accumulated,
                             ActingLoopOutcome* outcome) {
    const QString batch_error = toolCallBatchError(outcome->response);
    if (!batch_error.isEmpty()) {
        return stopBatch(outcome,
                         toolContractFailureResult(
                             *ctx.task, batch_error, outcome->executed_tools, accumulated));
    }
    QVector<AiSubagentToolOutput> outputs;
    outputs.reserve(outcome->response.tool_calls.size());
    for (const auto& call : outcome->response.tool_calls) {
        if (!toolTurnMayProceed(ctx, agent_token, deadline, accumulated, outcome)) {
            return false;
        }
        outcome->executed_tools << call.name;
        const AiSubagentToolOutput output =
            ctx.tools.executor->executeToolCall(*ctx.task, call, agent_token);
        const QString output_error = toolOutputError(call, output);
        if (!output_error.isEmpty()) {
            return stopBatch(outcome,
                             toolContractFailureResult(
                                 *ctx.task, output_error, outcome->executed_tools, accumulated));
        }
        outputs.append(output);
    }
    if (!toolTurnMayProceed(ctx, agent_token, deadline, accumulated, outcome)) {
        return false;
    }
    outcome->response = ctx.model_client->continueWithToolOutputs(
        *ctx.request, outcome->response.response_id, outputs, agent_token);
    return true;
}

// Decides whether a turn that still carries tool calls can be treated as an
// answer. It cannot when no executor is available (single-shot runner, or a
// policy that forbids local execution): the model's pending calls were never run,
// so this turn is NOT a final answer. Fail closed instead of silently discarding
// the pending work and parsing the unfinished turn as a completed one. Returns
// false when the loop must stop, with outcome->early_attempt holding that failure.
bool pendingToolCallsAreRunnable(const AttemptContext& ctx,
                                 const TokenUsage& accumulated,
                                 ActingLoopOutcome* outcome) {
    if (!outcome->response.success || outcome->response.tool_calls.isEmpty() ||
        ctx.tools.executor != nullptr) {
        return true;
    }
    AiSubagentResult failed = baseResult(
        *ctx.task,
        AiSubagentStatus::Failed,
        QStringLiteral("Model requested %1 tool call(s) but no tool executor is available "
                       "under this task's policy; the turn produced no final answer")
            .arg(outcome->response.tool_calls.size()));
    failed.usage = accumulated;
    recordExecutedTools(&failed, outcome->executed_tools);
    return stopBatch(outcome, failed);
}

// Drives model<->tool round trips: while the model keeps requesting tool calls,
// execute them through the injected executor and continue the turn, until the
// model returns a final answer or the iteration cap / deadline / cancellation
// stops it. When no executor is set the loop is a no-op (single-shot behavior).
ActingLoopOutcome runToolCallLoop(const AttemptContext& ctx,
                                  const CancellationToken& agent_token,
                                  const QDeadlineTimer& deadline,
                                  IAiModelClient::Response initial_response) {
    const AiSubagentTask& task = *ctx.task;
    ActingLoopOutcome outcome;
    outcome.response = std::move(initial_response);
    TokenUsage accumulated = outcome.response.usage;
    int iterations = 0;
    while (outcome.response.success && !outcome.response.tool_calls.isEmpty() &&
           ctx.tools.executor != nullptr) {
        if (!toolTurnMayProceed(ctx, agent_token, deadline, accumulated, &outcome)) {
            return outcome;
        }
        if (iterations >= ctx.tools.max_iterations) {
            outcome.early_return = true;
            outcome.early_attempt = {toolIterationCapResult(task,
                                                            ctx.tools.max_iterations,
                                                            outcome.executed_tools,
                                                            accumulated),
                                     false};
            return outcome;
        }
        if (!executeToolCallsForTurn(ctx, agent_token, deadline, accumulated, &outcome)) {
            return outcome;
        }
        addUsage(&accumulated, outcome.response.usage);
        ++iterations;
    }
    if (!pendingToolCallsAreRunnable(ctx, accumulated, &outcome)) {
        return outcome;
    }
    // The final turn carries the usage summed across every round trip so callers
    // (budget checks, telemetry) see the whole cost, not just the last turn.
    outcome.response.usage = accumulated;
    return outcome;
}

SubagentAttempt invokeSubagentAttempt(const AttemptContext& ctx,
                                      const CancellationToken& agent_token,
                                      const QDeadlineTimer& deadline) {
    const AiSubagentTask& task = *ctx.task;
    const ActingLoopOutcome loop = runToolCallLoop(
        ctx, agent_token, deadline, ctx.model_client->invoke(*ctx.request, agent_token));
    if (loop.early_return) {
        return loop.early_attempt;
    }
    const IAiModelClient::Response& response = loop.response;
    // Retrying an attempt replays the WHOLE attempt from invoke(), so any tool it
    // already dispatched would run a second time with no idempotency key, no
    // call-id de-duplication, and no renewed human confirmation. An attempt that
    // touched the machine is therefore never retryable, whatever failed after it.
    const bool tools_ran = !loop.executed_tools.isEmpty();
    if (tokenCancelled(agent_token)) {
        AiSubagentResult cancelled =
            baseResult(task, AiSubagentStatus::Cancelled, agent_token.cancelReason());
        cancelled.usage = response.usage;
        recordExecutedTools(&cancelled, loop.executed_tools);
        return {cancelled, false};
    }
    if (!response.success) {
        AiSubagentResult failed = baseResult(task,
                                             AiSubagentStatus::Failed,
                                             response.error_message.isEmpty()
                                                 ? QStringLiteral("Model invocation failed")
                                                 : response.error_message);
        failed.usage = response.usage;
        recordExecutedTools(&failed, loop.executed_tools);
        return {failed, !tools_ran};
    }
    bool retryable = false;
    AiSubagentResult parsed = parseSubagentJsonResult(task, response, &retryable);
    recordExecutedTools(&parsed, loop.executed_tools);
    return {parsed, retryable && !tools_ran};
}

// The retry backoff must not outlive the run: sleep in short slices so a cancel
// or the wall-clock deadline stops the wait instead of burning the whole delay.
void sleepBeforeRetry(int retry_delay_ms,
                      const CancellationToken& agent_token,
                      const QDeadlineTimer& deadline) {
    constexpr int kRetrySleepSliceMs = 20;
    int remaining_ms = retry_delay_ms;
    while (remaining_ms > 0) {
        if (tokenCancelled(agent_token) || deadline.hasExpired()) {
            return;
        }
        const int slice_ms = std::min(remaining_ms, kRetrySleepSliceMs);
        QThread::msleep(static_cast<unsigned long>(slice_ms));
        remaining_ms -= slice_ms;
    }
}

AiSubagentResult timeoutResult(const AiSubagentTask& task, qint64 wall_clock_timeout_ms) {
    return baseResult(
        task,
        AiSubagentStatus::TimedOut,
        QStringLiteral("Subagent wall-clock timeout exceeded (%1 ms)").arg(wall_clock_timeout_ms));
}

// Once the wall clock has expired, the only honest non-timeout terminal state is
// Cancelled. Any other outcome produced past the deadline -- including a
// Complete/Degraded "late success" whose model call returned after the bound
// elapsed -- must be demoted to TimedOut so a result generated past budget is
// never laundered into a clean success. Already-TimedOut results stay as-is.
bool deadlineOverridesResult(AiSubagentStatus status) {
    return status != AiSubagentStatus::Cancelled && status != AiSubagentStatus::TimedOut;
}

// The effective wall-clock bound is the TIGHTEST of the runner-wide option and the per-task
// timeout_seconds. The per-task value was previously parsed but never enforced, so a task could
// run to the runner-wide limit (or forever) regardless of its own declared budget. A source that
// is 0/unset does not constrain; if both are unset the deadline is Forever.
qint64 effectiveWallClockMs(const AiSubagentRunnerOptions& options, const AiSubagentTask& task) {
    constexpr qint64 kMillisecondsPerSecond = 1000;
    const qint64 option_ms = options.wall_clock_timeout_ms > 0 ? options.wall_clock_timeout_ms : 0;
    const qint64 task_ms = task.timeout_seconds > 0
                               ? static_cast<qint64>(task.timeout_seconds) * kMillisecondsPerSecond
                               : 0;
    if (option_ms > 0 && task_ms > 0) {
        return std::min(option_ms, task_ms);
    }
    return std::max(option_ms, task_ms);
}

// A terminal timeout or cancellation must not erase what earlier attempts already
// did: a run can have mutated the machine and spent tokens before the deadline
// fired, so the executed-tool record and the accumulated cost are carried into
// the terminal result instead of being replaced by a blank one.
AiSubagentResult carryForwardEvidence(AiSubagentResult result, const AiSubagentResult& prior) {
    result.usage = prior.usage;
    for (const auto& action : prior.actions_taken) {
        if (!result.actions_taken.contains(action)) {
            result.actions_taken.append(action);
        }
    }
    return result;
}

AiSubagentResult runSubagentAttempts(const AttemptContext& ctx,
                                     const AiSubagentRunnerOptions& options,
                                     const CancellationToken& agent_token) {
    const AiSubagentTask& task = *ctx.task;
    const int max_attempts = std::max(1, options.max_retries + 1);
    const qint64 effective_timeout_ms = effectiveWallClockMs(options, task);
    const QDeadlineTimer deadline = effective_timeout_ms > 0
                                        ? QDeadlineTimer(effective_timeout_ms)
                                        : QDeadlineTimer(QDeadlineTimer::Forever);
    AiSubagentResult last_attempt =
        baseResult(task, AiSubagentStatus::Failed, QStringLiteral("Subagent did not run"));
    TokenUsage attempts_usage;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (deadline.hasExpired()) {
            return carryForwardEvidence(timeoutResult(task, effective_timeout_ms), last_attempt);
        }
        if (tokenCancelled(agent_token)) {
            return carryForwardEvidence(
                baseResult(task, AiSubagentStatus::Cancelled, agent_token.cancelReason()),
                last_attempt);
        }

        const SubagentAttempt current = invokeSubagentAttempt(ctx, agent_token, deadline);
        addUsage(&attempts_usage, current.result.usage);
        last_attempt = current.result;
        // Every attempt's tokens are real spend, so the reported usage is the sum
        // across attempts, not just whatever the surviving attempt reported.
        last_attempt.usage = attempts_usage;
        if (!current.retryable || attempt >= max_attempts || deadline.hasExpired()) {
            break;
        }
        sleepBeforeRetry(options.retry_delay_ms, agent_token, deadline);
    }
    if (deadline.hasExpired() && deadlineOverridesResult(last_attempt.status)) {
        return carryForwardEvidence(timeoutResult(task, effective_timeout_ms), last_attempt);
    }
    return last_attempt;
}

}  // namespace

IAiModelClient::Response IAiModelClient::continueWithToolOutputs(
    const Request& /*request*/,
    const QString& /*response_id*/,
    const QVector<AiSubagentToolOutput>& /*outputs*/,
    const CancellationToken& /*token*/) {
    Response response;
    response.error_message =
        QStringLiteral("This model client does not support tool-call continuation");
    return response;
}

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
    case AiSubagentStatus::Degraded:
        return QStringLiteral("degraded");
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
    if (normalized == QLatin1String("degraded")) {
        return AiSubagentStatus::Degraded;
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
    task.timeout_seconds = readBoundedTaskInt(object,
                                              QStringLiteral("timeout_seconds"),
                                              kDefaultSubagentTimeoutSeconds);
    task.token_budget =
        readBoundedTaskInt(object, QStringLiteral("token_budget"), kDefaultSubagentTokenBudget);
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
    usage_obj[QStringLiteral("cached_input_tokens")] =
        static_cast<double>(usage.cached_input_tokens);
    usage_obj[QStringLiteral("output_tokens")] = static_cast<double>(usage.output_tokens);
    usage_obj[QStringLiteral("reasoning_tokens")] = static_cast<double>(usage.reasoning_tokens);
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
    result.usage.input_tokens = readTokenCount(usage_obj, QStringLiteral("input_tokens"));
    result.usage.cached_input_tokens = readTokenCount(usage_obj,
                                                      QStringLiteral("cached_input_tokens"));
    result.usage.output_tokens = readTokenCount(usage_obj, QStringLiteral("output_tokens"));
    result.usage.reasoning_tokens = readTokenCount(usage_obj, QStringLiteral("reasoning_tokens"));
    result.usage.total_tokens = readTokenCount(usage_obj, QStringLiteral("total_tokens"));
    return result;
}

AiSubagentRunner::AiSubagentRunner(IAiModelClient* model_client) : m_model_client(model_client) {}

void AiSubagentRunner::setOptions(const AiSubagentRunnerOptions& options) {
    m_options = options;
    m_options.max_retries = std::max(m_options.max_retries, 0);
    m_options.retry_delay_ms = std::max(m_options.retry_delay_ms, 0);
    m_options.wall_clock_timeout_ms = std::max(m_options.wall_clock_timeout_ms, 0);
    if (m_options.max_tool_iterations < 0) {
        m_options.max_tool_iterations = kDefaultSubagentToolIterationCap;
    }
}

void AiSubagentRunner::setModelClientFactory(AiModelClientFactory factory) {
    m_model_client_factory = std::move(factory);
}

void AiSubagentRunner::setToolExecutor(IAiSubagentToolExecutor* executor) {
    m_tool_executor = executor;
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
    IAiModelClient::Request request = modelRequestForTask(task);
    // Fail closed: tools are only offered/executed when an executor is wired AND
    // the task's policy permits any local execution. A NoLocalExecution subagent
    // stays single-shot even if a global executor is present.
    const bool tools_allowed = (m_tool_executor != nullptr) &&
                               (task.tool_policy != AiToolPolicy::NoLocalExecution);
    request.enable_local_tools = tools_allowed;
    AttemptContext ctx;
    ctx.model_client = model_client;
    ctx.task = &task;
    ctx.request = &request;
    ctx.tools.executor = tools_allowed ? m_tool_executor : nullptr;
    ctx.tools.max_iterations = m_options.max_tool_iterations;
    return runSubagentAttempts(ctx, m_options, agent_token);
}

}  // namespace sak::ai
