// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file recycle_bin.h
/// @brief Shared Windows Recycle Bin helper.

#pragma once

#include <QString>

namespace sak {

/// True only when @p path sits on a volume that actually HAS a Recycle Bin.
/// SHFileOperationW with FOF_ALLOWUNDO deletes PERMANENTLY on a volume without
/// one (network shares, removable and optical media), so a caller that promised
/// the user a recoverable delete must consult this and refuse rather than let the
/// request silently become unrecoverable. A fixed volume is the reliable proxy;
/// a UNC or device path is refused outright, without a network round trip.
[[nodiscard]] bool pathVolumeHasRecycleBin(const QString& path);

/// Move @p path (a file or a whole directory tree) to the Windows Recycle Bin.
/// @p path is resolved to an ABSOLUTE path first: SHFileOperationW ignores
/// FOF_ALLOWUNDO when pFrom is not fully qualified (the item would be destroyed
/// PERMANENTLY while this reported a recoverable recycle), and a relative spelling
/// also races the process-wide current directory.
/// Returns false when the shell refused or aborted the operation, and ALSO when
/// @ref pathVolumeHasRecycleBin is false: on such a volume the shell would delete
/// the item permanently, and a recycle request must never quietly become an
/// unrecoverable delete. Callers surface that refusal so the user can ask for an
/// explicit permanent delete instead. When @p error is non-null it receives the
/// real reason for a false return (rejected input, unresolvable path, no Recycle
/// Bin on the volume, or the shell's own error code) instead of a bare false.
[[nodiscard]] bool sendPathToRecycleBin(const QString& path, QString* error = nullptr);

}  // namespace sak
