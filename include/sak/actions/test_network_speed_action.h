// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Test Network Speed Action
 * 
 * Tests internet download/upload speed using PowerShell and speedtest-cli.
 */
class TestNetworkSpeedAction : public QuickAction {
    Q_OBJECT

public:
    explicit TestNetworkSpeedAction(QObject* parent = nullptr);

    QString name() const override { return "Test Network Speed"; }
    QString description() const override { return "Measure internet speed"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    // Speed metrics
    double m_download_speed{0.0};
    double m_max_download_speed{0.0};
    double m_upload_speed{0.0};
    int m_download_tests_successful{0};
    bool m_upload_test_successful{false};
    
    // Latency metrics
    int m_latency{0};
    int m_min_latency{0};
    int m_max_latency{0};
    double m_jitter{0.0};
    double m_packet_loss{0.0};
    
    // Connection info
    bool m_has_internet{false};
    QString m_public_ip;
    QString m_isp;
    QString m_city;
    QString m_region;
    QString m_country;

    // Test methods
    void checkConnectivity();
    void testDownloadSpeed();
    void testUploadSpeed();
    void testLatencyAndJitter();
    void getPublicIPInfo();
};

} // namespace sak

