// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file layout_constants.h
/// @brief Centralized layout, sizing, and timing constants for the SAK Utility
///
/// All dialog sizes, widget dimensions, timer intervals, and byte-unit
/// conversion factors should reference these constants instead of hardcoded
/// numeric literals.  Complements style_constants.h (colors, spacing, fonts).

#pragma once

#include <cstdint>

namespace sak {

// ============================================================================
// Byte-unit conversion factors
// ============================================================================

constexpr int64_t kBytesPerKB = 1024LL;
constexpr int64_t kBytesPerMB = 1024LL * 1024;
constexpr int64_t kBytesPerGB = 1024LL * 1024 * 1024;

constexpr double kBytesPerKBf = 1024.0;
constexpr double kBytesPerMBf = 1024.0 * 1024.0;
constexpr double kBytesPerGBf = 1024.0 * 1024.0 * 1024.0;

// ============================================================================
// Timer intervals (milliseconds)
// ============================================================================

constexpr int kTimerSnapMs = 50;              ///< Snap/animation tick
constexpr int kTimerPollingFastMs = 100;      ///< Fast polling (USB creation, install)
constexpr int kTimerDelayShortMs = 200;       ///< Short UI delay
constexpr int kTimerRetryBaseMs = 500;        ///< Retry backoff base
constexpr int kTimerProgressPollMs = 1000;    ///< Progress poll (downloads, conversions)
constexpr int kTimerSplashMs = 1500;          ///< Splash screen display
constexpr int kTimerBroadcastMs = 2000;       ///< Peer discovery broadcast
constexpr int kTimerServiceDelayMs = 2000;    ///< Service restart delay
constexpr int kTimerStatusMessageMs = 3000;   ///< Status bar message display
constexpr int kTimerStatusWarnMs = 4000;      ///< Status bar warning display
constexpr int kTimerHeartbeatMs = 5000;       ///< Connection heartbeat
constexpr int kTimerRefreshMs = 5000;         ///< Drive/device refresh
constexpr int kTimerStatusDefaultMs = 5000;   ///< Default status bar timeout
constexpr int kTimerStatusLongMs = 6000;      ///< Long status bar display
constexpr int kTimerStatusExtendedMs = 7000;  ///< Extended status bar display
constexpr int kTimerNetshWaitMs = 8000;       ///< netsh command timeout
constexpr int kTimerStoreRepairMs = 8000;     ///< Windows Store repair wait
constexpr int kTimerHealthPollMs = 10'000;    ///< Migration health check

// ============================================================================
// Process timeouts (milliseconds)
// ============================================================================

constexpr int kTimeoutProcessStartMs = 5000;       ///< waitForStarted
constexpr int kTimeoutProcessShortMs = 5000;       ///< Quick process finish
constexpr int kTimeoutProcessMediumMs = 10'000;    ///< Medium process (aria2c start, app scan)
constexpr int kTimeoutProcessLongMs = 30'000;      ///< Long process (diskpart, bcdboot)
constexpr int kTimeoutProcessVeryLongMs = 60'000;  ///< Very long process (format, listISO)
constexpr int kTimeoutProcessResetMs = 20'000;     ///< WSReset / audio reset
constexpr int kTimeoutStoreReinstallMs = 45'000;   ///< Windows Store reinstall
constexpr int kTimeoutRestorePointMs = 90'000;     ///< System restore point creation
constexpr int kTimeoutDismCheckMs = 120'000;       ///< DISM CheckHealth / update install
constexpr int kTimeoutBrowserCacheMs = 180'000;    ///< Browser cache cleanup
constexpr int kTimeoutArchiveMs = 300'000;         ///< Archive compression (5 min)
constexpr int kTimeoutDismScanMs = 600'000;        ///< DISM ScanHealth (10 min)
constexpr int kTimeoutMalwareScanMs = 900'000;     ///< Malware scan (15 min)
constexpr int kTimeoutSystemRepairMs = 1'800'000;  ///< SFC / DISM RestoreHealth (30 min)
constexpr int kTimeoutDefragMs = 3'600'000;        ///< Disk defragmentation (60 min)
constexpr int kTimeoutNetworkReadMs = 15'000;      ///< waitForReadyRead
constexpr int kTimeoutChocoListMs = 15'000;        ///< Chocolatey list packages
constexpr int kTimeoutThermalQueryMs = 3000;       ///< WMI thermal query
constexpr int kTimeoutSmartQueryMs = 5000;         ///< SMART disk query
constexpr int kTimeoutWifiProfileMs = 1000;        ///< Individual netsh profile query
constexpr int kTimeoutChocoDefaultSec = 300;       ///< Default Chocolatey timeout (seconds)
constexpr int kTimeoutThreadShutdownMs = 15'000;   ///< Worker thread graceful shutdown
constexpr int kTimeoutThreadTerminateMs = 5000;    ///< Worker thread force-terminate wait
constexpr int kTimeoutWorkerResetMs = 3000;        ///< Worker thread reset (resetWorker)

// ============================================================================
// Dialog / window sizes (pixels)
// ============================================================================

// Main window
constexpr int kMainWindowMinW = 900;
constexpr int kMainWindowMinH = 600;
constexpr int kMainWindowInitW = 1200;
constexpr int kMainWindowInitH = 800;

// Standard dialogs
constexpr int kDialogWidthSmall = 380;    ///< Small dialogs (organizer, duplicate)
constexpr int kDialogWidthMedium = 420;   ///< Medium dialogs (network settings)
constexpr int kDialogWidthLarge = 520;    ///< Large dialogs (settings, quick action)
constexpr int kDialogWidthXLarge = 640;   ///< Extra-large dialogs
constexpr int kDialogHeightSmall = 300;   ///< Short dialogs
constexpr int kDialogHeightMedium = 420;  ///< Standard dialogs
constexpr int kDialogHeightLarge = 540;   ///< Tall dialogs

// Wizard windows
constexpr int kWizardWidth = 700;        ///< Backup/restore wizard
constexpr int kWizardHeight = 500;       ///< Backup/restore wizard
constexpr int kWizardLargeWidth = 900;   ///< Customization/restore wizard
constexpr int kWizardLargeHeight = 700;  ///< Customization/restore wizard

// ISO/download dialogs
constexpr int kIsoDialogWidthWin = 720;   ///< Windows ISO dialog
constexpr int kIsoDialogHeightWin = 620;  ///< Windows ISO dialog
constexpr int kIsoDialogWidthLin = 780;   ///< Linux ISO dialog
constexpr int kIsoDialogHeightLin = 680;  ///< Linux ISO dialog

// Common widget dimensions
constexpr int kIconSize = 64;             ///< Application icon
constexpr int kButtonHeightTall = 48;     ///< Tall action buttons
constexpr int kButtonHeightStd = 40;      ///< Standard button height
constexpr int kButtonHeightCompact = 36;  ///< Compact transfer buttons
constexpr int kButtonWidthSmall = 100;    ///< Small button min width
constexpr int kButtonWidthMedium = 120;   ///< Medium button min width
constexpr int kButtonWidthLarge = 140;    ///< Large button min width
constexpr int kButtonWidthXLarge = 160;   ///< Extra-large button width

// Progress bar
constexpr int kProgressBarMaxW = 250;  ///< Main window progress bar max width
constexpr int kProgressBarMaxH = 18;   ///< Main window progress bar max height

// Log/list areas
constexpr int kLogAreaMaxH = 200;       ///< Log text area max height
constexpr int kListAreaMinH = 100;      ///< List widget min height
constexpr int kListAreaMaxH = 160;      ///< List widget max height
constexpr int kLogAreaSmallMaxH = 150;  ///< Compact log area max height

// Detachable log window
constexpr int kDetachLogMinW = 360;
constexpr int kDetachLogMinH = 200;
constexpr int kDetachLogInitW = 420;
constexpr int kDetachLogInitH = 300;
constexpr int kSnapButtonW = 80;
constexpr int kSnapButtonH = 28;

// Image flasher settings
constexpr int kFlasherSettingsW = 500;
constexpr int kFlasherSettingsH = 450;

// Info button / tooltip
constexpr int kInfoButtonSize = 20;  ///< Info (i) button diameter
constexpr int kTooltipMaxW = 280;    ///< Info tooltip max width

// Eye button (password toggle)
constexpr int kEyeButtonSize = 22;

// Table columns
constexpr int kCheckboxColumnW = 30;  ///< Checkbox column width
constexpr int kSelectColumnW = 36;    ///< Select column width

// QR code
constexpr int kQrImageSize = 180;  ///< QR code image display size

}  // namespace sak
