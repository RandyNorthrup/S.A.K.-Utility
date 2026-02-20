// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

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
};

} // namespace sak

