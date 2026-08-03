// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file firewall_rule_auditor.cpp
/// @brief Windows Firewall rule enumeration with conflict/gap analysis via COM

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/firewall_rule_auditor.h"

#include "sak/layout_constants.h"

#include <QSet>

#include <algorithm>
#include <optional>

#include <winsock2.h>

#include <windows.h>

#include <comdef.h>
#include <netfw.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace sak {

namespace {
constexpr int kMaxPortValue = 65'535;
constexpr long kProtocolTcp = 6;
constexpr long kProtocolUdp = 17;
constexpr long kProtocolIcmpV6 = 58;
constexpr long kProtocolAny = 256;  // Windows firewall "Any" protocol sentinel
constexpr int kPortRangePartCount = 2;
constexpr int kHResultHexWidth = 8;
constexpr uint16_t kRdpPort = 3389;
constexpr uint16_t kSmbPort = 445;
constexpr int kWildcardRuleWarningThreshold = 5;
constexpr int kDisabledBlockRuleWarningThreshold = 10;

/// @brief RAII wrapper for COM initialization
struct ComInitializer {
    HRESULT hr;
    ComInitializer() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInitializer() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr); }
};

/// @brief Convert BSTR to QString safely
[[nodiscard]] QString bstrToQString(BSTR bstr) {
    if (bstr == nullptr) {
        return {};
    }
    return QString::fromWCharArray(bstr, static_cast<int>(SysStringLen(bstr)));
}

template <typename T>
struct ComPtr {
    T* ptr = nullptr;
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    void reset(T* newPtr = nullptr) {
        if (ptr != nullptr) {
            ptr->Release();
        }
        ptr = newPtr;
    }

    [[nodiscard]] T* get() const { return ptr; }
    [[nodiscard]] T** put() {
        reset();
        return &ptr;
    }

    [[nodiscard]] explicit operator bool() const { return ptr != nullptr; }
    [[nodiscard]] T* operator->() const { return ptr; }
};

struct VariantHolder {
    VARIANT var;
    VariantHolder() { VariantInit(&var); }
    ~VariantHolder() { VariantClear(&var); }
    void reset() {
        VariantClear(&var);
        VariantInit(&var);
    }
};

void populateRuleIdentity(INetFwRule* pRule, FirewallRule& rule) {
    BSTR name = nullptr;
    if (SUCCEEDED(pRule->get_Name(&name))) {
        rule.name = bstrToQString(name);
        SysFreeString(name);
    }

    BSTR desc = nullptr;
    if (SUCCEEDED(pRule->get_Description(&desc))) {
        rule.description = bstrToQString(desc);
        SysFreeString(desc);
    }
}

FirewallRule::Protocol protocolFromNumber(long proto) {
    switch (proto) {
    case kProtocolTcp:
        return FirewallRule::Protocol::TCP;
    case kProtocolUdp:
        return FirewallRule::Protocol::UDP;
    case 1:
        return FirewallRule::Protocol::ICMPv4;
    case kProtocolIcmpV6:
        return FirewallRule::Protocol::ICMPv6;
    case kProtocolAny:
        return FirewallRule::Protocol::Any;
    default:
        // A specific but unmodeled protocol (e.g. GRE=47) is NOT "Any"; mapping it to Any made a
        // GRE rule falsely overlap every TCP/UDP rule, producing phantom conflicts.
        return FirewallRule::Protocol::Other;
    }
}

void populateRuleDirectionActionAndProtocol(INetFwRule* pRule, FirewallRule& rule) {
    Q_ASSERT(pRule);
    // Any getter that fails leaves a DEFAULT (Allow/Inbound/Any) that would
    // misrepresent the rule's posture in a security audit, so flag the rule as
    // incomplete rather than silently trusting the default (B9-10).
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (SUCCEEDED(pRule->get_Enabled(&enabled))) {
        rule.enabled = (enabled == VARIANT_TRUE);
    } else {
        rule.complete = false;
    }

    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
    if (SUCCEEDED(pRule->get_Direction(&dir))) {
        rule.direction = (dir == NET_FW_RULE_DIR_IN) ? FirewallRule::Direction::Inbound
                                                     : FirewallRule::Direction::Outbound;
    } else {
        rule.complete = false;
    }

    NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
    if (SUCCEEDED(pRule->get_Action(&action))) {
        rule.action = (action == NET_FW_ACTION_ALLOW) ? FirewallRule::Action::Allow
                                                      : FirewallRule::Action::Block;
    } else {
        rule.complete = false;
    }

    long proto = 0;
    if (SUCCEEDED(pRule->get_Protocol(&proto))) {
        rule.protocol = protocolFromNumber(proto);
    } else {
        rule.complete = false;
    }
}

void populateRulePortsAndAddresses(INetFwRule* pRule, FirewallRule& rule) {
    Q_ASSERT(pRule);
    // Ports and addresses define the rule's scope; a failed getter leaves it
    // empty, which the conflict/gap logic would read as a different scope, so a
    // failure flags the rule incomplete (B9-10).
    BSTR localPorts = nullptr;
    if (SUCCEEDED(pRule->get_LocalPorts(&localPorts))) {
        rule.localPorts = bstrToQString(localPorts);
        SysFreeString(localPorts);
    } else {
        rule.complete = false;
    }

    BSTR remotePorts = nullptr;
    if (SUCCEEDED(pRule->get_RemotePorts(&remotePorts))) {
        rule.remotePorts = bstrToQString(remotePorts);
        SysFreeString(remotePorts);
    } else {
        rule.complete = false;
    }

    BSTR localAddrs = nullptr;
    if (SUCCEEDED(pRule->get_LocalAddresses(&localAddrs))) {
        rule.localAddresses = bstrToQString(localAddrs);
        SysFreeString(localAddrs);
    } else {
        rule.complete = false;
    }

    BSTR remoteAddrs = nullptr;
    if (SUCCEEDED(pRule->get_RemoteAddresses(&remoteAddrs))) {
        rule.remoteAddresses = bstrToQString(remoteAddrs);
        SysFreeString(remoteAddrs);
    } else {
        rule.complete = false;
    }
}

void populateRuleAppAndProfileInfo(INetFwRule* pRule, FirewallRule& rule) {
    Q_ASSERT(pRule);
    // Application, service, and profile scope which program/service/network the
    // rule applies to; a failed getter leaves the scope unknown, so flag the rule
    // incomplete (B9-10). Grouping is a display label and is not flagged.
    BSTR appPath = nullptr;
    if (SUCCEEDED(pRule->get_ApplicationName(&appPath))) {
        rule.applicationPath = bstrToQString(appPath);
        SysFreeString(appPath);
    } else {
        rule.complete = false;
    }

    BSTR svcName = nullptr;
    if (SUCCEEDED(pRule->get_ServiceName(&svcName))) {
        rule.serviceName = bstrToQString(svcName);
        SysFreeString(svcName);
    } else {
        rule.complete = false;
    }

    long profiles = 0;
    if (SUCCEEDED(pRule->get_Profiles(&profiles))) {
        rule.profiles = static_cast<int>(profiles);
    } else {
        rule.complete = false;
    }

    BSTR grouping = nullptr;
    if (SUCCEEDED(pRule->get_Grouping(&grouping))) {
        rule.grouping = bstrToQString(grouping);
        SysFreeString(grouping);
    }
}

bool tryExtractFirewallRuleFromVariant(const VARIANT& var, FirewallRule& outRule) {
    if (var.vt != VT_DISPATCH || var.pdispVal == nullptr) {
        return false;
    }

    ComPtr<INetFwRule> pRule;
    const HRESULT hr = var.pdispVal->QueryInterface(__uuidof(INetFwRule),
                                                    reinterpret_cast<void**>(pRule.put()));
    if (FAILED(hr) || !pRule) {
        return false;
    }

    FirewallRule rule;
    populateRuleIdentity(pRule.get(), rule);
    populateRuleDirectionActionAndProtocol(pRule.get(), rule);
    populateRulePortsAndAddresses(pRule.get(), rule);
    populateRuleAppAndProfileInfo(pRule.get(), rule);
    outRule = rule;
    return true;
}

bool tryParsePortValue(const QString& s, uint16_t& port) {
    bool ok = false;
    const int value = s.trimmed().toInt(&ok);
    if (!ok || value < 0 || value > kMaxPortValue) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool tryParsePortRange(const QString& s, int& start, int& end) {
    Q_ASSERT(!s.isEmpty());
    if (!s.contains(QLatin1Char('-'))) {
        return false;
    }

    const auto parts = s.split(QLatin1Char('-'));
    if (parts.size() != kPortRangePartCount) {
        return false;
    }

    bool ok1 = false;
    bool ok2 = false;
    start = parts[0].trimmed().toInt(&ok1);
    end = parts[1].trimmed().toInt(&ok2);
    if (!ok1 || !ok2) {
        return false;
    }

    if (start < 0 || end > kMaxPortValue || start > end) {
        return false;
    }
    return true;
}

void appendPortRange(QVector<uint16_t>& ports, int start, int end) {
    for (int p = start; p <= end; ++p) {
        ports.append(static_cast<uint16_t>(p));
    }
}
}  // namespace

FirewallRuleAuditor::FirewallRuleAuditor(QObject* parent) : QObject(parent) {}

void FirewallRuleAuditor::cancel() {
    m_cancelled.store(true);
}

void FirewallRuleAuditor::enumerateRules() {
    m_cancelled.store(false);
    m_rules = enumerateViaCOM();

    // Do not emit a clean result over a failed enumeration: errorOccurred already
    // fired, and emitting an empty rulesEnumerated would let a broken audit read
    // as "no rules" (B9-09). An empty-but-successful enumeration still emits.
    if (!m_cancelled.load() && m_enumerationOk) {
        Q_EMIT rulesEnumerated(m_rules);
    }
}

void FirewallRuleAuditor::detectConflicts() {
    m_cancelled.store(false);
    auto conflicts = findConflicts(m_rules);
    Q_EMIT conflictsDetected(conflicts);
}

void FirewallRuleAuditor::analyzeGaps() {
    m_cancelled.store(false);
    auto gaps = findGaps(m_rules);
    Q_EMIT gapsAnalyzed(gaps);
}

void FirewallRuleAuditor::fullAudit() {
    m_cancelled.store(false);
    m_rules = enumerateViaCOM();

    if (m_cancelled.load()) {
        Q_EMIT auditComplete({}, {}, {});
        return;
    }
    if (!m_enumerationOk) {
        // Enumeration failed (errorOccurred already fired). Do NOT emit a clean
        // empty auditComplete -- a failed security audit must not look clean
        // (B9-09).
        return;
    }

    auto conflicts = findConflicts(m_rules);
    auto gaps = findGaps(m_rules);

    Q_EMIT auditComplete(m_rules, conflicts, gaps);
}

QVector<FirewallRule> FirewallRuleAuditor::findRulesByPort(
    uint16_t port, FirewallRule::Direction direction) const {
    QVector<FirewallRule> matching;

    for (const auto& rule : m_rules) {
        if (rule.direction != direction) {
            continue;
        }

        const auto ports = parsePorts(rule.localPorts);
        if (ports.contains(port) || rule.localPorts == QStringLiteral("*")) {
            matching.append(rule);
        }
    }
    return matching;
}

QVector<FirewallRule> FirewallRuleAuditor::findRulesByApplication(const QString& appPath) const {
    QVector<FirewallRule> matching;
    for (const auto& rule : m_rules) {
        if (rule.applicationPath.contains(appPath, Qt::CaseInsensitive)) {
            matching.append(rule);
        }
    }
    return matching;
}

QVector<FirewallRule> FirewallRuleAuditor::findRulesByName(const QString& nameFilter) const {
    QVector<FirewallRule> matching;
    for (const auto& rule : m_rules) {
        if (rule.name.contains(nameFilter, Qt::CaseInsensitive)) {
            matching.append(rule);
        }
    }
    return matching;
}

namespace {

std::optional<ComPtr<IEnumVARIANT>> initFirewallRuleEnumerator(ComInitializer& com,
                                                               QString& error_out) {
    if (!com.ok()) {
        error_out = QStringLiteral("Failed to initialize COM");
        return std::nullopt;
    }

    ComPtr<INetFwPolicy2> pPolicy;
    HRESULT hr = CoCreateInstance(__uuidof(NetFwPolicy2),
                                  nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(INetFwPolicy2),
                                  reinterpret_cast<void**>(pPolicy.put()));
    if (FAILED(hr) || !pPolicy) {
        error_out =
            QStringLiteral("Failed to access firewall policy (HRESULT 0x%1)")
                .arg(static_cast<unsigned long>(hr), kHResultHexWidth, kHexBase, QLatin1Char('0'));
        return std::nullopt;
    }

    ComPtr<INetFwRules> pRules;
    hr = pPolicy->get_Rules(pRules.put());
    if (FAILED(hr) || !pRules) {
        error_out = QStringLiteral("Failed to enumerate firewall rules");
        return std::nullopt;
    }

    ComPtr<IUnknown> pEnumerator;
    hr = pRules->get__NewEnum(pEnumerator.put());
    if (FAILED(hr) || !pEnumerator) {
        // Fail CLOSED: set error_out so enumerateViaCOM emits errorOccurred. Reporting an
        // empty rule set as a successful "clean" audit here would be a dangerous fail-open for
        // a security audit (mirrors the CoCreateInstance/get_Rules failures above, which also
        // set error_out).
        error_out =
            QStringLiteral("Failed to acquire firewall rule enumerator (HRESULT 0x%1)")
                .arg(static_cast<unsigned long>(hr), kHResultHexWidth, kHexBase, QLatin1Char('0'));
        return std::nullopt;
    }

    ComPtr<IEnumVARIANT> pVariant;
    hr = pEnumerator->QueryInterface(__uuidof(IEnumVARIANT),
                                     reinterpret_cast<void**>(pVariant.put()));
    if (FAILED(hr) || !pVariant) {
        error_out =
            QStringLiteral("Failed to acquire firewall rule iterator (HRESULT 0x%1)")
                .arg(static_cast<unsigned long>(hr), kHResultHexWidth, kHexBase, QLatin1Char('0'));
        return std::nullopt;
    }
    return pVariant;
}

}  // namespace

bool FirewallRuleAuditor::handleEnumerationBreak(long next_hr, int dropped) {
    // A clean end is SUCCEEDED (S_OK/S_FALSE) with fetched == 0. A FAILED Next means the
    // enumeration broke partway: fail CLOSED (return true -> caller returns empty) rather than
    // silently reporting a TRUNCATED rule set as a complete audit. A cooperative cancel leaves
    // next_hr SUCCEEDED and is handled by fullAudit's separate m_cancelled check.
    if (FAILED(next_hr)) {
        m_enumerationOk = false;
        Q_EMIT errorOccurred(
            QStringLiteral("Firewall rule enumeration failed partway (HRESULT 0x%1)")
                .arg(static_cast<unsigned long>(next_hr),
                     kHResultHexWidth,
                     kHexBase,
                     QLatin1Char('0')));
        return true;
    }
    // A dropped rule (COM object entirely unreadable) silently shrinks the audit, so fail
    // CLOSED exactly like a partway break: mark the enumeration not-OK so no clean result is
    // emitted over a truncated rule set (B9-12).
    if (dropped > 0) {
        m_enumerationOk = false;
        Q_EMIT errorOccurred(
            QStringLiteral("%1 firewall rule(s) could not be read at all and were dropped; the "
                           "audit is incomplete.")
                .arg(dropped));
        return true;
    }
    return false;
}

QVector<FirewallRule> FirewallRuleAuditor::enumerateViaCOM() {
    QVector<FirewallRule> rules;
    m_enumerationOk = true;

    ComInitializer com;
    QString error_msg;
    auto enumerator = initFirewallRuleEnumerator(com, error_msg);
    if (!enumerator) {
        m_enumerationOk = false;
        if (!error_msg.isEmpty()) {
            Q_EMIT errorOccurred(error_msg);
        }
        return rules;
    }

    ULONG fetched = 0;
    VariantHolder var;
    int dropped = 0;
    HRESULT next_hr = (*enumerator)->Next(1, &var.var, &fetched);
    while (SUCCEEDED(next_hr) && fetched > 0) {
        if (m_cancelled.load()) {
            break;
        }

        FirewallRule rule;
        if (tryExtractFirewallRuleFromVariant(var.var, rule)) {
            rules.append(rule);
        } else {
            // The COM object for this element could not be obtained at all
            // (non-dispatch variant or a failed QueryInterface to INetFwRule).
            // Unlike a partial-read rule it is not even present to be flagged --
            // it is DROPPED, silently shrinking the audit.
            ++dropped;
        }
        var.reset();
        next_hr = (*enumerator)->Next(1, &var.var, &fetched);
    }

    if (handleEnumerationBreak(next_hr, dropped)) {
        return {};
    }

    // Surface rules whose COM getters partially failed: their fields hold
    // defaults that may misrepresent the rule, so the audit must not present them
    // as fully-read fact (B9-10). This is a warning, not an enumeration failure,
    // so the (flagged) rules are still returned.
    const auto incomplete = std::count_if(rules.cbegin(), rules.cend(), [](const FirewallRule& r) {
        return !r.complete;
    });
    if (incomplete > 0) {
        Q_EMIT errorOccurred(QStringLiteral("%1 firewall rule(s) could not be fully read; their "
                                            "attributes may be incomplete.")
                                 .arg(incomplete));
    }

    return rules;
}

bool protocolsOverlap(FirewallRule::Protocol proto_a, FirewallRule::Protocol proto_b) {
    if (proto_a == proto_b) {
        return true;
    }
    return proto_a == FirewallRule::Protocol::Any || proto_b == FirewallRule::Protocol::Any;
}

bool profilesOverlap(int profiles_a, int profiles_b) {
    // 0 means the profile mask was not read; treat as "overlaps everything" (conservative -- never
    // hide a real conflict). Two disjoint masks (e.g. Domain=1 vs Public=4) never apply together.
    if (profiles_a == 0 || profiles_b == 0) {
        return true;
    }
    return (profiles_a & profiles_b) != 0;
}

bool applicationPathsMatch(const QString& path_a, const QString& path_b) {
    if (path_a.isEmpty() || path_b.isEmpty()) {
        return true;
    }
    return path_a.compare(path_b, Qt::CaseInsensitive) == 0;
}

// Two rules bound to DIFFERENT non-empty services do not apply to the same
// traffic, so they cannot conflict. An empty service means "any" and matches.
bool servicesMatch(const QString& svc_a, const QString& svc_b) {
    if (svc_a.isEmpty() || svc_b.isEmpty()) {
        return true;
    }
    return svc_a.compare(svc_b, Qt::CaseInsensitive) == 0;
}

bool FirewallRuleAuditor::rulesConflict(const FirewallRule& a, const FirewallRule& b) {
    if (!a.enabled || !b.enabled) {
        return false;
    }
    // A conflict is a same-direction Allow-vs-Block pair whose scope overlaps.
    if (a.direction != b.direction || a.action == b.action) {
        return false;
    }
    if (!profilesOverlap(a.profiles, b.profiles) || !protocolsOverlap(a.protocol, b.protocol)) {
        return false;
    }
    // Both the LOCAL and the REMOTE port ranges must overlap; a rule pair that
    // differs only on remote ports (e.g. remote 80 vs 443) does NOT conflict --
    // remote ports were previously ignored, producing false conflicts (B9-11).
    if (!portsOverlap(a.localPorts, b.localPorts) || !portsOverlap(a.remotePorts, b.remotePorts)) {
        return false;
    }
    // Different applications or services scope the rules to disjoint traffic.
    // Addresses are conservatively assumed to overlap (proving CIDR disjointness
    // is out of scope), so they never HIDE a conflict.
    return applicationPathsMatch(a.applicationPath, b.applicationPath) &&
           servicesMatch(a.serviceName, b.serviceName);
}

FirewallConflict buildConflict(const FirewallRule& a, const FirewallRule& b) {
    FirewallConflict conflict;
    conflict.ruleA = a;
    conflict.ruleB = b;

    const auto actionA = (a.action == FirewallRule::Action::Allow) ? QStringLiteral("ALLOW")
                                                                   : QStringLiteral("BLOCK");
    const auto actionB = (b.action == FirewallRule::Action::Allow) ? QStringLiteral("ALLOW")
                                                                   : QStringLiteral("BLOCK");

    conflict.conflictDescription =
        QStringLiteral("\"%1\" (%2) conflicts with \"%3\" (%4) on ports %5/%6")
            .arg(a.name, actionA, b.name, actionB, a.localPorts, b.localPorts);

    if (!a.applicationPath.isEmpty() &&
        a.applicationPath.compare(b.applicationPath, Qt::CaseInsensitive) == 0) {
        conflict.severity = FirewallConflict::Severity::Critical;
    } else if (a.localPorts == b.localPorts) {
        conflict.severity = FirewallConflict::Severity::Warning;
    } else {
        conflict.severity = FirewallConflict::Severity::Info;
    }

    return conflict;
}

QVector<FirewallConflict> FirewallRuleAuditor::findConflicts(
    const QVector<FirewallRule>& rules) const {
    QVector<FirewallConflict> conflicts;

    for (int i = 0; i < rules.size(); ++i) {
        if (m_cancelled.load()) {
            break;
        }
        for (int j = i + 1; j < rules.size(); ++j) {
            if (rulesConflict(rules[i], rules[j])) {
                conflicts.append(buildConflict(rules[i], rules[j]));
            }
        }
    }

    return conflicts;
}

QVector<FirewallGap> FirewallRuleAuditor::findGaps(const QVector<FirewallRule>& rules) const {
    QVector<FirewallGap> gaps;
    checkRdpGap(rules, gaps);
    checkIcmpGap(rules, gaps);
    checkWildcardGap(rules, gaps);
    checkSmbGap(rules, gaps);
    checkDisabledBlockGap(rules, gaps);
    return gaps;
}

void FirewallRuleAuditor::checkRdpGap(const QVector<FirewallRule>& rules,
                                      QVector<FirewallGap>& gaps) const {
    for (const auto& rule : rules) {
        if (!rule.enabled || rule.action != FirewallRule::Action::Allow) {
            continue;
        }
        if (rule.direction != FirewallRule::Direction::Inbound) {
            continue;
        }
        if (!localPortsCoverPort(rule.localPorts, kRdpPort)) {
            continue;
        }
        if (rule.remoteAddresses != QStringLiteral("*") && !rule.remoteAddresses.isEmpty()) {
            continue;
        }
        FirewallGap gap;
        gap.description = QStringLiteral("RDP (port 3389) is open to all addresses");
        gap.recommendation =
            QStringLiteral("Restrict RDP access to specific IP ranges or use a VPN");
        gap.severity = FirewallGap::Severity::Warning;
        gaps.append(gap);
        return;
    }
}

void FirewallRuleAuditor::checkIcmpGap(const QVector<FirewallRule>& rules,
                                       QVector<FirewallGap>& gaps) const {
    for (const auto& rule : rules) {
        if (!rule.enabled) {
            continue;
        }
        if (rule.protocol != FirewallRule::Protocol::ICMPv4) {
            continue;
        }
        if (rule.action == FirewallRule::Action::Block) {
            return;
        }
    }
    FirewallGap gap;
    gap.description = QStringLiteral("No explicit ICMP block rules found");
    gap.recommendation =
        QStringLiteral("Consider adding ICMP rate limiting on public-facing networks");
    gap.severity = FirewallGap::Severity::Info;
    gaps.append(gap);
}

void FirewallRuleAuditor::checkWildcardGap(const QVector<FirewallRule>& rules,
                                           QVector<FirewallGap>& gaps) const {
    int wildcardRules = 0;
    for (const auto& rule : rules) {
        if (!rule.enabled || rule.action != FirewallRule::Action::Allow) {
            continue;
        }
        if (rule.applicationPath.isEmpty() && rule.localPorts == QStringLiteral("*")) {
            wildcardRules++;
        }
    }
    if (wildcardRules <= kWildcardRuleWarningThreshold) {
        return;
    }
    FirewallGap gap;
    gap.description = QStringLiteral("%1 rules allow all ports without application restrictions")
                          .arg(wildcardRules);
    gap.recommendation =
        QStringLiteral("Review and restrict overly permissive rules to specific applications");
    gap.severity = FirewallGap::Severity::Warning;
    gaps.append(gap);
}

void FirewallRuleAuditor::checkSmbGap(const QVector<FirewallRule>& rules,
                                      QVector<FirewallGap>& gaps) const {
    for (const auto& rule : rules) {
        if (!rule.enabled || rule.action != FirewallRule::Action::Allow) {
            continue;
        }
        if (rule.direction != FirewallRule::Direction::Inbound) {
            continue;
        }
        // A wildcard ("*") OR empty port rule allows ALL ports, which includes SMB
        // (445); matching only the explicit-445 case missed an all-ports rule that
        // exposes SMB on Public just as much (B9-11/B9-12), mirroring checkRdpGap.
        if (!localPortsCoverPort(rule.localPorts, kSmbPort)) {
            continue;
        }
        if (rule.profiles & static_cast<int>(FirewallRule::Profile::Public)) {
            FirewallGap gap;
            gap.description = QStringLiteral("SMB (port 445) is allowed on Public profile");
            gap.recommendation = QStringLiteral(
                "Disable SMB on Public networks to prevent lateral movement attacks");
            gap.severity = FirewallGap::Severity::Warning;
            gaps.append(gap);
            return;
        }
    }
}

void FirewallRuleAuditor::checkDisabledBlockGap(const QVector<FirewallRule>& rules,
                                                QVector<FirewallGap>& gaps) const {
    int disabledBlockRules = 0;
    for (const auto& rule : rules) {
        if (rule.enabled) {
            continue;
        }
        if (rule.action == FirewallRule::Action::Block) {
            disabledBlockRules++;
        }
    }
    if (disabledBlockRules <= kDisabledBlockRuleWarningThreshold) {
        return;
    }
    FirewallGap gap;
    gap.description = QStringLiteral("%1 block rules are disabled").arg(disabledBlockRules);
    gap.recommendation = QStringLiteral(
        "Review disabled block rules -- they may have been turned off inadvertently");
    gap.severity = FirewallGap::Severity::Info;
    gaps.append(gap);
}

QVector<uint16_t> FirewallRuleAuditor::parsePorts(const QString& portStr) {
    QVector<uint16_t> ports;

    // An empty port string is LEGITIMATE input, not a precondition violation: most Windows firewall
    // rules place no port restriction, so localPorts is empty, and checkRdpGap/checkSmbGap call
    // this unconditionally. (A prior Q_ASSERT(!isEmpty()) here fired abort() -> __fastfail
    // 0xC0000409 in Debug on any host whose enumerated rules include an enabled inbound-allow rule
    // with no ports.)
    if (portStr.isEmpty() || portStr == QStringLiteral("*")) {
        return ports;
    }

    const auto parts = portStr.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        const auto trimmed = part.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        int start = 0;
        int end = 0;
        if (tryParsePortRange(trimmed, start, end)) {
            appendPortRange(ports, start, end);
            continue;
        }

        uint16_t port = 0;
        if (tryParsePortValue(trimmed, port)) {
            ports.append(port);
        }
    }

    return ports;
}

bool FirewallRuleAuditor::localPortsCoverPort(const QString& localPorts, uint16_t port) {
    // An empty LocalPorts is the Windows default "no port restriction" == ALL
    // ports, which includes `port`; treat it as a wildcard exactly like "*"
    // (mirrors portsOverlap). Missing the empty case made an all-ports RDP/SMB
    // allow rule skip its gap check and silently hide the exposure (B9-12).
    if (localPorts.isEmpty() || localPorts == QStringLiteral("*")) {
        return true;
    }
    return parsePorts(localPorts).contains(port);
}

bool FirewallRuleAuditor::portsOverlap(const QString& a, const QString& b) {
    // Wildcard matches everything
    if (a == QStringLiteral("*") || b == QStringLiteral("*") || a.isEmpty() || b.isEmpty()) {
        return true;
    }

    const auto portsA = parsePorts(a);
    const auto portsB = parsePorts(b);

    // A non-empty, non-wildcard expression that parsed to NO ports is an
    // unrecognized form (e.g. a named service like "RPC"/"RPC-EPMap"). We cannot
    // prove it disjoint, so treat it as overlapping rather than hiding a possible
    // conflict -- a conflict detector must fail SAFE, not fail-open to no-overlap
    // (B9-11).
    if (portsA.isEmpty() || portsB.isEmpty()) {
        return true;
    }

    for (const auto port : portsA) {
        if (portsB.contains(port)) {
            return true;
        }
    }
    return false;
}

}  // namespace sak
