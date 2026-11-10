// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace sak::ai {

struct AiHumanGate {
    QString gate_id;
    QString run_id;
    QString workflow_id;
    QString phase_id;
    QString kind;
    QString name;
    QString status;
    QString question;
    QString decision;
    QString response_summary;
    QDateTime created_utc;
    QDateTime resolved_utc;
    QJsonObject metadata;

    [[nodiscard]] bool isPending() const;
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static AiHumanGate fromJson(const QJsonObject& object);
};

[[nodiscard]] QString humanGateWaitingStatus();
[[nodiscard]] QString humanGateCompletedStatus();
[[nodiscard]] QString humanGateCancelledStatus();
[[nodiscard]] QString humanGateRejectedStatus();
[[nodiscard]] QString humanGateSkippedStatus();
[[nodiscard]] QString humanGateApprovedStatus();
[[nodiscard]] QString humanGateFailedStatus();

// True only for one of the defined gate lifecycle statuses. A record whose status is
// absent, empty, or an unrecognized string is malformed and must never be trusted to
// resolve (erase) a pending gate -- callers ignore such records rather than treating an
// unknown/statusless record as "not pending" and silently dropping a real pending gate.
[[nodiscard]] bool isKnownHumanGateStatus(const QString& status);

}  // namespace sak::ai
