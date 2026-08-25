// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_worker.h
/// @brief Worker thread for directory-recursive file content searching
///
/// Extends WorkerBase to perform text, regex, metadata, archive, and binary
/// searches on a background thread with cancellation and progress reporting.

#pragma once

#include "sak/advanced_search_types.h"
#include "sak/worker_base.h"

#include <QByteArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <expected>
#include <optional>
#include <type_traits>

namespace sak {

/// @brief Worker thread that executes file content searches
///
/// Runs directory-recursive search on a background thread using WorkerBase.
/// Supports text content, regex, image metadata, file metadata, archive, and
/// binary/hex search modes. Results are emitted in batches for UI responsiveness.
class AdvancedSearchWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Construct search worker with configuration
    /// @param config Search configuration
    /// @param parent Optional parent QObject
    explicit AdvancedSearchWorker(SearchConfig config, QObject* parent = nullptr);

    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    //
    // cppcheck-suppress duplInheritedMember  ; false positive: this destructor CALLS
    // WorkerBase::stopAndJoin(), it does not redeclare it. cppcheck attributes the call inside
    // an inline destructor body to the derived class and reports a duplicate member that does
    // not exist -- this class declares no stopAndJoin of its own.
    ~AdvancedSearchWorker() override { stopAndJoin(); }

    // Disable copy/move (inherited from WorkerBase)
    AdvancedSearchWorker(const AdvancedSearchWorker&) = delete;
    AdvancedSearchWorker& operator=(const AdvancedSearchWorker&) = delete;
    AdvancedSearchWorker(AdvancedSearchWorker&&) = delete;
    AdvancedSearchWorker& operator=(AdvancedSearchWorker&&) = delete;

    /// @brief The first invalid exclude regex in @p patterns, or nullopt if all
    /// are valid. A silently-dropped invalid exclude would let the search descend
    /// into paths the user asked to exclude, so the caller fails closed on it.
    [[nodiscard]] static std::optional<QString> firstInvalidExcludePattern(
        const QStringList& patterns);

    /// @brief Files that could not be READ (open failed, over a size/line cap, or
    /// whose lines were too long to scan) on the last run. These produce no matches
    /// indistinguishable from a clean file, so a non-zero count means the results
    /// are incomplete (a false-negative risk) and must be surfaced, not reported as
    /// an authoritative "no matches".
    [[nodiscard]] int filesUnreadable() const { return m_files_unreadable; }

    /// @brief Whether the last run failed to cover everything it was asked to
    /// cover: an unreadable/over-cap file, a failed or truncated directory
    /// listing, an archive whose entry chain stopped early, or a hit result cap.
    /// A "0 matches" (or low) count is then NOT authoritative and callers must
    /// report the run as incomplete rather than as complete.
    [[nodiscard]] bool scanIncomplete() const { return m_scan_incomplete; }

    /// @brief Human-readable reasons the last run was incomplete (capped list).
    [[nodiscard]] QStringList incompleteReasons() const { return m_incomplete_notes; }

    /// @brief Whether @p path must go through the bounded network-accessibility
    /// probe: a UNC path (\\server\share) or a path on a mapped network drive.
    /// An unresponsive SMB share otherwise blocks the whole walk.
    [[nodiscard]] static bool isNetworkPath(const QString& path);

    /// @brief The isNetworkPath decision with the Win32 drive type injected.
    /// @param path            Path being classified.
    /// @param root_drive_type GetDriveTypeW() of @p path's "X:\" root, or 0
    ///                        (DRIVE_UNKNOWN) when it has no drive-letter root.
    ///
    /// Split out of isNetworkPath so the mapped-network-drive rule is unit
    /// testable without an actual SMB mapping.
    [[nodiscard]] static bool requiresNetworkProbe(const QString& path,
                                                   unsigned int root_drive_type);

    /// @brief Outcome of one bounded overlapped read window over a network share.
    enum class NetworkReadStep {
        Complete,   ///< The full window landed; keep reading.
        EndOfFile,  ///< Fewer bytes than asked for arrived, so the file ended
                    ///< here. Whatever DID arrive is real and must be kept.
        TimedOut,   ///< The wait expired. Fail closed: the caller cancels the I/O
                    ///< and discards the whole buffer, because handing back a
                    ///< truncated read would make a hung share look like a clean
                    ///< "no match" for everything it never delivered.
        Failed      ///< The I/O reported an error.
    };

    /// @brief Classify one bounded overlapped read window.
    /// @param wait_signalled   The completion event signalled inside the budget.
    /// @param io_succeeded     GetOverlappedResult reported success. End-of-file
    ///                         counts as success: the bytes that landed are real.
    /// @param bytes_read       Bytes the I/O reported.
    /// @param bytes_requested  Bytes the window asked for.
    ///
    /// Pure, so the fail-closed rule (a timeout is a FAILURE, never a short read)
    /// is unit-testable without an actual hung SMB server.
    [[nodiscard]] static NetworkReadStep classifyNetworkReadStep(bool wait_signalled,
                                                                 bool io_succeeded,
                                                                 qint64 bytes_read,
                                                                 qint64 bytes_requested);

    /// @brief Per-window wall-clock budget in milliseconds for a network read,
    /// derived from the configured network_timeout_sec. An unset (<=0) value takes
    /// the documented default and the result is clamped so the seconds-to-
    /// milliseconds conversion cannot overflow int -- there is no "no timeout".
    [[nodiscard]] static int networkReadTimeoutMs(int network_timeout_sec);

Q_SIGNALS:
    /// @brief Emitted with accumulated results (batch updates for performance)
    void resultsReady(QVector<sak::SearchMatch> matches);

    /// @brief Emitted per-file as search progresses
    void fileSearched(const QString& filePath, int matchCount);

protected:
    /// @brief Main search execution -- runs on worker thread
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /// @brief Check if path should be excluded from search
    [[nodiscard]] bool isExcluded(const QString& path) const;

    /// @brief Search a single file for the pattern (dispatches by type)
    [[nodiscard]] QVector<SearchMatch> searchFile(const QString& filePath,
                                                  const QRegularExpression& regex);

    /// @brief Search file text content line-by-line with context extraction
    [[nodiscard]] QVector<SearchMatch> searchTextContent(const QString& filePath,
                                                         const QRegularExpression& regex);

    /// @brief Search the one file named by root_path (the non-directory root)
    void searchSingleFile(const QRegularExpression& regex, int& total_matches, int& total_files);

    /// @brief Append a context-carrying match for every regex hit in @p lines,
    ///        stopping at the configured max_results or on a stop request
    void scanLinesForMatches(const QString& file_path,
                             const QStringList& lines,
                             const QRegularExpression& regex,
                             QVector<SearchMatch>& matches);

    /// @brief Whether @p line is short enough to hand to the regex engine.
    ///
    /// Qt exposes no match/time limit for PCRE2, so an over-long line is skipped
    /// rather than scanned. The skip is recorded once per file (via
    /// @p already_recorded) so the omission is surfaced, never silent.
    [[nodiscard]] bool lineWithinScanLimit(const QString& file_path,
                                           const QString& line,
                                           int line_index,
                                           bool& already_recorded);

    /// @brief Search image metadata (EXIF, GPS, PNG) for the pattern
    [[nodiscard]] QVector<SearchMatch> searchImageMetadata(const QString& filePath,
                                                           const QRegularExpression& regex);

    /// @brief Search file metadata (PDF, Office, audio, video) for the pattern
    [[nodiscard]] QVector<SearchMatch> searchFileMetadata(const QString& filePath,
                                                          const QRegularExpression& regex);

    /// @brief Search inside archive files (ZIP, EPUB)
    [[nodiscard]] QVector<SearchMatch> searchArchive(const QString& filePath,
                                                     const QRegularExpression& regex);

    /// @brief Search binary file for hex/text patterns
    [[nodiscard]] QVector<SearchMatch> searchBinary(const QString& filePath,
                                                    const QRegularExpression& regex);

    /// @brief Check network path accessibility with timeout
    [[nodiscard]] bool checkNetworkPathAccessible(const QString& path) const;

    /// @brief Whether reads of @p file_path go over a network share.
    ///
    /// The root's classification is computed once per run (every scanned file
    /// lives under it) and a UNC path is caught by a cheap prefix test, so the
    /// local walk never pays for a per-file drive-type lookup.
    [[nodiscard]] bool readsOverNetwork(const QString& file_path) const;

    /// @brief Read at most @p max_bytes leading bytes of @p file_path.
    ///
    /// A local path takes the plain QFile read and stays fast. A path on a UNC
    /// share or a mapped network drive takes a bounded overlapped read instead:
    /// QFile offers no read timeout, so an unresponsive SMB server would block
    /// this worker thread with nothing able to interrupt it. On failure the error
    /// string is the caller's recordUnreadableFile reason; a partial buffer is
    /// never returned as if it were the file.
    [[nodiscard]] std::expected<QByteArray, QString> readSearchFileBytes(const QString& file_path,
                                                                         qint64 max_bytes) const;

    /// @brief Metadata for @p ext built from the already-read prefix @p leading,
    ///        reading any extra bytes that one format needs through the same
    ///        bounded reader. Unexpected carries the fail-closed read error.
    [[nodiscard]] std::expected<QMap<QString, QString>, QString> metadataForExtension(
        const QString& file_path, const QString& ext, const QByteArray& leading) const;

    /// @brief Compile the search regex from config
    [[nodiscard]] std::expected<QRegularExpression, QString> compileRegex() const;

    /// @brief Check if a file extension matches configured filters
    [[nodiscard]] bool matchesExtensionFilter(const QString& filePath) const;

    /// @brief Prepare regex + exclusion patterns before search
    [[nodiscard]] std::expected<QRegularExpression, sak::error_code> prepareSearchConfig();

    /// @brief Run directory-recursive search loop
    void runDirectorySearch(const QRegularExpression& regex, int& total_matches, int& total_files);

    /// @brief Emit the terminal progress line for a finished run: "complete" only
    ///        when nothing was skipped, capped or truncated, otherwise an explicit
    ///        INCOMPLETE notice so a low match count is never read as authoritative
    void reportScanOutcome(int total_matches, int total_files, int files_scanned);

    /// @brief Run a search through FileManagementFileSystemBridge
    void runFileSystemTargetSearch(const QRegularExpression& regex,
                                   int& total_matches,
                                   int& total_files);

    /// @brief Process a single file during search, returns false if max results reached
    bool processSearchFile(const QString& file_path,
                           const QRegularExpression& regex,
                           QVector<SearchMatch>& batch_matches,
                           int& total_matches,
                           int& total_files);

    /// @brief Process a virtual/raw target file during search
    bool processTargetFile(const FileManagementEntry& file,
                           const QRegularExpression& regex,
                           QVector<SearchMatch>& batch_matches,
                           int& total_matches,
                           int& total_files);

    /// @brief Check if a file should be skipped (excluded, wrong extension, too large)
    [[nodiscard]] bool shouldSkipFile(const QString& file_path) const;

    /// @brief Check if a virtual/raw target file should be skipped
    [[nodiscard]] bool shouldSkipTargetFile(const FileManagementEntry& file) const;

    /// @brief Mutable accumulators threaded through a recursive target search.
    struct TargetBatchState {
        QVector<SearchMatch> batch_matches;
        int file_count = 0;
    };

    /// @brief Stream-search a virtual/raw target directory recursively
    bool searchTargetDirectory(const QString& directory_path,
                               const QRegularExpression& regex,
                               int& total_matches,
                               int& total_files,
                               TargetBatchState& batch);

    /// @brief Record that the run omitted something (failed/truncated listing,
    ///        filtered-out explicit target, hit result cap)
    void markScanIncomplete(const QString& note);

    /// @brief Record a file that could not be fully searched: bumps the unreadable
    ///        count AND marks the run incomplete, so an empty result for it is
    ///        never reported as an authoritative "no matches"
    void recordUnreadableFile(const QString& file_path, const QString& reason);

    /// @brief Process one regular-file entry during a recursive target search
    bool processTargetEntry(const FileManagementEntry& entry,
                            const QRegularExpression& regex,
                            int& total_matches,
                            int& total_files,
                            TargetBatchState& batch);

    /// @brief Search a virtual/raw target file from bytes
    [[nodiscard]] QVector<SearchMatch> searchTargetFile(const FileManagementEntry& file,
                                                        const QRegularExpression& regex);

    /// @brief Search text bytes with normal line-context behavior
    [[nodiscard]] QVector<SearchMatch> searchTextBytes(const QString& file_path,
                                                       const QByteArray& data,
                                                       const QRegularExpression& regex);

    /// @brief Search binary bytes from a virtual/raw target file
    [[nodiscard]] QVector<SearchMatch> searchBinaryBytes(const QString& file_path,
                                                         const QByteArray& data,
                                                         const QRegularExpression& regex) const;

    /// @brief Build a SearchMatch with context lines around a regex hit
    [[nodiscard]] SearchMatch buildContextMatch(const QString& file_path,
                                                const QStringList& lines,
                                                int line_index,
                                                const QRegularExpressionMatch& regex_match) const;

    struct ArchiveEntry {
        QString name;
        QString path;
        int data_start = 0;
        int entry_size = 0;
        int next_offset = 0;
        std::uint16_t compression_method = 0;
        std::uint32_t uncompressed_size = 0;
    };

    [[nodiscard]] std::optional<ArchiveEntry> readArchiveEntry(const QByteArray& archive_data,
                                                               const QString& file_path,
                                                               int offset) const;

    [[nodiscard]] bool appendArchiveNameMatches(const ArchiveEntry& entry,
                                                const QRegularExpression& regex,
                                                QVector<SearchMatch>& matches) const;

    [[nodiscard]] bool appendArchiveContentMatches(const QByteArray& archive_data,
                                                   const ArchiveEntry& entry,
                                                   const QRegularExpression& regex,
                                                   QVector<SearchMatch>& matches);

    [[nodiscard]] bool appendArchiveLineMatches(const QString& archive_path,
                                                const QStringList& lines,
                                                const QRegularExpression& regex,
                                                QVector<SearchMatch>& matches);

    /// @brief Walk the ZIP local-file-header chain, appending name/content matches.
    /// @return The offset the walk stopped at (which must be the central directory
    ///         for the archive to have been fully covered), or -1 when the walk was
    ///         cut short by the result cap or a stop request.
    [[nodiscard]] qsizetype scanArchiveEntries(const QByteArray& archive_data,
                                               const QString& file_path,
                                               const QRegularExpression& regex,
                                               QVector<SearchMatch>& matches);

    [[nodiscard]] QVector<SearchMatch> collectMetadataMatches(
        const QString& file_path,
        const QRegularExpression& regex,
        const QMap<QString, QString>& metadata) const;

    SearchConfig m_config;

    /// @brief Compiled exclusion patterns (built once in execute())
    QVector<QRegularExpression> m_compiled_excludes;

    /// @brief Files that could not be read on the last run (see filesUnreadable).
    int m_files_unreadable{0};

    /// @brief Whether the last run omitted anything (see scanIncomplete).
    bool m_scan_incomplete{false};

    /// @brief Why the last run was incomplete (capped, see markScanIncomplete).
    QStringList m_incomplete_notes;

    /// @brief Whether this run's root lives on a network share (see readsOverNetwork).
    bool m_root_on_network{false};

    /// @brief Batch size for result emission
    static constexpr int kBatchSize = 50;
};

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

/// AdvancedSearchWorker must inherit WorkerBase.
static_assert(std::is_base_of_v<WorkerBase, AdvancedSearchWorker>,
              "AdvancedSearchWorker must inherit WorkerBase.");

/// AdvancedSearchWorker must not be copyable.
static_assert(!std::is_copy_constructible_v<AdvancedSearchWorker>,
              "AdvancedSearchWorker must not be copy-constructible.");

/// AdvancedSearchWorker must not be movable.
static_assert(!std::is_move_constructible_v<AdvancedSearchWorker>,
              "AdvancedSearchWorker must not be move-constructible.");

}  // namespace sak
