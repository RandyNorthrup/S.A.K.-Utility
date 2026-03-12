// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file port_scanner.h
/// @brief TCP connect scanning with service fingerprinting

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief TCP connect port scanner with service identification
///
/// Performs TCP connect scanning via QTcpSocket. Supports concurrent
/// connections, banner grabbing, and well-known service identification.
class PortScanner : public QObject {
    Q_OBJECT

public:
    struct ScanConfig {
        QString target;
        QVector<uint16_t> ports;
        uint16_t portRangeStart = 0;
        uint16_t portRangeEnd = 0;
        int timeoutMs = netdiag::kDefaultPortScanTimeoutMs;
        int maxConcurrent = netdiag::kDefaultMaxConcurrent;
        bool grabBanners = true;
    };

    explicit PortScanner(QObject* parent = nullptr);
    ~PortScanner() override = default;

    PortScanner(const PortScanner&) = delete;
    PortScanner& operator=(const PortScanner&) = delete;
    PortScanner(PortScanner&&) = delete;
    PortScanner& operator=(PortScanner&&) = delete;

    /// @brief Scan ports (blocking)
    void scan(const ScanConfig& config);

    void cancel();

    /// @brief Get common port presets
    [[nodiscard]] static QVector<PortPreset> getPresets();

    /// @brief Look up service name for a port number
    [[nodiscard]] static QString getServiceName(uint16_t port);

Q_SIGNALS:
    void scanStarted(QString target, int totalPorts);
    void portScanned(sak::PortScanResult result);
    void scanProgress(int scanned, int total);
    void scanComplete(QVector<sak::PortScanResult> results);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] PortScanResult scanPort(const QString& target,
                                          uint16_t port,
                                          int timeoutMs,
                                          bool grabBanner);
    [[nodiscard]] static QString grabBannerData(const QString& target,
                                                uint16_t port,
                                                int timeoutMs);

    /// @brief Well-known port -> service name map
    [[nodiscard]] static const QHash<uint16_t, QString>& serviceDatabase();
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::PortScanner>, "PortScanner must not be copyable.");
