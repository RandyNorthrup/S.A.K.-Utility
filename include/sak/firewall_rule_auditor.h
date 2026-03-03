// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file firewall_rule_auditor.h
/// @brief Windows Firewall rule enumeration with conflict/gap analysis

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief Windows Firewall rule auditor
///
/// Enumerates all firewall rules via INetFwPolicy2 COM interface,
/// detects conflicting rules, and identifies coverage gaps.
class FirewallRuleAuditor : public QObject {
    Q_OBJECT

public:
    explicit FirewallRuleAuditor(QObject* parent = nullptr);
    ~FirewallRuleAuditor() override = default;

    FirewallRuleAuditor(const FirewallRuleAuditor&) = delete;
    FirewallRuleAuditor& operator=(const FirewallRuleAuditor&) = delete;
    FirewallRuleAuditor(FirewallRuleAuditor&&) = delete;
    FirewallRuleAuditor& operator=(FirewallRuleAuditor&&) = delete;

    /// @brief Enumerate all firewall rules (blocking)
    void enumerateRules();

    /// @brief Detect conflicting rules from last enumeration
    void detectConflicts();

    /// @brief Analyze coverage gaps from last enumeration
    void analyzeGaps();

    /// @brief Full audit: enumerate + conflicts + gaps (blocking)
    void fullAudit();

    void cancel();

    /// @brief Find rules matching a specific port
    [[nodiscard]] QVector<FirewallRule> findRulesByPort(
        uint16_t port, FirewallRule::Direction direction) const;

    /// @brief Find rules matching an application path
    [[nodiscard]] QVector<FirewallRule> findRulesByApplication(
        const QString& appPath) const;

    /// @brief Find rules matching a name filter
    [[nodiscard]] QVector<FirewallRule> findRulesByName(
        const QString& nameFilter) const;

Q_SIGNALS:
    void rulesEnumerated(QVector<sak::FirewallRule> rules);
    void conflictsDetected(QVector<sak::FirewallConflict> conflicts);
    void gapsAnalyzed(QVector<sak::FirewallGap> gaps);
    void auditComplete(QVector<sak::FirewallRule> rules,
                       QVector<sak::FirewallConflict> conflicts,
                       QVector<sak::FirewallGap> gaps);
    void errorOccurred(QString error);

private:
    QVector<FirewallRule> m_rules;
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] QVector<FirewallRule> enumerateViaCOM();
    [[nodiscard]] QVector<FirewallConflict> findConflicts(
        const QVector<FirewallRule>& rules) const;
    [[nodiscard]] QVector<FirewallGap> findGaps(
        const QVector<FirewallRule>& rules) const;

    /// @brief Parse port range string to list of ports
    [[nodiscard]] static QVector<uint16_t> parsePorts(const QString& portStr);

    /// @brief Check if two port ranges overlap
    [[nodiscard]] static bool portsOverlap(const QString& a, const QString& b);
};

} // namespace sak

static_assert(!std::is_copy_constructible_v<sak::FirewallRuleAuditor>,
    "FirewallRuleAuditor must not be copyable.");
