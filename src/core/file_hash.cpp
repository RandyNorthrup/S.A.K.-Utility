// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_hash.cpp
/// @brief Implementation of file hashing utilities

#include "sak/file_hash.h"

#include "sak/logger.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace sak {

namespace {
constexpr qsizetype kMd5DigestBytes = 16;
constexpr std::size_t kMd5HexChars = 32;
constexpr qsizetype kSha256DigestBytes = 32;
constexpr std::size_t kSha256HexChars = 64;

// Convert a filesystem path to a QString without a lossy narrow round-trip. On
// Windows std::filesystem::path::string() emits CP_ACP bytes that QString would
// then misread as UTF-8, mangling any non-ASCII path; native() preserves the
// wide path. On POSIX native() is already a byte string.
QString pathToQString(const std::filesystem::path& path) {
#ifdef _WIN32
    return QString::fromStdWString(path.native());
#else
    return QString::fromStdString(path.native());
#endif
}
}  // namespace

file_hasher::file_hasher(hash_algorithm algorithm, std::size_t chunk_size) noexcept
    // A zero chunk_size only trips the debug assert; in a release build it would
    // reach file.read(0), which returns an empty buffer, so hashing would stop at
    // once and report the EMPTY-input digest as the file's hash. Coerce it to the
    // default so release builds never hash with a zero chunk (B8-23).
    //
    // There is deliberately no assert alongside the coercion. The coercion is the
    // contract in every configuration; an assert would abort a Debug build on the
    // exact input Release accepts, which is the behaviour the test pins down.
    //
    // An over-large chunk_size is clamped to MAX_CHUNK_SIZE so file.read() can never be
    // handed a value that narrows when converted to qint64 or allocates an outsized buffer.
    : m_algorithm(algorithm)
    , m_chunk_size(chunk_size == 0 ? DEFAULT_CHUNK_SIZE : std::min(chunk_size, MAX_CHUNK_SIZE)) {}

auto file_hasher::calculateHash(const std::filesystem::path& file_path,
                                hash_progress_callback progress,
                                std::stop_token stop_token) const
    -> std::expected<std::string, error_code> {
    // An empty path is rejected by the exists() check below and reported as
    // file_not_found. No assert, for the reason given on the constructor.

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

auto file_hasher::calculateHash(std::span<const std::byte> data) const
    -> std::expected<std::string, error_code> {
    switch (m_algorithm) {
    case hash_algorithm::md5:
        return calculateMd5(data);
    case hash_algorithm::sha256:
        return calculateSha256(data);
    default:
        return std::unexpected(error_code::internal_error);
    }
}

auto file_hasher::verifyHash(const std::filesystem::path& file_path,
                             std::string_view expected_hash,
                             std::stop_token stop_token) const -> std::expected<bool, error_code> {
    // An empty file_path is reported as file_not_found by calculateHash, and an
    // empty expected_hash simply does not match. No asserts: both are handled
    // identically in Release, and asserting would abort a Debug build instead.
    auto calculated = calculateHash(file_path, nullptr, stop_token);
    if (!calculated) {
        return std::unexpected(calculated.error());
    }

    // Case-insensitive comparison. Route each byte through unsigned char before
    // std::tolower: a plain (signed) char with a value > 127 -- reachable via a
    // garbled/hostile expected_hash -- is negative, and passing a negative value
    // other than EOF to std::tolower is undefined behavior (B8-23).
    auto lower_expected = std::string(expected_hash);
    auto lower_calculated = *calculated;
    const auto to_lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::ranges::transform(lower_expected, lower_expected.begin(), to_lower);
    std::ranges::transform(lower_calculated, lower_calculated.begin(), to_lower);

    return lower_expected == lower_calculated;
}

auto file_hasher::calculateMd5(const std::filesystem::path& file_path,
                               hash_progress_callback& progress,
                               std::stop_token stop_token) const
    -> std::expected<std::string, error_code> {
    // file_path is not asserted: an empty path fails the QFile open below and is
    // reported. m_chunk_size stays asserted because it is an invariant this
    // class guarantees - the constructor coerces any non-positive value to
    // DEFAULT_CHUNK_SIZE, so a zero here would mean the object is corrupt.
    Q_ASSERT_X(m_chunk_size > 0, "calculateMd5", "chunk_size must be positive");

    try {
        // Open file with Qt
        const QString qpath = pathToQString(file_path);
        QFile file(qpath);
        if (!file.open(QIODevice::ReadOnly)) {
            logError("Failed to open file: {}", qpath.toStdString());
            return std::unexpected(error_code::read_error);
        }

        // Initialize MD5 hash
        QCryptographicHash hash(QCryptographicHash::Md5);
        if (!hashFileInChunks(file, hash, progress, stop_token)) {
            return std::unexpected(error_code::read_error);
        }

        if (stop_token.stop_requested()) {
            return std::unexpected(error_code::operation_cancelled);
        }

        // Get final hash as hex string. QCryptographicHash::result() for Md5
        // always yields hashLength(Md5) == 16 bytes and toHex() emits exactly two
        // chars per byte, so both sizes below are guaranteed by the Qt crypto API.
        const QByteArray result = hash.result();
        Q_ASSERT_X(result.size() == kMd5DigestBytes, "calculateMd5", "MD5 digest must be 16 bytes");
        auto hex = result.toHex().toStdString();
        Q_ASSERT_X(hex.size() == kMd5HexChars, "calculateMd5", "MD5 hex string must be 32 chars");
        return hex;

    } catch (const std::exception& e) {
        logError("Exception calculating MD5: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculateSha256(const std::filesystem::path& file_path,
                                  hash_progress_callback& progress,
                                  std::stop_token stop_token) const
    -> std::expected<std::string, error_code> {
    // See calculateMd5: file_path is handled at the open, m_chunk_size is a
    // constructor-guaranteed invariant.
    Q_ASSERT_X(m_chunk_size > 0, "calculateSha256", "chunk_size must be positive");

    try {
        // Open file with Qt
        const QString qpath = pathToQString(file_path);
        QFile file(qpath);
        if (!file.open(QIODevice::ReadOnly)) {
            logError("Failed to open file: {}", qpath.toStdString());
            return std::unexpected(error_code::read_error);
        }

        // Initialize SHA-256 hash
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hashFileInChunks(file, hash, progress, stop_token)) {
            return std::unexpected(error_code::read_error);
        }

        if (stop_token.stop_requested()) {
            return std::unexpected(error_code::operation_cancelled);
        }

        // Get final hash as hex string. QCryptographicHash::result() for Sha256
        // always yields hashLength(Sha256) == 32 bytes and toHex() emits exactly
        // two chars per byte, so both sizes below are Qt-guaranteed.
        const QByteArray result = hash.result();
        Q_ASSERT_X(result.size() == kSha256DigestBytes,
                   "calculateSha256",
                   "SHA-256 digest must be 32 bytes");
        auto hex = result.toHex().toStdString();
        Q_ASSERT_X(hex.size() == kSha256HexChars,
                   "calculateSha256",
                   "SHA-256 hex string must be 64 chars");
        return hex;

    } catch (const std::exception& e) {
        logError("Exception calculating SHA-256: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculateMd5(std::span<const std::byte> data)
    -> std::expected<std::string, error_code> {
    try {
        QCryptographicHash hash(QCryptographicHash::Md5);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(data.data()),
                                    static_cast<qsizetype>(data.size())));
        const QByteArray result = hash.result();
        return result.toHex().toStdString();

    } catch (const std::exception& e) {
        logError("Exception calculating in-memory MD5 hash: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

auto file_hasher::calculateSha256(std::span<const std::byte> data)
    -> std::expected<std::string, error_code> {
    try {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(data.data()),
                                    static_cast<qsizetype>(data.size())));
        const QByteArray result = hash.result();
        return result.toHex().toStdString();

    } catch (const std::exception& e) {
        logError("Exception calculating in-memory SHA-256 hash: {}", e.what());
        return std::unexpected(error_code::hash_calculation_failed);
    }
}

bool file_hasher::hashFileInChunks(
    QFile& file,
    QCryptographicHash& hash,
    // cppcheck-suppress constParameterReference ; move_only_function has non-const operator()
    hash_progress_callback& progress,
    const std::stop_token& stop_token) const {
    const auto file_size = static_cast<std::size_t>(file.size());
    std::size_t bytes_processed = 0;

    while (!stop_token.stop_requested()) {
        const QByteArray buffer = file.read(static_cast<qint64>(m_chunk_size));
        if (buffer.isEmpty()) {
            // An empty read is either a clean end-of-file or an I/O fault. The
            // previous code did `continue`, which on a mid-file read error spun
            // forever (atEnd stayed false) or, if atEnd flipped, returned a hash
            // over only the readable prefix as if it were complete. Distinguish
            // the two via error() so a fault fails closed.
            if (file.error() != QFileDevice::NoError) {
                logError("Read error while hashing '{}': {}",
                         file.fileName().toStdString(),
                         file.errorString().toStdString());
                return false;
            }
            // Clean end-of-file. The file must have delivered exactly the byte count it
            // reported when hashing began; a shorter (truncated) or longer (grown) total
            // means it was mutated underneath us and the digest would cover a mixed or
            // prefix view -- fail closed rather than report that as the file's hash.
            if (bytes_processed != file_size) {
                logError("File changed while hashing '{}' ({} of {} bytes) - mutated concurrently",
                         file.fileName().toStdString(),
                         bytes_processed,
                         file_size);
                return false;
            }
            break;  // clean end-of-file
        }

        hash.addData(buffer);
        bytes_processed += buffer.size();
        if (progress) {
            progress(bytes_processed, file_size);
        }
    }
    return true;
}

}  // namespace sak
