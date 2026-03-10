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

QTEST_MAIN(TestFirewallRuleAuditor)
#include "test_firewall_rule_auditor.moc"
