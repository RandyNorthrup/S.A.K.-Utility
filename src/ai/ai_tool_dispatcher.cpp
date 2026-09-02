// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_dispatcher.h"

#include "sak/logger.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <exception>

namespace sak::ai {

namespace {

constexpr qsizetype kHealthDiscriminatorPartChars = 64;
constexpr qsizetype kHealthNestedArgumentsMaxChars = 4096;

QString normalizeName(const QString& tool_name) {
    return tool_name.trimmed().toLower();
}

QJsonObject emptyHandlerResult(const AiToolCallRequest& request) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] = QStringLiteral("Tool handler returned no data");
    return obj;
}

QJsonObject handlerThrewResult(const AiToolCallRequest& request, const QString& what) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("failure_class")] = QStringLiteral("handler_exception");
    obj[QStringLiteral("error_message")] =
        QStringLiteral("Tool handler threw an exception: %1").arg(what);
    return obj;
}

bool resultHasFailureMarker(const QJsonObject& result) {
    if (!result.value(QStringLiteral("failure_class")).toString().trimmed().isEmpty()) {
        return true;
    }
    static const QStringList kFailureFlags = {
        QStringLiteral("policy_denied"),
        QStringLiteral("handler_missing"),
        QStringLiteral("lease_denied"),
        QStringLiteral("availability_denied"),
        QStringLiteral("health_suppressed"),
        QStringLiteral("cancelled"),
        QStringLiteral("canceled"),
    };
    return std::ranges::any_of(kFailureFlags, [&result](const QString& field) {
        return result.value(field).toBool(false);
    });
}

bool resultSucceeded(const QJsonObject& result) {
    const QJsonValue success = result.value(QStringLiteral("success"));
    return success.isBool() && success.toBool() && !resultHasFailureMarker(result);
}

QString resultErrorMessage(const QJsonObject& result) {
    const QString error = result.value(QStringLiteral("error_message")).toString().trimmed();
    if (!error.isEmpty()) {
        return error;
    }
    return result.value(QStringLiteral("message")).toString().trimmed();
}

QString boundedHealthPart(const QJsonValue& value) {
    if (!value.isString()) {
        return {};
    }
    return value.toString().left(kHealthDiscriminatorPartChars).trimmed().toLower();
}

QJsonObject nestedGatewayArguments(const QJsonValue& value) {
    if (value.isObject()) {
        return value.toObject();
    }
    if (!value.isString() || value.toString().size() > kHealthNestedArgumentsMaxChars) {
        return {};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object()
                                                                          : QJsonObject{};
}

void appendHealthPart(const QString& value, QStringList* parts) {
    if (!value.isEmpty()) {
        parts->append(value);
    }
}

void appendProviderHealthTarget(const QJsonObject& arguments, QStringList* parts) {
    appendHealthPart(boundedHealthPart(arguments.value(QStringLiteral("provider_id"))), parts);
}

void appendAppHealthTarget(const QJsonObject& arguments, QStringList* parts) {
    appendHealthPart(boundedHealthPart(arguments.value(QStringLiteral("app_id"))), parts);
    appendHealthPart(boundedHealthPart(arguments.value(QStringLiteral("action"))), parts);
}

void appendWin32HealthTarget(const QJsonObject& extra, QStringList* parts) {
    QString tool = boundedHealthPart(extra.value(QStringLiteral("tool_name")));
    if (tool.isEmpty()) {
        tool = boundedHealthPart(extra.value(QStringLiteral("tool")));
    }
    appendHealthPart(tool, parts);
}

QString providerGatewayDiscriminator(const AiToolCallRequest& request,
                                     const QJsonObject& arguments) {
    QString operation = request.operation.left(kHealthDiscriminatorPartChars).trimmed().toLower();
    if (operation.isEmpty()) {
        operation = boundedHealthPart(arguments.value(QStringLiteral("operation")));
    }
    const QJsonObject extra = nestedGatewayArguments(arguments.value(QStringLiteral("arguments")));

    QStringList parts;
    appendHealthPart(operation, &parts);
    if (operation == QLatin1String("docs_query") || operation == QLatin1String("provider_status")) {
        appendProviderHealthTarget(arguments, &parts);
    } else if (operation.startsWith(QLatin1String("app_"))) {
        appendAppHealthTarget(arguments, &parts);
    } else if (operation == QLatin1String("win32_mcp_call")) {
        appendWin32HealthTarget(extra, &parts);
    }
    return parts.join(QLatin1Char(':'));
}

QString healthKeyFor(const AiToolCallRequest& request, const QJsonObject& arguments) {
    const QString name = normalizeName(request.tool_name);
    if (name == QLatin1String("sak_provider_gateway")) {
        const QString discriminator = providerGatewayDiscriminator(request, arguments);
        if (!discriminator.isEmpty()) {
            return QStringLiteral("%1:%2").arg(name, discriminator);
        }
    }
    return name;
}

bool failureAffectsHealth(const QJsonObject& result) {
    const QString failure_class = AiToolHealthLedger::classifyResult(result).trimmed().toLower();
    static const QSet<QString> kNonHealthFailures{
        QStringLiteral("cancelled"),
        QStringLiteral("canceled"),
        QStringLiteral("policy_denied"),
        QStringLiteral("policy_blocked"),
        QStringLiteral("lease_denied"),
        QStringLiteral("user_declined"),
        QStringLiteral("restore_point_declined"),
        QStringLiteral("tool_busy"),
    };
    return !failure_class.isEmpty() && !kNonHealthFailures.contains(failure_class) &&
           !failure_class.startsWith(QLatin1String("invalid_")) &&
           !failure_class.endsWith(QLatin1String("_not_found"));
}

QString handlerResultContractError(const QJsonObject& result) {
    const QJsonValue success = result.value(QStringLiteral("success"));
    if (!success.isBool()) {
        return QStringLiteral("Tool handler result omitted a boolean success field");
    }
    if (success.toBool() && resultHasFailureMarker(result)) {
        return QStringLiteral("Tool handler result contradicted success with a failure marker");
    }
    return {};
}

void stampHandlerContractFailure(const QString& contract_error, QJsonObject* result) {
    if (contract_error.isEmpty()) {
        return;
    }
    (*result)[QStringLiteral("success")] = false;
    if (!result->value(QStringLiteral("failure_class")).isString() ||
        result->value(QStringLiteral("failure_class")).toString().trimmed().isEmpty()) {
        (*result)[QStringLiteral("failure_class")] = QStringLiteral("malformed_handler_result");
    }
    if (resultErrorMessage(*result).isEmpty()) {
        (*result)[QStringLiteral("error_message")] = contract_error;
    }
}

void stampHandlerFailureEnvelope(const AiToolCallRequest& request, QJsonObject* result) {
    if (resultSucceeded(*result)) {
        return;
    }
    if (resultErrorMessage(*result).isEmpty()) {
        (*result)[QStringLiteral("error_message")] =
            QStringLiteral("Tool handler reported failure without an error message");
    }
    (*result)[QStringLiteral("tool_name")] = request.tool_name;
    (*result)[QStringLiteral("operation")] = request.operation;
}

void normalizeHandlerResult(const AiToolCallRequest& request, QJsonObject* result) {
    if (result->isEmpty()) {
        *result = emptyHandlerResult(request);
        return;
    }
    stampHandlerContractFailure(handlerResultContractError(*result), result);
    stampHandlerFailureEnvelope(request, result);
}

}  // namespace

void AiToolDispatcher::registerHandler(const QString& tool_name, Handler handler) {
    const QString name = normalizeName(tool_name);
    if (name.isEmpty()) {
        return;
    }
    if (handler) {
        m_handlers.insert(name, std::move(handler));
    } else {
        m_handlers.remove(name);
    }
}

void AiToolDispatcher::registerAvailabilityChecker(const QString& tool_name,
                                                   AvailabilityChecker checker) {
    const QString name = normalizeName(tool_name);
    if (name.isEmpty()) {
        return;
    }
    if (checker) {
        m_availability_checkers.insert(name, std::move(checker));
    } else {
        m_availability_checkers.remove(name);
    }
}

void AiToolDispatcher::clearRegistrations() {
    m_handlers.clear();
    m_availability_checkers.clear();
}

bool AiToolDispatcher::hasHandler(const QString& tool_name) const {
    return m_handlers.contains(normalizeName(tool_name));
}

int AiToolDispatcher::handlerCount() const {
    return static_cast<int>(m_handlers.size());
}

void AiToolDispatcher::setLeaseManager(AiLeaseManager* manager) {
    m_lease_manager = manager;
}

void AiToolDispatcher::setHealthLedger(AiToolHealthLedger* ledger) {
    m_health_ledger = ledger;
}

void AiToolDispatcher::invokeHandler(const Handler& handler,
                                     const AiToolCallRequest& request,
                                     const QJsonObject& arguments,
                                     DispatchOutcome* outcome) {
    QElapsedTimer timer;
    timer.start();
    try {
        outcome->result = handler(arguments, outcome->policy_decision);
    } catch (const std::exception& ex) {
        // Fail closed: a throwing handler must not leak the mutation lease -- an unreleased
        // lease blocks every future mutating call until its TTL expires -- nor skip health
        // recording. Convert the throw into a structured failure so the caller's normal
        // record-health / release-lease path still runs.
        sak::logError("AI tool handler '{}' threw: {}", request.tool_name.toStdString(), ex.what());
        outcome->result = handlerThrewResult(request, QString::fromUtf8(ex.what()));
    } catch (...) {
        sak::logError("AI tool handler '{}' threw a non-standard exception",
                      request.tool_name.toStdString());
        outcome->result = handlerThrewResult(request, QStringLiteral("unknown exception"));
    }
    outcome->latency_ms = timer.elapsed();
    outcome->dispatched = true;
    normalizeHandlerResult(request, &outcome->result);
}

AiToolDispatcher::DispatchOutcome AiToolDispatcher::dispatch(AiToolPolicy policy,
                                                             const AiToolCallRequest& request,
                                                             const QJsonObject& arguments,
                                                             const QString& agent_id) const {
    DispatchOutcome outcome;
    outcome.policy_decision = evaluateToolPolicy(policy, request);
    if (!outcome.policy_decision.allowed) {
        outcome.result = policyDeniedResult(request, outcome.policy_decision);
        return outcome;
    }

    const QString name = normalizeName(request.tool_name);
    outcome.health_key = healthKeyFor(request, arguments);
    if (!applyHealthGate(&outcome, request)) {
        return outcome;
    }
    if (!applyAvailabilityGate(&outcome, request, arguments, name)) {
        return outcome;
    }

    const Handler handler = resolveHandler(&outcome, request, name);
    if (!handler) {
        return outcome;
    }

    QString lease_id;
    if (!acquireLeaseForDispatch(&outcome, request, agent_id, &lease_id)) {
        return outcome;
    }

    invokeHandler(handler, request, arguments, &outcome);
    // Release BEFORE recording health: a release that reports the lease was already gone means
    // the TTL sweep reclaimed it mid-op, which fails the result closed -- and the health ledger
    // must then record that corrected failure, not the handler's (now untrustworthy) success.
    releaseLeaseForDispatch(&outcome, request, lease_id);
    recordHealthForResult(outcome);
    return outcome;
}

bool AiToolDispatcher::applyHealthGate(DispatchOutcome* outcome,
                                       const AiToolCallRequest& request) const {
    if (m_health_ledger == nullptr) {
        return true;
    }
    const AiToolAvailability availability = m_health_ledger->check(outcome->health_key);
    if (availability.available) {
        return true;
    }
    outcome->health_suppressed = true;
    outcome->result = healthSuppressedResult(request, availability);
    return false;
}

bool AiToolDispatcher::applyAvailabilityGate(DispatchOutcome* outcome,
                                             const AiToolCallRequest& request,
                                             const QJsonObject& arguments,
                                             const QString& normalized_name) const {
    const auto checker_it = m_availability_checkers.constFind(normalized_name);
    if (checker_it == m_availability_checkers.constEnd() || !checker_it.value()) {
        return true;
    }
    // Copy the checker out of the hash before invoking it. Invoking through the live
    // iterator would dangle if the callback re-entrantly registers or clears a checker:
    // a QHash rehash destroys the referenced std::function mid-call (use-after-free).
    const AvailabilityChecker checker = checker_it.value();
    QJsonObject availability;
    try {
        availability = checker(arguments, outcome->policy_decision);
    } catch (const std::exception& ex) {
        sak::logError("AI tool availability checker '{}' threw: {}",
                      request.tool_name.toStdString(),
                      ex.what());
        // Fail closed: a throwing checker denies (no explicit success below).
        availability[QStringLiteral("error_message")] =
            QStringLiteral("Availability check threw: %1").arg(QString::fromUtf8(ex.what()));
    } catch (...) {
        sak::logError("AI tool availability checker '{}' threw a non-standard exception",
                      request.tool_name.toStdString());
        availability[QStringLiteral("error_message")] =
            QStringLiteral("Availability check threw an unknown exception");
    }
    // Fail closed: a registered checker that returns success:true passes; anything else --
    // including an empty/malformed object with no explicit success -- denies the call. An
    // empty result is not evidence of availability.
    if (availability.value(QStringLiteral("success")).toBool(false)) {
        return true;
    }
    outcome->availability_denied = true;
    outcome->result = availabilityDeniedResult(request, availability);
    if (m_health_ledger != nullptr && failureAffectsHealth(outcome->result)) {
        m_health_ledger->recordFailure(
            outcome->health_key,
            outcome->result.value(QStringLiteral("failure_class"))
                .toString(QStringLiteral("availability_failed")),
            outcome->result.value(QStringLiteral("error_message")).toString(),
            0);
    }
    return false;
}

AiToolDispatcher::Handler AiToolDispatcher::resolveHandler(DispatchOutcome* outcome,
                                                           const AiToolCallRequest& request,
                                                           const QString& normalized_name) const {
    const auto it = m_handlers.constFind(normalized_name);
    if (it != m_handlers.constEnd() && it.value()) {
        return it.value();
    }
    outcome->handler_missing = true;
    outcome->result = handlerMissingResult(request);
    if (m_health_ledger != nullptr) {
        m_health_ledger->recordFailure(
            outcome->health_key,
            QStringLiteral("handler_missing"),
            outcome->result.value(QStringLiteral("error_message")).toString(),
            0);
    }
    return {};
}

bool AiToolDispatcher::acquireLeaseForDispatch(DispatchOutcome* outcome,
                                               const AiToolCallRequest& request,
                                               const QString& agent_id,
                                               QString* lease_id) const {
    if (!outcome->policy_decision.requires_lease) {
        return true;
    }
    if (m_lease_manager == nullptr) {
        // Fail closed: a call that requires a mutation lease must NOT run when no lease
        // manager is wired. Proceeding would let a mutating tool execute with no
        // cross-agent serialization -- the exact guarantee the lease exists to provide.
        outcome->lease_denied = true;
        outcome->result = leaseDeniedResult(
            request, QStringLiteral("Mutating lease required but no lease manager is configured"));
        return false;
    }
    const auto acquire = m_lease_manager->acquire(agent_id);
    if (acquire.granted) {
        *lease_id = acquire.lease.lease_id;
        outcome->lease_id = *lease_id;
        return true;
    }
    outcome->lease_denied = true;
    outcome->result = leaseDeniedResult(request, acquire.reason);
    return false;
}

void AiToolDispatcher::releaseLeaseForDispatch(DispatchOutcome* outcome,
                                               const AiToolCallRequest& request,
                                               const QString& lease_id) const {
    if (lease_id.isEmpty() || (m_lease_manager == nullptr)) {
        return;
    }
    if (m_lease_manager->release(lease_id)) {
        // Normal path: the lease this dispatch acquired was still held and is now released.
        return;
    }
    // release() reports false only when the id is no longer held. A lease id minted for this
    // dispatch carries an unguessable token no other agent can present. A missing lease therefore
    // means internal ownership was violated; fail closed and never report the mutation as clean.
    outcome->lease_release_failed = true;
    outcome->result[QStringLiteral("success")] = false;
    outcome->result[QStringLiteral("lease_release_failed")] = true;
    outcome->result[QStringLiteral("failure_class")] = QStringLiteral("lease_release_failed");
    outcome->result[QStringLiteral("tool_name")] = request.tool_name;
    outcome->result[QStringLiteral("operation")] = request.operation;
    outcome->result[QStringLiteral("error_message")] =
        QStringLiteral(
            "Mutating lease '%1' was reclaimed by TTL while the handler was still "
            "running; exclusive access could not be guaranteed for this call")
            .arg(lease_id);
}

void AiToolDispatcher::recordHealthForResult(const DispatchOutcome& outcome) const {
    if (m_health_ledger == nullptr) {
        return;
    }
    if (resultSucceeded(outcome.result)) {
        m_health_ledger->recordSuccess(outcome.health_key, outcome.latency_ms);
        return;
    }
    if (!failureAffectsHealth(outcome.result)) {
        return;
    }
    m_health_ledger->recordFailure(outcome.health_key,
                                   AiToolHealthLedger::classifyResult(outcome.result),
                                   resultErrorMessage(outcome.result),
                                   outcome.latency_ms);
}

QJsonObject AiToolDispatcher::leaseDeniedResult(const AiToolCallRequest& request,
                                                const QString& reason) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("lease_denied")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] =
        reason.isEmpty() ? QStringLiteral("Mutating lease unavailable") : reason;
    return obj;
}

QJsonObject AiToolDispatcher::policyDeniedResult(const AiToolCallRequest& request,
                                                 const AiToolPolicyDecision& decision) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("policy_denied")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("risky_change")] = decision.risky_change;
    obj[QStringLiteral("requires_lease")] = decision.requires_lease;
    obj[QStringLiteral("requires_exclusive_lease")] = decision.requires_exclusive_lease;
    obj[QStringLiteral("restore_point_recommended")] = decision.restore_point_recommended;
    obj[QStringLiteral("error_message")] =
        decision.reason.isEmpty() ? QStringLiteral("Tool policy denied call") : decision.reason;
    return obj;
}

QJsonObject AiToolDispatcher::handlerMissingResult(const AiToolCallRequest& request) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("handler_missing")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] =
        QStringLiteral("No handler registered for tool '%1'").arg(request.tool_name);
    return obj;
}

QJsonObject AiToolDispatcher::availabilityDeniedResult(const AiToolCallRequest& request,
                                                       const QJsonObject& availability) {
    QJsonObject obj = availability;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("availability_denied")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    if (!obj.contains(QStringLiteral("failure_class"))) {
        obj[QStringLiteral("failure_class")] = QStringLiteral("availability_failed");
    }
    if (!obj.contains(QStringLiteral("error_message"))) {
        obj[QStringLiteral("error_message")] = QStringLiteral("Tool availability check failed");
    }
    return obj;
}

QJsonObject AiToolDispatcher::healthSuppressedResult(const AiToolCallRequest& request,
                                                     const AiToolAvailability& availability) {
    QJsonObject obj = availability.toJson();
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("health_suppressed")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] =
        availability.reason.isEmpty() ? QStringLiteral("Tool/provider temporarily disabled")
                                      : availability.reason;
    return obj;
}

}  // namespace sak::ai
