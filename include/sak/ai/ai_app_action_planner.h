// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::ai {

struct AiAppActionPlan {
    QJsonObject manifest;
    QJsonObject action_profile;
    QJsonArray evidence;
    AiCommandRequest request;
    QString app_id;
    QString action;
    QString display_name;
    QString method;
    QString preview;
    QString error_message;
    QString guard_block_error;
    QString guard_approval_reason;
    bool risky{false};

    [[nodiscard]] bool ok() const { return error_message.isEmpty(); }
};

class AiAppActionPlanner {
public:
    struct Options {
        int default_output_bytes{262'144};
        int min_output_bytes{1024};
        int max_output_bytes{4 * 1024 * 1024};
    };

    [[nodiscard]] static AiAppActionPlan buildPlan(const QString& app_id,
                                                   const QString& action,
                                                   const QJsonObject& manifest,
                                                   const QJsonObject& arguments,
                                                   Options options = {});
};

}  // namespace sak::ai
