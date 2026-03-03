// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_share_browser.cpp
/// @brief SMB share discovery and access testing via NetShareEnum

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <lm.h>

#include "sak/network_share_browser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QUuid>

#pragma comment(lib, "netapi32.lib")

namespace sak {

namespace {
constexpr int kShareInfoLevel = 1; // SHARE_INFO_1 level

[[nodiscard]] NetworkShareInfo::ShareType mapShareType(DWORD type)
{
    switch (type & 0x0000FFFF) {
    case STYPE_DISKTREE:  return NetworkShareInfo::ShareType::Disk;
    case STYPE_PRINTQ:    return NetworkShareInfo::ShareType::Printer;
    case STYPE_DEVICE:    return NetworkShareInfo::ShareType::Device;
    case STYPE_IPC:       return NetworkShareInfo::ShareType::IPC;
    default:              return NetworkShareInfo::ShareType::Special;
    }
}

[[nodiscard]] bool isSpecialShare(const QString& name)
{
    return name.endsWith(QLatin1Char('$'));
}
} // namespace

NetworkShareBrowser::NetworkShareBrowser(QObject* parent)
    : QObject(parent)
{
}

void NetworkShareBrowser::cancel()
{
    m_cancelled.store(true);
}

void NetworkShareBrowser::discoverShares(const QString& hostname)
{
    m_cancelled.store(false);

    if (hostname.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Hostname cannot be empty"));
        return;
    }

    auto shares = enumerateShares(hostname);

    for (const auto& share : shares) {
        if (m_cancelled.load()) {
            break;
        }
        Q_EMIT shareDiscovered(share);
    }

    Q_EMIT discoveryComplete(shares);
}

void NetworkShareBrowser::testAccess(const QString& uncPath)
{
    m_cancelled.store(false);

    if (uncPath.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("UNC path cannot be empty"));
        return;
    }

    auto [canRead, canWrite] = testReadWriteAccess(uncPath);
    Q_EMIT accessTestComplete(uncPath, canRead, canWrite);
}

QVector<NetworkShareInfo> NetworkShareBrowser::enumerateShares(
    const QString& hostname)
{
    QVector<NetworkShareInfo> shares;

    const auto serverName = hostname.toStdWString();

    PSHARE_INFO_1 shareInfo = nullptr;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;

    NET_API_STATUS status = NetShareEnum(
        const_cast<LPWSTR>(serverName.c_str()),
        kShareInfoLevel,
        reinterpret_cast<LPBYTE*>(&shareInfo),
        MAX_PREFERRED_LENGTH,
        &entriesRead,
        &totalEntries,
        &resumeHandle);

    if (status != NERR_Success && status != ERROR_MORE_DATA) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to enumerate shares on %1 (error %2)")
                .arg(hostname)
                .arg(status));
        return shares;
    }

    if (shareInfo == nullptr) {
        return shares;
    }

    for (DWORD i = 0; i < entriesRead; ++i) {
        if (m_cancelled.load()) {
            break;
        }

        NetworkShareInfo info;
        info.hostName = hostname;
        info.shareName = QString::fromWCharArray(shareInfo[i].shi1_netname);
        info.uncPath = QStringLiteral("\\\\%1\\%2").arg(hostname, info.shareName);
        info.type = mapShareType(shareInfo[i].shi1_type);
        info.remark = QString::fromWCharArray(shareInfo[i].shi1_remark);
        info.requiresAuth = isSpecialShare(info.shareName);
        info.discovered = QDateTime::currentDateTime();

        // Test access for non-special disk shares
        if (info.type == NetworkShareInfo::ShareType::Disk &&
            !isSpecialShare(info.shareName)) {
            auto [canRead, canWrite] = testReadWriteAccess(info.uncPath);
            info.canRead = canRead;
            info.canWrite = canWrite;
        }

        shares.append(info);
    }

    NetApiBufferFree(shareInfo);
    return shares;
}

QPair<bool, bool> NetworkShareBrowser::testReadWriteAccess(const QString& uncPath)
{
    bool canRead = false;
    bool canWrite = false;

    // Test read access
    QDir dir(uncPath);
    if (dir.exists()) {
        canRead = true;

        // Try listing contents as another read test
        const auto entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot);
        (void)entries;
    }

    // Test write access by creating a temporary file
    if (canRead) {
        const QString testFile = uncPath + QStringLiteral("\\._sak_write_test_") +
                                  QUuid::createUuid().toString(QUuid::Id128).left(8) +
                                  QStringLiteral(".tmp");

        QFile file(testFile);
        if (file.open(QIODevice::WriteOnly)) {
            file.write("SAK write test");
            file.close();
            canWrite = true;

            // Clean up test file
            file.remove();
        }
    }

    return {canRead, canWrite};
}

} // namespace sak
