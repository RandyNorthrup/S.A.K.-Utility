// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QString>

#include <vector>

namespace sak {

/**
 * @brief MigrationReport - the list of applications an install run works through.
 *
 * The install panel fills it from its queue, and the install worker records each entry's outcome
 * in it.
 */
class MigrationReport {
public:
    /**
     * @brief Migration entry - combines app info with match result
     */
    struct MigrationEntry {
        // Source app information
        QString app_name;
        QString app_version;
        QString app_publisher;
        QString install_location;
        QDateTime install_date;
        QString registry_key;

        // Match information
        QString choco_package;
        double confidence{0.0};
        QString match_type;  // "exact", "fuzzy", "search", "manual", "none"
        bool available{false};
        QString available_version;

        // Migration control
        bool selected{false};      // User wants to migrate this app
        bool version_lock{false};  // Install specific version (if available)
        QString locked_version;    // Version to install when locked
        QString notes;             // User notes

        // Execution status
        QString status;  // "pending", "installing", "success", "failed", "skipped"
        QString error_message;
        QDateTime executed_at;
    };

    void addEntry(const MigrationEntry& entry);

    // Accessors
    const std::vector<MigrationEntry>& getEntries() const { return m_entries; }
    std::vector<MigrationEntry>& getEntries() { return m_entries; }

    /// @brief Bounds-checked entry access. std::vector::operator[] is UB on a bad index; an
    /// out-of-range (or negative) index instead returns a reference to a shared default entry so a
    /// stale index from the GUI can never read or write past the vector.
    const MigrationEntry& getEntry(int index) const {
        static const MigrationEntry kEmptyEntry{};
        if (index < 0 || index >= static_cast<int>(m_entries.size())) {
            return kEmptyEntry;
        }
        return m_entries[static_cast<size_t>(index)];
    }
    MigrationEntry& getEntry(int index) {
        // Callers (e.g. app_installation_worker) mutate the returned reference, so a valid index
        // must yield the live entry. An out-of-range index instead returns a thread_local scratch:
        // it keeps the bounds-safety guard (a stray OOB write is discarded, not UB) while removing
        // the cross-thread data race a single shared static would create (P10-55).
        static thread_local MigrationEntry s_scratch_entry;
        if (index < 0 || index >= static_cast<int>(m_entries.size())) {
            s_scratch_entry = MigrationEntry{};  // reset so a prior stray write never leaks back
            return s_scratch_entry;
        }
        return m_entries[static_cast<size_t>(index)];
    }

private:
    std::vector<MigrationEntry> m_entries;
};

}  // namespace sak
