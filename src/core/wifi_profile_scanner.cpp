// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_profile_scanner.cpp
/// @brief Utility for scanning Windows WiFi profiles via netsh

#include "sak/wifi_profile_scanner.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace sak {

namespace {

/// @brief Run a netsh command with timeout, returning its stdout
QString runNetsh(const QStringList& args, int timeout_ms, const WifiScanLogger& logger) {
    Q_ASSERT(!args.isEmpty());
    Q_ASSERT(timeout_ms >= 0);

    const auto result = sak::runProcess(QStringLiteral("netsh"), args, timeout_ms);
    if (!result.succeeded()) {
        sak::logWarning("netsh failed/timed out: {}", args.join(' ').toStdString());
        if (logger) {
            logger(QStringLiteral("netsh timed out"));
        }
        return {};
    }
    return result.std_out;
}

/// @brief Read the single exported WLANProfile XML file from a directory (empty on any failure).
QString readSingleXmlFromDir(const QString& dir_path) {
    const QStringList xmls = QDir(dir_path).entryList(QStringList{"*.xml"}, QDir::Files);
    if (xmls.isEmpty()) {
        return {};
    }
    QFile file(QDir(dir_path).filePath(xmls.first()));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

/// @brief Export a WiFi profile to real re-importable WLANProfile XML via `netsh wlan export`.
/// @note Deliberately omits `key=clear`, so the exported keyMaterial is DPAPI-protected
///       (protected=true) rather than a plaintext PSK written to disk. Trade-off: DPAPI is
///       machine/user-bound, so a backup restored on a DIFFERENT PC yields a profile without a
///       usable key. Fails closed (returns empty) on any error.
QString exportWifiProfileXml(const QString& name, int timeout_ms, const WifiScanLogger& logger) {
    QTemporaryDir dir;
    if (!dir.isValid()) {
        return {};
    }
    const auto result =
        sak::runProcess(QStringLiteral("netsh"),
                        {"wlan", "export", "profile", "name=" + name, "folder=" + dir.path()},
                        timeout_ms);
    if (!result.succeeded()) {
        sak::logWarning("netsh wlan export failed: {}", name.toStdString());
        if (logger) {
            logger(QStringLiteral("netsh export failed"));
        }
        return {};
    }
    return readSingleXmlFromDir(dir.path());
}

}  // namespace

QStringList parseWifiProfileNames(const QString& output) {
    Q_ASSERT(!output.isEmpty());
    QStringList names;
    for (const QString& line : output.split('\n')) {
        const int colon_idx = line.indexOf(':');
        if (colon_idx < 0) {
            continue;
        }
        if (!line.contains("Profile", Qt::CaseInsensitive)) {
            continue;
        }
        const QString name = line.mid(colon_idx + 1).trimmed();
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

QString parseWifiSecurityType(const QString& detail_output) {
    Q_ASSERT(!detail_output.isEmpty());
    for (const QString& line : detail_output.split('\n')) {
        if (!line.contains("Authentication", Qt::CaseInsensitive)) {
            continue;
        }
        const int colon_idx = line.indexOf(':');
        if (colon_idx >= 0) {
            return line.mid(colon_idx + 1).trimmed();
        }
        break;
    }
    return {};
}

QVector<WifiProfileInfo> scanAllWifiProfiles(const WifiScanLogger& logger) {
    const QString list_output =
        runNetsh({"wlan", "show", "profiles"}, kTimeoutNetworkReadMs, logger);
    if (list_output.isEmpty()) {
        return {};
    }

    const QStringList profile_names = parseWifiProfileNames(list_output);
    QVector<WifiProfileInfo> profiles;
    profiles.reserve(profile_names.size());

    for (const QString& name : profile_names) {
        WifiProfileInfo info;
        info.profile_name = name;
        info.selected = true;

        const QString detail =
            runNetsh({"wlan", "show", "profile", "name=" + name}, kTimeoutWifiProfileMs, logger);
        if (!detail.isEmpty()) {
            info.security_type = parseWifiSecurityType(detail);
        }

        // Store real WLANProfile XML (DPAPI-protected key), NOT the `key=clear` console text --
        // that leaked the plaintext PSK into the backup and was not re-importable anyway.
        info.xml_data = exportWifiProfileXml(name, kTimeoutWifiProfileMs, logger);

        profiles.append(info);
    }

    return profiles;
}

}  // namespace sak
