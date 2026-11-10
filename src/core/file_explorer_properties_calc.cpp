// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_properties_calc.cpp
/// @brief Pure size-walk and hash-labeling logic for the properties dialog.

#include "sak/file_explorer_properties_calc.h"

#include <limits>

namespace sak {

namespace {
constexpr quint64 kBytesPerMebibyte = 1024ULL * 1024ULL;

// Add @p add into @p acc, saturating at the quint64 maximum. Returns false when
// the sum would have wrapped. File sizes on a foreign/hostile file system are
// attacker-controlled and could otherwise overflow into a small, plausible-but-
// wrong total; the caller then marks the walk incomplete so the figure reads as
// a lower bound rather than an exact -- and wrong -- number.
bool addBytesSaturating(quint64& acc, quint64 add) {
    if (acc > std::numeric_limits<quint64>::max() - add) {
        acc = std::numeric_limits<quint64>::max();
        return false;
    }
    acc += add;
    return true;
}

// Fold a single directory entry into the running @p result: descend a real
// subdirectory, refuse to follow a symlink/junction, or add a regular file's own
// bytes. Each path also carries its completeness signal -- a truncated subtree, a
// skipped link, or a saturated total -- up into @p result, so the reported figure
// stays honest about whether every byte was actually counted.
void accumulateEntry(TreeSizeResult& result,
                     const DirectoryLister& list,
                     const FileManagementEntry& entry,
                     const int max_depth,
                     const int max_entries_per_directory) {
    if (entry.directory && !entry.symlink) {
        const TreeSizeResult sub =
            treeSize(list, entry.path, max_depth - 1, max_entries_per_directory);
        const bool no_overflow = addBytesSaturating(result.bytes, sub.bytes);
        result.complete = result.complete && sub.complete && no_overflow;
    } else if (entry.directory) {
        // A symlinked / junctioned directory is NOT descended: following it can
        // redirect the walk out of the tree or loop back into it (a cycle that
        // inflates the total), so the "lower bound" would become an over-count.
        // Treat the subtree as not walked -> lower bound, mirroring the
        // export/import walks that skip symlinks.
        result.complete = false;
    } else if (entry.regular_file) {
        const bool no_overflow = addBytesSaturating(result.bytes, entry.size_bytes);
        result.complete = result.complete && no_overflow;
    }
}
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
    // Probe one past the cap so a directory holding EXACTLY the cap count is not
    // mistaken for a truncated one: only MORE entries than the cap means the
    // listing was cut short. Previously an exact-cap directory was always marked
    // incomplete even when it was whole (B8-24).
    // Guard the +1 against signed-int overflow (UB) when the cap is INT_MAX.
    const bool has_entry_cap = max_entries_per_directory > 0;
    const bool cap_has_headroom = max_entries_per_directory < std::numeric_limits<int>::max();
    const int probe_cap = (has_entry_cap && cap_has_headroom) ? max_entries_per_directory + 1
                                                              : max_entries_per_directory;
    const FileManagementListResult listing = list(path, probe_cap);
    if (!listing.ok) {
        result.complete = false;
        return result;
    }
    if (!listing.blockers.isEmpty() || !listing.warnings.isEmpty()) {
        // An ok listing that still surfaced a blocker or warning enumerated only part of
        // the directory (a skipped/undecodable entry on a foreign file system, a
        // truncated read). The byte total is then a lower bound, so mark it incomplete
        // rather than report a partial count as clean.
        result.complete = false;
    }
    if (max_entries_per_directory > 0 && listing.entries.size() > max_entries_per_directory) {
        result.complete = false;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        accumulateEntry(result, list, entry, max_depth, max_entries_per_directory);
    }
    return result;
}

TreeSizeResult combinedSize(const DirectoryLister& list,
                            const QVector<FileManagementEntry>& entries,
                            const int max_depth,
                            const int max_entries_per_directory) {
    TreeSizeResult result;
    for (const FileManagementEntry& entry : entries) {
        if (entry.directory && !entry.symlink) {
            const TreeSizeResult sub =
                treeSize(list, entry.path, max_depth, max_entries_per_directory);
            const bool no_overflow = addBytesSaturating(result.bytes, sub.bytes);
            result.complete = result.complete && sub.complete && no_overflow;
        } else if (entry.directory) {
            // Symlinked / junctioned directory: not descended (see treeSize).
            result.complete = false;
        } else {
            const bool no_overflow = addBytesSaturating(result.bytes, entry.size_bytes);
            result.complete = result.complete && no_overflow;
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
