// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_report_generator.h
/// @brief HTML/JSON reports for Partition Manager operations.

#pragma once

#include "sak/partition_manager_types.h"

namespace sak {

class PartitionReportGenerator {
public:
    /// What the Log window shows: plain text, with each step's whole output, which on a dry run is
    /// the script that would run.
    [[nodiscard]] static QString generateLogText(const PartitionExecutionResult& result);
    [[nodiscard]] static QString generateHtml(const PartitionInventory& before,
                                              const PartitionInventory& after,
                                              const PartitionExecutionResult& result);
    [[nodiscard]] static QString generateJson(const PartitionInventory& before,
                                              const PartitionInventory& after,
                                              const PartitionExecutionResult& result);
    [[nodiscard]] static QJsonObject diskToJson(const PartitionDiskInfo& disk);
    [[nodiscard]] static QJsonObject operationStepToJson(const PartitionExecutionStep& step);
};

}  // namespace sak
