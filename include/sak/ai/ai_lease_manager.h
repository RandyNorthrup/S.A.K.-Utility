// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QMutex>
#include <QString>

#include <optional>

namespace sak::ai {

/// @brief Enforces one process-local mutating AI action at a time.
///
/// A held lease never expires automatically. Time-based reclamation can release exclusivity while
/// an overrun or hung handler still mutates, so recovery is deliberately fail-closed: the exact
/// bearer id must be released, or the process must restart. Process restart also stops every
/// in-process holder and creates a fresh manager, preserving the single-writer invariant.
class AiLeaseManager {
public:
    struct Lease {
        QString lease_id;
        QString agent_id;
    };

    struct AcquireResult {
        bool granted{false};
        Lease lease;
        /// Refusal reason using publicLabel(), never the bearer token.
        QString reason;
    };

    AiLeaseManager() = default;

    /// Grants a lease only when no mutating lease is active.
    [[nodiscard]] static AcquireResult acquire(const QString& agent_id);

    /// Releases @p lease_id. Returns false for a blank, wrong, or already-released bearer id.
    [[nodiscard]] static bool release(const QString& lease_id);

    /// Returns the non-secret counter prefix of a lease id for logs and user-visible diagnostics.
    [[nodiscard]] static QString publicLabel(const QString& lease_id);

private:
    static QMutex s_mutex;
    static std::optional<Lease> s_active;
    static quint64 s_next_id;
};

}  // namespace sak::ai
