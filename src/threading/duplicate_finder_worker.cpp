#include "sak/duplicate_finder_worker.h"
#include "sak/logger.h"
#include <QFileInfo>
#include <QThreadPool>
#include <QtConcurrent>
#include <thread>

DuplicateFinderWorker::DuplicateFinderWorker(const Config& config, QObject* parent)
    : WorkerBase(parent)
    , m_config(config)
    , m_hasher(sak::hash_algorithm::md5)
{
}

auto DuplicateFinderWorker::execute() -> std::expected<void, sak::error_code>
{
    sak::log_info("Starting duplicate file scan");

    // Scan all directories
    auto files_result = scan_directories();
    if (!files_result) {
        return std::unexpected(files_result.error());
    }

    const auto& files = files_result.value();
    sak::log_info("Found {} files to analyze", files.size());

    if (files.empty()) {
        Q_EMIT results_ready("No files found to scan.", 0, 0);
        return {};
    }

    // Calculate hashes for all files (parallel or sequential)
    std::expected<std::vector<std::pair<std::filesystem::path, std::string>>, sak::error_code> hashed_result;
    
    if (m_config.parallel_hashing) {
        sak::log_info("Using parallel hash calculation");
        hashed_result = calculate_hashes_parallel(files);
    } else {
        sak::log_info("Using sequential hash calculation");
        std::vector<std::pair<std::filesystem::path, std::string>> hashed_files;
        hashed_files.reserve(files.size());

        for (size_t i = 0; i < files.size(); ++i) {
            if (check_stop()) {
                return std::unexpected(sak::error_code::operation_cancelled);
            }

            const auto& file = files[i];
            Q_EMIT scan_progress(static_cast<int>(i + 1), static_cast<int>(files.size()),
                               QString::fromStdString(file.string()));

            auto hash_result = calculate_file_hash(file);
            if (hash_result) {
                hashed_files.emplace_back(file, hash_result.value());
            } else {
                sak::log_warning("Failed to hash file: {}", file.string());
            }
        }
        hashed_result = std::move(hashed_files);
    }

    if (!hashed_result) {
        return std::unexpected(hashed_result.error());
    }

    const auto& hashed_files = hashed_result.value();
    sak::log_info("Hashed {} files successfully", hashed_files.size());

    // Group files by hash
    auto hash_groups = group_by_hash(hashed_files);

    // Build duplicate groups
    std::vector<DuplicateGroup> duplicate_groups;
    int total_duplicates = 0;
    qint64 total_wasted = 0;

    for (const auto& [hash, paths] : hash_groups) {
        if (paths.size() > 1) {
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
                sak::log_warning("Failed to get file size: {}", e.what());
            }
        }
    }

    sak::log_info("Found {} duplicate groups, {} duplicate files, {} bytes wasted",
                  duplicate_groups.size(), total_duplicates, total_wasted);

    // Generate and emit results
    QString summary = generate_summary(duplicate_groups);
    Q_EMIT results_ready(summary, total_duplicates, total_wasted);

    return {};
}

auto DuplicateFinderWorker::scan_directories() 
    -> std::expected<std::vector<std::filesystem::path>, sak::error_code>
{
    std::vector<std::filesystem::path> files;

    for (const auto& dir_str : m_config.scan_directories) {
        if (check_stop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        std::filesystem::path dir_path(dir_str.toStdString());

        if (!std::filesystem::exists(dir_path)) {
            sak::log_warning("Directory does not exist: {}", dir_path.string());
            continue;
        }

        if (!std::filesystem::is_directory(dir_path)) {
            sak::log_warning("Path is not a directory: {}", dir_path.string());
            continue;
        }

        try {
            if (m_config.recursive_scan) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
                    if (check_stop()) {
                        return std::unexpected(sak::error_code::operation_cancelled);
                    }

                    if (entry.is_regular_file()) {
                        auto size = static_cast<qint64>(entry.file_size());
                        if (size >= m_config.minimum_file_size) {
                            files.push_back(entry.path());
                        }
                    }
                }
            } else {
                for (const auto& entry : std::filesystem::directory_iterator(dir_path)) {
                    if (check_stop()) {
                        return std::unexpected(sak::error_code::operation_cancelled);
                    }

                    if (entry.is_regular_file()) {
                        auto size = static_cast<qint64>(entry.file_size());
                        if (size >= m_config.minimum_file_size) {
                            files.push_back(entry.path());
                        }
                    }
                }
            }
        } catch (const std::filesystem::filesystem_error& e) {
            sak::log_error("Error scanning directory {}: {}", dir_path.string(), e.what());
            return std::unexpected(sak::error_code::scan_failed);
        }
    }

    return files;
}

auto DuplicateFinderWorker::calculate_file_hash(const std::filesystem::path& file_path)
    -> std::expected<std::string, sak::error_code>
{
    auto result = m_hasher.calculate_hash(file_path.string());
    if (!result) {
        return std::unexpected(result.error());
    }
    return result.value();
}

auto DuplicateFinderWorker::group_by_hash(
    const std::vector<std::pair<std::filesystem::path, std::string>>& files)
    -> std::unordered_map<std::string, std::vector<std::filesystem::path>>
{
    std::unordered_map<std::string, std::vector<std::filesystem::path>> groups;

    for (const auto& [path, hash] : files) {
        groups[hash].push_back(path);
    }

    return groups;
}

auto DuplicateFinderWorker::generate_summary(const std::vector<DuplicateGroup>& groups) -> QString
{
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
    summary += QString("Total wasted space: %1 MB\n\n").arg(total_wasted / (1024.0 * 1024.0), 0, 'f', 2);

    summary += "Top duplicate groups:\n";
    int count = 0;
    for (const auto& group : groups) {
        if (++count > 10) break; // Show only top 10
        
        summary += QString("\nGroup %1 (%2 files, %3 KB wasted):\n")
            .arg(count)
            .arg(group.file_paths.size())
            .arg(group.wasted_space / 1024.0, 0, 'f', 1);
        
        for (const auto& path : group.file_paths) {
            QFileInfo info(path);
            summary += QString("  - %1\n").arg(info.fileName());
        }
    }

    return summary;
}

auto DuplicateFinderWorker::calculate_hashes_parallel(const std::vector<std::filesystem::path>& files)
    -> std::expected<std::vector<std::pair<std::filesystem::path, std::string>>, sak::error_code>
{
    // Determine thread count
    int thread_count = m_config.hash_thread_count;
    if (thread_count <= 0) {
        thread_count = static_cast<int>(std::thread::hardware_concurrency());
        if (thread_count <= 0) {
            thread_count = 4; // Default fallback
        }
    }

    sak::log_info("Using {} threads for parallel hashing", thread_count);

    // Set up thread pool
    QThreadPool pool;
    pool.setMaxThreadCount(thread_count);

    // Atomic counters for progress
    std::atomic<int> processed_count{0};
    std::atomic<bool> error_occurred{false};

    // Thread-safe result storage
    std::vector<std::pair<std::filesystem::path, std::string>> results;
    results.resize(files.size());
    QMutex results_mutex;

    // Lambda for hashing a single file
    auto hash_file = [this, &files, &results, &processed_count, &error_occurred, &results_mutex](int index) {
        if (check_stop() || error_occurred.load()) {
            return;
        }

        const auto& file = files[static_cast<size_t>(index)];
        
        // Create a file hasher for this thread
        sak::file_hasher hasher(sak::hash_algorithm::md5);
        auto hash_result = hasher.calculate_hash(file);

        if (hash_result) {
            QMutexLocker locker(&results_mutex);
            results[static_cast<size_t>(index)] = std::make_pair(file, hash_result.value());
        } else {
            sak::log_warning("Failed to hash file: {}", file.string());
            error_occurred.store(true);
        }

        // Update progress
        int current = ++processed_count;
        if (current % 10 == 0 || current == static_cast<int>(files.size())) {
            Q_EMIT scan_progress(current, static_cast<int>(files.size()),
                               QString::fromStdString(file.string()));
        }
    };

    // Map files to thread pool
    QVector<int> indices;
    indices.reserve(static_cast<int>(files.size()));
    for (size_t i = 0; i < files.size(); ++i) {
        indices.push_back(static_cast<int>(i));
    }

    // Execute parallel map
    QtConcurrent::blockingMap(indices, hash_file);

    if (check_stop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    if (error_occurred.load()) {
        sak::log_error("Errors occurred during parallel hashing");
    }

    // Filter out empty results (failed hashes)
    std::vector<std::pair<std::filesystem::path, std::string>> valid_results;
    valid_results.reserve(results.size());
    
    for (const auto& result : results) {
        if (!result.first.empty() && !result.second.empty()) {
            valid_results.push_back(result);
        }
    }

    sak::log_info("Parallel hashing complete: {}/{} files successful", 
                  valid_results.size(), files.size());

    return valid_results;
}
