// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_constants.h
/// @brief Named constants for the OST/PST Converter tab

#pragma once

#include <cstdint>

namespace sak::ost {

// ============================================================================
// Thread Limits
// ============================================================================

constexpr int kMinThreads = 1;
constexpr int kMaxThreads = 8;
constexpr int kDefaultThreads = 2;

// ============================================================================
// PST Split Sizes (bytes)
// ============================================================================

constexpr int64_t kSplit2GbBytes = 2LL * 1024 * 1024 * 1024;
constexpr int64_t kSplit5GbBytes = 5LL * 1024 * 1024 * 1024;
constexpr int64_t kSplit10GbBytes = 10LL * 1024 * 1024 * 1024;
constexpr int64_t kDefaultCustomSplitMb = 5120;

// ============================================================================
// EML Writer Limits
// ============================================================================

constexpr int kMaxEmlFilenameLength = 200;
constexpr int kMaxEmlSubjectLength = 150;

// ============================================================================
// Progress / UI
// ============================================================================

constexpr int kProgressPollIntervalMs = 500;
constexpr int kFilePreScanTimeoutMs = 30'000;

// ============================================================================
// Timer / Timeout Durations
// ============================================================================

constexpr int kTimerStatusMessageMs = 3000;
constexpr int kTimerStatusLongMs = 10'000;
constexpr int kTimeoutThreadShutdownMs = 5000;
constexpr int kTimeoutThreadTerminateMs = 2000;

// ============================================================================
// Queue Table Column Indices
// ============================================================================

enum QueueColumn {
    ColFile = 0,
    ColSize,
    ColItems,
    ColStatus,
    ColProgress,
    ColCount
};

}  // namespace sak::ost
