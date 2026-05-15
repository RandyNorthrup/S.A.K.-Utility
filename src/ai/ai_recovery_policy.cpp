// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_recovery_policy.h"

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

bool isRiskyFailure(const QString& risk) {
    return containsAny(risk, {"system_change", "repair", "uninstall", "destructive"});
}

std::optional<AiRecoveryDecision> errorRecoveryDecision(const QString& error,
                                                        const QString& phase_type) {
    if (isMissingInputError(error)) {
        return askHumanDecision(QStringLiteral("Missing or ambiguous required input."));
    }
    if (isPolicyGateError(error)) {
        return askHumanDecision(QStringLiteral("Policy gate or approval blocked the action."));
    }
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
    if (tool == QLatin1String("sak_package_manager") && risk == QLatin1String("read_only")) {
        return continueDecision(
            QStringLiteral("Built-in package lookup failed; allow official-source fallback."));
    }
    if (isDownloadFailure(tool, phase)) {
        return continueDecision(
            QStringLiteral("Built-in download path failed; allow official-source fallback."));
    }
    return std::nullopt;
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
    return decision;
}

AiRecoveryDecision AiRecoveryPolicy::classifyFailure(const AiFailureContext& context) {
    const QString error = context.error_message.trimmed().toLower();
    const QString risk = context.risk.trimmed().toLower();
    const QString phase_type = context.phase_type.trimmed().toLower();
    const QString tool = context.tool_name.trimmed().toLower();
    const QString phase = context.phase_id.trimmed().toLower();

    if (context.user_cancelled || error.contains(QStringLiteral("cancel"))) {
        return abortDecision(QStringLiteral("User or parent run cancelled this work."));
    }

    const auto error_decision = errorRecoveryDecision(error, phase_type);
    if (error_decision.has_value()) {
        return *error_decision;
    }

    const auto tool_decision = toolRecoveryDecision(tool, risk, phase);
    if (tool_decision.has_value()) {
        return *tool_decision;
    }

    if (isRiskyFailure(risk)) {
        return askHumanDecision(
            QStringLiteral("Risky or mutating action failed; human decision needed."));
    }

    if (context.agent_id.contains(QStringLiteral("critic"), Qt::CaseInsensitive)) {
        return reassignDecision(QStringLiteral("Critic failed; reassign review to overseer/report "
                                               "agent."),
                                QStringLiteral("overseer"));
    }

    return abortDecision(QStringLiteral("No safe automatic recovery path."));
}

}  // namespace sak::ai
