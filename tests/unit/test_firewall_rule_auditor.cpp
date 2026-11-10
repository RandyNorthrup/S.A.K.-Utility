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

// findRulesByApplication is a case-insensitive SUBSTRING filter, not an exact or prefix match:
// "which rules govern chrome.exe" must find the rule bound to its full path. Every other probe in
// this file queries with a WHOLE path, under which contains(), startsWith() and an exact
// compare() == 0 are indistinguishable, so the substring contract was unpinned. Derive a strict
// INFIX of a real application path (first and last characters dropped) and flip its case: only a
// case-insensitive contains() matches it.
void verifyApplicationFilterIsCaseInsensitiveSubstring(const FirewallRuleAuditor& auditor,
                                                       const QVector<FirewallRule>& all_rules) {
    QString path_owner;
    QString path_infix;
    for (const auto& rule : all_rules) {
        if (rule.applicationPath.size() < 4) {
            continue;
        }
        const QString infix = rule.applicationPath.mid(1, rule.applicationPath.size() - 2);
        const QString candidate = (infix == infix.toUpper()) ? infix.toLower() : infix.toUpper();
        // Require a real case flip, an infix that still matches case-insensitively (guards
        // against exotic case mappings), and an owner that does NOT start with it, so neither a
        // case-sensitive contains(), a startsWith(), nor an exact compare() can match by accident.
        if (candidate == infix || !rule.applicationPath.contains(candidate, Qt::CaseInsensitive) ||
            rule.applicationPath.startsWith(candidate, Qt::CaseInsensitive)) {
            continue;
        }
        path_owner = rule.applicationPath;
        path_infix = candidate;
        break;
    }
    QVERIFY2(!path_infix.isEmpty(),
             "expected a firewall rule application path with a cased interior substring");

    const auto infix_results = auditor.findRulesByApplication(path_infix);
    // Narrowing the filter to an exact (or prefix) match empties this -- a fail-open
    // "no rule governs this program".
    QVERIFY2(!infix_results.isEmpty(), qPrintable(path_infix));
    bool infix_owner_found = false;
    for (const auto& rule : infix_results) {
        QVERIFY2(rule.applicationPath.contains(path_infix, Qt::CaseInsensitive),
                 qPrintable(rule.name + QStringLiteral(" -> ") + rule.applicationPath));
        infix_owner_found = infix_owner_found || rule.applicationPath == path_owner;
    }
    QVERIFY2(infix_owner_found, qPrintable(path_owner));
}

// rule.name must come from the NAME getter, not from a sibling block in the same populate helper.
// A mis-read (grouping or description into name) leaves name equal to that sibling field for
// EVERY rule, which a per-rule non-empty check cannot see. Windows always ships a rule whose name
// differs from all of them (e.g. "Core Networking - DNS (UDP-Out)" under "Core Networking").
void verifyRuleNamesAreTheirOwnField(const QVector<FirewallRule>& rules) {
    // The file's stated invariant is UNIVERSAL, so pin it per-rule: an OR-fold over hundreds of
    // rules is satisfied by a single named one, which is exactly where a wrong-getter bug hides.
    for (const auto& rule : rules) {
        QVERIFY2(!rule.name.isEmpty(),
                 qPrintable(QStringLiteral("unnamed rule (grouping=%1, app=%2, ports=%3)")
                                .arg(rule.grouping, rule.applicationPath, rule.localPorts)));
    }
    bool name_is_its_own_field = false;
    for (const auto& rule : rules) {
        if (!rule.name.isEmpty() && rule.name != rule.grouping && rule.name != rule.description &&
            rule.name != rule.applicationPath && rule.name != rule.serviceName &&
            rule.name != rule.localPorts && rule.name != rule.remotePorts) {
            name_is_its_own_field = true;
            break;
        }
    }
    QVERIFY2(name_is_its_own_field,
             "rule.name aliased a sibling COM field on every rule (wrong getter in "
             "populateRuleIdentity)");
}

// Membership alone ("everything reported really is a conflict") is VACUOUS when the vector is
// empty, so that loop degrades to a no-op the moment findConflicts stops appending -- bound its
// inner pair loop to nothing and the test still passes. Recompute the expected set: findConflicts
// is the full i<j pair scan over the very rule set the signal carries, and reaching the emit at
// all proves the cancel break never fired, so the counts must agree exactly. This pins
// COMPLETENESS -- that no genuine conflict is DROPPED, the half a security audit lives on. It
// deliberately does NOT pin rulesConflict itself: both sides call it, so a broken predicate moves
// them together. What it catches is a truncated, mis-indexed or short-circuited pair scan, which
// the predicate cannot mask.
void verifyReportedConflictsAreCompleteAndReal(const QVector<FirewallRule>& rules,
                                               const QVector<FirewallConflict>& conflicts) {
    qsizetype expected_conflicts = 0;
    for (qsizetype i = 0; i < rules.size(); ++i) {
        for (qsizetype j = i + 1; j < rules.size(); ++j) {
            if (FirewallRuleAuditor::rulesConflict(rules.at(i), rules.at(j))) {
                ++expected_conflicts;
            }
        }
    }
    QCOMPARE(conflicts.size(), expected_conflicts);

    for (const auto& conflict : conflicts) {
        QVERIFY2(FirewallRuleAuditor::rulesConflict(conflict.ruleA, conflict.ruleB),
                 qPrintable(conflict.conflictDescription));
        QVERIFY(!conflict.conflictDescription.isEmpty());
    }
}

// The gap loop was vacuous in the same way: gutting findGaps to `return {}` would silently
// disable every gap check and still pass. Three of the five checks are threshold-free, so their
// verdict is FULLY DETERMINED by the same rule set the signal carries -- derive each expected
// answer here and pin it in BOTH directions, host-independently. Unlike the conflict count this
// IS an independent oracle: the gap conditions are re-derived from the rule fields rather than by
// calling the checks. The wildcard and disabled-block gaps are deliberately excluded: their
// verdicts turn on private tuning thresholds this test must not mirror.
struct DeterministicGapExpectations {
    bool has_icmp_block = false;   // checkIcmpGap: gap UNLESS this holds
    bool rdp_open_to_all = false;  // checkRdpGap:  gap IFF this holds
    bool smb_on_public = false;    // checkSmbGap:  gap IFF this holds
};

bool isEnabledInboundAllow(const FirewallRule& rule) {
    return rule.enabled && rule.action == FirewallRule::Action::Allow &&
           rule.direction == FirewallRule::Direction::Inbound;
}

bool isOpenToEveryAddress(const FirewallRule& rule) {
    return rule.remoteAddresses.isEmpty() || rule.remoteAddresses == QStringLiteral("*");
}

DeterministicGapExpectations deriveGapExpectations(const QVector<FirewallRule>& rules) {
    DeterministicGapExpectations expected;
    for (const auto& rule : rules) {
        if (rule.enabled && rule.protocol == FirewallRule::Protocol::ICMPv4 &&
            rule.action == FirewallRule::Action::Block) {
            expected.has_icmp_block = true;
        }
        if (!isEnabledInboundAllow(rule)) {
            continue;
        }
        if (FirewallRuleAuditor::localPortsCoverPort(rule.localPorts, 3389) &&
            isOpenToEveryAddress(rule)) {
            expected.rdp_open_to_all = true;
        }
        if (FirewallRuleAuditor::localPortsCoverPort(rule.localPorts, 445) &&
            (rule.profiles & static_cast<int>(FirewallRule::Profile::Public)) != 0) {
            expected.smb_on_public = true;
        }
    }
    return expected;
}

void verifyDeterministicGapsMatchTheRuleSet(const QVector<FirewallRule>& rules,
                                            const QVector<FirewallGap>& gaps) {
    const auto gapReported = [&gaps](const QString& description) {
        for (const auto& gap : gaps) {
            if (gap.description == description) {
                return true;
            }
        }
        return false;
    };

    const DeterministicGapExpectations expected = deriveGapExpectations(rules);
    const bool has_icmp_block = expected.has_icmp_block;
    const bool rdp_open_to_all = expected.rdp_open_to_all;
    const bool smb_on_public = expected.smb_on_public;

    const QString kIcmpGap = QStringLiteral("No explicit ICMP block rules found");
    const QString kRdpGap = QStringLiteral("RDP (port 3389) is open to all addresses");
    const QString kSmbGap = QStringLiteral("SMB (port 445) is allowed on Public profile");
    QCOMPARE(gapReported(kIcmpGap), !has_icmp_block);
    QCOMPARE(gapReported(kRdpGap), rdp_open_to_all);
    QCOMPARE(gapReported(kSmbGap), smb_on_public);

    // Every reported gap carries the description + recommendation the UI renders, and the
    // deterministic literals above must be the exact ones production emits.
    for (const auto& gap : gaps) {
        QVERIFY(!gap.description.isEmpty());
        QVERIFY(!gap.recommendation.isEmpty());
        if (gap.description == kIcmpGap) {
            QCOMPARE(gap.recommendation,
                     QStringLiteral(
                         "Consider adding ICMP rate limiting on public-facing networks"));
            QCOMPARE(gap.severity, FirewallGap::Severity::Info);
        } else if (gap.description == kRdpGap) {
            QCOMPARE(gap.recommendation,
                     QStringLiteral("Restrict RDP access to specific IP ranges or use a VPN"));
            QCOMPARE(gap.severity, FirewallGap::Severity::Warning);
        } else if (gap.description == kSmbGap) {
            QCOMPARE(gap.recommendation,
                     QStringLiteral(
                         "Disable SMB on Public networks to prevent lateral movement attacks"));
            QCOMPARE(gap.severity, FirewallGap::Severity::Warning);
        }
    }
}

// tryParsePortRange refuses on FOUR independent conditions, and the two probes in the caller
// both land on the SAME bounds guard -- the other two are unreached by any fixture in this
// file, so either could be deleted and the malformed span would then parse as a REAL range
// whose concrete port set looks provably disjoint, hiding a conflict.
void verifyMalformedPortRangesAreRefusedWholesale() {
    // Extra dashes are not a range: without the part-count guard "80-90-100" reads as 80-90.
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("80-90-100"), QStringLiteral("443")));
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("80-90-100"), 85));
    // A MISSING start is not port 0: QString::toInt leaves 0 behind on failure, so without the
    // parse-success arm "-80" would clear the bounds check as the range 0-80.
    QVERIFY(FirewallRuleAuditor::portsOverlap(QStringLiteral("-80"), QStringLiteral("443")));
    QVERIFY(!FirewallRuleAuditor::localPortsCoverPort(QStringLiteral("-80"), 0));
}

// The profile and protocol selector arms, each fed a DISJOINT pair so deleting either arm is
// caught, plus the conservative directions that must never HIDE a real conflict.
void verifyProfileAndProtocolOverlapArms(const FirewallRule& allow, const FirewallRule& block) {
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
    // The OTHER arm of that zero guard. Every fixture here is built from conflictRule(), which
    // always sets profiles = Domain, so the LEFT-hand mask is never 0 and its arm could be
    // deleted unnoticed. An unread mask must overlap from EITHER side -- a rule whose
    // get_Profiles failed leaves profiles at its 0 default and would otherwise vanish from the
    // conflict scan whenever it happened to sort first.
    QVERIFY(FirewallRuleAuditor::rulesConflict(blockUnknownProfile, allow));

    // protocol: TCP vs UDP is disjoint traffic.
    FirewallRule blockUdp = block;
    blockUdp.protocol = FirewallRule::Protocol::UDP;
    QVERIFY(!FirewallRuleAuditor::rulesConflict(allow, blockUdp));
    // Protocol::Any matches every protocol (conservative, never hides a conflict).
    blockUdp.protocol = FirewallRule::Protocol::Any;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, blockUdp));
    // ...and the wildcard is SYMMETRIC. findConflicts pairs rules in raw enumeration order, so
    // an "Any" rule is just as often the FIRST argument; pinning only the right-hand arm lets
    // the left one be deleted in silence, dropping every Any-vs-TCP conflict where the Any rule
    // happens to sort first. conflictRule() hard-codes TCP, so no other fixture reaches it.
    FirewallRule allowAny = allow;
    allowAny.protocol = FirewallRule::Protocol::Any;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowAny, block));
    // Two Other rules cannot be proven distinct, so they must still conflict
    // (over-report is fail-safe; Other != Other would hide a real GRE-vs-GRE clash).
    FirewallRule allowOther = allow;
    allowOther.protocol = FirewallRule::Protocol::Other;
    FirewallRule blockOther = block;
    blockOther.protocol = FirewallRule::Protocol::Other;
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowOther, blockOther));
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

    verifyApplicationFilterIsCaseInsensitiveSubstring(auditor, all_rules);

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
    bool any_enabled = false;
    bool any_outbound = false;
    bool any_concrete_protocol = false;
    for (const auto& rule : rules) {
        any_enabled = any_enabled || rule.enabled;
        any_outbound = any_outbound || (rule.direction == FirewallRule::Direction::Outbound);
        any_concrete_protocol = any_concrete_protocol ||
                                (rule.protocol == FirewallRule::Protocol::TCP ||
                                 rule.protocol == FirewallRule::Protocol::UDP);
    }
    verifyRuleNamesAreTheirOwnField(rules);
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

    verifyReportedConflictsAreCompleteAndReal(rules, conflicts);
    verifyDeterministicGapsMatchTheRuleSet(rules, gaps);
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
    verifyMalformedPortRangesAreRefusedWholesale();

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

    verifyProfileAndProtocolOverlapArms(allow, block);

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

    // An EMPTY serviceName means "any service", so it must still conflict with a service-bound
    // rule -- one assertion per arm of that guard. Every fixture in this file makes the two
    // services either BOTH empty or BOTH set, so the arms are never separated and EITHER could
    // be deleted with every assertion still green: a svchost-bound rule would then silently
    // stop conflicting with an any-service rule.
    QVERIFY(FirewallRuleAuditor::rulesConflict(allowSvc, block));  // right-hand service empty
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, blockSvc));  // left-hand service empty

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
    // The MIRRORED arm: an EMPTY path on the LEFT must also match a bound one on the right.
    // findConflicts pairs rules in enumeration order and most Windows rules carry no
    // application path, so the unrestricted "any program" rule lands on the left half the time.
    // Every other fixture here leaves BOTH paths empty, which keeps the second arm true on its
    // own -- so without this the first arm can be deleted unnoticed.
    QVERIFY(FirewallRuleAuditor::rulesConflict(allow, blockApp));
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
