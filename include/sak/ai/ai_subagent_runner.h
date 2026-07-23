// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_token_usage_tracker.h"
#include "sak/ai/ai_tool_policy.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace sak::ai {

inline constexpr int kDefaultSubagentTimeoutSeconds = 600;
inline constexpr int kDefaultSubagentTokenBudget = 8000;

/// @brief Structured task description handed by the overseer to a subagent.
struct AiSubagentTask {
    QString task_id;
    QString run_id;
    QString workflow_id;
    QString phase_id;
    QString agent_id;
    QString objective;
    QString system_instructions;
    QStringList context_refs;
    QStringList instructions_refs;
    AiToolPolicy tool_policy{AiToolPolicy::NoLocalExecution};
    int timeout_seconds{kDefaultSubagentTimeoutSeconds};
    int token_budget{kDefaultSubagentTokenBudget};
    QString expected_output_schema;
    QString model;
    QString reasoning_effort;
    QString api_key;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiSubagentTask fromJson(const QJsonObject& object);
};

struct AiSubagentFinding {
    QString severity;
    QString title;
    QStringList evidence_refs;
    QString recommendation;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiSubagentFinding fromJson(const QJsonObject& object);
};

enum class AiSubagentStatus {
    Complete,
    Failed,
    Blocked,
    Cancelled,
    TimedOut,
    // Ran and did not hard-fail, but the output is untrustworthy or empty (e.g. a
    // subagent that reported "failed" with no error/content). Reported honestly as
    // degraded instead of being laundered into Complete; the run continues on prior
    // evidence. Appended last so existing enumerator values are unchanged.
    Degraded,
};

[[nodiscard]] QString subagentStatusToString(AiSubagentStatus status);
[[nodiscard]] AiSubagentStatus subagentStatusFromString(const QString& value);

/// @brief Structured result returned to the overseer from a subagent run.
struct AiSubagentResult {
    QString task_id;
    QString agent_id;
    AiSubagentStatus status{AiSubagentStatus::Failed};
    QString summary;
    QVector<AiSubagentFinding> findings;
    QStringList artifacts;
    QStringList citations;
    QStringList actions_taken;
    QStringList risks;
    QStringList questions_for_human;
    QStringList recommended_next_steps;
    double confidence{0.0};
    TokenUsage usage;
    QString error_message;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiSubagentResult fromJson(const QJsonObject& object);
};

/// @brief A tool call the model wants executed during a subagent turn.
struct AiSubagentToolCall {
    QString call_id;
    QString name;
    QString arguments_json;
};

/// @brief The output of executing one subagent tool call, fed back to the model
/// to continue the turn. `output_json` is the JSON string the model receives.
struct AiSubagentToolOutput {
    QString call_id;
    QString output_json;
};

/// @brief Abstract model client used by production providers and deterministic test doubles.
class IAiModelClient {
public:
    struct Request {
        QString system_instructions;
        QString objective;
        QString context;
        QString expected_output_schema;
        QString model;
        QString reasoning_effort;
        QString api_key;
        QString safety_identifier;
        int token_budget{0};
        bool enable_local_tools{false};  ///< Acting subagents opt in; default off.
    };

    struct Response {
        bool success{false};
        QString text;
        TokenUsage usage;
        QString error_message;
        /// Non-empty when the model requested tool execution before finishing.
        /// The runner executes these via its injected tool executor and continues
        /// the turn with continueWithToolOutputs().
        QVector<AiSubagentToolCall> tool_calls;
        /// Opaque continuation handle (e.g. an OpenAI previous_response_id) used to
        /// resume the same turn after tool outputs are submitted.
        QString response_id;
    };

    virtual ~IAiModelClient() = default;
    [[nodiscard]] virtual Response invoke(const Request& request,
                                          const CancellationToken& token) = 0;

    /// @brief Continues a turn after the runner executed the tool calls from a
    /// prior Response. The default implementation reports lack of support, so
    /// single-shot clients need not override it (the runner only calls this when
    /// a prior Response returned tool calls).
    [[nodiscard]] virtual Response continueWithToolOutputs(
        const Request& request,
        const QString& response_id,
        const QVector<AiSubagentToolOutput>& outputs,
        const CancellationToken& token);
};

/// @brief Executes a single subagent tool call under the task's tool policy.
/// Injected into the runner so the real gate chain (router/policy/lease/health)
/// lives with the panel while the runner stays deterministic and testable. The
/// implementation MUST fail closed when the task's policy disallows the call.
class IAiSubagentToolExecutor {
public:
    virtual ~IAiSubagentToolExecutor() = default;
    [[nodiscard]] virtual AiSubagentToolOutput executeToolCall(const AiSubagentTask& task,
                                                               const AiSubagentToolCall& call,
                                                               const CancellationToken& token) = 0;
};

using AiModelClientFactory = std::function<std::unique_ptr<IAiModelClient>()>;

inline constexpr int kDefaultSubagentToolIterationCap = 8;

struct AiSubagentRunnerOptions {
    int max_retries{0};            ///< Extra attempts after the first failure.
    int retry_delay_ms{0};         ///< Delay between attempts (no sleep when 0).
    int wall_clock_timeout_ms{0};  ///< 0 disables wall-clock timeout.
    /// Max model<->tool round trips within a single attempt before the subagent
    /// is stopped with an honest degraded status. Only active when a tool
    /// executor is set and the model returns tool calls.
    int max_tool_iterations{kDefaultSubagentToolIterationCap};
};

/// @brief Runs one specialist subagent loop against an injected model client.
///
/// Wraps a single model call, parses the JSON response into an
/// AiSubagentResult, and respects a hierarchical CancellationToken. The runner
/// does not own a worker thread; callers schedule it on whatever executor
/// fits.
class AiSubagentRunner {
public:
    explicit AiSubagentRunner(IAiModelClient* model_client);

    void setOptions(const AiSubagentRunnerOptions& options);
    [[nodiscard]] AiSubagentRunnerOptions options() const { return m_options; }
    void setModelClientFactory(AiModelClientFactory factory);
    /// @brief Sets the executor used to run tool calls the model requests. When
    /// null (the default) the subagent stays single-shot: any tool calls the
    /// model returns are ignored and the turn is parsed as-is.
    void setToolExecutor(IAiSubagentToolExecutor* executor);

    [[nodiscard]] AiSubagentResult run(const AiSubagentTask& task,
                                       const CancellationToken& parent_token) const;

private:
    IAiModelClient* m_model_client{nullptr};
    AiModelClientFactory m_model_client_factory;
    IAiSubagentToolExecutor* m_tool_executor{nullptr};
    AiSubagentRunnerOptions m_options;
};

}  // namespace sak::ai
