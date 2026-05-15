// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>

namespace sak::ai {

/// @brief Tracks active mutating-tool leases so the dispatcher can enforce
/// "only one mutating action runs at a time" across overseer + subagents.
class AiLeaseManager {
public:
    struct Lease {
        QString lease_id;
        QString agent_id;
        QStringList tool_scope;
        QString risk_level;
        QDateTime acquired_at_utc;
        bool exclusive{false};
    };

    struct AcquireResult {
        bool granted{false};
        Lease lease;
        QString reason;
    };

    AiLeaseManager() = default;

    [[nodiscard]] AcquireResult acquire(const QString& agent_id,
                                        const QStringList& tool_scope,
                                        const QString& risk_level,
                                        bool exclusive);
    void release(const QString& lease_id);

    [[nodiscard]] bool hasActiveExclusive() const;
    [[nodiscard]] int activeLeaseCount() const;
    [[nodiscard]] QStringList activeLeaseIds() const;

private:
    mutable QMutex m_mutex;
    QHash<QString, Lease> m_active;
    quint64 m_next_id{1};
};

}  // namespace sak::ai
