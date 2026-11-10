// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_properties_calc.h
/// @brief Pure size-walk and hash-labeling logic for the properties dialog.
///
/// Extracted from the dialog so the completeness tracking and hash truncation
/// marking are unit testable with a synthetic directory lister, independent of
/// the file-system bridge.

#pragma once

#include "sak/file_management_file_system.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

namespace sak {

/// Result of a recursive size walk. @c complete is false when any part of the
/// tree was not fully counted (depth cap, a directory that failed to list, or a
/// directory truncated at the per-directory entry cap), so the caller can show
/// the total as a lower bound rather than an exact -- and wrong -- figure.
struct TreeSizeResult {
    quint64 bytes{0};
    bool complete{true};
};

/// Lists one directory: (path, max_entries) -> listing. Injected so the walk is
/// testable without the file-system bridge.
using DirectoryLister =
    std::function<FileManagementListResult(const QString& path, int max_entries)>;

/// Recursively sum the regular-file sizes under @p path.
[[nodiscard]] TreeSizeResult treeSize(const DirectoryLister& list,
                                      const QString& path,
                                      int max_depth,
                                      int max_entries_per_directory);

/// Sum @p entries: directories are walked, files add their own size.
[[nodiscard]] TreeSizeResult combinedSize(const DirectoryLister& list,
                                          const QVector<FileManagementEntry>& entries,
                                          int max_depth,
                                          int max_entries_per_directory);

/// True when a file larger than the hash byte cap will be digested from a prefix
/// only, so the digest is not the whole-file hash.
[[nodiscard]] bool hashInputTruncated(quint64 full_size, quint64 cap_bytes);

/// Annotate a digest that was computed over a truncated prefix so it is never
/// mistaken for the full-file hash. Returns @p digest_hex unchanged otherwise.
[[nodiscard]] QString formatHashValue(const QString& digest_hex, bool truncated, quint64 cap_bytes);

}  // namespace sak
