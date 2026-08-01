// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dns_diagnostic_tool.h
/// @brief DNS queries, multi-server comparison, and cache management

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>
#include <QPair>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief Comprehensive DNS diagnostic tool
///
/// Performs DNS queries using DnsQuery_W API with support for
/// specific DNS server targeting, multiple record types, and
/// multi-server comparison analysis.
class DnsDiagnosticTool : public QObject {
    Q_OBJECT

public:
    explicit DnsDiagnosticTool(QObject* parent = nullptr);
    ~DnsDiagnosticTool() override = default;

    DnsDiagnosticTool(const DnsDiagnosticTool&) = delete;
    DnsDiagnosticTool& operator=(const DnsDiagnosticTool&) = delete;
    DnsDiagnosticTool(DnsDiagnosticTool&&) = delete;
    DnsDiagnosticTool& operator=(DnsDiagnosticTool&&) = delete;

    /// @brief Query a hostname (blocking)
    void query(const QString& hostname,
               const QString& recordType = "A",
               const QString& dnsServer = "");

    /// @brief Reverse lookup -- IP to hostname (blocking)
    void reverseLookup(const QString& ipAddress, const QString& dnsServer = "");

    /// @brief Compare same query across multiple DNS servers (blocking)
    void compareServers(const QString& hostname,
                        const QString& recordType,
                        const QStringList& dnsServers);

    /// @brief Update comparison state with a single query result
    void updateComparisonWithResult(const DnsQueryResult& result,
                                    DnsServerComparison& comparison,
                                    QVector<QString>& firstAnswers);

    /// @brief Inspect the local DNS cache (blocking)
    void inspectDnsCache();

    /// @brief Flush the local DNS cache (blocking, requires admin)
    void flushDnsCache();

    void cancel();

    /// @brief Well-known public DNS servers
    [[nodiscard]] static QVector<QPair<QString, QString>> wellKnownDnsServers();

    /// @brief Supported DNS record types
    [[nodiscard]] static QStringList supportedRecordTypes();

    /// @brief True if @p recordType is one of supportedRecordTypes() (case-insensitive).
    /// Guards against a typo'd type being silently downgraded to an A query.
    [[nodiscard]] static bool isSupportedRecordType(const QString& recordType);

    /// @brief True if @p ipAddress is a dotted-quad IPv4 with four 0..255 numeric octets.
    /// Reverse lookup must reject non-numeric octets rather than build a bogus PTR name.
    [[nodiscard]] static bool isNumericIpv4(const QString& ipAddress);

    /// @brief Order-insensitive comparison of two DNS answer sets. DNS servers may
    /// return the same records in different order (round-robin), so an ordered
    /// compare would wrongly flag agreeing servers as disagreeing.
    [[nodiscard]] static bool answersEquivalent(const QVector<QString>& a,
                                                const QVector<QString>& b);

Q_SIGNALS:
    void queryComplete(sak::DnsQueryResult result);
    void comparisonComplete(sak::DnsServerComparison comparison);
    void dnsCacheResults(QVector<QPair<QString, QString>> entries);
    void dnsCacheFlushed();
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] DnsQueryResult performQuery(const QString& hostname,
                                              const QString& recordType,
                                              const QString& dnsServer);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::DnsDiagnosticTool>,
              "DnsDiagnosticTool must not be copyable.");
