// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wifi_profile_scanner.cpp
/// @brief Utility for scanning Windows WiFi profiles via netsh

#include "sak/wifi_profile_scanner.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QProcess>

namespace sak {

namespace {

constexpr int kKillWaitMs = 2000;

/// @brief Run a netsh command with timeout, returning its stdout
QString runNetsh(const QStringList& args, int timeout_ms, const WifiScanLogger& logger) {
    Q_ASSERT(!args.isEmpty());
    Q_ASSERT(timeout_ms >= 0);
    QProcess process;
    process.start("netsh", args);
    if (!process.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logError("netsh failed to start: {}", args.join(' ').toStdString());
        if (logger) {
            logger(QStringLiteral("Failed to start netsh"));
        }
        return {};
    }
    if (!process.waitForFinished(timeout_ms)) {
        sak::logWarning("netsh timed out: {}", args.join(' ').toStdString());
        process.kill();
        process.waitForFinished(kKillWaitMs);
        if (logger) {
            logger(QStringLiteral("netsh timed out"));
        }
        return {};
    }
    return QString::fromLocal8Bit(process.readAllStandardOutput());
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

        const QString xml_output =
            runNetsh({"wlan", "show", "profile", "name=" + name, "key=clear"},
                     kTimeoutWifiProfileMs,
                     logger);
        info.xml_data = xml_output;

        profiles.append(info);
    }

    return profiles;
}

}  // namespace sak
