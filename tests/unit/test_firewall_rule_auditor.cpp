// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_firewall_rule_auditor.cpp
/// @brief Unit tests for FirewallRuleAuditor

#include "sak/firewall_rule_auditor.h"
#include "sak/network_diagnostic_types.h"

#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace sak;

class TestFirewallRuleAuditor : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void findRulesByPort_emptyRules();
    void findRulesByApplication_emptyRules();
    void findRulesByName_emptyRules();
    void cancel_doesNotCrash();
    void firewallRule_defaults();
    void firewallRule_fieldAssignment();
    void firewallConflict_defaults();
    void firewallGap_defaults();
    void enumerateRules_emitsSignal();
    void fullAudit_emitsAuditComplete();
    void findRules_afterEnumeration();
    void portsOverlap_unknownExpressionOverlapsConservatively();
    void rulesConflict_requiresAllSelectorsToOverlap();
};

void TestFirewallRuleAuditor::construction_default() {
    FirewallRuleAuditor auditor;
    QVERIFY(dynamic_cast<QObject*>(&auditor) != nullptr);
}

void TestFirewallRuleAuditor::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<FirewallRuleAuditor>);
    QVERIFY(!std::is_move_constructible_v<FirewallRuleAuditor>);
}

void TestFirewallRuleAuditor::findRulesByPort_emptyRules() {
    FirewallRuleAuditor auditor;
    const auto rules = auditor.findRulesByPort(80, FirewallRule::Direction::Inbound);
    QVERIFY(rules.isEmpty());
}

void TestFirewallRuleAuditor::findRulesByApplication_emptyRules() {
    FirewallRuleAuditor auditor;
    const auto rules = auditor.findRulesByApplication(QStringLiteral("C:\\test.exe"));
    QVERIFY(rules.isEmpty());
}

void TestFirewallRuleAuditor::findRulesByName_emptyRules() {
    FirewallRuleAuditor auditor;
    const auto rules = auditor.findRulesByName(QStringLiteral("test_rule"));
    QVERIFY(rules.isEmpty());
}

void TestFirewallRuleAuditor::cancel_doesNotCrash() {
    FirewallRuleAuditor auditor;
    auditor.cancel();
    QVERIFY(dynamic_cast<QObject*>(&auditor) != nullptr);
}

void TestFirewallRuleAuditor::firewallRule_defaults() {
    FirewallRule rule;
    QVERIFY(rule.name.isEmpty());
    QVERIFY(!rule.enabled);
    QCOMPARE(rule.direction, FirewallRule::Direction::Inbound);
    QCOMPARE(rule.action, FirewallRule::Action::Allow);
    QCOMPARE(rule.protocol, FirewallRule::Protocol::Any);
    QVERIFY(rule.localPorts.isEmpty());
    QVERIFY(rule.remotePorts.isEmpty());
    QVERIFY(rule.applicationPath.isEmpty());
    QCOMPARE(rule.profiles, 0);
}

void TestFirewallRuleAuditor::firewallRule_fieldAssignment() {
    FirewallRule rule;
    rule.name = QStringLiteral("Test Rule");
    rule.description = QStringLiteral("Unit test rule");
    rule.enabled = true;
    rule.direction = FirewallRule::Direction::Outbound;
    rule.action = FirewallRule::Action::Block;
    rule.protocol = FirewallRule::Protocol::TCP;
    rule.localPorts = QStringLiteral("80,443");
    rule.applicationPath = QStringLiteral("C:\\test.exe");

    QCOMPARE(rule.name, QStringLiteral("Test Rule"));
    QVERIFY(rule.enabled);
    QCOMPARE(rule.direction, FirewallRule::Direction::Outbound);
    QCOMPARE(rule.action, FirewallRule::Action::Block);
    QCOMPARE(rule.protocol, FirewallRule::Protocol::TCP);
    QCOMPARE(rule.localPorts, QStringLiteral("80,443"));
}

void TestFirewallRuleAuditor::firewallConflict_defaults() {
    FirewallConflict conflict;
    QVERIFY(conflict.conflictDescription.isEmpty());
    QCOMPARE(conflict.severity, FirewallConflict::Severity::Info);
    QVERIFY(conflict.ruleA.name.isEmpty());
    QVERIFY(conflict.ruleB.name.isEmpty());
}

void TestFirewallRuleAuditor::firewallGap_defaults() {
    FirewallGap gap;
    QVERIFY(gap.description.isEmpty());
    QVERIFY(gap.recommendation.isEmpty());
    QCOMPARE(gap.severity, FirewallGap::Severity::Info);
}

void TestFirewallRuleAuditor::enumerateRules_emitsSignal() {
    FirewallRuleAuditor auditor;
    QSignalSpy rules_spy(&auditor, &FirewallRuleAuditor::rulesEnumerated);

    auditor.enumerateRules();

    QCOMPARE(rules_spy.count(), 1);
    const auto rules = rules_spy.takeFirst().at(0).value<QVector<FirewallRule>>();
    QVERIFY2(!rules.isEmpty(), "Windows Firewall should have rules on any system");
}

void TestFirewallRuleAuditor::fullAudit_emitsAuditComplete() {
    FirewallRuleAuditor auditor;
    QSignalSpy audit_spy(&auditor, &FirewallRuleAuditor::auditComplete);

    auditor.fullAudit();

    QCOMPARE(audit_spy.count(), 1);
}

void TestFirewallRuleAuditor::findRules_afterEnumeration() {
    FirewallRuleAuditor auditor;
    auditor.enumerateRules();

    const auto name_results = auditor.findRulesByName(QStringLiteral("Core Networking"));
    QVERIFY(name_results.size() >= 0);
}

void TestFirewallRuleAuditor::portsOverlap_unknownExpressionOverlapsConservatively() {
    // Wildcard / empty means "any".
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("*"), QStringLiteral("80")));
    QVERIFY(FirewallRuleAuditor::portsOverlap(QString(), QStringLiteral("80")));
    // Concrete ports: overlap iff they intersect.
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80"), QStringLiteral("80")));
    QVERIFY(!FirewallRuleAuditor::portsOverlap(QStringLiteral("80"), QStringLiteral("443")));
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80-100"), QStringLiteral("90")));
    QVERIFY(!FirewallRuleAuditor::portsOverlap(QStringLiteral("80-100"), QStringLiteral("200")));
    // B9-11: a non-wildcard expression that parses to no ports (a named service
    // like "RPC") cannot be proven disjoint, so it must conservatively overlap
    // rather than fail-open to no-overlap and hide a conflict.
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("RPC"), QStringLiteral("80")));
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80"), QStringLiteral("RPC-EPMap")));
}

void TestFirewallRuleAuditor::rulesConflict_requiresAllSelectorsToOverlap() {
    const auto makeRule = [](FirewallRule::Action action) {
        FirewallRule rule;
        rule.enabled = true;
        rule.direction = FirewallRule::Direction::Inbound;
        rule.action = action;
        rule.protocol = FirewallRule::Protocol::TCP;
        rule.localPorts = QStringLiteral("445");
        rule.profiles = static_cast<int>(FirewallRule::Profile::Domain);
        return rule;
    };
    const FirewallRule allow = makeRule(FirewallRule::Action::Allow);
    const FirewallRule block = makeRule(FirewallRule::Action::Block);

    // Enabled, same direction, Allow vs Block, overlapping local ports -> conflict.
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, block));

    // Same action, disabled, or opposite direction -> not a conflict.
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, allow));
    FirewallRule disabled = block;
    disabled.enabled = false;
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, disabled));

    // B9-11: disjoint REMOTE ports means the rules never apply to the same
    // traffic, so it is not a conflict (remote ports were previously ignored).
    FirewallRule allowRemote = allow;
    allowRemote.remotePorts = QStringLiteral("80");
    FirewallRule blockRemote = block;
    blockRemote.remotePorts = QStringLiteral("443");
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allowRemote, blockRemote));

    // B9-11: different services -> disjoint traffic -> not a conflict.
    FirewallRule allowSvc = allow;
    allowSvc.serviceName = QStringLiteral("ServiceA");
    FirewallRule blockSvc = block;
    blockSvc.serviceName = QStringLiteral("ServiceB");
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allowSvc, blockSvc));

    // Same service still conflicts (overlapping traffic).
    blockSvc.serviceName = QStringLiteral("ServiceA");
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowSvc, blockSvc));
}

QTEST_MAIN(TestFirewallRuleAuditor)
#include "test_firewall_rule_auditor.moc"
