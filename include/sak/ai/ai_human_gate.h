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

}  // namespace sak::ai
