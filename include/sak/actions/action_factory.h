// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <memory>
#include <vector>

namespace sak {

/**
 * @brief Factory for creating all quick actions
 * 
 * Separates action creation from panel code to avoid windows.h pollution.
 */
class ActionFactory {
public:
    /**
     * @brief Create all quick actions
     * @param backup_location Default location for backup actions
     * @return Vector of all actions
     */
    static std::vector<std::unique_ptr<QuickAction>> createAllActions(const QString& backup_location);
};

} // namespace sak

