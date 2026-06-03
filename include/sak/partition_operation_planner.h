// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_operation_planner.h
/// @brief Build Partition Manager operation previews.

#pragma once

#include "sak/partition_manager_types.h"
#include "sak/partition_safety_validator.h"

namespace sak {

class PartitionOperationPlanner {
public:
    [[nodiscard]] OperationPreview previewOperation(const PartitionInventory& inventory,
                                                    PartitionOperation operation) const;

    [[nodiscard]] static PartitionOperation makeOperation(PartitionOperationType type,
                                                          const PartitionTarget& target,
                                                          const QJsonObject& payload = {});

private:
    PartitionSafetyValidator m_validator;

    void fillRiskAndSummary(PartitionOperation* operation) const;
    void expandCompositeOperation(const PartitionOperation& operation,
                                  OperationPreview* preview) const;
};

}  // namespace sak
