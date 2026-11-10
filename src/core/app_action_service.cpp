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

#include <QHash>
#include <QThread>

#include <mutex>
#include <utility>

namespace sak {

// Keyed by thread: a call's thunk reads its folder synchronously on the thread that set it.
struct AppActionService::OutputFolders {
    mutable std::mutex mutex;
    QHash<Qt::HANDLE, QString> by_thread;

    [[nodiscard]] QString forCurrentThread() const {
        const std::lock_guard<std::mutex> lock(mutex);
        return by_thread.value(QThread::currentThreadId());
    }
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
                                   QObject* parent)
    : QObject(parent)
    , m_quick_actions(std::move(quick_actions))
    , m_output_folders(std::make_shared<OutputFolders>()) {}

AppActionService::~AppActionService() = default;

AppActionService::OutputDirectoryScope::OutputDirectoryScope(const AppActionService& service,
                                                             const QString& output_directory)
    : m_folders(service.m_output_folders) {
    const std::lock_guard<std::mutex> lock(m_folders->mutex);
    m_folders->by_thread.insert(QThread::currentThreadId(), output_directory.trimmed());
}

AppActionService::OutputDirectoryScope::~OutputDirectoryScope() {
    const std::lock_guard<std::mutex> lock(m_folders->mutex);
    m_folders->by_thread.remove(QThread::currentThreadId());
}

QString AppActionService::outputDirectory() const {
    return m_output_folders->forCurrentThread();
}

int AppActionService::registerInto(AppActionRegistry& registry) {
    if (!m_quick_actions) {
        return 0;
    }
    // Read when a run is requested, on the thread of the call that asked for it, so each run
    // carries that call's staging folder.
    return registerQuickActionsInto(*m_quick_actions, registry, [folders = m_output_folders]() {
        return folders->forCurrentThread();
    });
}

}  // namespace sak
