// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_human_gate.h"

namespace sak::ai {
namespace {

QString dateToString(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime dateFromString(const QJsonObject& object, const QString& key) {
    return QDateTime::fromString(object.value(key).toString(), Qt::ISODateWithMs);
}

}  // namespace

QString humanGateWaitingStatus() {
    return QStringLiteral("waiting_for_human");
}

QString humanGateCompletedStatus() {
    return QStringLiteral("completed");
}

QString humanGateCancelledStatus() {
    return QStringLiteral("cancelled");
}

QString humanGateRejectedStatus() {
    return QStringLiteral("rejected");
}

QString humanGateSkippedStatus() {
    return QStringLiteral("skipped");
}

QString humanGateApprovedStatus() {
    return QStringLiteral("approved");
}

QString humanGateFailedStatus() {
    return QStringLiteral("failed");
}

bool AiHumanGate::isPending() const {
    return status == humanGateWaitingStatus();
}

QJsonObject AiHumanGate::toJson() const {
    QJsonObject object;
    object[QStringLiteral("gate_id")] = gate_id;
    object[QStringLiteral("run_id")] = run_id;
    object[QStringLiteral("workflow_id")] = workflow_id;
    object[QStringLiteral("phase_id")] = phase_id;
    object[QStringLiteral("kind")] = kind;
    object[QStringLiteral("name")] = name;
    object[QStringLiteral("status")] = status;
    object[QStringLiteral("question")] = question;
    object[QStringLiteral("decision")] = decision;
    object[QStringLiteral("response_summary")] = response_summary;
    object[QStringLiteral("created_utc")] = dateToString(created_utc);
    object[QStringLiteral("resolved_utc")] = dateToString(resolved_utc);
    object[QStringLiteral("metadata")] = metadata;
    return object;
}

AiHumanGate AiHumanGate::fromJson(const QJsonObject& object) {
    AiHumanGate gate;
    gate.gate_id = object.value(QStringLiteral("gate_id")).toString();
    gate.run_id = object.value(QStringLiteral("run_id")).toString();
    gate.workflow_id = object.value(QStringLiteral("workflow_id")).toString();
    gate.phase_id = object.value(QStringLiteral("phase_id")).toString();
    gate.kind = object.value(QStringLiteral("kind")).toString();
    gate.name = object.value(QStringLiteral("name")).toString();
    gate.status = object.value(QStringLiteral("status")).toString();
    gate.question = object.value(QStringLiteral("question")).toString();
    gate.decision = object.value(QStringLiteral("decision")).toString();
    gate.response_summary = object.value(QStringLiteral("response_summary")).toString();
    gate.created_utc = dateFromString(object, QStringLiteral("created_utc"));
    gate.resolved_utc = dateFromString(object, QStringLiteral("resolved_utc"));
    gate.metadata = object.value(QStringLiteral("metadata")).toObject();
    return gate;
}

}  // namespace sak::ai
