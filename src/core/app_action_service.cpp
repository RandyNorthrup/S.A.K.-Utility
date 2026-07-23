// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Concrete half of the app-action service: constructs the app-scoped
// QuickActionController with the built-in technician actions. The generic bridge
// and registration logic live in app_action_bridge.cpp (action-agnostic).

#include "sak/app_action_service.h"

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/generate_system_report_action.h"
#include "sak/actions/optimize_power_settings_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/screenshot_settings_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/quick_action_controller.h"

namespace sak {

AppActionService::AppActionService(const QString& backup_dir, QObject* parent)
    : QObject(parent)
    , m_controller(std::make_unique<QuickActionController>())
    , m_backup_dir(backup_dir) {
    m_controller->setBackupLocation(backup_dir);
    m_controller->registerAction(std::make_unique<OptimizePowerSettingsAction>());
    m_controller->registerAction(std::make_unique<VerifySystemFilesAction>());
    m_controller->registerAction(std::make_unique<CheckDiskErrorsAction>());
    m_controller->registerAction(std::make_unique<ResetNetworkAction>());
    m_controller->registerAction(std::make_unique<GenerateSystemReportAction>(backup_dir));
    m_controller->registerAction(std::make_unique<ScreenshotSettingsAction>(backup_dir));
    m_controller->registerAction(std::make_unique<BackupBitlockerKeysAction>(backup_dir));
}

AppActionService::~AppActionService() = default;

QuickActionController* AppActionService::controller() const {
    return m_controller.get();
}

int AppActionService::registerInto(AppActionRegistry& registry) {
    return registerQuickActionsInto(*m_controller, registry);
}

}  // namespace sak
