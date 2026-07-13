// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file recycle_bin.h
/// @brief Shared Windows Recycle Bin helper.

#pragma once

#include <QString>

namespace sak {

/// Move @p path (a file or a whole directory tree) to the Windows Recycle Bin.
/// Returns false when the shell refused or aborted the operation. On volumes
/// without a bin the shell deletes permanently, matching the Files app's
/// fallback when an item cannot be sent to the bin.
[[nodiscard]] bool sendPathToRecycleBin(const QString& path);

}  // namespace sak
