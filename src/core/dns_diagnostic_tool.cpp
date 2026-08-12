// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dns_diagnostic_tool.cpp
/// @brief DNS queries via DnsQuery_W API with multi-server comparison

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "sak/dns_diagnostic_tool.h"

#include "sak/process_runner.h"

#include <algorithm>
#include <chrono>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <windns.h>

#pragma comment(lib, "dnsapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace sak {

namespace {
constexpr qsizetype kIpv4OctetCount = 4;
constexpr int kMaxOctetValue = 255;
// Bound on the ipconfig /displaydns child process so a wedged launch cannot hang the tool.
constexpr int kDisplayDnsTimeoutMs = 10'000;
}  // namespace

DnsDiagnosticTool::DnsDiagnosticTool(QObject* parent) : QObject(parent) {}

void DnsDiagnosticTool::cancel() {
    m_cancelled.store(true);
}

QVector<QPair<QString, QString>> DnsDiagnosticTool::wellKnownDnsServers() {
    return {
        {QStringLiteral("System Default"), QStringLiteral("")},
        {QStringLiteral("Google DNS"), QStringLiteral("8.8.8.8")},
        {QStringLiteral("Google DNS (Secondary)"), QStringLiteral("8.8.4.4")},
        {QStringLiteral("Cloudflare"), QStringLiteral("1.1.1.1")},
        {QStringLiteral("Cloudflare (Secondary)"), QStringLiteral("1.0.0.1")},
        {QStringLiteral("Quad9"), QStringLiteral("9.9.9.9")},
        {QStringLiteral("Quad9 (Secondary)"), QStringLiteral("149.112.112.112")},
        {QStringLiteral("OpenDNS"), QStringLiteral("208.67.222.222")},
        {QStringLiteral("OpenDNS (Secondary)"), QStringLiteral("208.67.220.220")},
    };
}

QStringList DnsDiagnosticTool::supportedRecordTypes() {
    return {
        QStringLiteral("A"),
        QStringLiteral("AAAA"),
        QStringLiteral("MX"),
        QStringLiteral("CNAME"),
        QStringLiteral("TXT"),
        QStringLiteral("SOA"),
        QStringLiteral("NS"),
        QStringLiteral("SRV"),
        QStringLiteral("PTR"),
    };
}

bool DnsDiagnosticTool::isSupportedRecordType(const QString& recordType) {
    return supportedRecordTypes().contains(recordType, Qt::CaseInsensitive);
}

bool DnsDiagnosticTool::isNumericIpv4(const QString& ipAddress) {
    const auto parts = ipAddress.split(QLatin1Char('.'));
    if (parts.size() != kIpv4OctetCount) {
        return false;
    }
    for (const auto& part : parts) {
        if (part.isEmpty()) {
            return false;
        }
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok || value < 0 || value > kMaxOctetValue) {
            return false;
        }
    }
    return true;
}

bool DnsDiagnosticTool::answersEquivalent(const QVector<QString>& a, const QVector<QString>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    QVector<QString> sortedA = a;
    QVector<QString> sortedB = b;
    std::ranges::sort(sortedA);
    std::ranges::sort(sortedB);
    return sortedA == sortedB;
}

}  // namespace sak

namespace {

WORD mapRecordType(const QString& recordType) {
    struct TypeMapping {
        const char* name;
        WORD type;
    };
    static constexpr TypeMapping kTypes[] = {
        {.name = "AAAA", .type = DNS_TYPE_AAAA},
        {.name = "MX", .type = DNS_TYPE_MX},
        {.name = "CNAME", .type = DNS_TYPE_CNAME},
        {.name = "TXT", .type = DNS_TYPE_TEXT},
        {.name = "SOA", .type = DNS_TYPE_SOA},
        {.name = "NS", .type = DNS_TYPE_NS},
        {.name = "SRV", .type = DNS_TYPE_SRV},
        {.name = "PTR", .type = DNS_TYPE_PTR},
    };
    // isSupportedRecordType() accepts a type case-insensitively, so normalize here before the
    // (case-sensitive) name compare -- otherwise "aaaa"/"mx" would fall through and silently query
    // an A record instead of the type the caller asked for.
    const QString normalized = recordType.toUpper();
    for (const auto& entry : kTypes) {
        if (normalized == QLatin1String(entry.name)) {
            return entry.type;
        }
    }
    return DNS_TYPE_A;
}

// First successful answer -- including an empty answer set -- establishes the agreement baseline,
// so the count of successes (not firstAnswers being empty) is what tells the comparison whether a
// baseline already exists.
int countSuccessfulResults(const QVector<sak::DnsQueryResult>& results) {
    return static_cast<int>(
        std::ranges::count_if(results, [](const sak::DnsQueryResult& r) { return r.success; }));
}

// Fail-closed failure text for 'ipconfig /displaydns', appending the child's stderr when present so
// the real OS error reaches the user instead of an empty cache being reported.
QString displayDnsFailureMessage(const sak::ProcessResult& result) {
    const QString detail = result.std_err.trimmed();
    QString message = QStringLiteral("Failed to read the DNS resolver cache.");
    if (!detail.isEmpty()) {
        message += QLatin1Char(' ') + detail;
    }
    return message;
}

void appendTxtRecords(DNS_RECORD* rec, sak::DnsQueryResult& result) {
    for (DWORD i = 0; i < rec->Data.TXT.dwStringCount; ++i) {
        result.answers.append(QString::fromWCharArray(rec->Data.TXT.pStringArray[i]));
    }
}

void extractDnsAnswer(DNS_RECORD* rec, sak::DnsQueryResult& result) {
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
        inet_ntop(AF_INET6, &rec->Data.AAAA.Ip6Address, ipBuf, sizeof(ipBuf));
        result.answers.append(QString::fromLatin1(ipBuf));
        break;
    }
    case DNS_TYPE_MX: {
        auto name = QString::fromWCharArray(rec->Data.MX.pNameExchange);
        result.answers.append(
            QStringLiteral("%1 (priority %2)").arg(name).arg(rec->Data.MX.wPreference));
        break;
    }
    case DNS_TYPE_CNAME: {
        result.answers.append(QString::fromWCharArray(rec->Data.CNAME.pNameHost));
        break;
    }
    case DNS_TYPE_TEXT:
        appendTxtRecords(rec, result);
        break;
    case DNS_TYPE_SOA: {
        auto primary = QString::fromWCharArray(rec->Data.SOA.pNamePrimaryServer);
        auto admin = QString::fromWCharArray(rec->Data.SOA.pNameAdministrator);
        result.answers.append(QStringLiteral("Primary: %1, Admin: %2, Serial: %3")
                                  .arg(primary, admin)
                                  .arg(rec->Data.SOA.dwSerialNo));
        break;
    }
    case DNS_TYPE_NS: {
        result.answers.append(QString::fromWCharArray(rec->Data.NS.pNameHost));
        break;
    }
    case DNS_TYPE_SRV: {
        auto target = QString::fromWCharArray(rec->Data.SRV.pNameTarget);
        result.answers.append(QStringLiteral("%1:%2 (priority %3, weight %4)")
                                  .arg(target)
                                  .arg(rec->Data.SRV.wPort)
                                  .arg(rec->Data.SRV.wPriority)
                                  .arg(rec->Data.SRV.wWeight));
        break;
    }
    case DNS_TYPE_PTR: {
        result.answers.append(QString::fromWCharArray(rec->Data.PTR.pNameHost));
        break;
    }
    default:
        break;
    }
}

}  // namespace

namespace sak {

DnsQueryResult DnsDiagnosticTool::performQuery(const QString& hostname,
                                               const QString& recordType,
                                               const QString& dnsServer) {
    DnsQueryResult result;
    result.queryName = hostname;
    result.recordType = recordType;
    result.dnsServer = dnsServer.isEmpty() ? QStringLiteral("System Default") : dnsServer;
    result.queryTimestamp = QDateTime::currentDateTime();

    const WORD type = mapRecordType(recordType);

    // Set up custom DNS server if specified
    IP4_ARRAY serverList{};
    PIP4_ARRAY pServerList = nullptr;
    if (!dnsServer.isEmpty()) {
        serverList.AddrCount = 1;
        const int inetResult =
            inet_pton(AF_INET, dnsServer.toLatin1().constData(), &serverList.AddrArray[0]);
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
        hostWide.c_str(), type, DNS_QUERY_BYPASS_CACHE, pServerList, &dnsRecord, nullptr);

    const auto end = std::chrono::high_resolution_clock::now();
    result.responseTimeMs = std::chrono::duration<double, std::milli>(end - start).count();

    if (status == 0 && dnsRecord != nullptr) {
        result.success = true;

        for (auto* rec = dnsRecord; rec != nullptr; rec = rec->pNext) {
            result.ttlSeconds = static_cast<int>(rec->dwTtl);
            extractDnsAnswer(rec, result);
        }

        DnsRecordListFree(dnsRecord, DnsFreeRecordListDeep);
    } else {
        result.success = false;
        result.errorMessage = QStringLiteral("DNS query failed (status %1)").arg(status);
    }

    return result;
}

void DnsDiagnosticTool::query(const QString& hostname,
                              const QString& recordType,
                              const QString& dnsServer) {
    m_cancelled.store(false);

    if (hostname.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Hostname cannot be empty"));
        Q_EMIT queryComplete({});
        return;
    }

    // Reject an unknown record type rather than silently querying A for it.
    if (!isSupportedRecordType(recordType)) {
        Q_EMIT errorOccurred(QStringLiteral("Unsupported DNS record type: %1").arg(recordType));
        Q_EMIT queryComplete({});
        return;
    }

    auto result = performQuery(hostname, recordType, dnsServer);
    Q_EMIT queryComplete(result);
}

void DnsDiagnosticTool::reverseLookup(const QString& ipAddress, const QString& dnsServer) {
    m_cancelled.store(false);

    if (ipAddress.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("IP address cannot be empty"));
        Q_EMIT queryComplete({});
        return;
    }

    // Require four numeric 0..255 octets: a non-numeric part (e.g. "a.b.c.d") would
    // otherwise build a bogus PTR name and issue a meaningless query.
    if (!isNumericIpv4(ipAddress)) {
        Q_EMIT errorOccurred(QStringLiteral("Invalid IPv4 address format"));
        Q_EMIT queryComplete({});
        return;
    }

    // Convert IP to reverse lookup format (e.g., 1.2.3.4 -> 4.3.2.1.in-addr.arpa)
    const auto parts = ipAddress.split(QLatin1Char('.'));

    QString reverseName;
    for (int i = static_cast<int>(parts.size() - 1); i >= 0; --i) {
        if (!reverseName.isEmpty()) {
            reverseName += QLatin1Char('.');
        }
        reverseName += parts[i];
    }
    reverseName += QStringLiteral(".in-addr.arpa");

    auto result = performQuery(reverseName, QStringLiteral("PTR"), dnsServer);
    result.queryName = ipAddress;  // Override for display
    Q_EMIT queryComplete(result);
}

void DnsDiagnosticTool::updateComparisonWithResult(const DnsQueryResult& result,
                                                   DnsServerComparison& comparison,
                                                   QVector<QString>& firstAnswers) {
    comparison.results.append(result);

    if (result.success && result.responseTimeMs < comparison.fastestTimeMs) {
        comparison.fastestTimeMs = result.responseTimeMs;
        comparison.fastestServer = result.dnsServer;
    }

    if (!result.success) {
        return;
    }

    // The first success (already appended above, so count == 1) sets the baseline even when its
    // answer set is empty; every later success is compared against it. Keying off the success
    // count -- not firstAnswers.isEmpty() -- stops an empty-answer baseline from being skipped and
    // a later differing answer from being mistaken for the baseline.
    if (countSuccessfulResults(comparison.results) == 1) {
        firstAnswers = result.answers;
    } else if (!answersEquivalent(result.answers, firstAnswers)) {
        comparison.allAgree = false;
    }
}

void DnsDiagnosticTool::compareServers(const QString& hostname,
                                       const QString& recordType,
                                       const QStringList& dnsServers) {
    m_cancelled.store(false);

    if (hostname.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Hostname cannot be empty"));
        Q_EMIT comparisonComplete({});
        return;
    }

    if (!isSupportedRecordType(recordType)) {
        Q_EMIT errorOccurred(QStringLiteral("Unsupported DNS record type: %1").arg(recordType));
        Q_EMIT comparisonComplete({});
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
        updateComparisonWithResult(result, comparison, firstAnswers);
    }

    if (comparison.fastestTimeMs == std::numeric_limits<double>::max()) {
        comparison.fastestTimeMs = 0.0;
    }

    // Fail closed: allAgree is a positive assurance that at least two servers returned the same
    // answer. With fewer than two successful queries there is nothing to compare (a lone server
    // cannot agree with itself, and an all-failed/empty-list run has no baseline at all), and a
    // run cancelled part-way never compared them all -- so report no agreement rather than a
    // misleading true (R5).
    constexpr int kMinComparableAnswers = 2;
    if (countSuccessfulResults(comparison.results) < kMinComparableAnswers || m_cancelled.load()) {
        comparison.allAgree = false;
    }

    Q_EMIT comparisonComplete(comparison);
}

void DnsDiagnosticTool::inspectDnsCache() {
    m_cancelled.store(false);

    // System32-qualified ipconfig, never the bare name: CreateProcess searches the current
    // directory ahead of System32, so a planted ipconfig could feed this tool a fabricated
    // DNS cache. Unresolvable -> surface the error (fail closed), never a PATH/CWD launch.
    const QString ipconfig_exe = sak::system32Path(QStringLiteral("ipconfig.exe"));
    if (ipconfig_exe.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Could not locate the System32 ipconfig.exe."));
        return;
    }

    const auto result = sak::runProcess(ipconfig_exe,
                                        {QStringLiteral("/displaydns")},
                                        kDisplayDnsTimeoutMs,
                                        [this]() { return m_cancelled.load(); });
    // Fail closed: a failed 'ipconfig /displaydns' must surface as an error, not an empty cache --
    // emitting no results here would be indistinguishable from a genuinely empty resolver cache.
    if (!result.succeeded()) {
        Q_EMIT errorOccurred(displayDnsFailureMessage(result));
        return;
    }

    const auto lines = result.std_out.split(QLatin1Char('\n'));

    QVector<QPair<QString, QString>> entries;
    QString currentName;

    for (const auto& line : lines) {
        const auto trimmed = line.trimmed();

        if (trimmed.startsWith(QStringLiteral("Record Name"))) {
            const int colonPos = static_cast<int>(trimmed.indexOf(QLatin1Char(':')));
            if (colonPos >= 0) {
                currentName = trimmed.mid(colonPos + 1).trimmed();
            }
        } else if (trimmed.startsWith(QStringLiteral("A (Host) Record")) ||
                   trimmed.startsWith(QStringLiteral("AAAA Record"))) {
            const int colonPos = static_cast<int>(trimmed.indexOf(QLatin1Char(':')));
            if (colonPos >= 0 && !currentName.isEmpty()) {
                const auto value = trimmed.mid(colonPos + 1).trimmed();
                entries.append({currentName, value});
            }
        }
    }

    Q_EMIT dnsCacheResults(entries);
}

void DnsDiagnosticTool::flushDnsCache() {
    m_cancelled.store(false);

    // Flush via the dnsapi entry point DnsFlushResolverCache -- the same call `ipconfig /flushdns`
    // makes internally, but it returns a BOOL that RELIABLY reflects success. `ipconfig /flushdns`
    // can exit 0 even when the flush fails (e.g. the DNS Client service is stopped, where it prints
    // "Could not flush the DNS Resolver Cache" yet still exits 0), which the exit-code check would
    // read as a false success. The export is undocumented (not in windns.h) but stable since
    // Windows 2000, so it is resolved by name at runtime from the already-loaded dnsapi.dll.
    using DnsFlushFn = BOOL(WINAPI*)();
    bool ok = false;
    if (const HMODULE dnsapi = GetModuleHandleW(L"dnsapi.dll")) {
        if (const auto flush =
                reinterpret_cast<DnsFlushFn>(GetProcAddress(dnsapi, "DnsFlushResolverCache"))) {
            ok = flush() != FALSE;
        }
    }

    if (!ok) {
        // Do not emit dnsCacheFlushed() on failure -- that signal means the cache was
        // actually flushed, and firing it here would report a false success.
        Q_EMIT errorOccurred(QStringLiteral("Failed to flush the DNS resolver cache."));
        return;
    }
    Q_EMIT dnsCacheFlushed();
}

}  // namespace sak
