// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/file_hash.h"
#include "sak/file_management_file_system.h"
#include "sak/worker_base.h"

#include <QMap>
#include <QMutex>
#include <QString>
#include <QVector>

#include <atomic>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @brief Worker thread for duplicate file detection
 *
 * Scans directories for duplicate files using SHA-256 hash comparison.
 * Groups duplicates and reports space savings potential.
 *
 * Thread-Safety: All signals are emitted from worker thread and should
 * be connected with Qt::QueuedConnection.
 */
class DuplicateFinderWorker : public WorkerBase {
    Q_OBJECT

public:
    /**
     * @brief Information about a duplicate file group
     */
    struct DuplicateGroup {
        QString hash;                 ///< SHA-256 hash of files
        QVector<QString> file_paths;  ///< Paths to duplicate files
        qint64 file_size{0};          ///< Size of each file
        qint64 wasted_space{0};       ///< Total space wasted by duplicates
    };

    /**
     * @brief Configuration for duplicate finder operation
     */
    struct Config {
        QVector<QString> scanDirectories;              ///< Directories to scan
        sak::FileManagementTarget file_system_target;  ///< Optional raw/image target
        QVector<QString> virtual_directories;          ///< Directories inside file_system_target
        qint64 minimum_file_size{0};                   ///< Minimum file size to consider (bytes)
        bool recursive_scan{true};                     ///< Scan subdirectories
        bool parallel_hashing{true};                   ///< Use parallel hash calculation
        int hash_thread_count{0};                      ///< Thread count (0 = auto-detect)
        bool use_file_system_target{false};            ///< Scan via FileManagementFileSystemBridge
        /// Skip symbolic-link FILE entries via a non-following symlink_status check made BEFORE
        /// is_regular_file()/file_size()/hashing (all of which resolve the target). Defaults to
        /// false (GUI behavior). A headless/injectable caller sets this true so a planted symlink
        /// to a UNC share is never opened for hashing (its first following stat would leak the
        /// credential hash via SMB/NTLM); it also stops a symlink being mis-reported as a duplicate
        /// of its own target. (Junctions target only directories, so they are already excluded here
        /// by the is_regular_file() gate -- only symlink files can reach a hash.)
        bool skip_symlinks{false};
    };

    /**
     * @brief Construct duplicate finder worker
     * @param config Scan configuration
     * @param parent Parent QObject
     */
    explicit DuplicateFinderWorker(const Config& config, QObject* parent = nullptr);

    /**
     * @brief Join the worker thread BEFORE the derived members are destroyed.
     *
     * ~WorkerBase joins the thread, but it runs AFTER this class's members (m_hash_stop, m_hasher,
     * m_config) are already destroyed -- and a still-running execute()/cancel-monitor reads them,
     * so relying on the base dtor is a use-after-free. Joining here (members still alive) closes
     * it, mirroring PartitionApplyWorker / NetworkProbeWorker.
     */
    ~DuplicateFinderWorker() override;

    /**
     * @brief Duplicate groups found by the last run.
     *
     * Populated on the worker thread just before resultsReady is emitted, so it is safe to read
     * from a slot connected to WorkerBase::finished (that emit happens-after execute() returns).
     * The GUI uses the resultsReady summary; this accessor lets a headless caller retrieve the
     * structured groups (paths + sizes) without parsing the summary text. Empty if no duplicates
     * were found or before a run completes.
     */
    [[nodiscard]] const std::vector<DuplicateGroup>& duplicateGroups() const {
        return m_duplicate_groups;
    }

    /**
     * @brief Number of scanned files that could NOT be hashed on the last run.
     *
     * Files that fail to hash (unreadable, locked, vanished mid-scan) are dropped
     * from the comparison set, so a non-zero count means the duplicate results are
     * INCOMPLETE. A caller must surface this rather than report a clean success.
     * Populated on the worker thread before finished is emitted (race-free from a
     * finished-slot, like duplicateGroups()).
     */
    [[nodiscard]] int filesUnhashed() const { return m_files_unhashed; }

    /// @name Bounds for the recursive walk of a raw/image file-system target
    ///
    /// The directory hierarchy inside an image is UNTRUSTED data (APFS/HFS/ext bytes that
    /// may be corrupt or crafted): it can be cyclic or arbitrarily deep, and the bridge's
    /// listDirectory is single-level, so the reader's own B-tree guards do not bound this
    /// hierarchy recursion. These caps mirror the export walker's and keep a hostile image
    /// from driving unbounded recursion (stack exhaustion) or unbounded memory.
    /// @{
    static constexpr int kVirtualWalkMaxDepth = 32;
    static constexpr int kVirtualWalkMaxDirectories = 100'000;
    static constexpr qsizetype kVirtualWalkMaxFiles = 500'000;
    /// @}

    /// @brief True while the image walk is still inside its depth / directory / file
    ///        budget. Evaluated on ENTRY to each directory, so the file count may overshoot
    ///        by at most one directory listing -- it is a bound, not an exact quota.
    ///        A breach REFUSES the scan (a silently truncated file set would misreport
    ///        duplicates), it does not trim the results.
    [[nodiscard]] static bool virtualWalkWithinBounds(int depth,
                                                      int directories_visited,
                                                      qsizetype files_collected);

    /// @brief Usable roots for an image scan: each configured entry trimmed, blanks dropped.
    ///
    /// An EMPTY result means the scan must be refused. It must never fall back to the image
    /// root, which would silently turn a mis-specified scan into a full walk of an untrusted
    /// image.
    [[nodiscard]] static QVector<QString> resolveVirtualRoots(const QVector<QString>& configured);

Q_SIGNALS:
    /**
     * @brief Emitted when scanning progresses
     * @param current_file Current file index
     * @param total_files Total files to scan
     * @param current_path Path being processed
     */
    void scanProgress(int current_file, int total_files, const QString& current_path);

    /**
     * @brief Emitted when duplicate groups are found
     * @param summary Text summary of results
     * @param duplicate_count Number of duplicate files found
     * @param wasted_space Total wasted space in bytes
     */
    void resultsReady(const QString& summary, int duplicate_count, qint64 wasted_space);

protected:
    /**
     * @brief Execute duplicate finding workflow
     * @return Success or error code
     */
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /**
     * @brief Scan all configured directories for files
     * @return File list or error
     */
    auto scanDirectories() -> std::expected<std::vector<std::filesystem::path>, sak::error_code>;

    /**
     * @brief Calculate hash for a single file
     * @param file_path File to hash
     * @return Hash string or error
     */
    auto calculateFileHash(const std::filesystem::path& file_path)
        -> std::expected<std::string, sak::error_code>;

    /**
     * @brief Calculate hashes in parallel for multiple files
     * @param files Files to hash
     * @return Vector of (path, hash) pairs
     */
    auto calculateHashesParallel(const std::vector<std::filesystem::path>& files)
        -> std::expected<std::vector<std::pair<std::filesystem::path, std::string>>,
                         sak::error_code>;

    /// @brief Filter out empty/failed results from parallel hashing
    static std::vector<std::pair<std::filesystem::path, std::string>> filterValidResults(
        const std::vector<std::pair<std::filesystem::path, std::string>>& results);

    /**
     * @brief Group files by hash
     * @param files Files with their hashes
     * @return Map of hash to file paths
     */
    auto groupByHash(const std::vector<std::pair<std::filesystem::path, std::string>>& files) const
        -> std::unordered_map<std::string, std::vector<std::filesystem::path>>;

    /**
     * @brief Generate results summary text
     * @param groups Duplicate groups
     * @return Summary string
     */
    auto generateSummary(const std::vector<DuplicateGroup>& groups) -> QString;

    /// @brief Hash all files (parallel or sequential based on config)
    auto hashFiles(const std::vector<std::filesystem::path>& files)
        -> std::expected<std::vector<std::pair<std::filesystem::path, std::string>>,
                         sak::error_code>;

    /// @brief Start a monitor thread that forwards the worker's cooperative stop (stopRequested())
    /// into m_hash_stop, so file hashing can be cancelled MID-file. Without it, checkStop() is only
    /// evaluated between files, so a large in-flight file could outlast the teardown join and force
    /// QThread::terminate(). The returned jthread stops+joins the monitor when it leaves scope.
    [[nodiscard]] std::jthread startHashCancelMonitor();

    struct VirtualFile {
        QString path;
        qint64 size{0};
    };

    /// @brief Accumulated state of one image walk, carried across the whole recursion so the
    ///        directory and file budgets are TOTALS rather than per-directory counts.
    struct VirtualWalkState {
        QVector<VirtualFile> files;
        int directories_visited{0};
    };

    /// @brief Execute duplicate detection against a raw/image file-system target
    auto executeFileSystemTarget() -> std::expected<void, sak::error_code>;

    /// @brief Collect files from the configured raw/image target
    auto scanFileSystemTarget() -> std::expected<QVector<VirtualFile>, sak::error_code>;

    /// @brief Recursively collect virtual file entries from a target directory. Refuses
    ///        (scan_failed) as soon as the untrusted-image walk bounds are breached.
    auto collectVirtualFiles(const QString& directory_path, VirtualWalkState& state, int depth)
        -> std::expected<void, sak::error_code>;

    /// @brief Apply one listing entry: record an eligible file and/or recurse into a
    ///        subdirectory. Split out so collectVirtualFiles stays inside the complexity
    ///        budget once the walk bounds are enforced.
    auto collectVirtualEntry(const sak::FileManagementEntry& entry,
                             VirtualWalkState& state,
                             int depth) -> std::expected<void, sak::error_code>;

    /// @brief Hash virtual files using the shared bridge reader
    auto hashVirtualFiles(const QVector<VirtualFile>& files)
        -> std::expected<QVector<QPair<VirtualFile, QString>>, sak::error_code>;

    /// @brief Build duplicate groups for virtual file hashes
    std::vector<DuplicateGroup> buildVirtualDuplicateGroups(
        const QVector<QPair<VirtualFile, QString>>& hashed_files,
        int& total_duplicates,
        qint64& total_wasted) const;

    /// @brief Collect files from a single directory with error handling
    auto collectFilesFromDirectory(const std::filesystem::path& dir_path,
                                   std::vector<std::filesystem::path>& files)
        -> std::expected<void, sak::error_code>;

    /// @brief Iterate directory entries, collecting eligible files
    template <typename DirIter>
    auto collectEntries(const std::filesystem::path& dir_path,
                        std::vector<std::filesystem::path>& files)
        -> std::expected<void, sak::error_code>;

    /// @brief Build duplicate groups from hash-grouped files
    void buildDuplicateGroups(
        const std::unordered_map<std::string, std::vector<std::filesystem::path>>& hash_groups,
        std::vector<DuplicateGroup>& duplicate_groups,
        int& total_duplicates,
        qint64& total_wasted);

    Config m_config;
    sak::file_hasher m_hasher;
    std::vector<DuplicateGroup>
        m_duplicate_groups;   ///< Result of the last run (see duplicateGroups)
    int m_files_unhashed{0};  ///< Scanned files that could not be hashed (see filesUnhashed)
    std::stop_source
        m_hash_stop;          ///< Cancels in-flight file hashing (fed by startHashCancelMonitor)
};
