// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_hash.cpp
/// @brief Implementation of file hashing utilities

#include "sak/file_hash.h"
#include "sak/logger.h"
#include <QtGlobal>
#include <QCryptographicHash>
#include <QFile>
#include <QByteArrayView>
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
    Q_ASSERT_X(chunk_size > 0, "file_hasher", "chunk_size must be positive");
}

auto file_hasher::calculateHash(
    const std::filesystem::path& file_path,
    hash_progress_callback progress,
    std::stop_token stop_token) const -> std::expected<std::string, error_code> {
    Q_ASSERT_X(!file_path.empty(), "calculateHash", "file_path must not be empty");
    
    // Validate file exists
    if (!std::filesystem::exists(file_path)) {
        logError("File not found: {}", file_path.string());
        return std::unexpected(error_code::file_not_found);
    }
    
    // Validate it's a regular file
    if (!std::filesystem::is_regular_file(file_path)) {
        logError("Path is not a regular file: {}", file_path.string());
        return std::unexpected(error_code::invalid_path);
    }
    
    // Delegate to algorithm-specific implementation
    switch (m_algorithm) {
        case hash_algorithm::md5:
            return calculateMd5(file_path, progress, stop_token);
        case hash_algorithm::sha256:
            return calculateSha256(file_path, progress, stop_token);
        default:
            return std::unexpected(error_code::internal_error);
    }
}

auto file_hasher::calculateHash(
    std::span<const std::byte> data) const -> std::expected<std::string, error_code> {
    
    switch (m_algorithm) {
        case hash_algorithm::md5:
            return calculateMd5(data);
        case hash_algorithm::sha256:
            return calculateSha256(data);
        default:
            return std::unexpected(error_code::internal_error);
    }
}

auto file_hasher::verifyHash(
    const std::filesystem::path& file_path,
    std::string_view expected_hash,
    std::stop_token stop_token) const -> std::expected<bool, error_code> {
    Q_ASSERT_X(!file_path.empty(), "verifyHash", "file_path must not be empty");
    Q_ASSERT_X(!expected_hash.empty(), "verifyHash", "expected_hash must not be empty");
    
    auto calculated = calculateHash(file_path, nullptr, stop_token);
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

auto file_hasher::calculateMd5(
    const std::filesystem::path& file_path,
    hash_progress_callback& progress,
    std::stop_token stop_token) const -> std::expected<std::string, error_code> {
    Q_ASSERT_X(!file_path.empty(), "calculateMd5", "file_path must not be empty");
    Q_ASSERT_X(m_chunk_size > 0, "calculateMd5", "chunk_size must be positive");
    
    try {
        // Open file with Qt
        QFile file(QString::fromStdString(file_path.string()));
        if (!file.open(QIODevice::ReadOnly)) {
            logError("Failed to open file: {}", file_path.string());
            return std::unexpected(error_code::read_error);
        }
        
        // Initialize MD5 hash
        QCryptographicHash hash(QCryptographicHash::Md5);
        hashFileInChunks(file, hash, progress, stop_token);
        
        if (stop_token.stop_requested()) {
            return std::unexpected(error_code::operation_cancelled);
        }
        
        // Get final hash as hex string
        QByteArray result = hash.result();
        Q_ASSERT_X(result.size() == 16, "calculateMd5", "MD5 digest must be 16 bytes");
        auto hex = result.toHex().toStdString();
        Q_ASSERT_X(hex.size() == 32, "calculateMd5", "MD5 hex string must be 32 chars");
        return hex;
        
    } catch (const std::exception& e) {
        logError("Exception calculating MD5: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculateSha256(
    const std::filesystem::path& file_path,
    hash_progress_callback& progress,
    std::stop_token stop_token) const -> std::expected<std::string, error_code> {
    Q_ASSERT_X(!file_path.empty(), "calculateSha256", "file_path must not be empty");
    Q_ASSERT_X(m_chunk_size > 0, "calculateSha256", "chunk_size must be positive");
    
    try {
        // Open file with Qt
        QFile file(QString::fromStdString(file_path.string()));
        if (!file.open(QIODevice::ReadOnly)) {
            logError("Failed to open file: {}", file_path.string());
            return std::unexpected(error_code::read_error);
        }
        
        // Initialize SHA-256 hash
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hashFileInChunks(file, hash, progress, stop_token);
        
        if (stop_token.stop_requested()) {
            return std::unexpected(error_code::operation_cancelled);
        }
        
        // Get final hash as hex string
        QByteArray result = hash.result();
        Q_ASSERT_X(result.size() == 32, "calculateSha256", "SHA-256 digest must be 32 bytes");
        auto hex = result.toHex().toStdString();
        Q_ASSERT_X(hex.size() == 64, "calculateSha256", "SHA-256 hex string must be 64 chars");
        return hex;
        
    } catch (const std::exception& e) {
        logError("Exception calculating SHA-256: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculateMd5(
    std::span<const std::byte> data) const -> std::expected<std::string, error_code> {
    
    try {
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(data.data()),
                        static_cast<qsizetype>(data.size())));
        QByteArray result = hash.result();
        return result.toHex().toStdString();
        
    } catch (const std::exception& e) {
        logError("Exception calculating in-memory MD5 hash: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculateSha256(
    std::span<const std::byte> data) const -> std::expected<std::string, error_code> {
    
    try {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(data.data()),
                        static_cast<qsizetype>(data.size())));
        QByteArray result = hash.result();
        return result.toHex().toStdString();
        
    } catch (const std::exception& e) {
        logError("Exception calculating in-memory SHA-256 hash: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

void file_hasher::hashFileInChunks(QFile& file, QCryptographicHash& hash,
                                    hash_progress_callback& progress,
                                    std::stop_token& stop_token) const {
    const auto file_size = static_cast<std::size_t>(file.size());
    std::size_t bytes_processed = 0;

    while (!file.atEnd() && !stop_token.stop_requested()) {
        QByteArray buffer = file.read(m_chunk_size);
        if (buffer.isEmpty()) continue;

        hash.addData(buffer);
        bytes_processed += buffer.size();
        if (progress) progress(bytes_processed, file_size);
    }
}

} // namespace sak
