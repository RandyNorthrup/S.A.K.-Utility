// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dns_diagnostic_tool.cpp
/// @brief DNS queries via DnsQuery_W API with multi-server comparison

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>

#include "sak/dns_diagnostic_tool.h"

#include <QProcess>

#include <chrono>

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace sak {

DnsDiagnosticTool::DnsDiagnosticTool(QObject* parent)
    : QObject(parent)
{
}

void DnsDiagnosticTool::cancel()
{
    m_cancelled.store(true);
}

QVector<QPair<QString, QString>> DnsDiagnosticTool::wellKnownDnsServers()
{
    return {
        {QStringLiteral("System Default"),       QStringLiteral("")},
        {QStringLiteral("Google DNS"),            QStringLiteral("8.8.8.8")},
        {QStringLiteral("Google DNS (Secondary)"),QStringLiteral("8.8.4.4")},
        {QStringLiteral("Cloudflare"),            QStringLiteral("1.1.1.1")},
        {QStringLiteral("Cloudflare (Secondary)"),QStringLiteral("1.0.0.1")},
        {QStringLiteral("Quad9"),                 QStringLiteral("9.9.9.9")},
        {QStringLiteral("Quad9 (Secondary)"),     QStringLiteral("149.112.112.112")},
        {QStringLiteral("OpenDNS"),               QStringLiteral("208.67.222.222")},
        {QStringLiteral("OpenDNS (Secondary)"),   QStringLiteral("208.67.220.220")},
    };
}

QStringList DnsDiagnosticTool::supportedRecordTypes()
{
    return {
        QStringLiteral("A"), QStringLiteral("AAAA"), QStringLiteral("MX"),
        QStringLiteral("CNAME"), QStringLiteral("TXT"), QStringLiteral("SOA"),
        QStringLiteral("NS"), QStringLiteral("SRV"), QStringLiteral("PTR"),
    };
}

DnsQueryResult DnsDiagnosticTool::performQuery(const QString& hostname,
                                                 const QString& recordType,
                                                 const QString& dnsServer)
{
    DnsQueryResult result;
    result.queryName = hostname;
    result.recordType = recordType;
    result.dnsServer = dnsServer.isEmpty()
                           ? QStringLiteral("System Default") : dnsServer;
    result.queryTimestamp = QDateTime::currentDateTime();

    // Map record type string to DNS_TYPE
    WORD type = DNS_TYPE_A;
    if (recordType == QStringLiteral("AAAA"))     type = DNS_TYPE_AAAA;
    else if (recordType == QStringLiteral("MX"))  type = DNS_TYPE_MX;
    else if (recordType == QStringLiteral("CNAME")) type = DNS_TYPE_CNAME;
    else if (recordType == QStringLiteral("TXT")) type = DNS_TYPE_TEXT;
    else if (recordType == QStringLiteral("SOA")) type = DNS_TYPE_SOA;
    else if (recordType == QStringLiteral("NS"))  type = DNS_TYPE_NS;
    else if (recordType == QStringLiteral("SRV")) type = DNS_TYPE_SRV;
    else if (recordType == QStringLiteral("PTR")) type = DNS_TYPE_PTR;

    // Set up custom DNS server if specified
    IP4_ARRAY serverList{};
    PIP4_ARRAY pServerList = nullptr;
    if (!dnsServer.isEmpty()) {
        serverList.AddrCount = 1;
        const int inetResult = inet_pton(AF_INET, dnsServer.toLatin1().constData(),
                                          &serverList.AddrArray[0]);
        if (inetResult != 1) {
            result.errorMessage =
                QStringLiteral("Invalid DNS server IP address: %1").arg(dnsServer);
            return result;
        }
        pServerList = &serverList;
    }

    const auto hostWide = hostname.toStdWString();

    PDNS_RECORD dnsRecord = nullptr;

    const auto start = std::chrono::high_resolution_clock::now();

    const DNS_STATUS status = DnsQuery_W(
        hostWide.c_str(),
        type,
        DNS_QUERY_BYPASS_CACHE,
        pServerList,
        &dnsRecord,
        nullptr);

    const auto end = std::chrono::high_resolution_clock::now();
    result.responseTimeMs =
        std::chrono::duration<double, std::milli>(end - start).count();

    if (status == 0 && dnsRecord != nullptr) {
        result.success = true;

        for (auto* rec = dnsRecord; rec != nullptr; rec = rec->pNext) {
            result.ttlSeconds = static_cast<int>(rec->dwTtl);

            switch (rec->wType) {
            case DNS_TYPE_A: {
                IN_ADDR addr;
                addr.S_un.S_addr = rec->Data.A.IpAddress;
                char ipBuf[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &addr, ipBuf, sizeof(ipBuf));
                result.answers.append(QString::fromLatin1(ipBuf));
                break;
            }
            case DNS_TYPE_AAAA: {
                char ipBuf[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET6, &rec->Data.AAAA.Ip6Address,
                          ipBuf, sizeof(ipBuf));
                result.answers.append(QString::fromLatin1(ipBuf));
                break;
            }
            case DNS_TYPE_MX: {
                auto name = QString::fromWCharArray(
                    rec->Data.MX.pNameExchange);
                result.answers.append(
                    QStringLiteral("%1 (priority %2)")
                        .arg(name)
                        .arg(rec->Data.MX.wPreference));
                break;
            }
            case DNS_TYPE_CNAME: {
                result.answers.append(QString::fromWCharArray(
                    rec->Data.CNAME.pNameHost));
                break;
            }
            case DNS_TYPE_TEXT: {
                for (DWORD i = 0; i < rec->Data.TXT.dwStringCount; ++i) {
                    result.answers.append(QString::fromWCharArray(
                        rec->Data.TXT.pStringArray[i]));
                }
                break;
            }
            case DNS_TYPE_SOA: {
                auto primary = QString::fromWCharArray(
                    rec->Data.SOA.pNamePrimaryServer);
                auto admin = QString::fromWCharArray(
                    rec->Data.SOA.pNameAdministrator);
                result.answers.append(
                    QStringLiteral("Primary: %1, Admin: %2, Serial: %3")
                        .arg(primary, admin)
                        .arg(rec->Data.SOA.dwSerialNo));
                break;
            }
            case DNS_TYPE_NS: {
                result.answers.append(QString::fromWCharArray(
                    rec->Data.NS.pNameHost));
                break;
            }
            case DNS_TYPE_SRV: {
                auto target = QString::fromWCharArray(
                    rec->Data.SRV.pNameTarget);
                result.answers.append(
                    QStringLiteral("%1:%2 (priority %3, weight %4)")
                        .arg(target)
                        .arg(rec->Data.SRV.wPort)
                        .arg(rec->Data.SRV.wPriority)
                        .arg(rec->Data.SRV.wWeight));
                break;
            }
            case DNS_TYPE_PTR: {
                result.answers.append(QString::fromWCharArray(
                    rec->Data.PTR.pNameHost));
                break;
            }
            default:
                break;
            }
        }

        DnsRecordListFree(dnsRecord, DnsFreeRecordListDeep);
    } else {
        result.success = false;
        result.errorMessage =
            QStringLiteral("DNS query failed (status %1)").arg(status);
    }

    return result;
}

void DnsDiagnosticTool::query(const QString& hostname,
                                const QString& recordType,
                                const QString& dnsServer)
{
    m_cancelled.store(false);

    if (hostname.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Hostname cannot be empty"));
        return;
    }

    auto result = performQuery(hostname, recordType, dnsServer);
    Q_EMIT queryComplete(result);
}

void DnsDiagnosticTool::reverseLookup(const QString& ipAddress,
                                        const QString& dnsServer)
{
    m_cancelled.store(false);

    if (ipAddress.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("IP address cannot be empty"));
        return;
    }

    // Convert IP to reverse lookup format (e.g., 1.2.3.4 → 4.3.2.1.in-addr.arpa)
    const auto parts = ipAddress.split(QLatin1Char('.'));
    if (parts.size() != 4) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid IPv4 address format"));
        return;
    }

    QString reverseName;
    for (int i = parts.size() - 1; i >= 0; --i) {
        if (!reverseName.isEmpty()) {
            reverseName += QLatin1Char('.');
        }
        reverseName += parts[i];
    }
    reverseName += QStringLiteral(".in-addr.arpa");

    auto result = performQuery(reverseName, QStringLiteral("PTR"), dnsServer);
    result.queryName = ipAddress; // Override for display
    Q_EMIT queryComplete(result);
}

void DnsDiagnosticTool::compareServers(const QString& hostname,
                                         const QString& recordType,
                                         const QStringList& dnsServers)
{
    m_cancelled.store(false);

    if (hostname.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Hostname cannot be empty"));
        return;
    }

    DnsServerComparison comparison;
    comparison.queryName = hostname;
    comparison.recordType = recordType;
    comparison.fastestTimeMs = std::numeric_limits<double>::max();
    comparison.allAgree = true;

    QVector<QString> firstAnswers;

    for (const auto& server : dnsServers) {
        if (m_cancelled.load()) {
            break;
        }

        auto result = performQuery(hostname, recordType, server);
        comparison.results.append(result);

        if (result.success && result.responseTimeMs < comparison.fastestTimeMs) {
            comparison.fastestTimeMs = result.responseTimeMs;
            comparison.fastestServer = result.dnsServer;
        }

        // Check agreement
        if (firstAnswers.isEmpty() && result.success) {
            firstAnswers = result.answers;
        } else if (result.success && result.answers != firstAnswers) {
            comparison.allAgree = false;
        }
    }

    if (comparison.fastestTimeMs == std::numeric_limits<double>::max()) {
        comparison.fastestTimeMs = 0.0;
    }

    Q_EMIT comparisonComplete(comparison);
}

void DnsDiagnosticTool::inspectDnsCache()
{
    m_cancelled.store(false);

    QProcess proc;
    proc.start(QStringLiteral("ipconfig"), {QStringLiteral("/displaydns")});
    proc.waitForFinished(10000);

    const auto output = proc.readAllStandardOutput();
    const auto lines = QString::fromLocal8Bit(output).split(QLatin1Char('\n'));

    QVector<QPair<QString, QString>> entries;
    QString currentName;

    for (const auto& line : lines) {
        const auto trimmed = line.trimmed();

        if (trimmed.startsWith(QStringLiteral("Record Name"))) {
            const int colonPos = trimmed.indexOf(QLatin1Char(':'));
            if (colonPos >= 0) {
                currentName = trimmed.mid(colonPos + 1).trimmed();
            }
        } else if (trimmed.startsWith(QStringLiteral("A (Host) Record"))
                   || trimmed.startsWith(QStringLiteral("AAAA Record"))) {
            const int colonPos = trimmed.indexOf(QLatin1Char(':'));
            if (colonPos >= 0 && !currentName.isEmpty()) {
                const auto value = trimmed.mid(colonPos + 1).trimmed();
                entries.append({currentName, value});
            }
        }
    }

    Q_EMIT dnsCacheResults(entries);
}

void DnsDiagnosticTool::flushDnsCache()
{
    m_cancelled.store(false);

    QProcess proc;
    proc.start(QStringLiteral("ipconfig"), {QStringLiteral("/flushdns")});
    proc.waitForFinished(10000);

    Q_EMIT dnsCacheFlushed();
}

} // namespace sak
