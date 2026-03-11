// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QString>

namespace sak {

/**
 * @brief Fix Audio Issues Action
 *
 * Restarts audio services, resets audio devices, and checks for driver issues.
 */
class FixAudioIssuesAction : public QuickAction {
    Q_OBJECT

public:
    explicit FixAudioIssuesAction(QObject* parent = nullptr);

    QString name() const override { return "Fix Audio Issues"; }
    QString description() const override { return "Restart audio services and reset devices"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    /// @brief Status information for a Windows audio service
    struct AudioServiceStatus {
        QString service_name;
        QString status;
        bool isExecuting;
    };

    AudioServiceStatus checkAudioService(const QString& service_name);
    bool restartAudioService();
    bool restartAudioEndpointBuilder();
    int resetAudioDevices();
    QString checkUSBAudioDevices();

    struct AudioRepairOutcome {
        bool audiosrv_restarted = false;
        bool endpoint_restarted = false;
        int device_count = 0;
        QString usb_info;
    };

    /// @brief Build the box-drawing diagnostic report from service/device results
    QString buildDiagnosticReport(const AudioServiceStatus& audiosrv,
                                  const AudioServiceStatus& endpoint_builder,
                                  const AudioRepairOutcome& repair);
};

}  // namespace sak
