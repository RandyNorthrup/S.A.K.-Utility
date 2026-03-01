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

// ============================================================================
// Retry / backoff policy
// ============================================================================

constexpr int kRetryCountDefault       = 3;      ///< Default retry attempts
constexpr int kRetryCountHigh          = 5;      ///< Higher retry attempts
constexpr int kRetryBackoffBaseMs      = 2000;   ///< Retry backoff base
constexpr int kRetryBackoffBaseFastMs  = 500;    ///< Fast retry backoff
constexpr int kRetryBackoffMaxMs       = 60000;  ///< Retry backoff ceiling
constexpr int kRetryBackoffSlowMs      = 5000;   ///< Slow retry backoff

// ============================================================================
// Process exit codes
// ============================================================================

constexpr int kExitSuccess           = 0;
constexpr int kExitRebootRequired    = 3010;  ///< Windows restart needed
constexpr int kExitNoUpdate          = 2;     ///< Chocolatey "already installed"

// ============================================================================
// Chocolatey / package manager
// ============================================================================

constexpr int kChocoTimeoutDefaultSec  = 300;   ///< Default choco --execution-timeout
constexpr int kChocoTimeoutExtendedSec = 900;   ///< Extended choco timeout
constexpr int kChocoMaxPackageBatch    = 10;    ///< Max packages in batch install

// ============================================================================
// Progress step helpers
// ============================================================================

/// Standard progress milestones for multi-step actions.
/// Instead of hardcoding percentages, actions can use these to indicate
/// logical phases of their workflow.
namespace progress {
    constexpr int kStart     = 0;
    constexpr int kStep10    = 10;
    constexpr int kStep20    = 20;
    constexpr int kStep25    = 25;
    constexpr int kStep30    = 30;
    constexpr int kStep40    = 40;
    constexpr int kStep50    = 50;
    constexpr int kStep60    = 60;
    constexpr int kStep70    = 70;
    constexpr int kStep75    = 75;
    constexpr int kStep80    = 80;
    constexpr int kStep85    = 85;
    constexpr int kStep90    = 90;
    constexpr int kStep95    = 95;
    constexpr int kComplete  = 100;
} // namespace progress

} // namespace sak
