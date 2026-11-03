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
    MigrationReport();
    ~MigrationReport() = default;

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

    /**
     * @brief Report metadata
     */
    struct ReportMetadata {
        QString source_machine;
        QString source_os;
        QString source_os_version;
        QString created_by;
        QDateTime created_at;
        int total_apps{0};
        int matched_apps{0};
        int selected_apps{0};
        double match_rate{0.0};
        QString report_version;  // Format version for compatibility
    };

    void addEntry(const MigrationEntry& entry);

    void selectAll();

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

    int getEntryCount() const { return static_cast<int>(m_entries.size()); }
    int getSelectedCount() const;
    int getMatchedCount() const;
    int getUnmatchedCount() const;

    const ReportMetadata& getMetadata() const { return m_metadata; }
    ReportMetadata& getMetadata() { return m_metadata; }

    // Clear
    void clear();

private:
    // Helper methods
    static QString getComputerName();
    static QString getCurrentUser();

    // Data members
    std::vector<MigrationEntry> m_entries;
    ReportMetadata m_metadata;
};

}  // namespace sak
