// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_properties_calc.cpp
/// @brief Pure size-walk and hash-labeling logic for the properties dialog.

#include "sak/file_explorer_properties_calc.h"

namespace sak {

namespace {
constexpr quint64 kBytesPerMebibyte = 1024ULL * 1024ULL;
}  // namespace

TreeSizeResult treeSize(const DirectoryLister& list,
                        const QString& path,
                        const int max_depth,
                        const int max_entries_per_directory) {
    TreeSizeResult result;
    if (max_depth < 0) {
        // Beyond the depth cap: this subtree is not counted, so the total the
        // caller reports is only a lower bound.
        result.complete = false;
        return result;
    }
    const FileManagementListResult listing = list(path, max_entries_per_directory);
    if (!listing.ok) {
        result.complete = false;
        return result;
    }
    // A directory that filled the entry cap was almost certainly truncated, so
    // its contribution cannot be trusted as complete.
    if (max_entries_per_directory > 0 && listing.entries.size() >= max_entries_per_directory) {
        result.complete = false;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        if (entry.directory) {
            const TreeSizeResult sub =
                treeSize(list, entry.path, max_depth - 1, max_entries_per_directory);
            result.bytes += sub.bytes;
            result.complete = result.complete && sub.complete;
        } else if (entry.regular_file) {
            result.bytes += entry.size_bytes;
        }
    }
    return result;
}

TreeSizeResult combinedSize(const DirectoryLister& list,
                            const QVector<FileManagementEntry>& entries,
                            const int max_depth,
                            const int max_entries_per_directory) {
    TreeSizeResult result;
    for (const FileManagementEntry& entry : entries) {
        if (entry.directory) {
            const TreeSizeResult sub =
                treeSize(list, entry.path, max_depth, max_entries_per_directory);
            result.bytes += sub.bytes;
            result.complete = result.complete && sub.complete;
        } else {
            result.bytes += entry.size_bytes;
        }
    }
    return result;
}

bool hashInputTruncated(const quint64 full_size, const quint64 cap_bytes) {
    return cap_bytes > 0 && full_size > cap_bytes;
}

QString formatHashValue(const QString& digest_hex, const bool truncated, const quint64 cap_bytes) {
    if (!truncated) {
        return digest_hex;
    }
    return digest_hex +
           QStringLiteral(" (hash of first %1 MB only)").arg(cap_bytes / kBytesPerMebibyte);
}

}  // namespace sak
