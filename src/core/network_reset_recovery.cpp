// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_reset_recovery.cpp
/// @brief After a network reset: read netsh's answer, put settings typed in by hand back, check
///        the adapters, and wait for them to settle. Declared in network_reset_safeguard.h.

#include "sak/network_adapter_admin.h"
#include "sak/network_reset_safeguard.h"
#include "sak/user_profile_types.h"

#include <QRegularExpression>

#include <algorithm>

namespace sak {

namespace {

// What English netsh prints. See readNetshReset for what happens on another language.
constexpr auto kItemResetSuffix = ", OK!";
constexpr auto kItemFailedSuffix = ", failed.";
constexpr auto kAccessDenied = "Access is denied.";
constexpr auto kRestartNotice = "Restart the computer to complete this action.";

// -- netsh --------------------------------------------------------------------------------------

struct NetshLineCounts {
    int reset{0};
    int denied{0};
    int unrecognised{0};
    bool restart_notice{false};
    QStringList failures;
};

NetshLineCounts countNetshLines(const QString& output) {
    static const QRegularExpression kLineBreaks(QStringLiteral("[\r\n]+"));
    QStringList lines = output.split(kLineBreaks, Qt::SkipEmptyParts);
    for (QString& line : lines) {
        line = line.trimmed();
    }
    lines.removeAll(QString());
    NetshLineCounts counts;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);
        if (line.endsWith(QLatin1String(kItemResetSuffix))) {
            ++counts.reset;
        } else if (line.endsWith(QLatin1String(kItemFailedSuffix))) {
            // An item's failure is two lines: the item, then the system's reason for it.
            const QString reason = i + 1 < lines.size() ? lines.at(i + 1) : QString();
            if (reason == QLatin1String(kAccessDenied)) {
                ++counts.denied;
                ++i;
            } else {
                counts.failures << (reason.isEmpty() ? line : line + QLatin1Char(' ') + reason);
                i += reason.isEmpty() ? 0 : 1;
            }
        } else if (line == QLatin1String(kRestartNotice)) {
            counts.restart_notice = true;
        } else {
            ++counts.unrecognised;
        }
    }
    return counts;
}

QString deniedNote(int denied) {
    return QStringLiteral(
               "%1 protected %2 not reset (Access is denied); Windows does this on a stock "
               "install, and the reset completes when the computer restarts")
        .arg(denied)
        .arg(denied == 1 ? QStringLiteral("item was") : QStringLiteral("items were"));
}

// -- Reapply ------------------------------------------------------------------------------------

/// The metric a gateway goes back with: none when Windows chose it, so Windows chooses again, and
/// the one it had otherwise. Given no metric, netsh's `set address ... gateway=` pins 1 and
/// `gwmetric=0` pins 0; only a route added with no metric is automatic (VM, fix round S).
std::optional<int> gatewayMetric(const StaticGateway& gateway) {
    return gateway.automatic ? std::nullopt : std::optional<int>(gateway.metric);
}

/// "10.0.2.15/24" -> {"10.0.2.15", 24}.
std::pair<QString, int> splitPrefix(const QString& address_with_prefix) {
    const qsizetype slash = address_with_prefix.lastIndexOf(QLatin1Char('/'));
    return {address_with_prefix.left(slash), address_with_prefix.mid(slash + 1).toInt()};
}

bool hasGateway(const QList<StaticGateway>& gateways, const QString& next_hop) {
    return std::ranges::any_of(gateways, [&next_hop](const StaticGateway& gateway) {
        return gateway.next_hop.compare(next_hop, Qt::CaseInsensitive) == 0;
    });
}

QStringList missingFrom(const QStringList& wanted, const QStringList& present) {
    QStringList missing;
    for (const QString& item : wanted) {
        if (!present.contains(item, Qt::CaseInsensitive)) {
            missing << item;
        }
    }
    return missing;
}

void addGatewaySteps(QList<ReapplyStep>& steps,
                     IpFamily family,
                     const QString& adapter,
                     const QList<StaticGateway>& gateways) {
    for (const StaticGateway& gateway : gateways) {
        steps.append(
            {adapter,
             QStringLiteral("gateway %1").arg(gateway.next_hop),
             adapterDefaultGatewayArgs(family, adapter, gateway.next_hop, gatewayMetric(gateway))});
    }
}

void addDnsSteps(QList<ReapplyStep>& steps,
                 IpFamily family,
                 const QString& adapter,
                 const QStringList& servers) {
    steps.append({adapter,
                  QStringLiteral("DNS %1").arg(servers.join(QStringLiteral(", "))),
                  adapterFamilyPrimaryDnsArgs(family, adapter, servers.first())});
    for (qsizetype i = 1; i < servers.size(); ++i) {
        steps.append({adapter,
                      QStringLiteral("DNS %1").arg(servers.at(i)),
                      adapterFamilyAdditionalDnsArgs(
                          family, adapter, servers.at(i), static_cast<int>(i) + 1)});
    }
}

/// Static IPv4: one `set address` with the first address and no gateway (it replaces whatever the
/// reset left, and turns DHCP off), then the other addresses, then every gateway as a route, each
/// with the metric it had (gatewayMetric). Only what is missing goes back when the first address
/// survived.
void planIpv4Addresses(QList<ReapplyStep>& steps,
                       const QString& adapter,
                       const IpFamilySettings& before,
                       const IpFamilySettings* now) {
    const QStringList missing = now == nullptr
                                    ? before.static_addresses
                                    : missingFrom(before.static_addresses, now->static_addresses);
    const bool redo_first = now == nullptr || now->dhcp ||
                            missing.contains(before.static_addresses.first(), Qt::CaseInsensitive);
    QList<StaticGateway> gateways_left = before.static_gateways;
    QStringList addresses_left = missing;
    if (redo_first) {
        const auto [address, prefix] = splitPrefix(before.static_addresses.first());
        steps.append({adapter,
                      QStringLiteral("IPv4 %1").arg(before.static_addresses.first()),
                      adapterStaticIpv4Args(
                          adapter, address, EthernetConfigInfo::prefixLengthToSubnetMask(prefix))});
        addresses_left = before.static_addresses.mid(1);
    } else {
        gateways_left.clear();
        for (const StaticGateway& gateway : before.static_gateways) {
            if (!hasGateway(now->static_gateways, gateway.next_hop)) {
                gateways_left.append(gateway);
            }
        }
    }
    for (const QString& extra : addresses_left) {
        const auto [address, prefix] = splitPrefix(extra);
        steps.append({adapter,
                      QStringLiteral("IPv4 %1").arg(extra),
                      adapterAdditionalIpv4AddressArgs(
                          adapter, address, EthernetConfigInfo::prefixLengthToSubnetMask(prefix))});
    }
    addGatewaySteps(steps, IpFamily::V4, adapter, gateways_left);
}

/// The gateways typed in by hand that @p current no longer has (all of them when it is unknown).
QList<StaticGateway> missingGateways(const QList<StaticGateway>& wanted,
                                     const IpFamilySettings* current) {
    QList<StaticGateway> missing;
    for (const StaticGateway& gateway : wanted) {
        if (current == nullptr || !hasGateway(current->static_gateways, gateway.next_hop)) {
            missing.append(gateway);
        }
    }
    return missing;
}

/// The DNS servers typed in by hand go back when the list Windows has now differs.
void planDns(QList<ReapplyStep>& steps,
             IpFamily family,
             const QString& adapter,
             const IpFamilySettings& before,
             const IpFamilySettings* current) {
    if (!before.static_dns_servers.isEmpty() &&
        (current == nullptr || current->static_dns_servers != before.static_dns_servers)) {
        addDnsSteps(steps, family, adapter, before.static_dns_servers);
    }
}

void planIpv4(QList<ReapplyStep>& steps,
              const AdapterSettings& before,
              const AdapterSettings* now,
              bool now_known) {
    const IpFamilySettings* current = now == nullptr ? nullptr : &now->ipv4;
    const bool has_static_addresses = !before.ipv4.static_addresses.isEmpty();
    if (before.ipv4.dhcp) {
        // Only when Windows was read: netsh refuses "turn DHCP on" where it is already on.
        if (now_known && current != nullptr && current->present && !current->dhcp) {
            steps.append({before.name,
                          QStringLiteral("IPv4 address from DHCP"),
                          adapterDhcpAddressArgs(before.name)});
        }
    } else if (has_static_addresses) {
        // Its gateways go back with its addresses (planIpv4Addresses).
        planIpv4Addresses(steps, before.name, before.ipv4, current);
    }
    if (before.ipv4.dhcp || !has_static_addresses) {
        addGatewaySteps(steps,
                        IpFamily::V4,
                        before.name,
                        missingGateways(before.ipv4.static_gateways, current));
    }
    planDns(steps, IpFamily::V4, before.name, before.ipv4, current);
}

void planIpv6(QList<ReapplyStep>& steps,
              const AdapterSettings& before,
              const AdapterSettings* now) {
    const IpFamilySettings* current = now == nullptr ? nullptr : &now->ipv6;
    const QStringList missing = current == nullptr ? before.ipv6.static_addresses
                                                   : missingFrom(before.ipv6.static_addresses,
                                                                 current->static_addresses);
    for (const QString& address : missing) {
        steps.append({before.name,
                      QStringLiteral("IPv6 %1").arg(address),
                      adapterIpv6AddressArgs(before.name, address)});
    }
    addGatewaySteps(
        steps, IpFamily::V6, before.name, missingGateways(before.ipv6.static_gateways, current));
    planDns(steps, IpFamily::V6, before.name, before.ipv6, current);
}

/// An adapter has something to put back when it is turned on and either has settings typed in by
/// hand or gets its IPv4 address from DHCP (which the reset could have turned off).
bool hasAnythingToPutBack(const AdapterSettings& adapter) {
    return !adapter.isDisabled() && (adapter.hasStaticSettings() || adapter.ipv4.dhcp);
}

/// Whether @p adapter is still there, and turned on, after the reset. When it is not, settings
/// typed in by hand cannot go back, and @p problems says so.
bool stillThere(const AdapterSettings& adapter, const AdapterSettings* now, QStringList* problems) {
    if (now != nullptr && !now->isDisabled()) {
        return true;
    }
    if (adapter.hasStaticSettings() && problems != nullptr) {
        *problems << QStringLiteral(
                         "%1 is %2 after the reset, so its settings were not put back: %3")
                         .arg(adapter.name,
                              now == nullptr ? QStringLiteral("gone")
                                             : QStringLiteral("turned off"),
                              describeStaticSettings(adapter));
    }
    return false;
}

// -- Checking -----------------------------------------------------------------------------------

QString addressOnly(const QString& address_with_prefix) {
    return address_with_prefix.section(QLatin1Char('/'), 0, 0);
}

/// "no usable IPv4 address (only the self-assigned 169.254.193.132/16); 10.0.2.15/24 typed in by
/// hand is not back", or "10.0.2.99/24 in use; 10.0.2.15/24 typed in by hand is not back".
QString unusableDetail(const QStringList& usable,
                       const QStringList& self_assigned,
                       const QStringList& missing) {
    const QString separator = QStringLiteral(", ");
    QString detail;
    if (!usable.isEmpty()) {
        detail = QStringLiteral("%1 in use").arg(usable.join(separator));
    } else if (!self_assigned.isEmpty()) {
        detail = QStringLiteral("no usable IPv4 address (only the self-assigned %1)")
                     .arg(self_assigned.join(separator));
    } else {
        detail = QStringLiteral("no IPv4 address");
    }
    if (!missing.isEmpty()) {
        detail += QStringLiteral("; %1 typed in by hand is not back").arg(missing.join(separator));
    }
    return detail;
}

AdapterCheck checkOne(const AdapterSettings& before, const AdapterSettings* now) {
    if (now == nullptr) {
        return {before.name, false, QStringLiteral("not found after the reset")};
    }
    QStringList usable;
    QStringList self_assigned;
    for (const QString& address : now->ipv4.addresses) {
        (isSelfAssignedIpv4(addressOnly(address)) ? self_assigned : usable) << address;
    }
    if (before.ipv4.dhcp) {
        if (!now->ipv4.dhcp) {
            return {before.name, false, QStringLiteral("DHCP is off now; it was on before")};
        }
        return {before.name,
                true,
                usable.isEmpty()
                    ? QStringLiteral("DHCP on, waiting for an address")
                    : QStringLiteral("DHCP on, %1").arg(usable.join(QStringLiteral(", ")))};
    }
    const QStringList missing = missingFrom(before.ipv4.static_addresses,
                                            now->ipv4.static_addresses);
    // Both halves are needed: every address typed in by hand back in the store, and one in use that
    // is not self-assigned. Either alone has been seen to be false while the other holds.
    if (!missing.isEmpty() || usable.isEmpty()) {
        return {before.name, false, unusableDetail(usable, self_assigned, missing)};
    }
    return {before.name,
            true,
            QStringLiteral("%1, typed in by hand").arg(usable.join(QStringLiteral(", ")))};
}

}  // namespace

NetshResetReading readNetshReset(const QString& output, int exit_code, bool timed_out) {
    NetshResetReading reading;
    if (timed_out) {
        reading.detail = QStringLiteral("timed out");
        return reading;
    }
    const NetshLineCounts counts = countNetshLines(output);
    reading.items_reset = counts.reset;
    reading.items_denied = counts.denied;
    if (!counts.failures.isEmpty()) {
        reading.detail = counts.failures.join(QStringLiteral("; "));
        return reading;
    }
    const bool recognised = counts.unrecognised == 0 && counts.reset > 0;
    if (exit_code != 0 && !(recognised && counts.restart_notice)) {
        reading.detail = recognised
                             ? QStringLiteral("exit %1 without its restart notice").arg(exit_code)
                             : QStringLiteral("exit %1; output not recognised").arg(exit_code);
        return reading;
    }
    reading.verdict = counts.restart_notice ? NetshResetVerdict::DoneRestartRequired
                                            : NetshResetVerdict::Done;
    if (counts.denied > 0) {
        reading.detail = deniedNote(counts.denied);
    }
    return reading;
}

QList<ReapplyStep> planSettingsReapply(const NetworkSettingsCapture& before,
                                       const std::optional<NetworkSettingsCapture>& after,
                                       QStringList* problems) {
    QList<ReapplyStep> steps;
    for (const AdapterSettings& adapter : before.adapters) {
        if (!hasAnythingToPutBack(adapter)) {
            continue;
        }
        const AdapterSettings* now = after.has_value() ? after->find(adapter.name) : nullptr;
        if (after.has_value() && !stillThere(adapter, now, problems)) {
            continue;
        }
        planIpv4(steps, adapter, now, after.has_value());
        planIpv6(steps, adapter, now);
    }
    return steps;
}

QList<AdapterCheck> checkAdaptersAfterReset(const NetworkSettingsCapture& before,
                                            const NetworkSettingsCapture& after) {
    QList<AdapterCheck> checks;
    for (const AdapterSettings& adapter : before.adapters) {
        // Only adapters whose IPv4 the technician owns: DHCP, or addresses typed in by hand. An
        // address another program sets (Hyper-V's switch, a VPN client) is that program's to
        // bring back, often only at the next start.
        const bool owned = adapter.ipv4.dhcp || !adapter.ipv4.static_addresses.isEmpty();
        if (adapter.isUp() && adapter.ipv4.present && owned) {
            checks.append(checkOne(adapter, after.find(adapter.name)));
        }
    }
    return checks;
}

bool allAdaptersUsable(const QList<AdapterCheck>& checks) {
    return std::ranges::all_of(checks, [](const AdapterCheck& check) { return check.usable; });
}

SettleResult waitForSettledNetwork(
    const SettleProbe& probe,
    const std::function<bool(const NetworkSettingsCapture&)>& settled,
    int budget_ms,
    int poll_ms) {
    SettleResult result;
    const qint64 start = probe.now_ms();
    for (;;) {
        ++result.attempts;
        auto capture = probe.capture();
        if (capture) {
            result.capture = std::move(*capture);
            result.last_error.clear();
            if (settled(*result.capture)) {
                result.settled = true;
                break;
            }
        } else {
            result.last_error = capture.error();
        }
        if (probe.cancelled() || probe.now_ms() - start + poll_ms > budget_ms) {
            break;
        }
        probe.sleep_ms(poll_ms);
    }
    return result;
}

}  // namespace sak
