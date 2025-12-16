// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/action_factory.h"

// System Optimization Actions
#include "sak/actions/disk_cleanup_action.h"
#include "sak/actions/clear_browser_cache_action.h"
#include "sak/actions/defragment_drives_action.h"
#include "sak/actions/clear_windows_update_cache_action.h"
#include "sak/actions/disable_startup_programs_action.h"
#include "sak/actions/clear_event_logs_action.h"
#include "sak/actions/optimize_power_settings_action.h"
#include "sak/actions/disable_visual_effects_action.h"

// Quick Backup Actions
#include "sak/actions/quickbooks_backup_action.h"
#include "sak/actions/browser_profile_backup_action.h"
#include "sak/actions/outlook_backup_action.h"
#include "sak/actions/sticky_notes_backup_action.h"
#include "sak/actions/saved_game_data_backup_action.h"
#include "sak/actions/tax_software_backup_action.h"
#include "sak/actions/photo_management_backup_action.h"
#include "sak/actions/development_configs_backup_action.h"

// Maintenance Actions
#include "sak/actions/check_disk_health_action.h"
#include "sak/actions/update_all_apps_action.h"
#include "sak/actions/windows_update_action.h"
#include "sak/actions/verify_system_files_action.h"
#include "sak/actions/check_disk_errors_action.h"
#include "sak/actions/rebuild_icon_cache_action.h"
#include "sak/actions/reset_network_action.h"
#include "sak/actions/clear_print_spooler_action.h"

// Troubleshooting Actions
#include "sak/actions/generate_system_report_action.h"
#include "sak/actions/check_bloatware_action.h"
#include "sak/actions/test_network_speed_action.h"
#include "sak/actions/scan_malware_action.h"
#include "sak/actions/repair_windows_store_action.h"
#include "sak/actions/fix_audio_issues_action.h"

// Emergency Recovery Actions
#include "sak/actions/backup_browser_data_action.h"
#include "sak/actions/backup_email_data_action.h"
#include "sak/actions/create_restore_point_action.h"
#include "sak/actions/export_registry_keys_action.h"
#include "sak/actions/backup_activation_keys_action.h"
#include "sak/actions/screenshot_settings_action.h"

#include <memory>
#include <vector>

namespace sak {

std::vector<std::unique_ptr<QuickAction>> ActionFactory::createAllActions(const QString& backup_location) {
    std::vector<std::unique_ptr<QuickAction>> actions;
    
    // System Optimization (8 actions)
    actions.push_back(std::make_unique<DiskCleanupAction>());
    actions.push_back(std::make_unique<ClearBrowserCacheAction>());
    actions.push_back(std::make_unique<DefragmentDrivesAction>());
    actions.push_back(std::make_unique<ClearWindowsUpdateCacheAction>());
    actions.push_back(std::make_unique<DisableStartupProgramsAction>());
    actions.push_back(std::make_unique<ClearEventLogsAction>());
    actions.push_back(std::make_unique<OptimizePowerSettingsAction>());
    actions.push_back(std::make_unique<DisableVisualEffectsAction>());
    
    // Quick Backups (8 actions)
    actions.push_back(std::make_unique<QuickBooksBackupAction>(backup_location));
    actions.push_back(std::make_unique<BrowserProfileBackupAction>(backup_location));
    actions.push_back(std::make_unique<OutlookBackupAction>(backup_location));
    actions.push_back(std::make_unique<StickyNotesBackupAction>(backup_location));
    actions.push_back(std::make_unique<SavedGameDataBackupAction>(backup_location));
    actions.push_back(std::make_unique<TaxSoftwareBackupAction>(backup_location));
    actions.push_back(std::make_unique<PhotoManagementBackupAction>(backup_location));
    actions.push_back(std::make_unique<DevelopmentConfigsBackupAction>(backup_location));
    
    // Maintenance (8 actions)
    actions.push_back(std::make_unique<CheckDiskHealthAction>());
    actions.push_back(std::make_unique<UpdateAllAppsAction>());
    actions.push_back(std::make_unique<WindowsUpdateAction>());
    actions.push_back(std::make_unique<VerifySystemFilesAction>());
    actions.push_back(std::make_unique<CheckDiskErrorsAction>());
    actions.push_back(std::make_unique<RebuildIconCacheAction>());
    actions.push_back(std::make_unique<ResetNetworkAction>());
    actions.push_back(std::make_unique<ClearPrintSpoolerAction>());
    
    // Troubleshooting (7 actions)
    actions.push_back(std::make_unique<GenerateSystemReportAction>(backup_location));
    actions.push_back(std::make_unique<CheckBloatwareAction>());
    actions.push_back(std::make_unique<TestNetworkSpeedAction>());
    actions.push_back(std::make_unique<ScanMalwareAction>());
    actions.push_back(std::make_unique<RepairWindowsStoreAction>());
    actions.push_back(std::make_unique<FixAudioIssuesAction>());
    
    // Emergency Recovery (6 actions)
    actions.push_back(std::make_unique<BackupBrowserDataAction>(backup_location));
    actions.push_back(std::make_unique<BackupEmailDataAction>(backup_location));
    actions.push_back(std::make_unique<CreateRestorePointAction>());
    actions.push_back(std::make_unique<ExportRegistryKeysAction>(backup_location));
    actions.push_back(std::make_unique<BackupActivationKeysAction>(backup_location));
    actions.push_back(std::make_unique<ScreenshotSettingsAction>(backup_location));
    
    return actions;
}

} // namespace sak
