// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file duplicate_finder_worker.cpp
/// @brief Implements the background worker thread for duplicate file detection

#include "sak/duplicate_finder_worker.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QHash>
#include <QtConcurrent>
#include <QtGlobal>
#include <QThreadPool>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <thread>

namespace {
constexpr std::size_t kExpectedDuplicateScanFiles = 1000;
constexpr int kDuplicateSummarySizePrecision = 2;
constexpr int kDuplicateSummaryGroupLimit = 10;
constexpr int kDuplicateProgressInterval = 10;
constexpr uint64_t kDuplicateVirtualReadMaxBytes = 512ULL * 1024ULL * 1024ULL;
constexpr int kDuplicateVirtualBrowseMaxEntries = 10'000;
// How often the cancel-monitor thread polls the worker's cooperative stop. Small enough that an
// in-flight file hash is interrupted promptly on teardown (well within ~WorkerBase's join window),
// large enough to be negligible on a normal run.
constexpr int kHashCancelPollMs = 50;
}  // namespace

DuplicateFinderWorker::DuplicateFinderWorker(const Config& config, QObject* parent)
    : WorkerBase(parent), m_config(config), m_hasher(sak::hash_algorithm::md5) {}

DuplicateFinderWorker::~DuplicateFinderWorker() {
    // Join the worker thread HERE, while m_hash_stop / m_hasher / m_config are still alive.
    // ~WorkerBase would join only AFTER those members are destroyed, and a still-running
    // execute()/cancel-monitor reads m_hash_stop -> a use-after-free (see the class doc).
    if (isRunning()) {
        requestStop();
        if (!wait(sak::kTimeoutThreadShutdownMs)) {
            terminate();
            wait(sak::kTimeoutThreadTerminateMs);
        }
    }
}

auto DuplicateFinderWorker::execute() -> std::expected<void, sak::error_code> {
    if (m_config.use_file_system_target) {
        return executeFileSystemTarget();
    }

    // No directory to scan -> REFUSE, the same way scanFileSystemTarget() refuses a target with
    // no usable root. Reporting "no files found" for a mis-specified request would hide the
    // caller's error behind a clean success.
    if (m_config.scanDirectories.isEmpty()) {
        sak::logError("Duplicate scan refused: no directory to scan");
        return std::unexpected(sak::error_code::invalid_argument);
    }

    sak::logInfo("Starting duplicate file scan");

    // Scan all directories
    auto files_result = scanDirectories();
    if (!files_result) {
        return std::unexpected(files_result.error());
    }

    const auto& files = files_result.value();
    sak::logInfo("Found {} files to analyze", files.size());

    if (files.empty()) {
        Q_EMIT resultsReady("No files found to scan.", 0, 0);
        return {};
    }

    // Calculate hashes for all files
    auto hashed_result = hashFiles(files);
    if (!hashed_result) {
        return std::unexpected(hashed_result.error());
    }

    const auto& hashed_files = hashed_result.value();
    sak::logInfo("Hashed {} files successfully", hashed_files.size());

    // Files that failed to hash were dropped from the comparison set; record how many so a caller
    // can report that the results are incomplete instead of a clean success.
    m_files_unhashed = static_cast<int>(files.size() - hashed_files.size());
    if (m_files_unhashed > 0) {
        sak::logWarning("{} file(s) could not be hashed and were skipped", m_files_unhashed);
    }

    // Group files by hash and find duplicates
    auto hash_groups = groupByHash(hashed_files);

    std::vector<DuplicateGroup> duplicate_groups;
    int total_duplicates = 0;
    qint64 total_wasted = 0;
    buildDuplicateGroups(hash_groups, duplicate_groups, total_duplicates, total_wasted);

    sak::logInfo("Found {} duplicate groups, {} duplicate files, {} bytes wasted",
                 duplicate_groups.size(),
                 total_duplicates,
                 total_wasted);

    // Expose the structured groups (for a headless caller) before emitting the summary. This runs
    // on the worker thread before resultsReady/finished, so a finished-slot read is race-free.
    m_duplicate_groups = duplicate_groups;

    // Generate and emit results
    const QString summary = generateSummary(duplicate_groups);
    Q_EMIT resultsReady(summary, total_duplicates, total_wasted);

    return {};
}

auto DuplicateFinderWorker::executeFileSystemTarget() -> std::expected<void, sak::error_code> {
    sak::logInfo("Starting duplicate file scan on file-system target: {}",
                 m_config.file_system_target.label.toStdString());

    auto files_result = scanFileSystemTarget();
    if (!files_result) {
        return std::unexpected(files_result.error());
    }
    const auto& files = files_result.value();
    if (files.isEmpty()) {
        Q_EMIT resultsReady("No files found to scan.", 0, 0);
        return {};
    }

    auto hashed_result = hashVirtualFiles(files);
    if (!hashed_result) {
        return std::unexpected(hashed_result.error());
    }

    m_files_unhashed = static_cast<int>(files.size() - hashed_result.value().size());

    int total_duplicates = 0;
    qint64 total_wasted = 0;
    const auto duplicate_groups =
        buildVirtualDuplicateGroups(hashed_result.value(), total_duplicates, total_wasted);

    m_duplicate_groups = duplicate_groups;  // expose structured groups before the summary emit
    Q_EMIT resultsReady(generateSummary(duplicate_groups), total_duplicates, total_wasted);
    return {};
}

std::jthread DuplicateFinderWorker::startHashCancelMonitor() {
    // Cancel-only monitor for the SEQUENTIAL path (the worker thread is blocked in
    // calculateFileHash, so it cannot poll itself mid-file). It dies promptly on stop -- well
    // before any terminate() -- so it never outlives the members it touches. m_hash_stop is reset
    // by the caller (hashFiles).
    return std::jthread([this](const std::stop_token& monitor_stop) {
        while (!monitor_stop.stop_requested()) {
            if (stopRequested()) {
                m_hash_stop.request_stop();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kHashCancelPollMs));
        }
    });
}

auto DuplicateFinderWorker::hashFiles(const std::vector<std::filesystem::path>& files)
    -> std::expected<std::vector<std::pair<std::filesystem::path, std::string>>, sak::error_code> {
    m_hash_stop = std::stop_source{};  // fresh cancel state for this run (both branches)

    if (m_config.parallel_hashing) {
        sak::logInfo("Using parallel hash calculation");
        return calculateHashesParallel(files);  // owns its own cancel + progress monitor
    }

    // Sequential: a cancel-only monitor gives the same mid-file cancel; progress is emitted below
    // on THIS (worker) thread, so the monitor need not report it.
    sak::logInfo("Using sequential hash calculation");
    // cppcheck-suppress unreadVariable ; RAII guard -- the jthread's DESTRUCTOR stops+joins the
    // cancel monitor at scope exit, so the value is used even though it is never read.
    const std::jthread cancel_monitor = startHashCancelMonitor();
    std::vector<std::pair<std::filesystem::path, std::string>> hashed_files;
    const size_t file_count = files.size();
    hashed_files.reserve(file_count);

    for (size_t i = 0; i < file_count; ++i) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        const auto& file = files[i];
        Q_EMIT scanProgress(static_cast<int>(i + 1),
                            static_cast<int>(file_count),
                            QString::fromStdString(file.string()));

        auto hash_result = calculateFileHash(file);
        if (hash_result) {
            hashed_files.emplace_back(file, hash_result.value());
        } else {
            sak::logWarning("Failed to hash file: {}", file.string());
        }
    }
    return hashed_files;
}

void DuplicateFinderWorker::buildDuplicateGroups(
    const std::unordered_map<std::string, std::vector<std::filesystem::path>>& hash_groups,
    std::vector<DuplicateGroup>& duplicate_groups,
    int& total_duplicates,
    qint64& total_wasted) {
    for (const auto& [hash, paths] : hash_groups) {
        if (paths.size() <= 1) {
            continue;
        }

        DuplicateGroup group;
        group.hash = QString::fromStdString(hash);

        for (const auto& path : paths) {
            group.file_paths.push_back(QString::fromStdString(path.string()));
        }

        try {
            group.file_size = std::filesystem::file_size(paths[0]);
            group.wasted_space = group.file_size * (paths.size() - 1);

            duplicate_groups.push_back(group);
            total_duplicates += static_cast<int>(paths.size() - 1);
            total_wasted += group.wasted_space;
        } catch (const std::filesystem::filesystem_error& e) {
            sak::logWarning("Failed to get file size: {}", e.what());
        }
    }
}

template <typename DirIter>
auto DuplicateFinderWorker::collectEntries(const std::filesystem::path& dir_path,
                                           std::vector<std::filesystem::path>& files)
    -> std::expected<void, sak::error_code> {
    for (const auto& entry : DirIter(dir_path)) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        // R2: drop a symbolic-link entry via a non-following symlink_status check BEFORE
        // is_regular_file()/file_size()/hashing, all of which resolve the target. A UNC-targeted
        // symlink would otherwise leak the credential hash on the first following stat. (Junctions
        // point only to directories, so they fall out at the is_regular_file() gate regardless.)
        if (m_config.skip_symlinks && entry.is_symlink()) {
            continue;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (static_cast<qint64>(entry.file_size()) < m_config.minimum_file_size) {
            continue;
        }
        files.push_back(entry.path());
    }
    return {};
}

auto DuplicateFinderWorker::collectFilesFromDirectory(const std::filesystem::path& dir_path,
                                                      std::vector<std::filesystem::path>& files)
    -> std::expected<void, sak::error_code> {
    try {
        if (m_config.recursive_scan) {
            return collectEntries<std::filesystem::recursive_directory_iterator>(dir_path, files);
        }
        return collectEntries<std::filesystem::directory_iterator>(dir_path, files);
    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Error scanning directory {}: {}", dir_path.string(), e.what());
        return std::unexpected(sak::error_code::scan_failed);
    }
}

auto DuplicateFinderWorker::scanDirectories()
    -> std::expected<std::vector<std::filesystem::path>, sak::error_code> {
    std::vector<std::filesystem::path> files;
    files.reserve(kExpectedDuplicateScanFiles);

    for (const auto& dir_str : m_config.scanDirectories) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        const std::filesystem::path dir_path(dir_str.toStdString());

        if (!std::filesystem::exists(dir_path)) {
            sak::logWarning("Directory does not exist: {}", dir_path.string());
            continue;
        }

        if (!std::filesystem::is_directory(dir_path)) {
            sak::logWarning("Path is not a directory: {}", dir_path.string());
            continue;
        }

        auto result = collectFilesFromDirectory(dir_path, files);
        if (!result) {
            return std::unexpected(result.error());
        }
    }

    return files;
}

bool DuplicateFinderWorker::virtualWalkWithinBounds(int depth,
                                                    int directories_visited,
                                                    qsizetype files_collected) {
    return depth >= 0 && depth <= kVirtualWalkMaxDepth &&
           directories_visited < kVirtualWalkMaxDirectories &&
           files_collected < kVirtualWalkMaxFiles;
}

QVector<QString> DuplicateFinderWorker::resolveVirtualRoots(const QVector<QString>& configured) {
    QVector<QString> roots;
    roots.reserve(configured.size());
    for (const QString& root : configured) {
        const QString trimmed = root.trimmed();
        if (!trimmed.isEmpty()) {
            roots.append(trimmed);
        }
    }
    return roots;
}

auto DuplicateFinderWorker::scanFileSystemTarget()
    -> std::expected<QVector<VirtualFile>, sak::error_code> {
    if (!m_config.file_system_target.can_duplicate_scan) {
        sak::logError("Duplicate scan blocked for target: {}",
                      m_config.file_system_target.label.toStdString());
        return std::unexpected(sak::error_code::invalid_argument);
    }

    // No usable root -> REFUSE the scan. Defaulting to the image root would silently turn a
    // mis-specified request into an unbounded walk of the whole untrusted image.
    const QVector<QString> roots = resolveVirtualRoots(m_config.virtual_directories);
    if (roots.isEmpty()) {
        sak::logError("Duplicate scan refused: no directory to scan inside target {}",
                      m_config.file_system_target.label.toStdString());
        return std::unexpected(sak::error_code::invalid_argument);
    }

    VirtualWalkState state;
    for (const auto& root : roots) {
        auto result = collectVirtualFiles(root, state, 0);
        if (!result) {
            return std::unexpected(result.error());
        }
    }
    return state.files;
}

auto DuplicateFinderWorker::collectVirtualFiles(const QString& directory_path,
                                                VirtualWalkState& state,
                                                int depth) -> std::expected<void, sak::error_code> {
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    // The hierarchy comes from an UNTRUSTED image and listDirectory is single-level, so the
    // reader's own cycle guards do not bound THIS recursion: a cyclic or extremely deep
    // directory graph would recurse until the stack is exhausted. A bound breach refuses the
    // scan outright -- a silently truncated file set would misreport the duplicate groups.
    if (!virtualWalkWithinBounds(depth, state.directories_visited, state.files.size())) {
        sak::logError("Duplicate scan refused: {} exceeds the image walk bounds",
                      directory_path.toStdString());
        return std::unexpected(sak::error_code::scan_failed);
    }
    ++state.directories_visited;

    const auto listing = sak::FileManagementFileSystemBridge::listDirectory(
        m_config.file_system_target, directory_path, kDuplicateVirtualBrowseMaxEntries);
    if (!listing.ok) {
        sak::logError("Duplicate scan target listing failed: {}",
                      listing.blockers.join(QStringLiteral("; ")).toStdString());
        return std::unexpected(sak::error_code::scan_failed);
    }

    for (const auto& entry : listing.entries) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        auto result = collectVirtualEntry(entry, state, depth);
        if (!result) {
            return result;
        }
    }
    return {};
}

auto DuplicateFinderWorker::collectVirtualEntry(const sak::FileManagementEntry& entry,
                                                VirtualWalkState& state,
                                                int depth) -> std::expected<void, sak::error_code> {
    const qint64 size = static_cast<qint64>(entry.size_bytes);
    if (entry.regular_file && size >= m_config.minimum_file_size) {
        state.files.append({entry.path, size});
    }
    if (!m_config.recursive_scan || !entry.directory) {
        return {};
    }
    return collectVirtualFiles(entry.path, state, depth + 1);
}

auto DuplicateFinderWorker::hashVirtualFiles(const QVector<VirtualFile>& files)
    -> std::expected<QVector<QPair<VirtualFile, QString>>, sak::error_code> {
    QVector<QPair<VirtualFile, QString>> hashed;
    hashed.reserve(files.size());
    for (int i = 0; i < files.size(); ++i) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        const auto& file = files.at(i);
        Q_EMIT scanProgress(i + 1, files.size(), file.path);
        if (file.size < 0 || static_cast<uint64_t>(file.size) > kDuplicateVirtualReadMaxBytes) {
            sak::logWarning("Skipping oversized file-system target entry: {}",
                            file.path.toStdString());
            continue;
        }
        const auto read = sak::FileManagementFileSystemBridge::readFile(
            m_config.file_system_target, file.path, kDuplicateVirtualReadMaxBytes);
        if (!read.ok) {
            sak::logWarning("Failed to read file-system target entry: {}", file.path.toStdString());
            continue;
        }
        const QByteArray hash =
            QCryptographicHash::hash(read.data, QCryptographicHash::Md5).toHex();
        hashed.append({file, QString::fromLatin1(hash)});
    }
    return hashed;
}

std::vector<DuplicateFinderWorker::DuplicateGroup>
DuplicateFinderWorker::buildVirtualDuplicateGroups(
    const QVector<QPair<VirtualFile, QString>>& hashed_files,
    int& total_duplicates,
    qint64& total_wasted) const {
    QHash<QString, QVector<VirtualFile>> byHash;
    for (const auto& item : hashed_files) {
        byHash[item.second].append(item.first);
    }

    std::vector<DuplicateGroup> groups;
    for (auto it = byHash.cbegin(); it != byHash.cend(); ++it) {
        const auto& paths = it.value();
        if (paths.size() <= 1) {
            continue;
        }
        DuplicateGroup group;
        group.hash = it.key();
        group.file_size = paths.first().size;
        group.wasted_space = group.file_size * (paths.size() - 1);
        for (const auto& file : paths) {
            group.file_paths.append(file.path);
        }
        groups.push_back(group);
        total_duplicates += paths.size() - 1;
        total_wasted += group.wasted_space;
    }
    return groups;
}

auto DuplicateFinderWorker::calculateFileHash(const std::filesystem::path& file_path)
    -> std::expected<std::string, sak::error_code> {
    // Pass the shared cancel token so a mid-file stop returns operation_cancelled (the sequential
    // path; the caller discards results on cancel).
    auto result = m_hasher.calculateHash(file_path.string(), nullptr, m_hash_stop.get_token());
    if (!result) {
        return std::unexpected(result.error());
    }
    return result.value();
}

auto DuplicateFinderWorker::groupByHash(
    const std::vector<std::pair<std::filesystem::path, std::string>>& files) const
    -> std::unordered_map<std::string, std::vector<std::filesystem::path>> {
    std::unordered_map<std::string, std::vector<std::filesystem::path>> groups;

    for (const auto& [path, hash] : files) {
        groups[hash].push_back(path);
    }

    return groups;
}

auto DuplicateFinderWorker::generateSummary(const std::vector<DuplicateGroup>& groups) -> QString {
    if (groups.empty()) {
        return "No duplicate files found.";
    }

    QString summary;
    summary += QString("Found %1 groups of duplicate files:\n\n").arg(groups.size());

    int total_duplicates = 0;
    qint64 total_wasted = 0;

    for (const auto& group : groups) {
        total_duplicates += (group.file_paths.size() - 1);
        total_wasted += group.wasted_space;
    }

    summary += QString("Total duplicate files: %1\n").arg(total_duplicates);
    summary += QString("Total wasted space: %1 MB\n\n")
                   .arg(total_wasted / sak::kBytesPerMBf, 0, 'f', kDuplicateSummarySizePrecision);

    summary += "Top duplicate groups:\n";
    int count = 0;
    for (const auto& group : groups) {
        if (++count > kDuplicateSummaryGroupLimit) {
            break;
        }

        summary += QString("\nGroup %1 (%2 files, %3 KB wasted):\n")
                       .arg(count)
                       .arg(group.file_paths.size())
                       .arg(group.wasted_space / sak::kBytesPerKBf, 0, 'f', 1);

        for (const auto& path : group.file_paths) {
            const QFileInfo info(path);
            summary += QString("  - %1\n").arg(info.fileName());
        }
    }

    return summary;
}

namespace {

// Self-contained state for the parallel hash tasks. Held by shared_ptr and captured BY VALUE (a
// shared_ptr copy) into each pooled task, so a task that outlives the worker -- e.g. still blocked
// in a wedged QFile::read() when the worker is force-terminated on teardown -- touches ONLY this
// shared state (kept alive by the shared_ptr) and NEVER the destroyed DuplicateFinderWorker or its
// stack. That is what keeps the terminate() path free of a use-after-free. The stop_token is a copy
// of the worker's m_hash_stop token; it shares the token's refcounted stop-state, so it also
// outlives the worker safely.
struct ParallelHashJob {
    std::vector<std::filesystem::path> files;
    std::vector<std::pair<std::filesystem::path, std::string>> results;
    std::atomic<int> processed_count{0};
    std::atomic<bool> error_occurred{false};
    QMutex results_mutex;
    std::stop_token stop;
};

// Build the per-index hashing task. It captures ONLY the shared job (no worker pointer, no stack
// references), so it is safe to run after the worker is gone. Progress is reported by the caller's
// monitor thread (which safely owns the worker), not from here.
std::function<void(int)> makeParallelHashTask(const std::shared_ptr<ParallelHashJob>& job) {
    return [job](int index) {
        // Only cancellation skips work. A prior file's READ failure must NOT abort the rest (that
        // would nondeterministically drop real duplicate groups depending on scheduling):
        // unreadable files are simply left un-hashed and filtered out, matching the sequential
        // path.
        if (job->stop.stop_requested()) {
            return;
        }
        const auto& file = job->files[static_cast<size_t>(index)];
        const sak::file_hasher hasher(sak::hash_algorithm::md5);
        // Honor the cancel token so a stop breaks this file at the next 1 MB chunk (not at EOF); on
        // cancel calculateHash returns operation_cancelled and the whole result set is discarded by
        // calculateHashesParallel's post-blockingMap checkStop().
        auto hash_result = hasher.calculateHash(file, nullptr, job->stop);
        if (hash_result) {
            const QMutexLocker locker(&job->results_mutex);
            job->results[static_cast<size_t>(index)] = std::make_pair(file, hash_result.value());
        } else {
            sak::logWarning("Failed to hash file: {}", file.string());
            job->error_occurred.store(true);
        }
        job->processed_count.fetch_add(1);
    };
}

int resolveHashThreadCount(int configured) {
    constexpr int kDefaultHashThreadCount = 4;
    if (configured > 0) {
        return configured;
    }
    const int detected = static_cast<int>(std::thread::hardware_concurrency());
    return detected > 0 ? detected : kDefaultHashThreadCount;
}

}  // namespace

auto DuplicateFinderWorker::calculateHashesParallel(const std::vector<std::filesystem::path>& files)
    -> std::expected<std::vector<std::pair<std::filesystem::path, std::string>>, sak::error_code> {
    const int thread_count = resolveHashThreadCount(m_config.hash_thread_count);
    sak::logInfo("Using {} threads for parallel hashing", thread_count);

    auto job = std::make_shared<ParallelHashJob>();
    job->files = files;  // own a copy so an orphaned task never derefs a dangling caller vector
    job->results.resize(files.size());
    job->stop = m_hash_stop.get_token();

    QVector<int> indices;
    indices.reserve(static_cast<int>(files.size()));
    for (size_t i = 0; i < files.size(); ++i) {
        indices.push_back(static_cast<int>(i));
    }

    // Monitor thread: forwards the worker's cooperative stop into m_hash_stop (mid-file cancel) and
    // reports progress. It safely holds the worker pointer -- it is stop+joined at this function's
    // scope exit (on the worker thread) well before ~DuplicateFinderWorker, and it RETURNS promptly
    // on stop, so it can never emit on a destroyed worker even on the terminate() path.
    const int total = static_cast<int>(files.size());
    const std::jthread monitor([this, job, total](const std::stop_token& monitor_stop) {
        while (!monitor_stop.stop_requested()) {
            if (stopRequested()) {
                m_hash_stop.request_stop();
                return;  // die within one poll of teardown, long before any terminate() at +15s
            }
            Q_EMIT scanProgress(job->processed_count.load(), total, QString());
            std::this_thread::sleep_for(std::chrono::milliseconds(kHashCancelPollMs));
        }
    });

    // Run on a LOCAL pool (honors hash_thread_count) with self-contained tasks, so even a task left
    // running on a wedged read after a force-terminate cannot use-after-free.
    QThreadPool pool;
    pool.setMaxThreadCount(thread_count);
    QtConcurrent::blockingMap(&pool, indices, makeParallelHashTask(job));

    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    if (job->error_occurred.load()) {
        sak::logError("Errors occurred during parallel hashing");
    }

    auto valid_results = filterValidResults(job->results);
    sak::logInfo("Parallel hashing complete: {}/{} files successful",
                 valid_results.size(),
                 files.size());
    return valid_results;
}

std::vector<std::pair<std::filesystem::path, std::string>>
DuplicateFinderWorker::filterValidResults(
    const std::vector<std::pair<std::filesystem::path, std::string>>& results) {
    std::vector<std::pair<std::filesystem::path, std::string>> valid;
    valid.reserve(results.size());

    std::copy_if(results.begin(), results.end(), std::back_inserter(valid), [](const auto& result) {
        return !result.first.empty() && !result.second.empty();
    });
    return valid;
}
