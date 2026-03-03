// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file firewall_rule_auditor.cpp
/// @brief Windows Firewall rule enumeration with conflict/gap analysis via COM

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <netfw.h>
#include <comdef.h>

#include "sak/firewall_rule_auditor.h"

#include <QSet>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace sak {

namespace {
constexpr int kMaxPortValue = 65535;

/// @brief RAII wrapper for COM initialization
struct ComInitializer {
    HRESULT hr;
    ComInitializer() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInitializer() { if (SUCCEEDED(hr)) CoUninitialize(); }
    [[nodiscard]] bool ok() const { return SUCCEEDED(hr); }
};

/// @brief Convert BSTR to QString safely
[[nodiscard]] QString bstrToQString(BSTR bstr)
{
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

    void reset(T* newPtr = nullptr)
    {
        if (ptr != nullptr) {
            ptr->Release();
        }
        ptr = newPtr;
    }

    [[nodiscard]] T* get() const { return ptr; }
    [[nodiscard]] T** put()
    {
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
    void reset()
    {
        VariantClear(&var);
        VariantInit(&var);
    }
};

void populateRuleIdentity(INetFwRule* pRule, FirewallRule& rule)
{
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

void populateRuleDirectionActionAndProtocol(INetFwRule* pRule, FirewallRule& rule)
{
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (SUCCEEDED(pRule->get_Enabled(&enabled))) {
        rule.enabled = (enabled == VARIANT_TRUE);
    }

    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
    if (SUCCEEDED(pRule->get_Direction(&dir))) {
        rule.direction = (dir == NET_FW_RULE_DIR_IN)
                             ? FirewallRule::Direction::Inbound
                             : FirewallRule::Direction::Outbound;
    }

    NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
    if (SUCCEEDED(pRule->get_Action(&action))) {
        rule.action = (action == NET_FW_ACTION_ALLOW)
                          ? FirewallRule::Action::Allow
                          : FirewallRule::Action::Block;
    }

    long proto = 0;
    if (!SUCCEEDED(pRule->get_Protocol(&proto))) {
        return;
    }

    switch (proto) {
    case 6:   rule.protocol = FirewallRule::Protocol::TCP; break;
    case 17:  rule.protocol = FirewallRule::Protocol::UDP; break;
    case 1:   rule.protocol = FirewallRule::Protocol::ICMPv4; break;
    case 58:  rule.protocol = FirewallRule::Protocol::ICMPv6; break;
    default:  rule.protocol = FirewallRule::Protocol::Any; break;
    }
}

void populateRulePortsAndAddresses(INetFwRule* pRule, FirewallRule& rule)
{
    BSTR localPorts = nullptr;
    if (SUCCEEDED(pRule->get_LocalPorts(&localPorts))) {
        rule.localPorts = bstrToQString(localPorts);
        SysFreeString(localPorts);
    }

    BSTR remotePorts = nullptr;
    if (SUCCEEDED(pRule->get_RemotePorts(&remotePorts))) {
        rule.remotePorts = bstrToQString(remotePorts);
        SysFreeString(remotePorts);
    }

    BSTR localAddrs = nullptr;
    if (SUCCEEDED(pRule->get_LocalAddresses(&localAddrs))) {
        rule.localAddresses = bstrToQString(localAddrs);
        SysFreeString(localAddrs);
    }

    BSTR remoteAddrs = nullptr;
    if (SUCCEEDED(pRule->get_RemoteAddresses(&remoteAddrs))) {
        rule.remoteAddresses = bstrToQString(remoteAddrs);
        SysFreeString(remoteAddrs);
    }
}

void populateRuleAppAndProfileInfo(INetFwRule* pRule, FirewallRule& rule)
{
    BSTR appPath = nullptr;
    if (SUCCEEDED(pRule->get_ApplicationName(&appPath))) {
        rule.applicationPath = bstrToQString(appPath);
        SysFreeString(appPath);
    }

    BSTR svcName = nullptr;
    if (SUCCEEDED(pRule->get_ServiceName(&svcName))) {
        rule.serviceName = bstrToQString(svcName);
        SysFreeString(svcName);
    }

    long profiles = 0;
    if (SUCCEEDED(pRule->get_Profiles(&profiles))) {
        rule.profiles = static_cast<int>(profiles);
    }

    BSTR grouping = nullptr;
    if (SUCCEEDED(pRule->get_Grouping(&grouping))) {
        rule.grouping = bstrToQString(grouping);
        SysFreeString(grouping);
    }
}

bool tryExtractFirewallRuleFromVariant(const VARIANT& var, FirewallRule& outRule)
{
    if (var.vt != VT_DISPATCH || var.pdispVal == nullptr) {
        return false;
    }

    ComPtr<INetFwRule> pRule;
    const HRESULT hr = var.pdispVal->QueryInterface(
        __uuidof(INetFwRule), reinterpret_cast<void**>(pRule.put()));
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

bool tryParsePortValue(const QString& s, uint16_t& port)
{
    bool ok = false;
    const int value = s.trimmed().toInt(&ok);
    if (!ok || value < 0 || value > kMaxPortValue) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

bool tryParsePortRange(const QString& s, int& start, int& end)
{
    if (!s.contains(QLatin1Char('-'))) {
        return false;
    }

    const auto parts = s.split(QLatin1Char('-'));
    if (parts.size() != 2) {
        return false;
    }

    bool ok1 = false;
    bool ok2 = false;
    start = parts[0].trimmed().toInt(&ok1);
    end   = parts[1].trimmed().toInt(&ok2);
    if (!ok1 || !ok2) {
        return false;
    }

    if (start < 0 || end > kMaxPortValue || start > end) {
        return false;
    }
    return true;
}

void appendPortRange(QVector<uint16_t>& ports, int start, int end)
{
    for (int p = start; p <= end; ++p) {
        ports.append(static_cast<uint16_t>(p));
    }
}
} // namespace

FirewallRuleAuditor::FirewallRuleAuditor(QObject* parent)
    : QObject(parent)
{
}

void FirewallRuleAuditor::cancel()
{
    m_cancelled.store(true);
}

void FirewallRuleAuditor::enumerateRules()
{
    m_cancelled.store(false);
    m_rules = enumerateViaCOM();

    if (!m_cancelled.load()) {
        Q_EMIT rulesEnumerated(m_rules);
    }
}

void FirewallRuleAuditor::detectConflicts()
{
    m_cancelled.store(false);
    auto conflicts = findConflicts(m_rules);
    Q_EMIT conflictsDetected(conflicts);
}

void FirewallRuleAuditor::analyzeGaps()
{
    m_cancelled.store(false);
    auto gaps = findGaps(m_rules);
    Q_EMIT gapsAnalyzed(gaps);
}

void FirewallRuleAuditor::fullAudit()
{
    m_cancelled.store(false);
    m_rules = enumerateViaCOM();

    if (m_cancelled.load()) {
        return;
    }

    auto conflicts = findConflicts(m_rules);
    auto gaps = findGaps(m_rules);

    Q_EMIT auditComplete(m_rules, conflicts, gaps);
}

QVector<FirewallRule> FirewallRuleAuditor::findRulesByPort(
    uint16_t port, FirewallRule::Direction direction) const
{
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

QVector<FirewallRule> FirewallRuleAuditor::findRulesByApplication(
    const QString& appPath) const
{
    QVector<FirewallRule> matching;
    for (const auto& rule : m_rules) {
        if (rule.applicationPath.contains(appPath, Qt::CaseInsensitive)) {
            matching.append(rule);
        }
    }
    return matching;
}

QVector<FirewallRule> FirewallRuleAuditor::findRulesByName(
    const QString& nameFilter) const
{
    QVector<FirewallRule> matching;
    for (const auto& rule : m_rules) {
        if (rule.name.contains(nameFilter, Qt::CaseInsensitive)) {
            matching.append(rule);
        }
    }
    return matching;
}

QVector<FirewallRule> FirewallRuleAuditor::enumerateViaCOM()
{
    QVector<FirewallRule> rules;

    ComInitializer com;
    if (!com.ok()) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to initialize COM"));
        return rules;
    }

    ComPtr<INetFwPolicy2> pPolicy;
    HRESULT hr = CoCreateInstance(
        __uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(INetFwPolicy2), reinterpret_cast<void**>(pPolicy.put()));

    if (FAILED(hr) || !pPolicy) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to access firewall policy (HRESULT 0x%1)")
                                 .arg(static_cast<unsigned long>(hr), 8, 16, QLatin1Char('0')));
        return rules;
    }

    ComPtr<INetFwRules> pRules;
    hr = pPolicy->get_Rules(pRules.put());
    if (FAILED(hr) || !pRules) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to enumerate firewall rules"));
        return rules;
    }
    ComPtr<IUnknown> pEnumerator;
    pRules->get__NewEnum(pEnumerator.put());
    if (!pEnumerator) {
        return rules;
    }

    ComPtr<IEnumVARIANT> pVariant;
    hr = pEnumerator->QueryInterface(__uuidof(IEnumVARIANT),
                                     reinterpret_cast<void**>(pVariant.put()));
    if (FAILED(hr) || !pVariant) {
        return rules;
    }

    ULONG fetched = 0;
    VariantHolder var;
    while (SUCCEEDED(pVariant->Next(1, &var.var, &fetched)) && fetched > 0) {
        if (m_cancelled.load()) {
            break;
        }

        FirewallRule rule;
        if (tryExtractFirewallRuleFromVariant(var.var, rule)) {
            rules.append(rule);
        }
        var.reset();
    }

    return rules;
}

QVector<FirewallConflict> FirewallRuleAuditor::findConflicts(
    const QVector<FirewallRule>& rules) const
{
    QVector<FirewallConflict> conflicts;

    for (int i = 0; i < rules.size(); ++i) {
        if (m_cancelled.load()) {
            break;
        }

        for (int j = i + 1; j < rules.size(); ++j) {
            const auto& a = rules[i];
            const auto& b = rules[j];

            // Only check enabled rules in the same direction
            if (!a.enabled || !b.enabled) {
                continue;
            }
            if (a.direction != b.direction) {
                continue;
            }

            // Check for conflicting actions (Allow vs Block)
            if (a.action == b.action) {
                continue;
            }

            // Check if protocols overlap
            bool protoOverlap = (a.protocol == b.protocol) ||
                                (a.protocol == FirewallRule::Protocol::Any) ||
                                (b.protocol == FirewallRule::Protocol::Any);
            if (!protoOverlap) {
                continue;
            }

            // Check if ports overlap
            bool portOverlap = portsOverlap(a.localPorts, b.localPorts);
            if (!portOverlap) {
                continue;
            }

            // Check if application paths match (both empty = match)
            bool appOverlap = a.applicationPath.isEmpty() ||
                              b.applicationPath.isEmpty() ||
                              (a.applicationPath.compare(b.applicationPath,
                                                          Qt::CaseInsensitive) == 0);
            if (!appOverlap) {
                continue;
            }

            FirewallConflict conflict;
            conflict.ruleA = a;
            conflict.ruleB = b;

            const auto actionA = (a.action == FirewallRule::Action::Allow)
                                     ? QStringLiteral("ALLOW") : QStringLiteral("BLOCK");
            const auto actionB = (b.action == FirewallRule::Action::Allow)
                                     ? QStringLiteral("ALLOW") : QStringLiteral("BLOCK");

            conflict.conflictDescription =
                QStringLiteral("\"%1\" (%2) conflicts with \"%3\" (%4) "
                               "on ports %5/%6")
                    .arg(a.name, actionA, b.name, actionB,
                         a.localPorts, b.localPorts);

            // Severity: Block+Allow on same app is critical
            if (!a.applicationPath.isEmpty() &&
                a.applicationPath.compare(b.applicationPath, Qt::CaseInsensitive) == 0) {
                conflict.severity = FirewallConflict::Severity::Critical;
            } else if (a.localPorts == b.localPorts) {
                conflict.severity = FirewallConflict::Severity::Warning;
            } else {
                conflict.severity = FirewallConflict::Severity::Info;
            }

            conflicts.append(conflict);
        }
    }

    return conflicts;
}

QVector<FirewallGap> FirewallRuleAuditor::findGaps(
    const QVector<FirewallRule>& rules) const
{
    QVector<FirewallGap> gaps;

    // Check for common security gaps

    // 1. Check if RDP (3389) is open to any address
    bool rdpOpen = false;
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.action != FirewallRule::Action::Allow) continue;
        if (rule.direction != FirewallRule::Direction::Inbound) continue;

        auto ports = parsePorts(rule.localPorts);
        if (ports.contains(3389) || rule.localPorts == QStringLiteral("*")) {
            if (rule.remoteAddresses == QStringLiteral("*") ||
                rule.remoteAddresses.isEmpty()) {
                rdpOpen = true;
                break;
            }
        }
    }

    if (rdpOpen) {
        FirewallGap gap;
        gap.description = QStringLiteral("RDP (port 3389) is open to all addresses");
        gap.recommendation = QStringLiteral(
            "Restrict RDP access to specific IP ranges or use a VPN");
        gap.severity = FirewallGap::Severity::Warning;
        gaps.append(gap);
    }

    // 2. Check for open ICMP
    bool icmpBlocked = false;
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.protocol != FirewallRule::Protocol::ICMPv4) continue;
        if (rule.action == FirewallRule::Action::Block) {
            icmpBlocked = true;
            break;
        }
    }

    if (!icmpBlocked) {
        FirewallGap gap;
        gap.description = QStringLiteral("No explicit ICMP block rules found");
        gap.recommendation = QStringLiteral(
            "Consider adding ICMP rate limiting on public-facing networks");
        gap.severity = FirewallGap::Severity::Info;
        gaps.append(gap);
    }

    // 3. Check for rules with wildcard application paths
    int wildcardRules = 0;
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.action != FirewallRule::Action::Allow) continue;
        if (rule.applicationPath.isEmpty() &&
            rule.localPorts == QStringLiteral("*")) {
            wildcardRules++;
        }
    }

    if (wildcardRules > 5) {
        FirewallGap gap;
        gap.description = QStringLiteral(
            "%1 rules allow all ports without application restrictions").arg(wildcardRules);
        gap.recommendation = QStringLiteral(
            "Review and restrict overly permissive rules to specific applications");
        gap.severity = FirewallGap::Severity::Warning;
        gaps.append(gap);
    }

    // 4. Check for SMB (445) accessible from external
    for (const auto& rule : rules) {
        if (!rule.enabled) continue;
        if (rule.action != FirewallRule::Action::Allow) continue;
        if (rule.direction != FirewallRule::Direction::Inbound) continue;

        auto ports = parsePorts(rule.localPorts);
        if (ports.contains(445)) {
            if (rule.profiles & static_cast<int>(FirewallRule::Profile::Public)) {
                FirewallGap gap;
                gap.description = QStringLiteral("SMB (port 445) is allowed on Public profile");
                gap.recommendation = QStringLiteral(
                    "Disable SMB on Public networks to prevent lateral movement attacks");
                gap.severity = FirewallGap::Severity::Warning;
                gaps.append(gap);
                break;
            }
        }
    }

    // 5. Check for disabled rules that were once important
    int disabledBlockRules = 0;
    for (const auto& rule : rules) {
        if (rule.enabled) continue;
        if (rule.action == FirewallRule::Action::Block) {
            disabledBlockRules++;
        }
    }

    if (disabledBlockRules > 10) {
        FirewallGap gap;
        gap.description = QStringLiteral(
            "%1 block rules are disabled").arg(disabledBlockRules);
        gap.recommendation = QStringLiteral(
            "Review disabled block rules — they may have been turned off inadvertently");
        gap.severity = FirewallGap::Severity::Info;
        gaps.append(gap);
    }

    return gaps;
}

QVector<uint16_t> FirewallRuleAuditor::parsePorts(const QString& portStr)
{
    QVector<uint16_t> ports;

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

bool FirewallRuleAuditor::portsOverlap(const QString& a, const QString& b)
{
    // Wildcard matches everything
    if (a == QStringLiteral("*") || b == QStringLiteral("*") ||
        a.isEmpty() || b.isEmpty()) {
        return true;
    }

    const auto portsA = parsePorts(a);
    const auto portsB = parsePorts(b);

    for (const auto port : portsA) {
        if (portsB.contains(port)) {
            return true;
        }
    }
    return false;
}

} // namespace sak
