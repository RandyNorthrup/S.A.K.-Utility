// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ethernet_config_manager.cpp
/// @brief Backup and restore of Ethernet adapter IP/DNS/gateway settings

#include "sak/ethernet_config_manager.h"

#include "sak/io_write_utils.h"
#include "sak/process_runner.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QTextStream>

namespace sak {

namespace {
constexpr int kAdapterTypeCaptureGroup = 3;
constexpr int kAdapterNameCaptureGroup = 4;

// 4 MiB. An Ethernet config backup is a few hundred bytes; a larger file is not a trusted backup
// and must be rejected before it is read into memory (self-DoS guard).
constexpr qint64 kMaxBackupFileBytes = 4LL * 1024 * 1024;

// Accepts only a dotted-quad IPv4 literal. QHostAddress rejects hostnames and IPv6, so a malformed
// captured field fails closed here before it can reach a live netsh apply.
bool isIpv4Literal(const QString& value) {
    QHostAddress addr;
    return addr.setAddress(value) && addr.protocol() == QAbstractSocket::IPv4Protocol;
}

// Every captured IPv4 field that is PRESENT must be a well-formed dotted-quad; empty optional
// fields (gateway, DNS, and the whole set on the DHCP path) are legitimate and are not rejected.
bool ipv4FieldsWellFormed(const EthernetConfigSnapshot& snap) {
    if (!snap.ipv4Address.isEmpty() && !isIpv4Literal(snap.ipv4Address)) {
        return false;
    }
    if (!snap.ipv4SubnetMask.isEmpty() && !isIpv4Literal(snap.ipv4SubnetMask)) {
        return false;
    }
    if (!snap.ipv4Gateway.isEmpty() && !isIpv4Literal(snap.ipv4Gateway)) {
        return false;
    }
    for (const QString& dns : snap.ipv4DnsServers) {
        if (!isIpv4Literal(dns)) {
            return false;
        }
    }
    return true;
}
}  // namespace

// -- EthernetConfigSnapshot --------------------------------------------------

QJsonObject EthernetConfigSnapshot::toJson() const {
    QJsonObject obj;
    obj["adapterName"] = adapterName;
    obj["description"] = description;
    obj["macAddress"] = macAddress;
    obj["dhcpEnabled"] = dhcpEnabled;
    obj["ipv4Address"] = ipv4Address;
    obj["ipv4SubnetMask"] = ipv4SubnetMask;
    obj["ipv4Gateway"] = ipv4Gateway;
    obj["backupTimestamp"] = backupTimestamp;
    obj["computerName"] = computerName;

    QJsonArray dnsArray;
    for (const auto& dns : ipv4DnsServers) {
        dnsArray.append(dns);
    }
    obj["ipv4DnsServers"] = dnsArray;

    return obj;
}

EthernetConfigSnapshot EthernetConfigSnapshot::fromJson(const QJsonObject& obj) {
    // Deserializes whatever is on disk, including an empty or truncated object;
    // the caller decides with isValid(). No assert on obj: a corrupt or missing
    // backup file is DATA, not a programming error, and must never abort the
    // process in a Debug build when Release handles it.
    EthernetConfigSnapshot snap;
    snap.adapterName = obj["adapterName"].toString();
    snap.description = obj["description"].toString();
    snap.macAddress = obj["macAddress"].toString();
    snap.dhcpEnabled = obj["dhcpEnabled"].toBool();
    snap.ipv4Address = obj["ipv4Address"].toString();
    snap.ipv4SubnetMask = obj["ipv4SubnetMask"].toString();
    snap.ipv4Gateway = obj["ipv4Gateway"].toString();
    snap.backupTimestamp = obj["backupTimestamp"].toString();
    snap.computerName = obj["computerName"].toString();

    const auto dnsArray = obj["ipv4DnsServers"].toArray();
    for (const auto& val : dnsArray) {
        snap.ipv4DnsServers.append(val.toString());
    }

    return snap;
}

bool EthernetConfigSnapshot::isValid() const {
    if (adapterName.isEmpty() || backupTimestamp.isEmpty()) {
        return false;
    }
    // Require a meaningful captured configuration so a localized/failed parse (which recognized no
    // fields, leaving dhcpEnabled=false AND ipv4Address empty) fails closed instead of persisting
    // or restoring an empty backup.
    if (!dhcpEnabled && ipv4Address.isEmpty()) {
        return false;
    }
    // Reject malformed IPv4 values before they can reach a live netsh apply: restoreStaticIp sets
    // the address before DNS, so a malformed field would otherwise leave a partial reconfiguration.
    return ipv4FieldsWellFormed(*this);
}

// -- EthernetConfigManager ---------------------------------------------------

EthernetConfigManager::EthernetConfigManager(QObject* parent) : QObject(parent) {}

QString EthernetConfigManager::lookupAdapterMac(const QString& adapterName) {
    if (adapterName.isEmpty()) {
        return {};
    }
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.humanReadableName() == adapterName || iface.name() == adapterName) {
            return iface.hardwareAddress();  // "AA:BB:CC:DD:EE:FF", or empty for virtual adapters
        }
    }
    return {};
}

EthernetConfigSnapshot EthernetConfigManager::captureSettings(const QString& adapterName) {
    // An unknown or empty adapter name produces no netsh output and is reported
    // below as a failed capture. No assert: see fromJson.
    Q_EMIT logOutput(QString("Capturing settings for adapter: %1").arg(adapterName));

    const QString output =
        runNetsh({"interface", "ip", "show", "config", QString("name=%1").arg(adapterName)});

    if (output.isEmpty()) {
        Q_EMIT errorOccurred(
            QString("Failed to read configuration for adapter: %1").arg(adapterName));
        return {};
    }

    auto snapshot = parseNetshConfig(output, adapterName);
    snapshot.backupTimestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    snapshot.computerName = QSysInfo::machineHostName();

    // Populate the MAC from QNetworkInterface. The previous code ran
    // `netsh interface show interface` (which does not even return a MAC) and then
    // DISCARDED its output, leaving macAddress permanently empty.
    snapshot.macAddress = lookupAdapterMac(adapterName);
    if (snapshot.adapterName.isEmpty()) {
        snapshot.adapterName = adapterName;
    }

    Q_EMIT logOutput(QString("Captured: DHCP=%1, IP=%2, Gateway=%3, DNS=%4")
                         .arg(snapshot.dhcpEnabled ? "Yes" : "No",
                              snapshot.ipv4Address,
                              snapshot.ipv4Gateway,
                              snapshot.ipv4DnsServers.join(", ")));

    return snapshot;
}

bool EthernetConfigManager::saveToFile(const EthernetConfigSnapshot& snapshot,
                                       const QString& filePath) {
    if (!snapshot.isValid()) {
        Q_EMIT errorOccurred("Cannot save invalid snapshot.");
        return false;
    }

    // QSaveFile stages to a temporary and renames on commit(), so a failed or
    // short write never truncates a pre-existing backup (the previous QFile
    // opened WriteOnly, truncating the target before writing) (B9-12).
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QString("Cannot write to file: %1").arg(filePath));
        return false;
    }

    const QJsonDocument doc(snapshot.toJson());
    if (!sak::writeFully(file, doc.toJson(QJsonDocument::Indented))) {
        file.cancelWriting();
        Q_EMIT errorOccurred(QString("Incomplete write to file: %1").arg(filePath));
        return false;
    }
    if (!file.commit()) {
        Q_EMIT errorOccurred(QString("Could not finalize file: %1").arg(filePath));
        return false;
    }

    Q_EMIT logOutput(QString("Settings saved to: %1").arg(filePath));
    return true;
}

EthernetConfigSnapshot EthernetConfigManager::loadFromFile(const QString& filePath) {
    // An empty path fails the open below and is reported. No assert.
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QString("Cannot read file: %1").arg(filePath));
        return {};
    }

    // Reject an oversized file before readAll() pulls it into memory: a backup is a few hundred
    // bytes, so anything past the cap is not a trusted configuration and must fail closed.
    const qint64 fileSize = file.size();
    if (fileSize > kMaxBackupFileBytes) {
        Q_EMIT errorOccurred(
            QString("Backup file is too large to be a valid configuration (%1 bytes).")
                .arg(fileSize));
        return {};
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        Q_EMIT errorOccurred(
            QString("Invalid JSON in backup file: %1").arg(parseError.errorString()));
        return {};
    }

    if (!doc.isObject()) {
        Q_EMIT errorOccurred("Backup file does not contain a valid configuration object.");
        return {};
    }

    auto snapshot = EthernetConfigSnapshot::fromJson(doc.object());
    if (!snapshot.isValid()) {
        Q_EMIT errorOccurred("Backup file contains incomplete configuration data.");
        return {};
    }

    Q_EMIT logOutput(QString("Loaded backup from: %1 (captured on %2 at %3)")
                         .arg(filePath, snapshot.computerName, snapshot.backupTimestamp));
    return snapshot;
}

bool EthernetConfigManager::restoreDhcpMode(const QString& adapterName, bool* dnsApplied) {
    Q_EMIT logOutput("Setting adapter to DHCP mode...");
    bool ok = false;
    const QString result = runNetsh(
        {"interface", "ip", "set", "address", QString("name=%1").arg(adapterName), "source=dhcp"},
        &ok);
    if (!ok || result.contains("error", Qt::CaseInsensitive)) {
        Q_EMIT errorOccurred(QString("Failed to set DHCP: %1").arg(result));
        return false;
    }

    // Switch DNS to automatic too. netsh returns a NON-ZERO exit for the benign "DNS is already
    // automatic" no-op and prints a message that never contains "error", so exit status alone
    // would false-close a restore that already reached the DHCP-DNS target state. Treat the DNS
    // switch as failed only on a genuine netsh error, or when the command could not run at all
    // (empty output + non-success) -- both leave DNS pinned to the old static servers, so the
    // restore is genuinely partial and must not be reported as a full success.
    bool dnsCmdOk = false;
    const QString dnsResult = runNetsh({"interface",
                                        "ip",
                                        "set",
                                        "dnsservers",
                                        QString("name=%1").arg(adapterName),
                                        "source=dhcp"},
                                       &dnsCmdOk);
    const bool dnsFailed = dnsResult.contains("error", Qt::CaseInsensitive) ||
                           (!dnsCmdOk && dnsResult.isEmpty());
    if (dnsApplied != nullptr) {
        *dnsApplied = !dnsFailed;
    }
    if (dnsFailed) {
        Q_EMIT errorOccurred(QString("Failed to set DNS to automatic (DHCP): %1").arg(dnsResult));
        return false;
    }
    return true;
}

bool EthernetConfigManager::restoreStaticIp(const EthernetConfigSnapshot& snapshot,
                                            const QString& adapterName) {
    Q_EMIT logOutput(QString("Setting static IP: %1 / %2 / %3")
                         .arg(snapshot.ipv4Address, snapshot.ipv4SubnetMask, snapshot.ipv4Gateway));

    QStringList addressArgs = {"interface",
                               "ip",
                               "set",
                               "address",
                               QString("name=%1").arg(adapterName),
                               "source=static",
                               QString("addr=%1").arg(snapshot.ipv4Address),
                               QString("mask=%1").arg(snapshot.ipv4SubnetMask)};

    if (!snapshot.ipv4Gateway.isEmpty()) {
        addressArgs << QString("gateway=%1").arg(snapshot.ipv4Gateway) << "gwmetric=0";
    }

    bool ok = false;
    const QString result = runNetsh(addressArgs, &ok);
    if (!ok || result.contains("error", Qt::CaseInsensitive)) {
        Q_EMIT errorOccurred(QString("Failed to set IP address: %1").arg(result));
        return false;
    }
    return true;
}

bool EthernetConfigManager::restoreDnsServers(const EthernetConfigSnapshot& snapshot,
                                              const QString& adapterName,
                                              bool* primaryApplied) {
    if (snapshot.ipv4DnsServers.isEmpty()) {
        return true;
    }

    Q_EMIT logOutput(QString("Setting primary DNS: %1").arg(snapshot.ipv4DnsServers.first()));

    bool ok = false;
    QString result = runNetsh({"interface",
                               "ip",
                               "set",
                               "dnsservers",
                               QString("name=%1").arg(adapterName),
                               "source=static",
                               QString("addr=%1").arg(snapshot.ipv4DnsServers.first()),
                               "register=primary"},
                              &ok);
    if (!ok || result.contains("error", Qt::CaseInsensitive)) {
        Q_EMIT errorOccurred(QString("Failed to set primary DNS: %1").arg(result));
        return false;
    }

    // Primary DNS is now LIVE: the `set dnsservers source=static` above replaced the adapter's
    // entire DNS list. A caller must be told this even if a secondary `add` fails next.
    if (primaryApplied != nullptr) {
        *primaryApplied = true;
    }

    for (int idx = 1; idx < snapshot.ipv4DnsServers.size(); ++idx) {
        Q_EMIT logOutput(QString("Adding DNS server: %1").arg(snapshot.ipv4DnsServers[idx]));

        ok = false;
        result = runNetsh({"interface",
                           "ip",
                           "add",
                           "dnsservers",
                           QString("name=%1").arg(adapterName),
                           QString("addr=%1").arg(snapshot.ipv4DnsServers[idx]),
                           QString("index=%1").arg(idx + 1)},
                          &ok);
        if (!ok || result.contains("error", Qt::CaseInsensitive)) {
            Q_EMIT errorOccurred(QString("Failed to add DNS server %1: %2")
                                     .arg(snapshot.ipv4DnsServers[idx], result));
            return false;
        }
    }
    return true;
}

bool EthernetConfigManager::restoreSettings(const EthernetConfigSnapshot& snapshot,
                                            const QString& adapterName,
                                            bool* dnsApplied) {
    if (!snapshot.isValid()) {
        Q_EMIT errorOccurred("Cannot restore from invalid snapshot.");
        return false;
    }

    Q_EMIT logOutput(QString("Restoring settings to adapter: %1").arg(adapterName));
    Q_EMIT logOutput(
        QString("Source: %1 (backed up from %2 on %3)")
            .arg(snapshot.adapterName, snapshot.computerName, snapshot.backupTimestamp));

    bool allSucceeded = true;
    if (snapshot.dhcpEnabled) {
        // DHCP restore: both the IP and the DNS come from DHCP. Applying the
        // captured servers as STATIC afterward would override the automatic DNS
        // that restoreDhcpMode just set, leaving a "DHCP" restore pinned to
        // static DNS. So the static DNS leg runs ONLY on the static path (B9-07).
        allSucceeded = restoreDhcpMode(adapterName, dnsApplied);
    } else {
        allSucceeded = restoreStaticIp(snapshot, adapterName);
        bool dnsPrimaryApplied = false;
        if (!restoreDnsServers(snapshot, adapterName, &dnsPrimaryApplied)) {
            allSucceeded = false;
        }
        if (dnsApplied != nullptr) {
            // No captured servers -> nothing to apply; otherwise report whether
            // the primary DNS actually went live.
            *dnsApplied = snapshot.ipv4DnsServers.isEmpty() ? true : dnsPrimaryApplied;
        }
    }

    if (allSucceeded) {
        Q_EMIT logOutput("All settings restored successfully.");
    } else {
        Q_EMIT logOutput("Some settings failed to restore. Check errors above.");
    }

    return allSucceeded;
}

bool EthernetConfigManager::setDnsServers(const QString& adapterName,
                                          const QStringList& dnsServers,
                                          bool& primaryApplied) {
    // Reuse the DNS-only restore leg: it issues `netsh interface ip set dnsservers source=static`
    // for the primary plus `add dnsservers` for each secondary, and touches nothing else -- so the
    // adapter's IP mode (DHCP or static) is preserved. An empty list is a no-op (returns true); the
    // caller (network.set_adapter_dns) requires a non-empty list. primaryApplied reports whether
    // the primary DNS went live, so a partial failure (primary set, a secondary add failed) is
    // honest.
    primaryApplied = false;
    EthernetConfigSnapshot snapshot;
    snapshot.adapterName = adapterName;
    snapshot.ipv4DnsServers = dnsServers;
    return restoreDnsServers(snapshot, adapterName, &primaryApplied);
}

QStringList EthernetConfigManager::listEthernetAdapters() {
    QStringList adapters;

    const QString output = runNetsh({"interface", "show", "interface"});
    if (output.isEmpty()) {
        return adapters;
    }

    // Parse netsh output: columns are Admin State, State, Type, Interface Name
    // Skip header lines (first 3 lines typically)
    const auto lines = output.split('\n', Qt::SkipEmptyParts);
    bool pastHeader = false;

    for (const auto& line : lines) {
        // Detect separator line (dashes)
        if (line.contains("---")) {
            pastHeader = true;
            continue;
        }
        if (!pastHeader) {
            continue;
        }

        // Parse columns -- name is the last field, may contain spaces
        // Format: "Enabled  Connected  Dedicated  Ethernet"
        static const QRegularExpression kAdapterLineRe(
            R"(^\s*(Enabled|Disabled)\s+(Connected|Disconnected|Not Present)\s+(\S+)\s+(.+)$)");

        auto match = kAdapterLineRe.match(line.trimmed());
        if (match.hasMatch()) {
            const QString adapterType = match.captured(kAdapterTypeCaptureGroup);
            const QString adapterName = match.captured(kAdapterNameCaptureGroup).trimmed();
            // Include Dedicated (Ethernet) adapters
            if (adapterType == "Dedicated") {
                adapters.append(adapterName);
            }
        }
    }

    return adapters;
}

// -- Private -----------------------------------------------------------------

QString EthernetConfigManager::runNetsh(const QStringList& args, bool* ok) {
    // Empty arguments are REFUSED, not asserted. This launches netsh elevated,
    // so "abort in Debug, run netsh with no arguments in Release" is the wrong
    // pair of behaviours; failing closed is the same answer in both.
    if (args.isEmpty()) {
        if (ok != nullptr) {
            *ok = false;
        }
        return {};
    }

    // System32-qualified netsh, never the bare name: adapter configuration runs elevated,
    // and CreateProcess searches the current directory ahead of System32. Fail closed when
    // it cannot be resolved instead of launching whatever PATH/CWD supplies.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        if (ok != nullptr) {
            *ok = false;
        }
        Q_EMIT errorOccurred(QStringLiteral("Cannot resolve the System32 netsh.exe path"));
        return {};
    }

    const auto result = sak::runProcess(netsh_exe, args, 10'000);
    // Propagate the authoritative exit status: netsh reports failures (elevation, bad adapter) on
    // stdout with a zero-length stderr, so callers cannot rely on the "error" substring alone.
    if (ok != nullptr) {
        *ok = result.succeeded();
    }
    if (result.timed_out) {
        Q_EMIT errorOccurred(QString("netsh command timed out: netsh %1").arg(args.join(' ')));
        return {};
    }

    if (result.exit_code != 0) {
        const QString errOutput = result.std_err.trimmed();
        if (!errOutput.isEmpty()) {
            Q_EMIT errorOccurred(QString("netsh error: %1").arg(errOutput));
        }
    }

    return result.std_out;
}

EthernetConfigSnapshot EthernetConfigManager::parseNetshConfig(const QString& output,
                                                               const QString& adapterName) {
    EthernetConfigSnapshot snap;
    snap.adapterName = adapterName;

    const auto lines = output.split('\n');

    for (const auto& rawLine : lines) {
        const QString line = rawLine.trimmed();

        if (line.startsWith("DHCP enabled:", Qt::CaseInsensitive)) {
            snap.dhcpEnabled = line.contains("Yes", Qt::CaseInsensitive);
        } else if (line.startsWith("IP Address:", Qt::CaseInsensitive)) {
            snap.ipv4Address = line.mid(line.indexOf(':') + 1).trimmed();
        } else if (line.startsWith("Subnet Prefix:", Qt::CaseInsensitive)) {
            // Format: "Subnet Prefix:  192.168.1.0/24 (mask 255.255.255.0)"
            static const QRegularExpression kMaskRe(R"(\(mask\s+([\d.]+)\))");
            auto match = kMaskRe.match(line);
            if (match.hasMatch()) {
                snap.ipv4SubnetMask = match.captured(1);
            }
        } else if (line.startsWith("Default Gateway:", Qt::CaseInsensitive)) {
            snap.ipv4Gateway = line.mid(line.indexOf(':') + 1).trimmed();
        } else if (line.startsWith("Statically Configured DNS Servers:", Qt::CaseInsensitive) ||
                   line.startsWith("DNS Servers configured through DHCP:", Qt::CaseInsensitive)) {
            const QString dns = line.mid(line.indexOf(':') + 1).trimmed();
            if (!dns.isEmpty()) {
                snap.ipv4DnsServers.append(dns);
            }
        }
    }

    // Subsequent lines after "DNS Servers" may contain additional DNS entries
    // (they appear as bare IP addresses on following lines)
    bool inDnsSection = false;
    static const QRegularExpression kIpRe(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$)");

    for (const auto& rawLine : lines) {
        const QString line = rawLine.trimmed();

        if (line.contains("DNS Servers", Qt::CaseInsensitive)) {
            inDnsSection = true;
            continue;
        }

        if (inDnsSection) {
            if (kIpRe.match(line).hasMatch()) {
                if (!snap.ipv4DnsServers.contains(line)) {
                    snap.ipv4DnsServers.append(line);
                }
            } else if (!line.isEmpty()) {
                inDnsSection = false;
            }
        }
    }

    return snap;
}

}  // namespace sak
