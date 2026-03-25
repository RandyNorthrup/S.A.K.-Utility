// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file error_codes.h
/// @brief Error code definitions for SAK Utility
/// @note Using std::expected pattern for type-safe error handling
/// @note All error codes form part of the stable API surface. Codes not yet
///       used in production are reserved for planned features (memory
///       diagnostics, etc.) and must not be removed or renumbered.

#pragma once

#include <algorithm>
#include <string_view>
#include <system_error>

namespace sak {

/// @brief General error codes for file system operations
enum class error_code {
    success = 0,

    // File system errors (1-99)
    file_not_found = 1,
    permission_denied = 2,
    path_too_long = 3,
    invalid_path = 4,
    disk_full = 5,
    file_already_exists = 6,
    directory_not_empty = 7,
    is_directory = 8,
    not_a_directory = 9,
    file_too_large = 10,
    invalid_filename = 11,
    circular_reference = 12,
    symlink_loop = 13,

    // I/O errors (100-199)
    read_error = 100,
    write_error = 101,
    seek_error = 102,
    truncate_error = 103,
    flush_error = 104,
    lock_error = 105,
    invalid_argument = 106,

    // Hash/verification errors (200-299)
    hash_calculation_failed = 200,
    hash_mismatch = 201,
    verification_failed = 202,
    corrupted_data = 203,

    // Configuration errors (300-399)
    invalid_configuration = 300,
    missing_required_field = 301,
    parse_error = 302,
    unsupported_version = 303,

    // Platform errors (400-499)
    platform_not_supported = 400,
    permission_update_failed = 401,
    registry_access_denied = 402,
    plist_parse_error = 403,
    elevation_required = 404,
    elevation_failed = 405,
    environment_error = 406,
    execution_failed = 407,
    not_found = 408,

    // Threading errors (500-599)
    thread_creation_failed = 500,
    operation_cancelled = 501,
    timeout = 502,
    deadlock_detected = 503,

    // Memory errors (600-699)
    out_of_memory = 600,
    allocation_failed = 601,
    buffer_overflow = 602,

    // Scanner/organizer errors (700-799)
    scan_failed = 700,
    organization_failed = 701,
    duplicate_resolution_failed = 702,
    license_scan_failed = 703,
    backup_failed = 704,

    // Network errors (800-899)
    network_unavailable = 800,
    connection_failed = 801,
    transfer_failed = 802,
    network_timeout = 803,
    protocol_error = 804,
    authentication_failed = 805,

    // Security/validation errors (850-899)
    validation_failed = 850,
    path_traversal_attempt = 851,
    invalid_file = 852,
    integer_overflow = 853,
    insufficient_disk_space = 854,
    insufficient_memory = 855,
    resource_limit_reached = 856,
    filesystem_error = 857,
    crypto_error = 858,
    decrypt_failed = 859,
    invalid_format = 860,

    // Generic errors (900-999)
    unknown_error = 900,
    not_implemented = 901,
    internal_error = 902,
    assertion_failed = 903,
    invalid_operation = 904,
    partial_failure = 905,

    // Email / PST parser errors (1000-1099)
    pst_invalid_header = 1000,
    pst_unsupported_encryption = 1001,
    pst_corrupted_btree = 1002,
    pst_node_not_found = 1003,
    pst_block_not_found = 1004,
    pst_invalid_heap = 1005,
    pst_property_context_error = 1006,
    pst_table_context_error = 1007,
    pst_folder_traversal_error = 1008,
    pst_item_read_error = 1009,
    pst_attachment_error = 1010,
    pst_decompression_failed = 1018,
    mbox_invalid_format = 1011,
    mbox_message_parse_error = 1012,
    email_search_failed = 1013,
    email_export_failed = 1014,
    email_profile_discovery_failed = 1015,
    email_backup_failed = 1016,
    email_restore_failed = 1017,
};

/// @brief Convert error code to human-readable message
/// @param ec Error code to convert
/// @return String view describing the error
[[nodiscard]] constexpr std::string_view to_string(error_code ec) noexcept {
    struct Mapping {
        error_code code;
        std::string_view text;
    };
    static constexpr Mapping kTable[] = {
        {error_code::success, "Success"},
        {error_code::file_not_found, "File not found"},
        {error_code::permission_denied, "Permission denied"},
        {error_code::path_too_long, "Path too long"},
        {error_code::invalid_path, "Invalid path"},
        {error_code::disk_full, "Disk full"},
        {error_code::file_already_exists, "File already exists"},
        {error_code::directory_not_empty, "Directory not empty"},
        {error_code::is_directory, "Path is a directory"},
        {error_code::not_a_directory, "Path is not a directory"},
        {error_code::file_too_large, "File too large"},
        {error_code::invalid_filename, "Invalid filename"},
        {error_code::circular_reference, "Circular reference detected"},
        {error_code::symlink_loop, "Symlink loop detected"},
        {error_code::read_error, "Read error"},
        {error_code::write_error, "Write error"},
        {error_code::seek_error, "Seek error"},
        {error_code::truncate_error, "Truncate error"},
        {error_code::flush_error, "Flush error"},
        {error_code::lock_error, "Lock error"},
        {error_code::invalid_argument, "Invalid argument"},
        {error_code::hash_calculation_failed, "Hash calculation failed"},
        {error_code::hash_mismatch, "Hash mismatch"},
        {error_code::verification_failed, "Verification failed"},
        {error_code::corrupted_data, "Corrupted data"},
        {error_code::invalid_configuration, "Invalid configuration"},
        {error_code::missing_required_field, "Missing required field"},
        {error_code::parse_error, "Parse error"},
        {error_code::unsupported_version, "Unsupported version"},
        {error_code::platform_not_supported, "Platform not supported"},
        {error_code::permission_update_failed, "Permission update failed"},
        {error_code::registry_access_denied, "Registry access denied"},
        {error_code::plist_parse_error, "Plist parse error"},
        {error_code::elevation_required, "Elevation required"},
        {error_code::elevation_failed, "Elevation failed"},
        {error_code::environment_error, "Environment error"},
        {error_code::execution_failed, "Execution failed"},
        {error_code::not_found, "Not found"},
        {error_code::thread_creation_failed, "Thread creation failed"},
        {error_code::operation_cancelled, "Operation cancelled"},
        {error_code::timeout, "Operation timed out"},
        {error_code::deadlock_detected, "Deadlock detected"},
        {error_code::out_of_memory, "Out of memory"},
        {error_code::allocation_failed, "Allocation failed"},
        {error_code::buffer_overflow, "Buffer overflow"},
        {error_code::scan_failed, "Scan failed"},
        {error_code::organization_failed, "Organization failed"},
        {error_code::duplicate_resolution_failed, "Duplicate resolution failed"},
        {error_code::license_scan_failed, "License scan failed"},
        {error_code::backup_failed, "Backup failed"},
        {error_code::network_unavailable, "Network unavailable"},
        {error_code::connection_failed, "Connection failed"},
        {error_code::transfer_failed, "Transfer failed"},
        {error_code::network_timeout, "Network timeout"},
        {error_code::protocol_error, "Protocol error"},
        {error_code::authentication_failed, "Authentication failed"},
        {error_code::validation_failed, "Validation failed"},
        {error_code::path_traversal_attempt, "Path traversal attempt detected"},
        {error_code::invalid_file, "Invalid file"},
        {error_code::integer_overflow, "Integer overflow"},
        {error_code::insufficient_disk_space, "Insufficient disk space"},
        {error_code::insufficient_memory, "Insufficient memory"},
        {error_code::resource_limit_reached, "Resource limit reached"},
        {error_code::filesystem_error, "Filesystem error"},
        {error_code::crypto_error, "Cryptographic error"},
        {error_code::decrypt_failed, "Decryption failed"},
        {error_code::invalid_format, "Invalid format"},
        {error_code::unknown_error, "Unknown error"},
        {error_code::not_implemented, "Not implemented"},
        {error_code::internal_error, "Internal error"},
        {error_code::assertion_failed, "Assertion failed"},
        {error_code::invalid_operation, "Invalid operation"},
        {error_code::partial_failure, "Partial failure"},
        {error_code::pst_invalid_header, "Invalid file header"},
        {error_code::pst_unsupported_encryption, "Unsupported encryption"},
        {error_code::pst_corrupted_btree, "Corrupted BTree structure"},
        {error_code::pst_node_not_found, "Node not found"},
        {error_code::pst_block_not_found, "Block not found"},
        {error_code::pst_invalid_heap, "Invalid heap structure"},
        {error_code::pst_property_context_error, "Property context error"},
        {error_code::pst_table_context_error, "Table context error"},
        {error_code::pst_folder_traversal_error, "Folder traversal error"},
        {error_code::pst_item_read_error, "Item read error"},
        {error_code::pst_attachment_error, "Attachment read error"},
        {error_code::pst_decompression_failed, "Block decompression failed"},
        {error_code::mbox_invalid_format, "MBOX invalid format"},
        {error_code::mbox_message_parse_error, "MBOX message parse error"},
        {error_code::email_search_failed, "Email search failed"},
        {error_code::email_export_failed, "Email export failed"},
        {error_code::email_profile_discovery_failed, "Email profile discovery failed"},
        {error_code::email_backup_failed, "Email backup failed"},
        {error_code::email_restore_failed, "Email restore failed"},
    };
    const auto it = std::find_if(std::begin(kTable), std::end(kTable), [ec](const auto& entry) {
        return entry.code == ec;
    });
    if (it != std::end(kTable)) {
        return it->text;
    }
    return "Undefined error";
}

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

/// Verify error_code has int-sized underlying type for ABI stability.
static_assert(sizeof(error_code) == sizeof(int), "error_code must be int-sized for ABI stability.");

/// Verify sentinel values are in expected ranges.
static_assert(static_cast<int>(error_code::success) == 0, "success must be zero.");
static_assert(static_cast<int>(error_code::file_not_found) >= 1 &&
                  static_cast<int>(error_code::file_not_found) < 100,
              "File system errors must be in range [1, 100).");
static_assert(static_cast<int>(error_code::read_error) >= 100 &&
                  static_cast<int>(error_code::read_error) < 200,
              "I/O errors must be in range [100, 200).");
static_assert(static_cast<int>(error_code::hash_calculation_failed) >= 200 &&
                  static_cast<int>(error_code::hash_calculation_failed) < 300,
              "Hash errors must be in range [200, 300).");
static_assert(static_cast<int>(error_code::unknown_error) >= 900,
              "Generic errors must be in range [900, 1000).");

}  // namespace sak
