// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_human_gate.h"

#include <QString>
#include <QVector>

namespace sak::ai {

/// @brief Persists human approval/input gates as JSONL in the AI session folder.
class AiHumanGateStore {
public:
    explicit AiHumanGateStore(QString session_dir = {});

    void setSessionDirectory(const QString& session_dir);
    [[nodiscard]] QString sessionDirectory() const;
    [[nodiscard]] QString gateLogPath() const;

    /// Appends @p gate after filling any missing generated id/timestamps in the caller's object.
    /// The error sink is mandatory: persistence failure or an incomplete read must never be
    /// indistinguishable from a valid empty gate history.
    [[nodiscard]] bool appendGate(AiHumanGate& gate, QString& error_message) const;
    [[nodiscard]] QVector<AiHumanGate> loadGates(QString& error_message) const;
    [[nodiscard]] AiHumanGate latestPendingGate(QString& error_message) const;

private:
    QString m_session_dir;
};

}  // namespace sak::ai
