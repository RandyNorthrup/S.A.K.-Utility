// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ethernet_config_manager.cpp
/// @brief Backup and restore of Ethernet adapter IP/DNS/gateway settings

#include "sak/ethernet_config_manager.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTextStream>

namespace sak {

// ── EthernetConfigSnapshot ──────────────────────────────────────────────────

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
    Q_ASSERT(!obj.isEmpty());
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
    return !adapterName.isEmpty() && !backupTimestamp.isEmpty();
}

// ── EthernetConfigManager ───────────────────────────────────────────────────

EthernetConfigManager::EthernetConfigManager(QObject* parent) : QObject(parent) {}

EthernetConfigSnapshot EthernetConfigManager::captureSettings(const QString& adapterName) {
    Q_ASSERT(!adapterName.isEmpty());
    Q_EMIT logOutput(QString("Capturing settings for adapter: %1").arg(adapterName));

    QString output =
        runNetsh({"interface", "ip", "show", "config", QString("name=%1").arg(adapterName)});

    if (output.isEmpty()) {
        Q_EMIT errorOccurred(
            QString("Failed to read configuration for adapter: %1").arg(adapterName));
        return {};
    }

    auto snapshot = parseNetshConfig(output, adapterName);
    snapshot.backupTimestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    snapshot.computerName = QSysInfo::machineHostName();

    // Get MAC address via separate netsh call
    QString macOutput =
        runNetsh({"interface", "show", "interface", QString("name=%1").arg(adapterName)});
    // MAC is typically available from getmac or adapter inspector
    // We'll use the adapter name as the identifier instead
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

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QString("Cannot write to file: %1").arg(filePath));
        return false;
    }

    QJsonDocument doc(snapshot.toJson());
    const QByteArray data = doc.toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        Q_EMIT errorOccurred(QString("Incomplete write to file: %1").arg(filePath));
        return false;
    }
    file.close();

    Q_EMIT logOutput(QString("Settings saved to: %1").arg(filePath));
    return true;
}

EthernetConfigSnapshot EthernetConfigManager::loadFromFile(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        Q_EMIT errorOccurred(QString("Cannot read file: %1").arg(filePath));
        return {};
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
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

bool EthernetConfigManager::restoreDhcpMode(const QString& adapterName) {
    Q_EMIT logOutput("Setting adapter to DHCP mode...");
    QString result = runNetsh(
        {"interface", "ip", "set", "address", QString("name=%1").arg(adapterName), "source=dhcp"});
    if (result.contains("error", Qt::CaseInsensitive)) {
        Q_EMIT errorOccurred(QString("Failed to set DHCP: %1").arg(result));
        return false;
    }

    static_cast<void>(runNetsh({"interface",
                                "ip",
                                "set",
                                "dnsservers",
                                QString("name=%1").arg(adapterName),
                                "source=dhcp"}));
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

    QString result = runNetsh(addressArgs);
    if (result.contains("error", Qt::CaseInsensitive)) {
        Q_EMIT errorOccurred(QString("Failed to set IP address: %1").arg(result));
        return false;
    }
    return true;
}

bool EthernetConfigManager::restoreDnsServers(const EthernetConfigSnapshot& snapshot,
                                              const QString& adapterName) {
    if (snapshot.ipv4DnsServers.isEmpty()) {
        return true;
    }

    Q_EMIT logOutput(QString("Setting primary DNS: %1").arg(snapshot.ipv4DnsServers.first()));

    QString result = runNetsh({"interface",
                               "ip",
                               "set",
                               "dnsservers",
                               QString("name=%1").arg(adapterName),
                               "source=static",
                               QString("addr=%1").arg(snapshot.ipv4DnsServers.first()),
                               "register=primary"});
    if (result.contains("error", Qt::CaseInsensitive)) {
        Q_EMIT errorOccurred(QString("Failed to set primary DNS: %1").arg(result));
        return false;
    }

    for (int idx = 1; idx < snapshot.ipv4DnsServers.size(); ++idx) {
        Q_EMIT logOutput(QString("Adding DNS server: %1").arg(snapshot.ipv4DnsServers[idx]));

        result = runNetsh({"interface",
                           "ip",
                           "add",
                           "dnsservers",
                           QString("name=%1").arg(adapterName),
                           QString("addr=%1").arg(snapshot.ipv4DnsServers[idx]),
                           QString("index=%1").arg(idx + 1)});
        if (result.contains("error", Qt::CaseInsensitive)) {
            Q_EMIT errorOccurred(QString("Failed to add DNS server %1: %2")
                                     .arg(snapshot.ipv4DnsServers[idx], result));
            return false;
        }
    }
    return true;
}

bool EthernetConfigManager::restoreSettings(const EthernetConfigSnapshot& snapshot,
                                            const QString& adapterName) {
    if (!snapshot.isValid()) {
        Q_EMIT errorOccurred("Cannot restore from invalid snapshot.");
        return false;
    }

    Q_EMIT logOutput(QString("Restoring settings to adapter: %1").arg(adapterName));
    Q_EMIT logOutput(
        QString("Source: %1 (backed up from %2 on %3)")
            .arg(snapshot.adapterName, snapshot.computerName, snapshot.backupTimestamp));

    bool allSucceeded = snapshot.dhcpEnabled ? restoreDhcpMode(adapterName)
                                             : restoreStaticIp(snapshot, adapterName);

    if (!restoreDnsServers(snapshot, adapterName)) {
        allSucceeded = false;
    }

    if (allSucceeded) {
        Q_EMIT logOutput("All settings restored successfully.");
    } else {
        Q_EMIT logOutput("Some settings failed to restore. Check errors above.");
    }

    return allSucceeded;
}

QStringList EthernetConfigManager::listEthernetAdapters() {
    QStringList adapters;

    QString output = runNetsh({"interface", "show", "interface"});
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

        // Parse columns — name is the last field, may contain spaces
        // Format: "Enabled  Connected  Dedicated  Ethernet"
        static const QRegularExpression kAdapterLineRe(
            R"(^\s*(Enabled|Disabled)\s+(Connected|Disconnected|Not Present)\s+(\S+)\s+(.+)$)");

        auto match = kAdapterLineRe.match(line.trimmed());
        if (match.hasMatch()) {
            QString adapterType = match.captured(3);
            QString adapterName = match.captured(4).trimmed();
            // Include Dedicated (Ethernet) adapters
            if (adapterType == "Dedicated") {
                adapters.append(adapterName);
            }
        }
    }

    return adapters;
}

// ── Private ─────────────────────────────────────────────────────────────────

QString EthernetConfigManager::runNetsh(const QStringList& args) {
    Q_ASSERT(!args.isEmpty());
    QProcess proc;
    proc.setProgram("netsh.exe");
    proc.setArguments(args);
    proc.start();

    if (!proc.waitForStarted(5000)) {
        Q_EMIT errorOccurred(QString("Failed to start netsh: netsh %1").arg(args.join(' ')));
        return {};
    }

    if (!proc.waitForFinished(10'000)) {
        Q_EMIT errorOccurred(QString("netsh command timed out: netsh %1").arg(args.join(' ')));
        return {};
    }

    if (proc.exitCode() != 0) {
        QString errOutput = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
        if (!errOutput.isEmpty()) {
            Q_EMIT errorOccurred(QString("netsh error: %1").arg(errOutput));
        }
    }

    return QString::fromLocal8Bit(proc.readAllStandardOutput());
}

EthernetConfigSnapshot EthernetConfigManager::parseNetshConfig(const QString& output,
                                                               const QString& adapterName) {
    EthernetConfigSnapshot snap;
    snap.adapterName = adapterName;

    const auto lines = output.split('\n');

    for (const auto& rawLine : lines) {
        QString line = rawLine.trimmed();

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
            QString dns = line.mid(line.indexOf(':') + 1).trimmed();
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
        QString line = rawLine.trimmed();

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
