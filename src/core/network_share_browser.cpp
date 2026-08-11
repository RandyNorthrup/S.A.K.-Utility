// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_share_browser.cpp
/// @brief SMB share discovery and access testing via NetShareEnum

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/network_share_browser.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTemporaryFile>
#include <QUuid>

#include <winsock2.h>

#include <windows.h>

#include <lm.h>

#pragma comment(lib, "netapi32.lib")

namespace sak {

namespace {
constexpr int kShareInfoLevel = 1;  // SHARE_INFO_1 level
constexpr DWORD kShareTypeMask = 0x00'00'FF'FF;
// First printable ASCII code point; anything below it is a C0 control character (including NUL).
constexpr char16_t kFirstPrintableCodePoint = 0x20;

// NetShareEnum is a blocking remote call whose result set is attacker-influenced. Bound the
// resume-handle loop and the accumulated share count so a server that keeps returning
// ERROR_MORE_DATA (or an implausibly huge share table) can neither spin forever nor exhaust
// memory; exceeding either bound fails the whole enumeration closed.
constexpr int kMaxEnumPages = 1024;
constexpr int kMaxTotalShares = 100'000;

// A share name delivered by a remote SMB server is untrusted. A legitimate share name is a
// single path component; one carrying a separator or traversal component would redirect a
// "\\host\name" probe away from the share root (writing/deleting under a different path).
[[nodiscard]] bool isSafeShareName(const QString& name) {
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    for (const QChar ch : name) {
        const char16_t code = ch.unicode();
        if (code < kFirstPrintableCodePoint || code == u'\\' || code == u'/' || code == u':') {
            return false;
        }
    }
    return true;
}

// A caller-supplied access-test target must be a real UNC share path ("\\server\share",
// optionally with a subpath) -- never a local path, a device path, or a bare string that could
// steer the write/delete probe at an unintended local object.
[[nodiscard]] bool isValidUncSharePath(const QString& path) {
    if (!path.startsWith(QLatin1String("\\\\"))) {
        return false;
    }
    for (const QChar ch : path) {
        if (ch.unicode() <
            kFirstPrintableCodePoint) {  // control characters, including embedded NUL
            return false;
        }
    }
    const QString rest = path.mid(2);
    const int sep = rest.indexOf(QLatin1Char('\\'));
    if (sep <= 0) {  // need a non-empty server followed by a share separator
        return false;
    }
    return !rest.mid(sep + 1).isEmpty();  // and a non-empty share component
}

[[nodiscard]] NetworkShareInfo::ShareType mapShareType(DWORD type) {
    switch (type & kShareTypeMask) {
    case STYPE_DISKTREE:
        return NetworkShareInfo::ShareType::Disk;
    case STYPE_PRINTQ:
        return NetworkShareInfo::ShareType::Printer;
    case STYPE_DEVICE:
        return NetworkShareInfo::ShareType::Device;
    case STYPE_IPC:
        return NetworkShareInfo::ShareType::IPC;
    default:
        return NetworkShareInfo::ShareType::Special;
    }
}

[[nodiscard]] bool isSpecialShare(const QString& name) {
    return name.endsWith(QLatin1Char('$'));
}

// A share host counts as "local" when no host is given, when it is "localhost", or when it is a
// loopback IP literal. Local enumeration is issued with a NULL server name so NetShareEnum reads
// the local machine directly -- no SMB round-trip, no credential exposure to any remote host.
[[nodiscard]] bool isLocalShareHost(const QString& hostname) {
    if (hostname.isEmpty() ||
        hostname.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    return QHostAddress(hostname).isLoopback();
}

struct ShareServer {
    QString displayHost;      // normalized host for UNC paths / messages
    std::wstring serverName;  // empty => enumerate the LOCAL machine (NULL server name)
};

// Normalize the target: a local host maps to an empty server name (pure local enumeration, no SMB
// round-trip). The display host keeps the caller's spelling when one was given (so an explicit
// "localhost"/loopback-literal request renders its own UNC paths); only a defaulted empty host is
// shown as "localhost". A remote host is passed through verbatim.
[[nodiscard]] ShareServer resolveShareServer(const QString& hostname) {
    if (isLocalShareHost(hostname)) {
        return {.displayHost = hostname.isEmpty() ? QStringLiteral("localhost") : hostname,
                .serverName = std::wstring()};
    }
    return {.displayHost = hostname, .serverName = hostname.toStdWString()};
}

// Build the model-facing record for one SHARE_INFO_1 entry (no access probe).
[[nodiscard]] NetworkShareInfo makeShareInfo(const SHARE_INFO_1& entry,
                                             const QString& displayHost) {
    NetworkShareInfo info;
    info.hostName = displayHost;
    info.shareName = QString::fromWCharArray(entry.shi1_netname);
    info.uncPath = QStringLiteral("\\\\%1\\%2").arg(displayHost, info.shareName);
    info.type = mapShareType(entry.shi1_type);
    info.remark = QString::fromWCharArray(entry.shi1_remark);
    info.requiresAuth = isSpecialShare(info.shareName);
    info.discovered = QDateTime::currentDateTime();
    return info;
}

// A non-special disk share is the only kind whose read/write access is worth probing.
[[nodiscard]] bool isProbeableDiskShare(const NetworkShareInfo& info) {
    return info.type == NetworkShareInfo::ShareType::Disk && !isSpecialShare(info.shareName) &&
           isSafeShareName(info.shareName);
}
}  // namespace

NetworkShareBrowser::NetworkShareBrowser(QObject* parent) : QObject(parent) {}

void NetworkShareBrowser::cancel() {
    m_cancelled.store(true);
}

void NetworkShareBrowser::emitDiscoveryOutcome(const QVector<NetworkShareInfo>& shares,
                                               bool ok,
                                               const QString& displayHost) {
    if (ok) {
        Q_EMIT discoveryComplete(shares);
        return;
    }
    // Fail closed: enumerateShares reports ok=false for a hard NetShareEnum error, for a cancel
    // that broke the resume-handle loop, and for a buffer truncated mid-append. Reporting any of
    // those through discoveryComplete would hand a caller a partial list carrying a wholeness
    // claim it cannot support -- and a CANCEL emits no errorOccurred of its own, so it would look
    // like a clean discovery that simply found fewer shares.
    const QString reason =
        m_cancelled.load() ? QStringLiteral(
                                 "Share discovery on %1 was cancelled; the partial list is not "
                                 "a complete enumeration")
                                 .arg(displayHost)
                           : QStringLiteral(
                                 "Share discovery on %1 did not complete; the partial list is not "
                                 "a complete enumeration")
                                 .arg(displayHost);
    Q_EMIT discoveryFailed(shares, reason);
}

void NetworkShareBrowser::discoverShares(const QString& hostname) {
    m_cancelled.store(false);

    if (hostname.isEmpty()) {
        // No enumeration was attempted, so there is no "complete" discovery to report.
        Q_EMIT errorOccurred(QStringLiteral("Hostname cannot be empty"));
        Q_EMIT discoveryFailed({}, QStringLiteral("Hostname cannot be empty"));
        return;
    }

    bool ok = false;
    auto shares = enumerateShares(hostname, /*testAccess=*/true, ok);

    for (const auto& share : shares) {
        if (m_cancelled.load()) {
            break;
        }
        Q_EMIT shareDiscovered(share);
    }

    emitDiscoveryOutcome(shares, ok, resolveShareServer(hostname).displayHost);
}

QVector<NetworkShareInfo> NetworkShareBrowser::listSharesReadOnly(const QString& hostname,
                                                                  bool& ok) {
    m_cancelled.store(false);
    // testAccess = false: never write a probe file to any share. An empty/loopback hostname is
    // enumerated locally (NULL server name) inside enumerateShares.
    return enumerateShares(hostname, /*testAccess=*/false, ok);
}

void NetworkShareBrowser::testAccess(const QString& uncPath) {
    m_cancelled.store(false);

    if (uncPath.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("UNC path cannot be empty"));
        return;
    }
    if (!isValidUncSharePath(uncPath)) {
        // Fail closed: testReadWriteAccess writes and deletes a file at this path, so only a real
        // UNC share path may reach it -- never a local/device path, a bare string, or one carrying
        // control characters -- so a malformed target cannot mutate an unintended object.
        Q_EMIT errorOccurred(QStringLiteral("Not a valid UNC share path: %1").arg(uncPath));
        return;
    }

    auto [canRead, canWrite] = testReadWriteAccess(uncPath);
    Q_EMIT accessTestComplete(uncPath, canRead, canWrite);
}

QVector<NetworkShareInfo> NetworkShareBrowser::enumerateShares(const QString& hostname,
                                                               bool testAccess,
                                                               bool& ok) {
    ok = false;
    QVector<NetworkShareInfo> shares;

    // Local host (empty / "localhost" / loopback) -> NULL server name = pure local enumeration,
    // no SMB round-trip. The UNC/display host is normalized to "localhost" for local reads.
    const ShareServer server = resolveShareServer(hostname);

    LPWSTR serverName = server.serverName.empty() ? nullptr
                                                  : const_cast<LPWSTR>(server.serverName.c_str());
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;
    NET_API_STATUS status = NERR_Success;

    // Loop on ERROR_MORE_DATA using resumeHandle: a single call can return only a
    // partial set, and treating that partial buffer as the complete enumeration would
    // silently drop shares. Keep pulling until a terminal NERR_Success (or cancel).
    int pageCount = 0;
    do {
        PSHARE_INFO_1 shareInfo = nullptr;
        DWORD entriesRead = 0;
        status = NetShareEnum(serverName,
                              kShareInfoLevel,
                              reinterpret_cast<LPBYTE*>(&shareInfo),
                              MAX_PREFERRED_LENGTH,
                              &entriesRead,
                              &totalEntries,
                              &resumeHandle);

        if (status != NERR_Success && status != ERROR_MORE_DATA) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to enumerate shares on %1 (error %2)")
                                     .arg(server.displayHost)
                                     .arg(status));
            return shares;  // ok stays false: a real enumeration failure, not an empty host
        }

        if (shareInfo != nullptr) {
            appendSharesFromBuffer(shareInfo, entriesRead, server.displayHost, testAccess, shares);
            NetApiBufferFree(shareInfo);
        }

        if (++pageCount >= kMaxEnumPages || shares.size() >= kMaxTotalShares) {
            // Fail closed: a server that never terminates the ERROR_MORE_DATA loop (or an
            // implausibly large share table) must not spin this loop or exhaust memory. Leave
            // ok=false so the partial list is reported as a failed, not complete, enumeration.
            Q_EMIT errorOccurred(
                QStringLiteral("Share enumeration on %1 exceeded safe limits; stopping")
                    .arg(server.displayHost));
            return shares;
        }
    } while (status == ERROR_MORE_DATA && !m_cancelled.load());

    // A cancel can break the ERROR_MORE_DATA loop (or truncate a buffer mid-append) before
    // every share was read. Fail closed: report ok=false so callers never treat a truncated
    // list as a complete enumeration.
    if (m_cancelled.load()) {
        return shares;  // ok stays false: partial/cancelled result, not authoritative
    }

    // Reached a terminal NERR_Success with every returned buffer consumed: a complete read,
    // not a silently truncated one.
    ok = true;
    return shares;
}

void NetworkShareBrowser::appendSharesFromBuffer(const void* shareInfoBuffer,
                                                 unsigned long entriesRead,
                                                 const QString& displayHost,
                                                 bool testAccess,
                                                 QVector<NetworkShareInfo>& shares) {
    const auto* entries = static_cast<const SHARE_INFO_1*>(shareInfoBuffer);
    for (DWORD i = 0; i < entriesRead; ++i) {
        if (m_cancelled.load()) {
            break;
        }

        NetworkShareInfo info = makeShareInfo(entries[i], displayHost);

        // Probe access ONLY when asked: testReadWriteAccess writes a temp file to the share, so
        // read-only callers pass testAccess=false and leave canRead/canWrite unset.
        if (testAccess && isProbeableDiskShare(info)) {
            auto [canRead, canWrite] = testReadWriteAccess(info.uncPath);
            info.canRead = canRead;
            info.canWrite = canWrite;
        }

        shares.append(info);
    }
}

QPair<bool, bool> NetworkShareBrowser::testReadWriteAccess(const QString& uncPath) {
    // Both callers guarantee it: testAccess() rejects an empty uncPath before calling, and
    // appendSharesFromBuffer() passes makeShareInfo()'s uncPath, always "\\\\host\\share".
    Q_ASSERT(!uncPath.isEmpty());
    bool canRead = false;
    bool canWrite = false;

    // Test read access
    const QDir dir(uncPath);
    if (dir.exists()) {
        canRead = true;

        // Bounded read probe: confirm the root is enumerable WITHOUT materializing a (possibly
        // hostile or huge) full directory listing. Touching just the first entry exercises real
        // read access; entryInfoList() would stat and copy the entire directory first.
        const QDirIterator it(uncPath, QDir::AllEntries | QDir::NoDotAndDotDot);
        (void)it.hasNext();
    }

    // Test write access by creating a temporary file
    if (canRead) {
        // Full UUID (not a truncated 8-char prefix) for collision resistance, and
        // NewOnly so the probe NEVER truncates a pre-existing remote file: a name
        // collision or a planted file makes open() fail (canWrite stays false)
        // instead of clobbering and then deleting someone else's file (B9-06).
        const QString testFile = uncPath + QStringLiteral("\\._sak_write_test_") +
                                 QUuid::createUuid().toString(QUuid::Id128) +
                                 QStringLiteral(".tmp");

        QFile file(testFile);
        if (file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            const QByteArray test_data("SAK write test");
            canWrite = (file.write(test_data) == test_data.size());
            file.close();

            // Safe: NewOnly guaranteed we created this file, so remove only ever
            // deletes our own probe, never a pre-existing remote file.
            file.remove();
        }
    }

    return {canRead, canWrite};
}

}  // namespace sak
