// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_recovery_policy.h"

#include <QRegularExpression>
#include <QStringList>

#include <optional>

namespace sak::ai {

namespace {

bool containsAny(const QString& value, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.contains(QString::fromLatin1(needle))) {
            return true;
        }
    }
    return false;
}

struct DecisionSeed {
    AiRecoveryAction action{AiRecoveryAction::Abort};
    QString reason;
    bool requires_human{false};
    bool retry_allowed{false};
    bool safe_to_continue{false};
    QString suggested_agent;
};

AiRecoveryDecision decision(const DecisionSeed& seed) {
    AiRecoveryDecision result;
    result.action = seed.action;
    result.reason = seed.reason;
    result.requires_human = seed.requires_human;
    result.retry_allowed = seed.retry_allowed;
    result.safe_to_continue = seed.safe_to_continue;
    result.suggested_agent = seed.suggested_agent;
    result.preserve_artifacts = true;
    return result;
}

AiRecoveryDecision abortDecision(const QString& reason) {
    return decision({AiRecoveryAction::Abort, reason});
}

AiRecoveryDecision askHumanDecision(const QString& reason) {
    return decision({AiRecoveryAction::AskHuman, reason, true});
}

AiRecoveryDecision retryDecision(const QString& reason) {
    return decision({AiRecoveryAction::Retry, reason, false, true});
}

AiRecoveryDecision continueDecision(const QString& reason) {
    return decision({AiRecoveryAction::ContinueDegraded, reason, false, false, true});
}

AiRecoveryDecision reassignDecision(const QString& reason, const QString& suggested_agent) {
    return decision({AiRecoveryAction::Reassign, reason, false, false, true, suggested_agent});
}

bool isMissingInputError(const QString& error) {
    return containsAny(error, {"missing", "ambiguous", "which ", "choose", "required input"});
}

bool isPolicyGateError(const QString& error) {
    return containsAny(error, {"policy", "denied", "approval", "restore point"});
}

bool isTransientError(const QString& error) {
    return containsAny(error,
                       {"timeout",
                        "timed out",
                        "connection closed",
                        "network",
                        "http 429",
                        "rate limit",
                        "temporar"});
}

bool isMalformedModelOutput(const QString& error) {
    return containsAny(error, {"invalid json", "schema", "malformed", "no output text"});
}

bool isDownloadFailure(const QString& tool, const QString& phase) {
    return tool == QLatin1String("sak_offline_downloader") ||
           phase.contains(QStringLiteral("download")) || phase.contains(QStringLiteral("bundle"));
}

// The ONLY risk labels that may skip the human gate: the catalog's declared non-mutating
// vocabulary (none / *read_only / download_only / artifact_cleanup) and an absent risk,
// which the orchestrator leaves empty for phases that run no tool at all. Anything else --
// system_change, uninstall, destructive, repair, or a risk class added to a workflow later
// -- cannot be proven non-mutating here, so it is treated as risky (fail closed) instead of
// being waved through because it matched no keyword.
bool isNonMutatingRisk(const QString& risk) {
    return risk.isEmpty() || risk == QLatin1String("none") || risk == QLatin1String("safe") ||
           risk == QLatin1String("download_only") || risk == QLatin1String("artifact_cleanup") ||
           risk.contains(QStringLiteral("read_only")) || risk.contains(QStringLiteral("readonly"));
}

bool isRiskyFailure(const QString& risk) {
    return !isNonMutatingRisk(risk);
}

// Match the critic ROLE as a whole token ("critic", "critic_agent", "code-critic") rather
// than as a free substring of a model- or catalog-supplied agent id, so an id that merely
// embeds those letters cannot trigger an automatic review reassignment.
bool agentIsCritic(const QString& agent_id) {
    static const QRegularExpression separator(QStringLiteral("[^a-z0-9]+"));
    const QStringList tokens = agent_id.trimmed().toLower().split(separator, Qt::SkipEmptyParts);
    return tokens.contains(QStringLiteral("critic"));
}

// The canned reason names the POLICY branch that fired; the operator and the run record
// also need the failure that actually happened, so the real error is carried through
// (single-line and bounded) instead of being discarded.
QString reasonWithCause(const QString& reason, const QString& error) {
    QString cause = error.trimmed();
    if (cause.isEmpty()) {
        return reason;
    }
    cause.replace(QLatin1Char('\r'), QLatin1Char(' '));
    cause.replace(QLatin1Char('\n'), QLatin1Char(' '));
    constexpr qsizetype kMaxCauseChars = 400;
    if (cause.size() > kMaxCauseChars) {
        cause = cause.left(kMaxCauseChars) + QStringLiteral("...");
    }
    return reason + QStringLiteral(" Underlying failure: ") + cause;
}

// Failures whose recovery is a human decision. These grant nothing on their own, so they
// are classified before the risk gate to keep the more specific reason.
std::optional<AiRecoveryDecision> humanGateDecision(const QString& error) {
    if (isMissingInputError(error)) {
        return askHumanDecision(QStringLiteral("Missing or ambiguous required input."));
    }
    if (isPolicyGateError(error)) {
        return askHumanDecision(QStringLiteral("Policy gate or approval blocked the action."));
    }
    return std::nullopt;
}

// Recovery paths that proceed WITHOUT a human: re-running the phase, or walking past it in
// a degraded state. Only reachable once the risk gate has proven the failed phase
// non-mutating.
std::optional<AiRecoveryDecision> automaticRecoveryDecision(const QString& error,
                                                            const QString& phase_type) {
    if (phase_type == QLatin1String("cleanup")) {
        return continueDecision(
            QStringLiteral("Cleanup failed; preserve artifacts and report cleanup debt."));
    }
    if (isTransientError(error)) {
        return retryDecision(QStringLiteral("Transient network/model/tool failure."));
    }
    if (isMalformedModelOutput(error)) {
        return retryDecision(QStringLiteral("Model output did not match expected schema."));
    }
    return std::nullopt;
}

std::optional<AiRecoveryDecision> toolRecoveryDecision(const QString& tool,
                                                       const QString& risk,
                                                       const QString& phase) {
    // Phase ids and tool names are workflow/model supplied, so a mutating phase must never
    // reach the degraded-continue paths just because its NAME mentions a download or a
    // bundle. Callers gate on the risk first; this repeats the check so the helper cannot
    // be reused into a fail-open path.
    if (!isNonMutatingRisk(risk)) {
        return std::nullopt;
    }
    // A read-only package lookup or a download/bundle step is non-destructive: continue in a
    // DEGRADED state that surfaces the failure (recorded as ContinueDegraded metadata; cleanup
    // debt is tracked separately) so the workflow's own next phase can decide. This is honest
    // degradation, NOT an error-silencing fallback -- no source is silently substituted here.
    if (tool == QLatin1String("sak_package_manager") && risk == QLatin1String("read_only")) {
        return continueDecision(QStringLiteral(
            "Built-in package lookup failed; continue degraded and surface the lookup failure."));
    }
    if (isDownloadFailure(tool, phase)) {
        return continueDecision(QStringLiteral(
            "Built-in download step failed; continue degraded and surface the download failure."));
    }
    return std::nullopt;
}

// A decision read back from JSON is untrusted input (persisted run state, model-authored
// recovery metadata). Absent or wrong-typed values already land on the restrictive side;
// this additionally refuses CONTRADICTORY combinations -- an abort that claims it is safe to
// continue, a non-retry that claims a retry is allowed -- by clearing the permissive flag
// rather than honoring it.
void enforceDecisionInvariants(AiRecoveryDecision* decision_out) {
    if (decision_out->action == AiRecoveryAction::AskHuman) {
        decision_out->requires_human = true;
    }
    if (decision_out->action != AiRecoveryAction::Retry) {
        decision_out->retry_allowed = false;
    }
    if (decision_out->action != AiRecoveryAction::ContinueDegraded &&
        decision_out->action != AiRecoveryAction::Reassign) {
        decision_out->safe_to_continue = false;
    }
    if (decision_out->action == AiRecoveryAction::Abort || decision_out->requires_human) {
        decision_out->safe_to_continue = false;
        decision_out->preserve_artifacts = true;
    }
}

AiRecoveryDecision classifyFailureCore(const AiFailureContext& context) {
    const QString error = context.error_message.trimmed().toLower();
    const QString risk = context.risk.trimmed().toLower();
    const QString phase_type = context.phase_type.trimmed().toLower();
    const QString tool = context.tool_name.trimmed().toLower();
    const QString phase = context.phase_id.trimmed().toLower();

    if (context.user_cancelled || error.contains(QStringLiteral("cancel"))) {
        return abortDecision(QStringLiteral("User or parent run cancelled this work."));
    }

    const auto human_decision = humanGateDecision(error);
    if (human_decision.has_value()) {
        return *human_decision;
    }

    // The risk gate runs BEFORE any automatic retry/degraded path. A mutating, destructive
    // or unrecognized-risk phase that failed must not be re-run, or declared safe to
    // continue, on the strength of its error text ("timeout"), its phase type ("cleanup") or
    // its phase name ("download") -- a half-applied system change needs a human.
    if (isRiskyFailure(risk)) {
        return askHumanDecision(
            QStringLiteral("Risky or mutating action failed; human decision needed."));
    }

    const auto error_decision = automaticRecoveryDecision(error, phase_type);
    if (error_decision.has_value()) {
        return *error_decision;
    }

    const auto tool_decision = toolRecoveryDecision(tool, risk, phase);
    if (tool_decision.has_value()) {
        return *tool_decision;
    }

    if (agentIsCritic(context.agent_id)) {
        return reassignDecision(QStringLiteral("Critic failed; reassign review to overseer/report "
                                               "agent."),
                                QStringLiteral("overseer"));
    }

    return abortDecision(QStringLiteral("No safe automatic recovery path."));
}

}  // namespace

QString recoveryActionToString(AiRecoveryAction action) {
    switch (action) {
    case AiRecoveryAction::Retry:
        return QStringLiteral("retry");
    case AiRecoveryAction::Reassign:
        return QStringLiteral("reassign");
    case AiRecoveryAction::ContinueDegraded:
        return QStringLiteral("continue_degraded");
    case AiRecoveryAction::AskHuman:
        return QStringLiteral("ask_human");
    case AiRecoveryAction::Abort:
        return QStringLiteral("abort");
    }
    return QStringLiteral("abort");
}

AiRecoveryAction recoveryActionFromString(const QString& value) {
    const QString normalized = value.trimmed().toLower();
    if (normalized == QLatin1String("retry")) {
        return AiRecoveryAction::Retry;
    }
    if (normalized == QLatin1String("reassign")) {
        return AiRecoveryAction::Reassign;
    }
    if (normalized == QLatin1String("continue_degraded")) {
        return AiRecoveryAction::ContinueDegraded;
    }
    if (normalized == QLatin1String("ask_human")) {
        return AiRecoveryAction::AskHuman;
    }
    // Absent, wrong-typed or unknown action text resolves to the most restrictive action
    // (abort), never to a permissive one.
    return AiRecoveryAction::Abort;
}

QJsonObject AiRecoveryDecision::toJson() const {
    QJsonObject obj;
    obj[QStringLiteral("action")] = recoveryActionToString(action);
    obj[QStringLiteral("reason")] = reason;
    obj[QStringLiteral("suggested_agent")] = suggested_agent;
    obj[QStringLiteral("requires_human")] = requires_human;
    obj[QStringLiteral("retry_allowed")] = retry_allowed;
    obj[QStringLiteral("safe_to_continue")] = safe_to_continue;
    obj[QStringLiteral("preserve_artifacts")] = preserve_artifacts;
    return obj;
}

AiRecoveryDecision AiRecoveryDecision::fromJson(const QJsonObject& object) {
    AiRecoveryDecision decision;
    decision.action = recoveryActionFromString(object.value(QStringLiteral("action")).toString());
    decision.reason = object.value(QStringLiteral("reason")).toString();
    decision.suggested_agent = object.value(QStringLiteral("suggested_agent")).toString();
    decision.requires_human = object.value(QStringLiteral("requires_human")).toBool(false);
    decision.retry_allowed = object.value(QStringLiteral("retry_allowed")).toBool(false);
    decision.safe_to_continue = object.value(QStringLiteral("safe_to_continue")).toBool(false);
    decision.preserve_artifacts = object.value(QStringLiteral("preserve_artifacts")).toBool(true);
    enforceDecisionInvariants(&decision);
    return decision;
}

AiRecoveryDecision AiRecoveryPolicy::classifyFailure(const AiFailureContext& context) {
    AiRecoveryDecision result = classifyFailureCore(context);
    result.reason = reasonWithCause(result.reason, context.error_message);
    return result;
}

}  // namespace sak::ai
