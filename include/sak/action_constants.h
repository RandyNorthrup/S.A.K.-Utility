// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file action_constants.h
/// @brief Centralized action, retry, concurrency, and exit-code constants
///
/// All retry counts, retry backoff values, exit codes, and concurrency limits
/// for SAK action and worker classes should be defined here.

#pragma once

#include <cstdint>

namespace sak {

namespace quick_action_elevation {
constexpr char kVerifySystemFilesName[] = "Verify System Files";
constexpr char kVerifySystemFilesTaskId[] = "Verify System Files";
constexpr char kCheckDiskErrorsName[] = "Check Disk Errors";
constexpr char kCheckDiskErrorsTaskId[] = "Check Disk Errors";
constexpr char kResetNetworkName[] = "Reset Network Settings";
constexpr char kResetNetworkTaskId[] = "Reset Network";
constexpr char kBackupBitLockerKeysName[] = "BitLocker Key Backup";
constexpr char kBackupBitLockerKeysTaskId[] = "Backup BitLocker Keys";
}  // namespace quick_action_elevation

// ============================================================================
// Retry / backoff policy
// ============================================================================

constexpr int kRetryCountDefault = 3;      ///< Default retry attempts
constexpr int kRetryCountHigh = 5;         ///< Higher retry attempts
constexpr int kRetryBackoffSlowMs = 5000;  ///< Slow retry backoff

// ============================================================================
// Process exit codes
// ============================================================================

constexpr int kExitSuccess = 0;
constexpr int kExitRebootRequired = 3010;  ///< Windows restart needed

// ============================================================================
// Chocolatey / package manager
// ============================================================================

constexpr int kChocoTimeoutDefaultSec = 300;   ///< Default choco --execution-timeout
constexpr int kChocoTimeoutExtendedSec = 900;  ///< Extended choco timeout

// ============================================================================
// Progress step helpers
// ============================================================================

/// Standard progress markers for multi-step actions.
/// Instead of hardcoding percentages, actions can use these to indicate
/// logical phases of their workflow.
namespace progress {
constexpr int kStart = 0;
constexpr int kStep3 = 3;
constexpr int kStep5 = 5;
constexpr int kStep10 = 10;
constexpr int kStep15 = 15;
constexpr int kStep25 = 25;
constexpr int kStep30 = 30;
constexpr int kStep35 = 35;
constexpr int kStep40 = 40;
constexpr int kStep45 = 45;
constexpr int kStep50 = 50;
constexpr int kStep60 = 60;
constexpr int kStep65 = 65;
constexpr int kStep70 = 70;
constexpr int kStep75 = 75;
constexpr int kStep80 = 80;
constexpr int kStep85 = 85;
constexpr int kStep90 = 90;
constexpr int kStep95 = 95;
constexpr int kComplete = 100;
}  // namespace progress

}  // namespace sak
