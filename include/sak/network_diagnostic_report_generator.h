// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_report_generator.h
/// @brief HTML/JSON diagnostic report generation

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>
#include <QSet>
#include <QString>

#include <type_traits>

class QJsonObject;

namespace sak {

/// @brief Generates professional HTML and JSON diagnostic reports
///
/// Aggregates results from all diagnostic tools into formatted reports
/// with pass/fail indicators and recommendations.
class NetworkDiagnosticReportGenerator : public QObject {
    Q_OBJECT

public:
    /// @brief Report sections that can be included
    enum class Section {
        AdapterConfig,
        PingResults,
        TracerouteResults,
        DnsResults,
        PortScanResults,
        BandwidthResults,
        WiFiAnalysis,
        FirewallAudit,
        ActiveConnections,
        NetworkShares
    };

    explicit NetworkDiagnosticReportGenerator(QObject* parent = nullptr);
    ~NetworkDiagnosticReportGenerator() override = default;

    NetworkDiagnosticReportGenerator(const NetworkDiagnosticReportGenerator&) = delete;
    NetworkDiagnosticReportGenerator& operator=(const NetworkDiagnosticReportGenerator&) = delete;
    NetworkDiagnosticReportGenerator(NetworkDiagnosticReportGenerator&&) = delete;
    NetworkDiagnosticReportGenerator& operator=(NetworkDiagnosticReportGenerator&&) = delete;

    // ── Data setters ──

    void setTechnicianName(const QString& name);
    void setTicketNumber(const QString& ticket);
    void setNotes(const QString& notes);
    void setIncludedSections(const QSet<Section>& sections);

    void setAdapterData(const QVector<NetworkAdapterInfo>& adapters);
    void setPingData(const PingResult& result);
    void setTracerouteData(const TracerouteResult& result);
    void setDnsData(const QVector<DnsQueryResult>& results);
    void setPortScanData(const QVector<PortScanResult>& results);
    void setBandwidthData(const BandwidthTestResult& result);
    void setWiFiData(const QVector<WiFiNetworkInfo>& networks);
    void setFirewallData(const QVector<FirewallRule>& rules,
                         const QVector<FirewallConflict>& conflicts,
                         const QVector<FirewallGap>& gaps);
    void setConnectionData(const QVector<ConnectionInfo>& connections);
    void setShareData(const QVector<NetworkShareInfo>& shares);

    // ── Generation ──

    /// @brief Generate HTML report (blocking)
    void generateHtml(const QString& outputPath);

    /// @brief Generate JSON report (blocking)
    void generateJson(const QString& outputPath);

    /// @brief Generate report content as string
    [[nodiscard]] QString toHtml() const;
    [[nodiscard]] QString toJson() const;

Q_SIGNALS:
    void reportGenerated(QString path);
    void errorOccurred(QString error);

private:
    QString m_technicianName;
    QString m_ticketNumber;
    QString m_notes;
    QSet<Section> m_sections;

    QVector<NetworkAdapterInfo> m_adapters;
    PingResult m_pingResult;
    TracerouteResult m_tracerouteResult;
    QVector<DnsQueryResult> m_dnsResults;
    QVector<PortScanResult> m_portScanResults;
    BandwidthTestResult m_bandwidthResult;
    QVector<WiFiNetworkInfo> m_wifiNetworks;
    QVector<FirewallRule> m_firewallRules;
    QVector<FirewallConflict> m_firewallConflicts;
    QVector<FirewallGap> m_firewallGaps;
    QVector<ConnectionInfo> m_connections;
    QVector<NetworkShareInfo> m_shares;

    // ── HTML section builders ──
    [[nodiscard]] QString buildHtmlHeader() const;
    [[nodiscard]] QString buildAdapterSection() const;
    [[nodiscard]] QString buildPingSection() const;
    [[nodiscard]] QString buildTracerouteSection() const;
    [[nodiscard]] QString buildDnsSection() const;
    [[nodiscard]] QString buildPortScanSection() const;
    [[nodiscard]] QString buildBandwidthSection() const;
    [[nodiscard]] QString buildWiFiSection() const;
    [[nodiscard]] QString buildFirewallSection() const;
    [[nodiscard]] QString buildConnectionSection() const;
    [[nodiscard]] QString buildShareSection() const;
    [[nodiscard]] QString buildHtmlFooter() const;

    // ── JSON section builders ──
    void populateRootJson(QJsonObject& root) const;
    void appendHtmlSections(QString& html) const;
    void appendJsonSections(QJsonObject& root) const;
    void appendAdapterConfigJson(QJsonObject& root) const;
    void appendPingResultsJson(QJsonObject& root) const;
    void appendTracerouteResultsJson(QJsonObject& root) const;
    void appendDnsResultsJson(QJsonObject& root) const;
    void appendPortScanResultsJson(QJsonObject& root) const;
    void appendBandwidthResultsJson(QJsonObject& root) const;
    void appendWiFiAnalysisJson(QJsonObject& root) const;
    void appendFirewallAuditJson(QJsonObject& root) const;
    void appendActiveConnectionsJson(QJsonObject& root) const;
    void appendNetworkSharesJson(QJsonObject& root) const;
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::NetworkDiagnosticReportGenerator>,
              "NetworkDiagnosticReportGenerator must not be copyable.");
