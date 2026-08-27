// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_execution_broker.h"
#include "sak/layout_constants.h"

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
    QJsonArray steps;  // win32_gui recipe: ordered {tool, arguments, optional?} desktop-tool steps
    bool risky{false};
    // A manifest command that formats/wipes/mass-deletes or is obfuscated/indirected. Unlike
    // `risky` (which only warrants a restore point in Unattended), a catastrophic app action must
    // take the SAME hard human confirm as the shell/own-action/workflow paths in every mode.
    bool catastrophic{false};

    [[nodiscard]] bool ok() const {
        // Fail closed: an empty error_message alone must never certify a default-constructed
        // or partially built plan as runnable. A plan is executable only when no error and no
        // guard block are recorded AND the structural fields an executor needs are populated
        // (app/action/method, plus an actual body -- win32_gui steps or a shell command).
        return error_message.isEmpty() && guard_block_error.isEmpty() && !app_id.isEmpty() &&
               !action.isEmpty() && !method.isEmpty() &&
               (!steps.isEmpty() || !request.command.isEmpty());
    }
};

class AiAppActionPlanner {
public:
    struct Options {
        static constexpr int kDefaultOutputKilobytes = 256;
        static constexpr int kMaximumOutputMegabytes = 4;

        int default_output_bytes{kDefaultOutputKilobytes * static_cast<int>(sak::kBytesPerKB)};
        int min_output_bytes{static_cast<int>(sak::kBytesPerKB)};
        /// Deliberately STRICTER than kAiCommandOutputBytesCeiling: a stricter local budget is a
        /// policy choice and works, because buildPlan clamps to it and the broker accepts the
        /// result. Wider would not be a choice at all -- the broker REFUSES rather than clamps
        /// above its ceiling, so a wider budget here would only decide which plans die at the
        /// door after a human approved them. (This struct already carried the same class of
        /// defect on the TIMEOUT axis; see kAiCommandMaxTimeoutSeconds.)
        int max_output_bytes{kMaximumOutputMegabytes * static_cast<int>(sak::kBytesPerMB)};
    };

    [[nodiscard]] static AiAppActionPlan buildPlan(const QString& app_id,
                                                   const QString& action,
                                                   const QJsonObject& manifest,
                                                   const QJsonObject& arguments,
                                                   Options options = {});
};

static_assert(AiAppActionPlanner::Options{}.max_output_bytes <= kAiCommandOutputBytesCeiling,
              "app-action output ceiling must not exceed what ExecutionBroker accepts");
static_assert(AiAppActionPlanner::Options{}.min_output_bytes <=
                  AiAppActionPlanner::Options{}.default_output_bytes,
              "app-action output floor must not exceed its default");
static_assert(AiAppActionPlanner::Options{}.default_output_bytes <=
                  AiAppActionPlanner::Options{}.max_output_bytes,
              "app-action output default must not exceed its ceiling");

}  // namespace sak::ai
