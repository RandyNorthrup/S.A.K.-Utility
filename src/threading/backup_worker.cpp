#include "sak/backup_worker.h"
#include "sak/logger.h"
#include "sak/path_utils.h"
#include <fstream>
#include <QDir>

BackupWorker::BackupWorker(const Config& config, QObject* parent)
    : WorkerBase(parent)
    , m_config(config)
{
}

auto BackupWorker::execute() -> std::expected<void, sak::error_code>
{
    sak::logInfo("Backup worker started");
    sak::logInfo("Source: {}", m_config.source_path.toStdString());
    sak::logInfo("Destination: {}", m_config.destination_path.toStdString());
    
    m_start_time = std::chrono::steady_clock::now();
    m_last_speed_update = m_start_time;
    
    // Scan source directory
    reportProgress(0, 100, "Scanning source directory...");
    
    auto scan_result = scanSource();
    if (!scan_result) {
        return std::unexpected(scan_result.error());
    }
    
    std::tie(m_total_files, m_total_bytes) = *scan_result;
    
    sak::logInfo("Found {} files ({} bytes)", m_total_files, m_total_bytes);
    
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }
    
    // Copy files
    reportProgress(0, 100, "Copying files...");
    
    auto copy_result = copyFiles();
    if (!copy_result) {
        return std::unexpected(copy_result.error());
    }
    
    sak::logInfo("Backup completed successfully");
    return {};
}

auto BackupWorker::scanSource() -> std::expected<std::pair<int, qint64>, sak::error_code>
{
    m_files_to_copy.clear();
    int file_count = 0;
    qint64 total_size = 0;
    
    try {
        std::filesystem::path source_path(m_config.source_path.toStdString());
        
        if (!std::filesystem::exists(source_path)) {
            sak::logError("Source path does not exist: {}", source_path.string());
            return std::unexpected(sak::error_code::file_not_found);
        }
        
        if (!std::filesystem::is_directory(source_path)) {
            sak::logError("Source path is not a directory: {}", source_path.string());
            return std::unexpected(sak::error_code::invalid_path);
        }
        
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source_path)) {
            if (checkStop()) {
                return std::unexpected(sak::error_code::operation_cancelled);
            }
            
            if (entry.is_regular_file()) {
                m_files_to_copy.push_back(entry.path());
                total_size += entry.file_size();
                file_count++;
                
                // Report scan progress every 100 files
                if (file_count % 100 == 0) {
                    reportProgress(file_count, file_count + 1,
                                  QString("Scanning... found %1 files").arg(file_count));
                }
            }
        }
        
        return std::make_pair(file_count, total_size);
        
    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Filesystem error during scan: {}", e.what());
        return std::unexpected(sak::error_code::scan_failed);
    } catch (const std::exception& e) {
        sak::logError("Error during scan: {}", e.what());
        return std::unexpected(sak::error_code::unknown_error);
    }
}

auto BackupWorker::copyFiles() -> std::expected<void, sak::error_code>
{
    std::filesystem::path source_root(m_config.source_path.toStdString());
    std::filesystem::path dest_root(m_config.destination_path.toStdString());
    
    try {
        // Create destination directory if it doesn't exist
        if (!std::filesystem::exists(dest_root)) {
            std::filesystem::create_directories(dest_root);
        }
        
        m_files_processed = 0;
        m_bytes_processed = 0;
        
        for (const auto& source_file : m_files_to_copy) {
            if (checkStop()) {
                return std::unexpected(sak::error_code::operation_cancelled);
            }
            
            // Calculate relative path
            auto relative_path = std::filesystem::relative(source_file, source_root);
            auto dest_file = dest_root / relative_path;
            
            // Create parent directory
            auto parent_dir = dest_file.parent_path();
            if (!std::filesystem::exists(parent_dir)) {
                std::filesystem::create_directories(parent_dir);
            }
            
            // Copy file
            auto copy_result = copyFile(source_file, dest_file);
            if (!copy_result) {
                sak::logError("Failed to copy {}: {}", 
                             source_file.string(),
                             sak::to_string(copy_result.error()));
                return std::unexpected(copy_result.error());
            }
            
            // Verify if requested
            if (m_config.verify_md5) {
                auto verify_result = verifyFile(source_file, dest_file);
                if (!verify_result) {
                    return std::unexpected(verify_result.error());
                }
                if (!*verify_result) {
                    sak::logError("MD5 verification failed for {}", source_file.string());
                    return std::unexpected(sak::error_code::hash_mismatch);
                }
            }
            
            m_files_processed++;
            m_bytes_processed += std::filesystem::file_size(source_file);
            
            // Emit progress
            Q_EMIT fileProgress(m_files_processed, m_total_files,
                               m_bytes_processed, m_total_bytes);
            
            updateSpeed();
        }
        
        return {};
        
    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Filesystem error during copy: {}", e.what());
        return std::unexpected(sak::error_code::backup_failed);
    } catch (const std::exception& e) {
        sak::logError("Error during copy: {}", e.what());
        return std::unexpected(sak::error_code::unknown_error);
    }
}

auto BackupWorker::copyFile(const std::filesystem::path& source,
                             const std::filesystem::path& destination)
    -> std::expected<void, sak::error_code>
{
    try {
        // Use std::filesystem::copy with overwrite
        std::filesystem::copy_options options = 
            std::filesystem::copy_options::overwrite_existing;
        
        if (m_config.preserve_timestamps) {
            options |= std::filesystem::copy_options::copy_symlinks;
        }
        
        std::filesystem::copy(source, destination, options);
        
        // Preserve timestamps manually if needed
        if (m_config.preserve_timestamps) {
            auto source_time = std::filesystem::last_write_time(source);
            std::filesystem::last_write_time(destination, source_time);
        }
        
        return {};
        
    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Failed to copy file: {}", e.what());
        return std::unexpected(sak::error_code::write_error);
    }
}

auto BackupWorker::verifyFile(const std::filesystem::path& source,
                               const std::filesystem::path& destination)
    -> std::expected<bool, sak::error_code>
{
    sak::file_hasher hasher(sak::hash_algorithm::md5);
    
    auto source_hash = hasher.calculateHash(source);
    if (!source_hash) {
        return std::unexpected(source_hash.error());
    }
    
    auto dest_hash = hasher.calculateHash(destination);
    if (!dest_hash) {
        return std::unexpected(dest_hash.error());
    }
    
    return *source_hash == *dest_hash;
}

void BackupWorker::updateSpeed()
{
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_last_speed_update).count();
    
    // Update speed every second
    if (elapsed >= 1000) {
        auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - m_start_time).count();
        
        if (total_elapsed > 0) {
            double mb_per_second = 
                (m_bytes_processed / 1024.0 / 1024.0) / total_elapsed;
            Q_EMIT speedUpdate(mb_per_second);
        }
        
        m_last_speed_update = now;
    }
}
