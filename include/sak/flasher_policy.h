// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file flasher_policy.h
/// @brief Pure decisions derived from the persisted Image Flasher settings.
///
/// These functions turn a stored setting into the answer the flasher acts on:
/// whether a target drive must trigger the large-drive warning, and how many
/// queued drives may start writing right now. They are free of Qt widgets,
/// Win32 and I/O so the behaviour behind each setting is unit-testable without
/// a drive, and so the panel and the coordinator cannot drift apart in how
/// they read the same value.

#pragma once

#include "sak/layout_constants.h"

#include <cstdint>
#include <limits>

namespace sak {

/// @brief Convert a large-drive warning threshold in GB to bytes.
/// @param thresholdGb Threshold as stored by ConfigManager.
/// @return The threshold in bytes, or -1 when @p thresholdGb is not usable
///         (non-positive).
/// @note -1 is not "no threshold". A caller that cannot establish the threshold
///       must fail closed and warn, never treat the check as satisfied.
/// @note There is no overflow guard because the widest possible input cannot
///       overflow the result: INT_MAX GB is about 2.3e18 bytes, a quarter of
///       int64's range. A guard here would be a branch no input can reach.
[[nodiscard]] constexpr int64_t largeDriveThresholdBytes(int thresholdGb) {
    static_assert(static_cast<int64_t>(std::numeric_limits<int>::max()) <=
                      std::numeric_limits<int64_t>::max() / kBytesPerGB,
                  "an int GB threshold must not be able to overflow the byte conversion");
    if (thresholdGb <= 0) {
        return -1;
    }
    return static_cast<int64_t>(thresholdGb) * kBytesPerGB;
}

/// @brief Does a target drive have to trigger the large-drive warning?
/// @param driveBytes Drive capacity in bytes, or < 0 when it is not known.
/// @param thresholdBytes Threshold from largeDriveThresholdBytes(), or < 0 when
///        the configured threshold was unusable.
/// @return true when the warning MUST be shown.
/// @note Fails closed on both unknowns. A drive whose capacity could not be read
///       is exactly the case the warning exists for -- "large drives are rarely
///       USB sticks" -- so an unreadable capacity warns rather than passing
///       silently, and an unusable threshold warns about every drive rather than
///       skipping a safety prompt the user asked for.
[[nodiscard]] constexpr bool driveExceedsLargeThreshold(int64_t driveBytes,
                                                        int64_t thresholdBytes) {
    if (driveBytes < 0 || thresholdBytes < 0) {
        return true;
    }
    return driveBytes > thresholdBytes;
}

/// @brief How many queued drives may begin writing right now.
/// @param maxConcurrent Configured concurrent-write ceiling (>= 1).
/// @param running Workers currently writing.
/// @param pending Workers created but not yet started.
/// @return The number of pending workers to start, in [0, pending].
/// @note A maxConcurrent below 1 would stall the run forever (nothing could ever
///       start), so it is treated as 1: the setting is a ceiling on parallelism,
///       never a reason to write nothing. ConfigManager already rejects a
///       non-positive value on read; this keeps the function total.
[[nodiscard]] constexpr int startableWorkerCount(int maxConcurrent, int running, int pending) {
    if (pending <= 0) {
        return 0;
    }
    const int ceiling = maxConcurrent < 1 ? 1 : maxConcurrent;
    const int free_slots = ceiling - (running < 0 ? 0 : running);
    if (free_slots <= 0) {
        return 0;
    }
    return free_slots < pending ? free_slots : pending;
}

}  // namespace sak
