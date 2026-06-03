// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_report_generator.h
/// @brief HTML/JSON reports for Partition Manager operations.

#pragma once

#include "sak/partition_manager_types.h"

namespace sak {

class PartitionReportGenerator {
public:
    [[nodiscard]] QString generateHtml(const PartitionInventory& before,
                                       const PartitionInventory& after,
                                       const PartitionExecutionResult& result) const;
    [[nodiscard]] QString generateJson(const PartitionInventory& before,
                                       const PartitionInventory& after,
                                       const PartitionExecutionResult& result) const;
    [[nodiscard]] static QJsonObject diskToJson(const PartitionDiskInfo& disk);
    [[nodiscard]] static QJsonObject operationStepToJson(const PartitionExecutionStep& step);
};

}  // namespace sak
