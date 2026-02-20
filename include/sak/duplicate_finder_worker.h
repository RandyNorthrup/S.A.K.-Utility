
#include "sak/worker_base.h"
#include "sak/file_hash.h"
#include <QString>
#include <QMap>
#include <QVector>
#include <filesystem>
#include <unordered_map>
#include <vector>

/**
 * @brief Worker thread for duplicate file detection
 * 
 * Scans directories for duplicate files using MD5 hash comparison.
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
        QString hash;                           ///< MD5 hash of files
        QVector<QString> file_paths;            ///< Paths to duplicate files
        qint64 file_size{0};                    ///< Size of each file
        qint64 wasted_space{0};                 ///< Total space wasted by duplicates
    };

    /**
     * @brief Configuration for duplicate finder operation
     */
    struct Config {
        QVector<QString> scanDirectories;      ///< Directories to scan
        qint64 minimum_file_size{0};            ///< Minimum file size to consider (bytes)
        bool recursive_scan{true};              ///< Scan subdirectories
        bool parallel_hashing{true};            ///< Use parallel hash calculation
        int hash_thread_count{0};               ///< Thread count (0 = auto-detect)
    };

    /**
     * @brief Construct duplicate finder worker
     * @param config Scan configuration
     * @param parent Parent QObject
     */
    explicit DuplicateFinderWorker(const Config& config, QObject* parent = nullptr);

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
        -> std::expected<std::vector<std::pair<std::filesystem::path, std::string>>, sak::error_code>;

    /**
     * @brief Group files by hash
     * @param files Files with their hashes
     * @return Map of hash to file paths
     */
    auto groupByHash(const std::vector<std::pair<std::filesystem::path, std::string>>& files)
        -> std::unordered_map<std::string, std::vector<std::filesystem::path>>;

    /**
     * @brief Generate results summary text
     * @param groups Duplicate groups
     * @return Summary string
     */
    auto generateSummary(const std::vector<DuplicateGroup>& groups) -> QString;

    Config m_config;
    sak::file_hasher m_hasher;
};
