// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Concrete half of the app-action service: the application's one QuickActionController with the
// built-in technician actions, and the assistant's view of it. The generic bridge and
// registration logic live in app_action_bridge.cpp (action-agnostic).

#include "sak/app_action_service.h"

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/generate_system_report_action.h"
#include "sak/actions/microsoft_defender_action.h"
#include "sak/actions/optimize_power_settings_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/screenshot_settings_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/quick_action_controller.h"

#include <mutex>
#include <utility>

namespace sak {

struct AppActionService::OutputFolder {
    mutable std::mutex mutex;
    QString path;
};

std::shared_ptr<QuickActionController> createAppQuickActionController() {
    auto controller = std::make_shared<QuickActionController>();
    // No action carries a folder of its own: each run brings its caller's (a panel asks the
    // user, the assistant hands its session staging folder).
    controller->registerAction(std::make_unique<OptimizePowerSettingsAction>());
    controller->registerAction(std::make_unique<VerifySystemFilesAction>());
    controller->registerAction(std::make_unique<CheckDiskErrorsAction>());
    controller->registerAction(std::make_unique<ResetNetworkAction>());
    controller->registerAction(std::make_unique<GenerateSystemReportAction>(QString()));
    controller->registerAction(std::make_unique<ScreenshotSettingsAction>(QString()));
    controller->registerAction(std::make_unique<BackupBitlockerKeysAction>(QString()));
    for (const DefenderOperation operation : {DefenderOperation::QuickScan,
                                              DefenderOperation::FullScan,
                                              DefenderOperation::SignatureUpdate}) {
        controller->registerAction(std::make_unique<MicrosoftDefenderAction>(operation));
    }
    return controller;
}

AppActionService::AppActionService(std::shared_ptr<QuickActionController> quick_actions,
                                   const QString& output_directory,
                                   QObject* parent)
    : QObject(parent)
    , m_quick_actions(std::move(quick_actions))
    , m_output_folder(std::make_shared<OutputFolder>()) {
    setOutputDirectory(output_directory);
}

AppActionService::~AppActionService() = default;

void AppActionService::setOutputDirectory(const QString& output_directory) {
    const std::lock_guard<std::mutex> lock(m_output_folder->mutex);
    m_output_folder->path = output_directory.trimmed();
}

QString AppActionService::outputDirectory() const {
    const std::lock_guard<std::mutex> lock(m_output_folder->mutex);
    return m_output_folder->path;
}

int AppActionService::registerInto(AppActionRegistry& registry) {
    if (!m_quick_actions) {
        return 0;
    }
    // Read when a run is requested, so each run carries the staging folder of the call that
    // asked for it (the panel replaces the folder per call and per session).
    return registerQuickActionsInto(*m_quick_actions, registry, [folder = m_output_folder]() {
        const std::lock_guard<std::mutex> lock(folder->mutex);
        return folder->path;
    });
}

}  // namespace sak
