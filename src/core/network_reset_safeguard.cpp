// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_reset_safeguard.cpp
/// @brief Reset Network's record of every adapter's settings, its reading of netsh's answer, and
///        the plan that puts settings typed in by hand back afterwards.

#include "sak/network_reset_safeguard.h"

#include "sak/app_paths.h"
#include "sak/io_write_utils.h"
#include "sak/network_adapter_admin.h"
#include "sak/user_profile_types.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>

#include <algorithm>

namespace sak {

namespace {

/// The marker networkCaptureScript prints. Output without it is not a capture.
constexpr auto kCaptureMarkerKey = "sak_network_capture";
constexpr int kCaptureMarkerValue = 1;
constexpr int kIpv4MaxPrefix = 32;
constexpr int kIpv6MaxPrefix = 128;
/// The preference position netsh gives the primary DNS server; secondaries follow it.
constexpr int kFirstAdditionalDnsIndex = 2;

constexpr auto kStatusUp = "Up";
constexpr auto kStatusDisabled = "Disabled";

// The read-only scan. Every value is a cmdlet enum, a number, an address, or the registry
// NameServer value -- nothing translated. A query that finds nothing answers ObjectNotFound, which
// is an empty answer, not a failure; anything else stops the script with a non-zero exit.
//
// "static" means the persistent store: what `netsh int ip reset` clears and what Windows loads at
// the next start. An address another program sets at each start (Hyper-V's Default Switch, a VPN
// client) is in the active store only, so it is neither recorded as typed in nor put back.
// Each route prefix is its own query: given both, Get-NetRoute answers ObjectNotFound for the one
// with no route, and under Stop that throws away the other's answer too (seen on the VM: its
// static gateway went unrecorded).
// A gateway's metric is automatic when the interface's DefaultGatewayMetric beside it is 0 and the
// route's metric is not: measured on the VM, Windows' own setting reads DefaultGatewayMetric 0 and
// route metric 256 ("Default" in route print), a pinned 0 reads 0 and 0, and a pinned 256 reads 256
// and 256.
constexpr auto kCaptureScript = R"PS(
$ErrorActionPreference = 'Stop'
function Find([scriptblock]$query) {
    try { @(& $query) } catch {
        if ($_.CategoryInfo.Category -eq 'ObjectNotFound') { @() } else { throw }
    }
}
function NoZone([string]$ip) { ($ip -split '%')[0] }
function GatewayMetrics([string]$key) {
    $map = @{}
    if (-not (Test-Path -LiteralPath $key)) { return $map }
    $p = Get-ItemProperty -LiteralPath $key
    $hops = @($p.DefaultGateway)
    $metrics = @($p.DefaultGatewayMetric)
    for ($i = 0; $i -lt $hops.Count; $i++) { if ($hops[$i]) { $map["$($hops[$i])"] = "$($metrics[$i])" } }
    return $map
}
function NameServers([string]$key) {
    if (-not (Test-Path -LiteralPath $key)) { return @() }
    $value = (Get-ItemProperty -LiteralPath $key).NameServer
    if ([string]::IsNullOrWhiteSpace($value)) { return @() }
    @($value -split '[,\s]+' | Where-Object { $_ })
}
$adapters = @(Get-NetAdapter)
$interfaces = Find { Get-NetIPInterface }
$active = Find { Get-NetIPAddress }
$saved = Find { Get-NetIPAddress -PolicyStore PersistentStore }
$routes = @(Find { Get-NetRoute -DestinationPrefix '0.0.0.0/0' }) +
    @(Find { Get-NetRoute -DestinationPrefix '::/0' })
$savedRoutes = @(Find { Get-NetRoute -PolicyStore PersistentStore -DestinationPrefix '0.0.0.0/0' }) +
    @(Find { Get-NetRoute -PolicyStore PersistentStore -DestinationPrefix '::/0' })
$dns = Find { Get-DnsClientServerAddress }
function Family($adapter, [string]$family, [int]$dnsFamily, [string]$tcpip) {
    $index = $adapter.ifIndex
    $mine = { param($list) @($list | Where-Object {
        $_.InterfaceIndex -eq $index -and "$($_.AddressFamily)" -eq $family }) }
    $ipif = @(& $mine $interfaces)
    $key = "HKLM:\SYSTEM\CurrentControlSet\Services\$tcpip\Parameters\Interfaces\$($adapter.InterfaceGuid)"
    $stored = GatewayMetrics $key
    [ordered]@{
        present = ($ipif.Count -gt 0)
        dhcp = ($ipif.Count -gt 0 -and "$($ipif[0].Dhcp)" -eq 'Enabled')
        addresses = @(& $mine $active | ForEach-Object { "$(NoZone $_.IPAddress)/$($_.PrefixLength)" })
        static_addresses = @(& $mine $saved | ForEach-Object { "$(NoZone $_.IPAddress)/$($_.PrefixLength)" })
        gateways = @(& $mine $routes | ForEach-Object { NoZone $_.NextHop })
        static_gateways = @(& $mine $savedRoutes | ForEach-Object {
            $hop = NoZone $_.NextHop
            [ordered]@{ next_hop = $hop; metric = [int]$_.RouteMetric
                automatic = ($stored["$hop"] -eq '0' -and [int]$_.RouteMetric -ne 0) } })
        dns_servers = @($dns | Where-Object {
            $_.InterfaceIndex -eq $index -and [int]$_.AddressFamily -eq $dnsFamily } |
            ForEach-Object { $_.ServerAddresses } | ForEach-Object { NoZone $_ })
        static_dns_servers = @(NameServers $key)
    }
}
$list = @(foreach ($a in $adapters) {
    [ordered]@{
        name = "$($a.Name)"
        description = "$($a.InterfaceDescription)"
        status = "$($a.Status)"
        ipv4 = (Family $a 'IPv4' 2 'Tcpip')
        ipv6 = (Family $a 'IPv6' 23 'Tcpip6')
    }
})
$json = ConvertTo-Json -Compress -Depth 6 -InputObject ([ordered]@{ sak_network_capture = 1; adapters = $list })
[regex]::Replace($json, '[^\x00-\x7F]', { param($c) '\u{0:x4}' -f [int][char]$c.Value })
)PS";

// -- Parsing ------------------------------------------------------------------------------------

bool isFamilyAddress(IpFamily family, const QString& value) {
    return family == IpFamily::V4 ? isDottedIpv4(value) : isIpv6Literal(value);
}

QString familyName(IpFamily family) {
    return family == IpFamily::V4 ? QStringLiteral("IPv4") : QStringLiteral("IPv6");
}

/// "address/prefix" with a real address of the family and a prefix in range.
bool isFamilyAddressWithPrefix(IpFamily family, const QString& value) {
    const qsizetype slash = value.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        return false;
    }
    bool prefix_ok = false;
    const int prefix = value.mid(slash + 1).toInt(&prefix_ok);
    const int max_prefix = family == IpFamily::V4 ? kIpv4MaxPrefix : kIpv6MaxPrefix;
    return prefix_ok && prefix >= 0 && prefix <= max_prefix &&
           isFamilyAddress(family, value.left(slash));
}

/// A JSON list of strings. PowerShell writes a one-item list as a bare string in some versions,
/// so a string is read as a one-item list; null is an empty list. Anything else is refused.
std::expected<QStringList, QString> stringList(const QJsonValue& value, const QString& what) {
    if (value.isNull() || value.isUndefined()) {
        return QStringList{};
    }
    if (value.isString()) {
        return QStringList{value.toString()};
    }
    if (!value.isArray()) {
        return std::unexpected(QStringLiteral("%1 is not a list").arg(what));
    }
    QStringList items;
    for (const QJsonValue& item : value.toArray()) {
        if (!item.isString()) {
            return std::unexpected(QStringLiteral("%1 holds a value that is not text").arg(what));
        }
        items << item.toString();
    }
    return items;
}

/// Read one list and check every entry with @p valid.
std::expected<QStringList, QString> checkedList(const QJsonObject& obj,
                                                const char* key,
                                                const QString& what,
                                                const std::function<bool(const QString&)>& valid) {
    auto items = stringList(obj.value(QLatin1String(key)), what);
    if (!items) {
        return items;
    }
    for (const QString& item : *items) {
        if (!valid(item)) {
            return std::unexpected(
                QStringLiteral("%1 holds '%2', which is not valid").arg(what, item));
        }
    }
    return items;
}

std::expected<QList<StaticGateway>, QString> staticGateways(const QJsonValue& value,
                                                            IpFamily family,
                                                            const QString& what) {
    QList<StaticGateway> gateways;
    const QJsonArray entries = value.isObject() ? QJsonArray{value} : value.toArray();
    for (const QJsonValue& entry : entries) {
        const QJsonObject obj = entry.toObject();
        const QString next_hop = obj.value(QStringLiteral("next_hop")).toString();
        const QJsonValue metric = obj.value(QStringLiteral("metric"));
        if (!isFamilyAddress(family, next_hop) || !metric.isDouble() || metric.toInt() < 0) {
            return std::unexpected(
                QStringLiteral("%1 holds a gateway that is not valid").arg(what));
        }
        gateways.append(
            {next_hop, metric.toInt(), obj.value(QStringLiteral("automatic")).toBool()});
    }
    return gateways;
}

std::expected<IpFamilySettings, QString> parseFamily(const QJsonValue& value,
                                                     IpFamily family,
                                                     const QString& adapter) {
    const QString what = QStringLiteral("Adapter '%1' %2").arg(adapter, familyName(family));
    if (!value.isObject()) {
        return std::unexpected(QStringLiteral("%1 settings are missing").arg(what));
    }
    const QJsonObject obj = value.toObject();
    const auto with_prefix = [family](const QString& v) {
        return isFamilyAddressWithPrefix(family, v);
    };
    const auto plain = [family](const QString& v) {
        return isFamilyAddress(family, v);
    };
    IpFamilySettings settings;
    settings.present = obj.value(QStringLiteral("present")).toBool();
    settings.dhcp = obj.value(QStringLiteral("dhcp")).toBool();
    const auto addresses =
        checkedList(obj, "addresses", what + QStringLiteral(" addresses"), with_prefix);
    const auto statics =
        checkedList(obj, "static_addresses", what + QStringLiteral(" addresses"), with_prefix);
    const auto gateways = checkedList(obj, "gateways", what + QStringLiteral(" gateways"), plain);
    const auto dns = checkedList(obj, "dns_servers", what + QStringLiteral(" DNS"), plain);
    const auto static_dns =
        checkedList(obj, "static_dns_servers", what + QStringLiteral(" DNS"), plain);
    const auto static_gw =
        staticGateways(obj.value(QStringLiteral("static_gateways")), family, what);
    for (const auto* list : {&addresses, &statics, &gateways, &dns, &static_dns}) {
        if (!list->has_value()) {
            return std::unexpected(list->error());
        }
    }
    if (!static_gw) {
        return std::unexpected(static_gw.error());
    }
    settings.addresses = *addresses;
    settings.static_addresses = *statics;
    settings.gateways = *gateways;
    settings.dns_servers = *dns;
    settings.static_dns_servers = *static_dns;
    settings.static_gateways = *static_gw;
    return settings;
}

std::expected<AdapterSettings, QString> parseAdapter(const QJsonValue& value) {
    const QJsonObject obj = value.toObject();
    AdapterSettings adapter;
    adapter.name = obj.value(QStringLiteral("name")).toString().trimmed();
    if (adapter.name.isEmpty()) {
        return std::unexpected(QStringLiteral("An adapter in the capture has no name"));
    }
    adapter.description = obj.value(QStringLiteral("description")).toString();
    adapter.status = obj.value(QStringLiteral("status")).toString();
    auto ipv4 = parseFamily(obj.value(QStringLiteral("ipv4")), IpFamily::V4, adapter.name);
    if (!ipv4) {
        return std::unexpected(ipv4.error());
    }
    auto ipv6 = parseFamily(obj.value(QStringLiteral("ipv6")), IpFamily::V6, adapter.name);
    if (!ipv6) {
        return std::unexpected(ipv6.error());
    }
    adapter.ipv4 = *ipv4;
    adapter.ipv6 = *ipv6;
    return adapter;
}

// -- Serialising --------------------------------------------------------------------------------

QJsonArray toArray(const QStringList& items) {
    return QJsonArray::fromStringList(items);
}

QJsonObject familyToJson(const IpFamilySettings& family) {
    QJsonArray gateways;
    for (const StaticGateway& gateway : family.static_gateways) {
        gateways.append(QJsonObject{{QStringLiteral("next_hop"), gateway.next_hop},
                                    {QStringLiteral("metric"), gateway.metric},
                                    {QStringLiteral("automatic"), gateway.automatic}});
    }
    return {{QStringLiteral("present"), family.present},
            {QStringLiteral("dhcp"), family.dhcp},
            {QStringLiteral("addresses"), toArray(family.addresses)},
            {QStringLiteral("static_addresses"), toArray(family.static_addresses)},
            {QStringLiteral("gateways"), toArray(family.gateways)},
            {QStringLiteral("static_gateways"), gateways},
            {QStringLiteral("dns_servers"), toArray(family.dns_servers)},
            {QStringLiteral("static_dns_servers"), toArray(family.static_dns_servers)}};
}

// -- Describing ---------------------------------------------------------------------------------

QStringList nextHops(const QList<StaticGateway>& gateways) {
    QStringList hops;
    for (const StaticGateway& gateway : gateways) {
        hops << gateway.next_hop;
    }
    return hops;
}

QString describeFamily(const IpFamilySettings& family, IpFamily which) {
    QStringList parts;
    if (!family.dhcp && !family.static_addresses.isEmpty()) {
        parts << family.static_addresses.join(QStringLiteral(", "));
    }
    if (!family.static_gateways.isEmpty()) {
        parts << QStringLiteral("gateway %1")
                     .arg(nextHops(family.static_gateways).join(QStringLiteral(", ")));
    }
    if (!family.static_dns_servers.isEmpty()) {
        parts << QStringLiteral("DNS %1").arg(family.static_dns_servers.join(QStringLiteral(", ")));
    }
    if (parts.isEmpty()) {
        return {};
    }
    const QString text = familyName(which) + QLatin1Char(' ') + parts.join(QStringLiteral(", "));
    return family.dhcp ? text + QStringLiteral(" (address from DHCP)") : text;
}

}  // namespace

// -- Types --------------------------------------------------------------------------------------

bool IpFamilySettings::hasStaticSettings() const {
    return (!dhcp && !static_addresses.isEmpty()) || !static_gateways.isEmpty() ||
           !static_dns_servers.isEmpty();
}

bool AdapterSettings::isUp() const {
    return status == QLatin1String(kStatusUp);
}

bool AdapterSettings::isDisabled() const {
    return status == QLatin1String(kStatusDisabled);
}

bool AdapterSettings::hasStaticSettings() const {
    return ipv4.hasStaticSettings() || ipv6.hasStaticSettings();
}

const AdapterSettings* NetworkSettingsCapture::find(const QString& name) const {
    const auto found = std::ranges::find_if(adapters, [&name](const AdapterSettings& adapter) {
        return adapter.name.compare(name, Qt::CaseInsensitive) == 0;
    });
    return found == adapters.cend() ? nullptr : &*found;
}

// -- Capture ------------------------------------------------------------------------------------

QString networkCaptureScript() {
    return QString::fromLatin1(kCaptureScript);
}

std::expected<NetworkSettingsCapture, QString> parseNetworkCapture(const QString& json) {
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json.trimmed().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return std::unexpected(QStringLiteral("The adapter settings scan did not return JSON"));
    }
    const QJsonObject root = doc.object();
    if (root.value(QLatin1String(kCaptureMarkerKey)).toInt() != kCaptureMarkerValue ||
        !root.value(QStringLiteral("adapters")).isArray()) {
        return std::unexpected(
            QStringLiteral("The adapter settings scan did not finish (its marker is missing)"));
    }
    NetworkSettingsCapture capture;
    for (const QJsonValue& entry : root.value(QStringLiteral("adapters")).toArray()) {
        auto adapter = parseAdapter(entry);
        if (!adapter) {
            return std::unexpected(adapter.error());
        }
        capture.adapters.append(*adapter);
    }
    return capture;
}

std::expected<NetworkSettingsCapture, QString> captureNetworkSettings(
    int timeout_ms, const CancelCheck& should_cancel) {
    const ProcessResult proc = runPowerShell(networkCaptureScript(),
                                             timeout_ms,
                                             /*no_profile=*/true,
                                             /*bypass_policy=*/true,
                                             should_cancel);
    if (proc.timed_out) {
        return std::unexpected(QStringLiteral("Reading the adapters' settings timed out"));
    }
    if (proc.exit_code != 0) {
        const QString reason = proc.std_err.trimmed().section(QLatin1Char('\n'), 0, 0);
        return std::unexpected(
            QStringLiteral("Reading the adapters' settings failed (exit %1)%2")
                .arg(proc.exit_code)
                .arg(reason.isEmpty() ? QString() : QStringLiteral(": ") + reason));
    }
    auto capture = parseNetworkCapture(proc.std_out);
    if (capture) {
        capture->captured_at = QDateTime::currentDateTime().toString(Qt::ISODate);
        capture->computer_name = QSysInfo::machineHostName();
    }
    return capture;
}

QJsonObject networkCaptureToJson(const NetworkSettingsCapture& capture) {
    QJsonArray adapters;
    for (const AdapterSettings& adapter : capture.adapters) {
        adapters.append(QJsonObject{{QStringLiteral("name"), adapter.name},
                                    {QStringLiteral("description"), adapter.description},
                                    {QStringLiteral("status"), adapter.status},
                                    {QStringLiteral("ipv4"), familyToJson(adapter.ipv4)},
                                    {QStringLiteral("ipv6"), familyToJson(adapter.ipv6)}});
    }
    return {{QLatin1String(kCaptureMarkerKey), kCaptureMarkerValue},
            {QStringLiteral("purpose"),
             QStringLiteral("Every adapter's settings, recorded by S.A.K. Utility before Reset "
                            "Network. static_* values were typed in by hand.")},
            {QStringLiteral("captured_at"), capture.captured_at},
            {QStringLiteral("computer_name"), capture.computer_name},
            {QStringLiteral("adapters"), adapters}};
}

QString networkBackupDirectory() {
    const QString root = app_paths::dataRoot();
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("network-backups"));
}

std::expected<QString, QString> saveNetworkCapture(const NetworkSettingsCapture& capture,
                                                   const QString& directory) {
    if (directory.trimmed().isEmpty() || !QDir().mkpath(directory)) {
        return std::unexpected(
            QStringLiteral("Cannot create the folder for the network settings backup: %1")
                .arg(directory));
    }
    const QString name =
        QStringLiteral("network-settings-before-reset-%1.json")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    const QString path = QDir(directory).filePath(name);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("Cannot write the network settings backup %1: %2")
                                   .arg(QDir::toNativeSeparators(path), file.errorString()));
    }
    const QByteArray bytes = QJsonDocument(networkCaptureToJson(capture)).toJson();
    if (!writeFully(file, bytes) || !file.commit()) {
        return std::unexpected(QStringLiteral("Could not finish writing the network settings "
                                              "backup %1")
                                   .arg(QDir::toNativeSeparators(path)));
    }
    return QDir::toNativeSeparators(path);
}

QString describeStaticSettings(const AdapterSettings& adapter) {
    QStringList parts;
    for (const auto& [family, which] :
         {std::pair{&adapter.ipv4, IpFamily::V4}, std::pair{&adapter.ipv6, IpFamily::V6}}) {
        const QString text = describeFamily(*family, which);
        if (!text.isEmpty()) {
            parts << text;
        }
    }
    return parts.join(QStringLiteral("; "));
}

QString resetConfirmationDetail(const NetworkSettingsCapture& capture) {
    QStringList by_hand;
    QStringList disabled;
    bool any_dhcp = false;
    for (const AdapterSettings& adapter : capture.adapters) {
        if (adapter.isDisabled()) {
            disabled << adapter.name;
        } else if (adapter.hasStaticSettings()) {
            by_hand
                << QStringLiteral("  %1: %2").arg(adapter.name, describeStaticSettings(adapter));
        }
        any_dhcp = any_dhcp || (adapter.ipv4.present && adapter.ipv4.dhcp);
    }
    QStringList paragraphs;
    if (!by_hand.isEmpty()) {
        paragraphs << QStringLiteral(
                          "The reset clears settings typed in by hand. S.A.K. saves them first, "
                          "puts them back after the reset, and checks them:\n") +
                          by_hand.join(QLatin1Char('\n'));
    }
    if (any_dhcp) {
        paragraphs << QStringLiteral(
            "Adapters that get their address automatically (DHCP) keep doing so.");
    }
    if (!disabled.isEmpty()) {
        paragraphs << QStringLiteral(
                          "Turned off: %1. Windows does not show a turned-off adapter's settings, "
                          "so S.A.K. cannot save them. If one has settings typed in by hand, note "
                          "them before you reset.")
                          .arg(disabled.join(QStringLiteral(", ")));
    }
    return paragraphs.join(QStringLiteral("\n\n"));
}

}  // namespace sak
