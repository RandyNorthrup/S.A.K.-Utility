/// @file file_hash.h
/// @brief File hashing utilities with MD5 and SHA-256 support
/// @note Thread-safe, memory-efficient chunked hashing

#ifndef SAK_FILE_HASH_H
#define SAK_FILE_HASH_H

#include "error_codes.h"
#include <cstddef>
#include <expected>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <stop_token>

namespace sak {

/// @brief Hash algorithm selection
enum class hash_algorithm {
    md5,        ///< MD5 hash (fast, less secure)
    sha256      ///< SHA-256 hash (slower, more secure)
};

/// @brief Progress callback function type
/// @param bytes_processed Bytes processed so far
/// @param total_bytes Total bytes to process
using hash_progress_callback = std::move_only_function<void(std::size_t, std::size_t)>;

/// @brief File hasher with chunked reading and progress reporting
class file_hasher {
public:
    /// @brief Default chunk size for reading (1MB)
    static constexpr std::size_t DEFAULT_CHUNK_SIZE = 1024 * 1024;
    
    /// @brief Constructor
    /// @param algorithm Hash algorithm to use
    /// @param chunk_size Size of chunks to read
    explicit file_hasher(
        hash_algorithm algorithm = hash_algorithm::md5,
        std::size_t chunk_size = DEFAULT_CHUNK_SIZE) noexcept;
    
    /// @brief Calculate hash of a file
    /// @param file_path Path to file
    /// @param progress Optional progress callback
    /// @param stop_token Optional cancellation token
    /// @return Expected containing hash string or error code
    [[nodiscard]] auto calculate_hash(
        const std::filesystem::path& file_path,
        hash_progress_callback progress = nullptr,
        std::stop_token stop_token = {}) const -> std::expected<std::string, error_code>;
    
    /// @brief Calculate hash of a buffer
    /// @param data Data to hash
    /// @return Expected containing hash string or error code
    [[nodiscard]] auto calculate_hash(
        std::span<const std::byte> data) const -> std::expected<std::string, error_code>;
    
    /// @brief Verify file hash matches expected value
    /// @param file_path Path to file
    /// @param expected_hash Expected hash value
    /// @param stop_token Optional cancellation token
    /// @return Expected containing true if match, false if mismatch, or error code
    [[nodiscard]] auto verify_hash(
        const std::filesystem::path& file_path,
        std::string_view expected_hash,
        std::stop_token stop_token = {}) const -> std::expected<bool, error_code>;
    
    /// @brief Get current hash algorithm
    /// @return Current algorithm
    [[nodiscard]] hash_algorithm get_algorithm() const noexcept {
        return m_algorithm;
    }
    
    /// @brief Get current chunk size
    /// @return Current chunk size in bytes
    [[nodiscard]] std::size_t get_chunk_size() const noexcept {
        return m_chunk_size;
    }

private:
    hash_algorithm m_algorithm;  ///< Hash algorithm to use
    std::size_t m_chunk_size;    ///< Chunk size for reading
    
    /// @brief Calculate MD5 hash of file
    /// @param file_path Path to file
    /// @param progress Optional progress callback
    /// @param stop_token Optional cancellation token
    /// @return Expected containing hash string or error code
    [[nodiscard]] auto calculate_md5(
        const std::filesystem::path& file_path,
        hash_progress_callback& progress,
        std::stop_token stop_token) const -> std::expected<std::string, error_code>;
    
    /// @brief Calculate SHA-256 hash of file
    /// @param file_path Path to file
    /// @param progress Optional progress callback
    /// @param stop_token Optional cancellation token
    /// @return Expected containing hash string or error code
    [[nodiscard]] auto calculate_sha256(
        const std::filesystem::path& file_path,
        hash_progress_callback& progress,
        std::stop_token stop_token) const -> std::expected<std::string, error_code>;
    
    /// @brief Calculate MD5 hash of buffer
    /// @param data Data to hash
    /// @return Expected containing hash string or error code
    [[nodiscard]] auto calculate_md5(
        std::span<const std::byte> data) const -> std::expected<std::string, error_code>;
    
    /// @brief Calculate SHA-256 hash of buffer
    /// @param data Data to hash
    /// @return Expected containing hash string or error code
    [[nodiscard]] auto calculate_sha256(
        std::span<const std::byte> data) const -> std::expected<std::string, error_code>;
};

/// @brief Convenience function to calculate MD5 hash of a file
/// @param file_path Path to file
/// @return Expected containing hash string or error code
[[nodiscard]] inline auto md5_file(
    const std::filesystem::path& file_path) -> std::expected<std::string, error_code> {
    return file_hasher(hash_algorithm::md5).calculate_hash(file_path);
}

/// @brief Convenience function to calculate SHA-256 hash of a file
/// @param file_path Path to file
/// @return Expected containing hash string or error code
[[nodiscard]] inline auto sha256_file(
    const std::filesystem::path& file_path) -> std::expected<std::string, error_code> {
    return file_hasher(hash_algorithm::sha256).calculate_hash(file_path);
}

/// @brief Convert hash bytes to hex string
/// @param hash_bytes Hash bytes
/// @return Hex string representation
[[nodiscard]] std::string hash_to_hex(std::span<const unsigned char> hash_bytes);

} // namespace sak

#endif // SAK_FILE_HASH_H
