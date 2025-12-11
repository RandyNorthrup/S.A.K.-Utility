/// @file file_hash.cpp
/// @brief Implementation of file hashing utilities

#include "sak/file_hash.h"
#include "sak/logger.h"
#include <QCryptographicHash>
#include <QFile>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace sak {

file_hasher::file_hasher(hash_algorithm algorithm, std::size_t chunk_size) noexcept
    : m_algorithm(algorithm)
    , m_chunk_size(chunk_size) {
}

auto file_hasher::calculate_hash(
    const std::filesystem::path& file_path,
    hash_progress_callback progress,
    std::stop_token stop_token) const -> std::expected<std::string, error_code> {
    
    // Validate file exists
    if (!std::filesystem::exists(file_path)) {
        log_error("File not found: {}", file_path.string());
        return std::unexpected(error_code::file_not_found);
    }
    
    // Validate it's a regular file
    if (!std::filesystem::is_regular_file(file_path)) {
        log_error("Path is not a regular file: {}", file_path.string());
        return std::unexpected(error_code::invalid_path);
    }
    
    // Delegate to algorithm-specific implementation
    switch (m_algorithm) {
        case hash_algorithm::md5:
            return calculate_md5(file_path, progress, stop_token);
        case hash_algorithm::sha256:
            return calculate_sha256(file_path, progress, stop_token);
        default:
            return std::unexpected(error_code::internal_error);
    }
}

auto file_hasher::calculate_hash(
    std::span<const std::byte> data) const -> std::expected<std::string, error_code> {
    
    switch (m_algorithm) {
        case hash_algorithm::md5:
            return calculate_md5(data);
        case hash_algorithm::sha256:
            return calculate_sha256(data);
        default:
            return std::unexpected(error_code::internal_error);
    }
}

auto file_hasher::verify_hash(
    const std::filesystem::path& file_path,
    std::string_view expected_hash,
    std::stop_token stop_token) const -> std::expected<bool, error_code> {
    
    auto calculated = calculate_hash(file_path, nullptr, stop_token);
    if (!calculated) {
        return std::unexpected(calculated.error());
    }
    
    // Case-insensitive comparison
    auto lower_expected = std::string(expected_hash);
    auto lower_calculated = *calculated;
    
    std::transform(lower_expected.begin(), lower_expected.end(),
                   lower_expected.begin(), ::tolower);
    std::transform(lower_calculated.begin(), lower_calculated.end(),
                   lower_calculated.begin(), ::tolower);
    
    return lower_expected == lower_calculated;
}

auto file_hasher::calculate_md5(
    const std::filesystem::path& file_path,
    hash_progress_callback& progress,
    std::stop_token stop_token) const -> std::expected<std::string, error_code> {
    
    try {
        // Open file with Qt
        QFile file(QString::fromStdString(file_path.string()));
        if (!file.open(QIODevice::ReadOnly)) {
            log_error("Failed to open file: {}", file_path.string());
            return std::unexpected(error_code::read_error);
        }
        
        // Get file size
        const auto file_size = static_cast<std::size_t>(file.size());
        
        // Initialize MD5 hash
        QCryptographicHash hash(QCryptographicHash::Md5);
        
        // Read and hash in chunks
        std::size_t bytes_processed = 0;
        
        while (!file.atEnd() && !stop_token.stop_requested()) {
            QByteArray buffer = file.read(m_chunk_size);
            
            if (!buffer.isEmpty()) {
                hash.addData(buffer);
                bytes_processed += buffer.size();
                
                if (progress) {
                    progress(bytes_processed, file_size);
                }
            }
        }
        
        if (stop_token.stop_requested()) {
            return std::unexpected(error_code::operation_cancelled);
        }
        
        // Get final hash as hex string
        QByteArray result = hash.result();
        return result.toHex().toStdString();
        
    } catch (const std::exception& e) {
        log_error("Exception calculating MD5: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    } catch (...) {
        log_error("Unknown exception calculating MD5");
        return std::unexpected(error_code::unknown_error);
    }
}

auto file_hasher::calculate_sha256(
    const std::filesystem::path& file_path,
    hash_progress_callback& progress,
    std::stop_token stop_token) const -> std::expected<std::string, error_code> {
    
    try {
        // Open file with Qt
        QFile file(QString::fromStdString(file_path.string()));
        if (!file.open(QIODevice::ReadOnly)) {
            log_error("Failed to open file: {}", file_path.string());
            return std::unexpected(error_code::read_error);
        }
        
        // Get file size
        const auto file_size = static_cast<std::size_t>(file.size());
        
        // Initialize SHA-256 hash
        QCryptographicHash hash(QCryptographicHash::Sha256);
        
        // Read and hash in chunks
        std::size_t bytes_processed = 0;
        
        while (!file.atEnd() && !stop_token.stop_requested()) {
            QByteArray buffer = file.read(m_chunk_size);
            
            if (!buffer.isEmpty()) {
                hash.addData(buffer);
                bytes_processed += buffer.size();
                
                if (progress) {
                    progress(bytes_processed, file_size);
                }
            }
        }
        
        if (stop_token.stop_requested()) {
            return std::unexpected(error_code::operation_cancelled);
        }
        
        // Get final hash as hex string
        QByteArray result = hash.result();
        return result.toHex().toStdString();
        
    } catch (const std::exception& e) {
        log_error("Exception calculating SHA-256: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    } catch (...) {
        log_error("Unknown exception calculating SHA-256");
        return std::unexpected(error_code::unknown_error);
    }
}

auto file_hasher::calculate_md5(
    std::span<const std::byte> data) const -> std::expected<std::string, error_code> {
    
    try {
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(reinterpret_cast<const char*>(data.data()), data.size());
        QByteArray result = hash.result();
        return result.toHex().toStdString();
        
    } catch (...) {
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculate_sha256(
    std::span<const std::byte> data) const -> std::expected<std::string, error_code> {
    
    try {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(reinterpret_cast<const char*>(data.data()), data.size());
        QByteArray result = hash.result();
        return result.toHex().toStdString();
        
    } catch (...) {
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

} // namespace sak
