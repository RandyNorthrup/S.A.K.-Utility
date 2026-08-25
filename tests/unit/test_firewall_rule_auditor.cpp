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
    void findRulesByApplication_filtersOnApplicationPathNotName();
    void findRulesByName_emptyRules();
    void cancel_doesNotWedgeTheAuditor();
    void firewallRule_defaults();
    void firewallRule_fieldAssignment();
    void firewallConflict_defaults();
    void firewallGap_defaults();
    void enumerateRules_emitsSignal();
    void fullAudit_emitsAuditComplete();
    void findRules_afterEnumeration();
    void portsOverlap_unknownExpressionOverlapsConservatively();
    void rulesConflict_requiresAllSelectorsToOverlap();
    void rulesConflict_scopeSelectorsNarrowTheMatch();
    void rulesConflict_profileAndProtocolArms();
    void localPortsCoverPort_emptyAndWildcardCoverAllPorts();
};

namespace {

// The baseline pair every rulesConflict case starts from: two enabled Inbound TCP rules on local
// port 445 in the Domain profile, differing only in action. Every selector test below mutates ONE
// field of a copy, so the mutated field is the only thing that can change the verdict.
FirewallRule conflictRule(FirewallRule::Action action) {
    FirewallRule rule;
    rule.enabled = true;
    rule.direction = FirewallRule::Direction::Inbound;
    rule.action = action;
    rule.protocol = FirewallRule::Protocol::TCP;
    rule.localPorts = QStringLiteral("445");
    rule.profiles = static_cast<int>(FirewallRule::Profile::Domain);
    return rule;
}

}  // namespace

void TestFirewallRuleAuditor::construction_default() {
    FirewallRuleAuditor auditor;
    QCOMPARE(auditor.parent(), static_cast<QObject*>(nullptr));
    // The dynamic_cast that stood here was a static upcast (firewall_rule_auditor.h:22
    // publicly derives from QObject), true of ANY ctor body. What is load-bearing is that
    // the explicit ctor (firewall_rule_auditor.h:26) actually FORWARDS parent to QObject
    // (firewall_rule_auditor.cpp:407 `: QObject(parent)`) -- that parent edge is the only
    // owner a heap-allocating caller gets, so dropping it silently leaks every auditor.
    QObject owner;
    FirewallRuleAuditor child(&owner);
    QCOMPARE(child.parent(), &owner);
    QVERIFY(owner.children().contains(&child));
}

void TestFirewallRuleAuditor::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<FirewallRuleAuditor>);
    QVERIFY(!std::is_move_constructible_v<FirewallRuleAuditor>);
}

void TestFirewallRuleAuditor::findRulesByPort_emptyRules() {
    FirewallRuleAuditor auditor;
    // Pre-enumeration state: nothing has been gathered, so nothing can match.
    QVERIFY(auditor.findRulesByPort(80, FirewallRule::Direction::Inbound).isEmpty());

    // The empty-fixture check above is satisfied by the fixture itself, so pin BOTH
    // of findRulesByPort's selectors (direction, and the localPortsCoverPort
    // coverage test) against a real enumerated rule set. rulesEnumerated hands back
    // the full set, so the expected match count is computed independently of
    // findRulesByPort's own loop.
    QSignalSpy rules_spy(&auditor, &FirewallRuleAuditor::rulesEnumerated);
    auditor.enumerateRules();
    QCOMPARE(rules_spy.count(), 1);
    const auto all_rules = rules_spy.takeFirst().at(0).value<QVector<FirewallRule>>();
    QVERIFY2(!all_rules.isEmpty(), "Windows Firewall should have rules on any system");

    constexpr uint16_t kProbePort = 445;
    const auto expectedCount = [&all_rules](FirewallRule::Direction direction) {
        qsizetype count = 0;
        for (const auto& rule : all_rules) {
            if (rule.direction == direction &&
                FirewallRuleAuditor::localPortsCoverPort(rule.localPorts, kProbePort)) {
                ++count;
            }
        }
        return count;
    };
    const qsizetype inbound_expected = expectedCount(FirewallRule::Direction::Inbound);
    const qsizetype outbound_expected = expectedCount(FirewallRule::Direction::Outbound);
    // An empty LocalPorts covers every port, so a live host always has port-445
    // rules; require it so the checks below can never pass vacuously.
    QVERIFY(inbound_expected + outbound_expected > 0);

    const auto inbound = auditor.findRulesByPort(kProbePort, FirewallRule::Direction::Inbound);
    for (const auto& rule : inbound) {
        // Drop the direction guard and outbound rules leak into an inbound query.
        QCOMPARE(rule.direction, FirewallRule::Direction::Inbound);
        QVERIFY2(FirewallRuleAuditor::localPortsCoverPort(rule.localPorts, kProbePort),
                 qPrintable(rule.name + QStringLiteral(" / ") + rule.localPorts));
    }
    // ... and no rule that should match is dropped.
    QCOMPARE(inbound.size(), inbound_expected);

    const auto outbound = auditor.findRulesByPort(kProbePort, FirewallRule::Direction::Outbound);
    for (const auto& rule : outbound) {
        QCOMPARE(rule.direction, FirewallRule::Direction::Outbound);
        QVERIFY2(FirewallRuleAuditor::localPortsCoverPort(rule.localPorts, kProbePort),
                 qPrintable(rule.name + QStringLiteral(" / ") + rule.localPorts));
    }
    QCOMPARE(outbound.size(), outbound_expected);
}

void TestFirewallRuleAuditor::findRulesByApplication_emptyRules() {
    FirewallRuleAuditor auditor;
    const auto rules = auditor.findRulesByApplication(QStringLiteral("C:\\test.exe"));
    QVERIFY(rules.isEmpty());
}

void TestFirewallRuleAuditor::findRulesByApplication_filtersOnApplicationPathNotName() {
    // findRulesByApplication must filter on the rule's APPLICATION PATH, not on its name:
    // "which rules govern this program" is answered from the path the rule actually binds to.
    // The empty-fixture test above never runs the filter body, so pin the field here against a
    // live enumeration.
    FirewallRuleAuditor auditor;
    QSignalSpy rules_spy(&auditor, &FirewallRuleAuditor::rulesEnumerated);
    auditor.enumerateRules();
    QCOMPARE(rules_spy.count(), 1);
    const auto all_rules = rules_spy.takeFirst().at(0).value<QVector<FirewallRule>>();
    QVERIFY2(!all_rules.isEmpty(), "Windows Firewall should have rules on any system");

    // Probe: a real rule that HAS an application path whose text does not also appear in its
    // name, so a filter reading the wrong field cannot match it by accident.
    FirewallRule probe;
    for (const auto& rule : all_rules) {
        if (!rule.applicationPath.isEmpty() &&
            !rule.name.contains(rule.applicationPath, Qt::CaseInsensitive)) {
            probe = rule;
            break;
        }
    }
    QVERIFY2(!probe.applicationPath.isEmpty(),
             "Expected at least one firewall rule bound to an application path");

    const auto results = auditor.findRulesByApplication(probe.applicationPath);
    bool probe_returned = false;
    for (const auto& rule : results) {
        if (rule.name == probe.name && rule.applicationPath == probe.applicationPath) {
            probe_returned = true;
        }
        // Every hit really carries that path -- not merely that name.
        QVERIFY2(rule.applicationPath.contains(probe.applicationPath, Qt::CaseInsensitive),
                 qPrintable(rule.name + QStringLiteral(" -> ") + rule.applicationPath));
    }
    QVERIFY2(probe_returned, qPrintable(probe.applicationPath));

    // An application path no rule can carry returns nothing, proving the argument is applied at
    // all (a filter that ignored it would return the whole non-empty set).
    QVERIFY(auditor.findRulesByApplication(QStringLiteral("C:\\ZZZ_NoSuchApp_9999\\nope.exe"))
                .isEmpty());
}

void TestFirewallRuleAuditor::findRulesByName_emptyRules() {
    FirewallRuleAuditor auditor;
    const auto rules = auditor.findRulesByName(QStringLiteral("test_rule"));
    QVERIFY(rules.isEmpty());
}

void TestFirewallRuleAuditor::cancel_doesNotWedgeTheAuditor() {
    FirewallRuleAuditor auditor;
    QSignalSpy rules_spy(&auditor, &FirewallRuleAuditor::rulesEnumerated);

    // cancel() only RAISES the cooperative cancel flag; it is every public
    // operation's job to clear it before running (firewall_rule_auditor.cpp:414
    // for enumerateRules). Pin that contract: a cancel with no operation in
    // flight must not WEDGE the auditor, so the next enumeration still completes
    // and emits. Without the reset, the flag survives into enumerateViaCOM, which
    // breaks out at :635 and marks m_enumerationOk=false at :660, and the emit
    // gate at :420 then suppresses rulesEnumerated permanently.
    auditor.cancel();
    auditor.enumerateRules();

    QCOMPARE(rules_spy.count(), 1);
    const auto rules = rules_spy.takeFirst().at(0).value<QVector<FirewallRule>>();
    QVERIFY2(!rules.isEmpty(), "a post-cancel enumeration must still return the host's rules");
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
    // description and applicationPath were assigned and never read back (FINDING N8 -- cppcheck
    // could not see this file at all until the gate was fixed). applicationPath in particular is
    // the selector findRulesByApplication filters on, so an unread field here left the
    // assignment itself unproven.
    QCOMPARE(rule.description, QStringLiteral("Unit test rule"));
    QCOMPARE(rule.applicationPath, QStringLiteral("C:\\test.exe"));
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

    // The signal must carry POPULATED rules, not N default-constructed ones. A default
    // FirewallRule is disabled/Inbound/Allow/Any (network_diagnostic_types.h:379-401), so a
    // COM population path that stopped writing those fields (firewall_rule_auditor.cpp:295)
    // would still hand us a large, plausible-looking vector. Existence claims only -- a live
    // Windows host always ships enabled rules, outbound rules, TCP/UDP rules, and every
    // Windows Firewall rule carries a Name -- so this cannot be host-fragile.
    bool any_named = false;
    bool any_enabled = false;
    bool any_outbound = false;
    bool any_concrete_protocol = false;
    for (const auto& rule : rules) {
        any_named = any_named || !rule.name.isEmpty();
        any_enabled = any_enabled || rule.enabled;
        any_outbound = any_outbound || (rule.direction == FirewallRule::Direction::Outbound);
        any_concrete_protocol = any_concrete_protocol ||
                                (rule.protocol == FirewallRule::Protocol::TCP ||
                                 rule.protocol == FirewallRule::Protocol::UDP);
    }
    QVERIFY2(any_named, "every enumerated rule had an empty name (identity never populated)");
    QVERIFY2(any_enabled, "no enumerated rule was enabled (Enabled never populated)");
    QVERIFY2(any_outbound, "no enumerated rule was Outbound (Direction never populated)");
    QVERIFY2(any_concrete_protocol,
             "no enumerated rule had a TCP/UDP protocol (Protocol never populated)");
}

void TestFirewallRuleAuditor::fullAudit_emitsAuditComplete() {
    FirewallRuleAuditor auditor;
    QSignalSpy audit_spy(&auditor, &FirewallRuleAuditor::auditComplete);

    auditor.fullAudit();

    QCOMPARE(audit_spy.count(), 1);

    // The count alone is satisfied by auditComplete({}, {}, {}) -- exactly the
    // fail-open "clean 0-rule audit" the cancellation/enumeration guards above
    // the emit exist to prevent -- so pin the payload the controller consumes.
    const auto payload = audit_spy.takeFirst();
    const auto rules = payload.at(0).value<QVector<FirewallRule>>();
    const auto conflicts = payload.at(1).value<QVector<FirewallConflict>>();
    const auto gaps = payload.at(2).value<QVector<FirewallGap>>();

    // fullAudit reaches the emit only on a successful enumeration, so the rule
    // set it reports must be the enumerated one, not an empty stand-in.
    QVERIFY2(!rules.isEmpty(), "fullAudit must report the enumerated rules, not an empty set");

    // conflicts/gaps may legitimately be empty on a given host, so pin their
    // CONTENT contract instead of a count: every reported conflict must really
    // be a conflict under the pure seam (findConflicts appends only when
    // rulesConflict() holds for the very pair it stores in ruleA/ruleB), and
    // every reported gap must carry the description + recommendation the UI
    // renders.
    for (const auto& conflict : conflicts) {
        QVERIFY2(FirewallRuleAuditor::rulesConflict(conflict.ruleA, conflict.ruleB),
                 qPrintable(conflict.conflictDescription));
        QVERIFY(!conflict.conflictDescription.isEmpty());
    }
    for (const auto& gap : gaps) {
        QVERIFY(!gap.description.isEmpty());
        QVERIFY(!gap.recommendation.isEmpty());
    }
}

void TestFirewallRuleAuditor::findRules_afterEnumeration() {
    FirewallRuleAuditor auditor;
    QSignalSpy rules_spy(&auditor, &FirewallRuleAuditor::rulesEnumerated);
    auditor.enumerateRules();
    QCOMPARE(rules_spy.count(), 1);
    const auto enumerated = rules_spy.takeFirst().at(0).value<QVector<FirewallRule>>();
    QVERIFY2(!enumerated.isEmpty(), "Windows Firewall should have rules on any system");

    // findRulesByName is a case-insensitive SUBSTRING filter over the enumerated
    // rules. Pin the exact contract, not a floor that two EMPTY vectors satisfy.

    // (1) An empty filter matches every rule -> the EXACT full set, not "a subset".
    const auto all_rules = auditor.findRulesByName(QString());
    QCOMPARE(all_rules.size(), enumerated.size());

    // (2) Substring, not prefix; case-insensitive, not case-sensitive. Derive a
    //     strict infix of a REAL rule name (first and last characters dropped)
    //     and flip its case, so only contains(..., Qt::CaseInsensitive) matches.
    QString owner;
    QString flipped;
    for (const auto& rule : enumerated) {
        if (rule.name.size() < 4) {
            continue;
        }
        const QString infix = rule.name.mid(1, rule.name.size() - 2);
        const QString candidate = (infix == infix.toUpper()) ? infix.toLower() : infix.toUpper();
        // Require a real case flip, an infix that still matches case-insensitively
        // (guards exotic case mappings), and an owner that does NOT start with it
        // (so a startsWith() implementation cannot match the owner by accident).
        if (candidate == infix || !rule.name.contains(candidate, Qt::CaseInsensitive) ||
            rule.name.startsWith(candidate, Qt::CaseInsensitive)) {
            continue;
        }
        owner = rule.name;
        flipped = candidate;
        break;
    }
    QVERIFY2(!flipped.isEmpty(), "expected a firewall rule name with a cased interior substring");

    const auto infix_results = auditor.findRulesByName(flipped);
    QVERIFY2(!infix_results.isEmpty(), qPrintable(flipped));
    QVERIFY(infix_results.size() <= all_rules.size());
    bool found_owner = false;
    for (const auto& rule : infix_results) {
        QVERIFY2(rule.name.contains(flipped, Qt::CaseInsensitive), qPrintable(rule.name));
        found_owner = found_owner || rule.name == owner;
    }
    QVERIFY2(found_owner, qPrintable(owner));

    // (3) An impossible filter returns nothing, proving the argument is applied.
    QVERIFY(auditor.findRulesByName(QStringLiteral("ZZZ_NoSuchRule_9999___")).isEmpty());
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
    // A MIXED named/numeric expression parses to a numeric subset that drops the
    // named token: "80,RPC" -> [80] and "443,RPC" -> [443] look disjoint, but the
    // shared RPC scope means they are NOT provably disjoint, so they must overlap.
    // Previously the fail-safe only fired when ALL tokens were unknown, so this
    // real conflict was silently missed (finding 2).
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80,RPC"), QStringLiteral("443")));
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80,RPC"), QStringLiteral("443,RPC")));
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("443"), QStringLiteral("80,RPC")));
    // Purely numeric mixed expressions with no shared port are still disjoint.
    QVERIFY(!FirewallRuleAuditor::portsOverlap(QStringLiteral("80,81"), QStringLiteral("443,444")));

    // A garbled/hostile range must be REJECTED WHOLESALE by tryParsePortRange, never handed to
    // markPortRange -- its seen.set(p) on a 65536-bit bitset throws std::out_of_range out of
    // parsePorts. An out-of-range end therefore reads as an unknown token: portsOverlap fails
    // SAFE to "overlap", and the gap check does not treat the bogus span as covering a port
    // inside it (firewall_rule_auditor.cpp:333).
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80-70000"), QStringLiteral("443")));
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("80-70000"), 90));
    // A REVERSED range is an unknown token too, not a "valid" range that expands to nothing:
    // otherwise "100-80,443" would parse to [443] and look provably disjoint from "80", hiding
    // a conflict.
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("100-80,443"), QStringLiteral("80")));

    // The kMaxPortTokens (256) fail-safe (src/core/firewall_rule_auditor.cpp:388-390)
    // bounds the O(n^2) rule-pair scan: an expression with more comma-separated tokens
    // than the cap is NOT expanded, and must fail SAFE to "overlap" instead of being
    // proven disjoint (B9-11). Build numerically DISJOINT all-numeric sets so only the
    // cap -- never a shared port -- can produce an overlap here.
    const auto makePortList = [](int first, int count) {
        QString expr = QString::number(first);
        for (int i = 1; i < count; ++i) {
            expr += QLatin1Char(',');
            expr += QString::number(first + i);
        }
        return expr;
    };
    const QString over_cap = makePortList(1, 300);   // 299 commas: over the 256-token cap
    const QString under_cap = makePortList(1, 200);  // 199 commas: under the cap
    QVERIFY(over_cap.count(QLatin1Char(',')) >= 256);
    QVERIFY(under_cap.count(QLatin1Char(',')) < 256);
    // Over-cap on EITHER side fails safe to overlap (pins both arms of the guard) ...
    QVERIFY(FirewallRuleAuditor::portsOverlap(over_cap, QStringLiteral("1000")));
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("1000"), over_cap));
    // ... while an under-cap expression is still expanded and proven disjoint, so the
    // cap is a real boundary and not a blanket "long expressions overlap".
    QVERIFY(!FirewallRuleAuditor::portsOverlap(under_cap, QStringLiteral("1000")));
}

void TestFirewallRuleAuditor::rulesConflict_requiresAllSelectorsToOverlap() {
    const FirewallRule allow = conflictRule(FirewallRule::Action::Allow);
    const FirewallRule block = conflictRule(FirewallRule::Action::Block);

    // Enabled, same direction, Allow vs Block, overlapping local ports -> conflict.
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, block));
}

void TestFirewallRuleAuditor::rulesConflict_profileAndProtocolArms() {
    const FirewallRule allow = conflictRule(FirewallRule::Action::Allow);
    const FirewallRule block = conflictRule(FirewallRule::Action::Block);

    // The selector guard has TWO arms (firewall_rule_auditor.cpp:730); feed each a
    // DISJOINT pair so deleting either arm is caught, plus the conservative
    // directions that must never HIDE a real conflict.

    // profiles: Domain(1) and Public(4) never apply to the same connection.
    FirewallRule blockPublic = block;
    blockPublic.profiles = static_cast<int>(FirewallRule::Profile::Public);
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, blockPublic));
    // ...but any shared bit in the mask still conflicts (overlap is bitwise, not equality).
    blockPublic.profiles = static_cast<int>(FirewallRule::Profile::Public) |
                           static_cast<int>(FirewallRule::Profile::Domain);
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, blockPublic));
    // profiles == 0 means the mask was NOT read, so it must conservatively overlap
    // everything rather than fail open and hide the conflict.
    FirewallRule blockUnknownProfile = block;
    blockUnknownProfile.profiles = 0;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, blockUnknownProfile));

    // protocol: TCP vs UDP is disjoint traffic.
    FirewallRule blockUdp = block;
    blockUdp.protocol = FirewallRule::Protocol::UDP;
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, blockUdp));
    // Protocol::Any matches every protocol (conservative, never hides a conflict).
    blockUdp.protocol = FirewallRule::Protocol::Any;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, blockUdp));
    // Two Other rules cannot be proven distinct, so they must still conflict
    // (over-report is fail-safe; Other != Other would hide a real GRE-vs-GRE clash).
    FirewallRule allowOther = allow;
    allowOther.protocol = FirewallRule::Protocol::Other;
    FirewallRule blockOther = block;
    blockOther.protocol = FirewallRule::Protocol::Other;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowOther, blockOther));

    // Same action is not a conflict -- in BOTH halves, Allow/Allow and Block/Block.
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, allow));
    QVERIFY(!FirewallRuleAuditor::rulesConflict(block, block));

    // Opposite direction is its OWN refusal reason: an Outbound Block and an
    // Inbound Allow never apply to the same traffic. The pair below is identical
    // to the conflicting allow/block pair above except for direction, so this
    // isolates the `a.direction != b.direction` arm of the guard.
    FirewallRule outboundBlock = block;
    outboundBlock.direction = FirewallRule::Direction::Outbound;
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, outboundBlock));
    FirewallRule inboundBlock = outboundBlock;
    inboundBlock.direction = FirewallRule::Direction::Inbound;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, inboundBlock));
    FirewallRule disabled = block;
    disabled.enabled = false;
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, disabled));
    QVERIFY(!FirewallRuleAuditor::rulesConflict(disabled, allow));
    // The LOCAL arm of the same port guard must refuse too: an Allow on local 445
    // and a Block on local 80 never apply to the same traffic. Without this, the
    // localPorts half of the conjunction at firewall_rule_auditor.cpp:736 is only
    // ever exercised positively (445 vs 445) and could be deleted unnoticed.
    FirewallRule blockOtherLocalPort = block;
    blockOtherLocalPort.localPorts = QStringLiteral("80");
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, blockOtherLocalPort));
}

void TestFirewallRuleAuditor::rulesConflict_scopeSelectorsNarrowTheMatch() {
    const FirewallRule allow = conflictRule(FirewallRule::Action::Allow);
    const FirewallRule block = conflictRule(FirewallRule::Action::Block);

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

    // B9-11: rules bound to DIFFERENT programs scope to disjoint traffic -> not a
    // conflict; this pins the applicationPathsMatch arm of the final conjunction,
    // which every other fixture leaves empty (and so short-circuits true).
    FirewallRule allowApp = allow;
    allowApp.applicationPath = QStringLiteral("C:\\Program Files\\App\\a.exe");
    FirewallRule blockApp = block;
    blockApp.applicationPath = QStringLiteral("C:\\Program Files\\App\\b.exe");
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allowApp, blockApp));

    // The SAME program still conflicts, and Windows paths compare case-insensitively.
    blockApp.applicationPath = QStringLiteral("c:\\program files\\app\\A.EXE");
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowApp, blockApp));

    // An empty applicationPath means "any program" and matches a bound one.
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowApp, block));
}

void TestFirewallRuleAuditor::localPortsCoverPort_emptyAndWildcardCoverAllPorts() {
    // B9-12: an empty LocalPorts is the Windows default "no port restriction" ==
    // ALL ports, so the RDP/SMB gap checks must treat it as a wildcard. Previously
    // an all-ports allow rule (empty LocalPorts) slipped past the gap check.
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QString(), 3389));
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("*"), 445));
    // Concrete ports: covered iff the parsed set contains the port.
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("3389"), 3389));
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("440-450"), 445));
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("80,443"), 3389));
    // A named-service expression that parses to no ports does NOT cover 445 here
    // (gap analysis needs the specific port present, unlike conflict overlap).
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("RPC"), 445));

    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("440-450"), 440));
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("440-450"), 445));
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("440-450"), 450));
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("440-450"), 439));
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("440-450"), 451));
    // A degenerate single-port range still covers its one port.
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("445-445"), 445));

    // B9-12: past the kMaxPortTokens cap localPortsCoverPort fails SAFE toward COVERAGE (the
    // opposite direction from portsOverlap): a pathological expression is assumed to cover the
    // port so the RDP/SMB gap is still flagged, rather than being expanded into a DoS. Pinned at
    // the exact boundary -- 256 commas trips the cap even though the parsed set is only {80}.
    QVERIFY(FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("80,").repeated(256), 445));
    // One token under the cap the expression IS expanded, so the same shape does NOT cover 445 --
    // proving the true above comes from the cap and not merely from a long string.
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("80,").repeated(255), 445));
}

QTEST_MAIN(TestFirewallRuleAuditor)
#include "test_firewall_rule_auditor.moc"
