
#include "sak/worker_base.h"
#include "sak/file_hash.h"
#include <QString>
#include <QStringList>
#include <filesystem>
#include <chrono>

/**
 * @brief Worker thread for backup operations
 * 
 * Performs multi-threaded file copying with optional MD5 verification,
 * progress tracking, and speed calculation.
 * 
 * Thread-Safety: All signals are emitted from worker thread and should
 * be connected with Qt::QueuedConnection.
 */
class BackupWorker : public WorkerBase {
    Q_OBJECT

public:
    /**
     * @brief Configuration for backup operation
     */
    struct Config {
        QString source_path;           ///< Source directory
        QString destination_path;      ///< Destination directory
        QStringList filter_patterns;   ///< File patterns to include (e.g., "*.txt")
        bool verify_md5{false};        ///< Enable MD5 verification
        int thread_count{4};           ///< Number of copy threads
        bool preserve_timestamps{true}; ///< Preserve file timestamps
    };

    /**
     * @brief Construct backup worker
     * @param config Backup configuration
     * @param parent Parent QObject
     */
    explicit BackupWorker(const Config& config, QObject* parent = nullptr);
    
    /**
     * @brief Destructor
     */
    ~BackupWorker() override = default;

Q_SIGNALS:
    /**
     * @brief Emitted when file copying progresses
     * @param files_processed Number of files processed
     * @param total_files Total number of files
     * @param bytes_processed Bytes processed
     * @param total_bytes Total bytes to process
     */
    void file_progress(int files_processed, int total_files,
                      qint64 bytes_processed, qint64 total_bytes);

    /**
     * @brief Emitted periodically with current speed
     * @param mb_per_second Current copy speed in MB/s
     */
    void speed_update(double mb_per_second);

protected:
    /**
     * @brief Execute backup operation
     * @return Expected containing success or error code
     */
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /**
     * @brief Scan source directory and build file list
     * @return Expected containing total files and bytes
     */
    auto scan_source() -> std::expected<std::pair<int, qint64>, sak::error_code>;

    /**
     * @brief Copy files from source to destination
     * @return Expected containing success or error code
     */
    auto copy_files() -> std::expected<void, sak::error_code>;

    /**
     * @brief Copy single file with progress
     * @param source Source file path
     * @param destination Destination file path
     * @return Expected containing success or error code
     */
    auto copy_file(const std::filesystem::path& source,
                   const std::filesystem::path& destination)
        -> std::expected<void, sak::error_code>;

    /**
     * @brief Verify file integrity with MD5
     * @param source Source file path
     * @param destination Destination file path
     * @return Expected containing true if match, false if mismatch, or error
     */
    auto verify_file(const std::filesystem::path& source,
                     const std::filesystem::path& destination)
        -> std::expected<bool, sak::error_code>;

    /**
     * @brief Update speed calculation
     */
    void update_speed();

    Config m_config;
    std::vector<std::filesystem::path> m_files_to_copy;
    int m_total_files{0};
    qint64 m_total_bytes{0};
    int m_files_processed{0};
    qint64 m_bytes_processed{0};
    std::chrono::steady_clock::time_point m_start_time;
    std::chrono::steady_clock::time_point m_last_speed_update;
};
