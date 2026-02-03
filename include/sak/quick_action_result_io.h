// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

[[nodiscard]] QString actionStatusToString(QuickAction::ActionStatus status);
[[nodiscard]] QuickAction::ActionStatus actionStatusFromString(const QString& status);

[[nodiscard]] bool writeExecutionResultFile(
    const QString& file_path,
    const QuickAction::ExecutionResult& result,
    QuickAction::ActionStatus status,
    QString* error_message = nullptr);

[[nodiscard]] bool readExecutionResultFile(
    const QString& file_path,
    QuickAction::ExecutionResult* result,
    QuickAction::ActionStatus* status,
    QString* error_message = nullptr);

} // namespace sak
