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
    [[nodiscard]] QVector<FirewallRule> findRulesByPort(uint16_t port,
                                                        FirewallRule::Direction direction) const;

    /// @brief Find rules matching an application path
    [[nodiscard]] QVector<FirewallRule> findRulesByApplication(const QString& appPath) const;

    /// @brief Find rules matching a name filter
    [[nodiscard]] QVector<FirewallRule> findRulesByName(const QString& nameFilter) const;

    /// @brief Check if two port ranges overlap
    [[nodiscard]] static bool portsOverlap(const QString& a, const QString& b);

    /// @brief Whether a rule's LocalPorts string covers a specific port for gap
    ///        analysis. An empty or "*" LocalPorts is the Windows default "no
    ///        port restriction" == ALL ports, so it covers the port; otherwise
    ///        the parsed port set must contain it. Exposed as a pure seam
    ///        (mirrors portsOverlap's wildcard/empty handling).
    [[nodiscard]] static bool localPortsCoverPort(const QString& localPorts, uint16_t port);

    /// @brief Whether two rules genuinely conflict (an enabled same-direction
    ///        Allow-vs-Block pair whose profile, protocol, LOCAL AND REMOTE
    ///        ports, application, and service all overlap). Exposed as a pure
    ///        seam; addresses are conservatively assumed to overlap because
    ///        proving CIDR disjointness is out of scope (B9-11).
    [[nodiscard]] static bool rulesConflict(const FirewallRule& a, const FirewallRule& b);

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
    // False when the last enumerateViaCOM() failed (COM init or a partway break),
    // so the public ops do not emit a clean empty result over a failed security
    // audit (B9-09). An empty-but-successful enumeration leaves this true.
    bool m_enumerationOk{true};

    [[nodiscard]] QVector<FirewallRule> enumerateViaCOM();
    // Fail-closed outcome check for the COM enumeration loop: returns true (and
    // sets m_enumerationOk=false + emits errorOccurred) when the enumeration broke
    // partway (FAILED HRESULT) or dropped an unreadable rule, so the caller
    // returns an empty set instead of a truncated "clean" audit. next_hr is an
    // HRESULT (long avoids leaking <windows.h> into this header).
    [[nodiscard]] bool handleEnumerationBreak(long next_hr, int dropped);
    [[nodiscard]] QVector<FirewallConflict> findConflicts(const QVector<FirewallRule>& rules) const;
    [[nodiscard]] QVector<FirewallGap> findGaps(const QVector<FirewallRule>& rules) const;

    void checkRdpGap(const QVector<FirewallRule>& rules, QVector<FirewallGap>& gaps) const;
    void checkIcmpGap(const QVector<FirewallRule>& rules, QVector<FirewallGap>& gaps) const;
    void checkWildcardGap(const QVector<FirewallRule>& rules, QVector<FirewallGap>& gaps) const;
    void checkSmbGap(const QVector<FirewallRule>& rules, QVector<FirewallGap>& gaps) const;
    void checkDisabledBlockGap(const QVector<FirewallRule>& rules,
                               QVector<FirewallGap>& gaps) const;

    /// @brief Parse port range string to list of ports
    [[nodiscard]] static QVector<uint16_t> parsePorts(const QString& portStr);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::FirewallRuleAuditor>,
              "FirewallRuleAuditor must not be copyable.");
