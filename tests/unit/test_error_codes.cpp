// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_error_codes.cpp
/// @brief Unit tests for error_code enum to_string completeness

#include "sak/error_codes.h"

#include <QtTest/QtTest>

#include <string_view>

class ErrorCodeTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void allCodesHaveNames();
    void successIsZero();
    void specificCodes();
    void noUndefinedForKnownCodes();
};

// Every declared error_code paired with the EXACT message to_string() must return. The
// expected text is an INDEPENDENT literal here (copied verbatim from kErrorCodeMessages in
// error_codes.h), NOT a read of the production table, so a message typo or a code
// accidentally mapped to another code's text fails this test instead of silently passing.
struct ErrorCodeExpectation {
    sak::error_code code;
    std::string_view expected;
};

static constexpr ErrorCodeExpectation kAllErrorCodes[] = {
    {sak::error_code::success, "Success"},
    // File system
    {sak::error_code::file_not_found, "File not found"},
    {sak::error_code::permission_denied, "Permission denied"},
    {sak::error_code::path_too_long, "Path too long"},
    {sak::error_code::invalid_path, "Invalid path"},
    {sak::error_code::disk_full, "Disk full"},
    {sak::error_code::file_already_exists, "File already exists"},
    {sak::error_code::directory_not_empty, "Directory not empty"},
    {sak::error_code::is_directory, "Path is a directory"},
    {sak::error_code::not_a_directory, "Path is not a directory"},
    {sak::error_code::file_too_large, "File too large"},
    {sak::error_code::invalid_filename, "Invalid filename"},
    {sak::error_code::circular_reference, "Circular reference detected"},
    {sak::error_code::symlink_loop, "Symlink loop detected"},
    // I/O
    {sak::error_code::read_error, "Read error"},
    {sak::error_code::write_error, "Write error"},
    {sak::error_code::seek_error, "Seek error"},
    {sak::error_code::truncate_error, "Truncate error"},
    {sak::error_code::flush_error, "Flush error"},
    {sak::error_code::lock_error, "Lock error"},
    {sak::error_code::invalid_argument, "Invalid argument"},
    // Hash
    {sak::error_code::hash_calculation_failed, "Hash calculation failed"},
    {sak::error_code::hash_mismatch, "Hash mismatch"},
    {sak::error_code::verification_failed, "Verification failed"},
    {sak::error_code::corrupted_data, "Corrupted data"},
    // Config
    {sak::error_code::invalid_configuration, "Invalid configuration"},
    {sak::error_code::missing_required_field, "Missing required field"},
    {sak::error_code::parse_error, "Parse error"},
    {sak::error_code::unsupported_version, "Unsupported version"},
    // Platform
    {sak::error_code::platform_not_supported, "Platform not supported"},
    {sak::error_code::permission_update_failed, "Permission update failed"},
    {sak::error_code::registry_access_denied, "Registry access denied"},
    {sak::error_code::plist_parse_error, "Plist parse error"},
    {sak::error_code::elevation_required, "Elevation required"},
    {sak::error_code::elevation_failed, "Elevation failed"},
    {sak::error_code::environment_error, "Environment error"},
    {sak::error_code::execution_failed, "Execution failed"},
    {sak::error_code::not_found, "Not found"},
    // Threading
    {sak::error_code::thread_creation_failed, "Thread creation failed"},
    {sak::error_code::operation_cancelled, "Operation cancelled"},
    {sak::error_code::timeout, "Operation timed out"},
    {sak::error_code::deadlock_detected, "Deadlock detected"},
    // Memory
    {sak::error_code::out_of_memory, "Out of memory"},
    {sak::error_code::allocation_failed, "Allocation failed"},
    {sak::error_code::buffer_overflow, "Buffer overflow"},
    // Scanner
    {sak::error_code::scan_failed, "Scan failed"},
    {sak::error_code::organization_failed, "Organization failed"},
    {sak::error_code::duplicate_resolution_failed, "Duplicate resolution failed"},
    {sak::error_code::license_scan_failed, "License scan failed"},
    {sak::error_code::backup_failed, "Backup failed"},
    // Network
    {sak::error_code::network_unavailable, "Network unavailable"},
    {sak::error_code::connection_failed, "Connection failed"},
    {sak::error_code::transfer_failed, "Transfer failed"},
    {sak::error_code::network_timeout, "Network timeout"},
    {sak::error_code::protocol_error, "Protocol error"},
    {sak::error_code::authentication_failed, "Authentication failed"},
    // Security
    {sak::error_code::validation_failed, "Validation failed"},
    {sak::error_code::path_traversal_attempt, "Path traversal attempt detected"},
    {sak::error_code::invalid_file, "Invalid file"},
    {sak::error_code::integer_overflow, "Integer overflow"},
    {sak::error_code::insufficient_disk_space, "Insufficient disk space"},
    {sak::error_code::insufficient_memory, "Insufficient memory"},
    {sak::error_code::resource_limit_reached, "Resource limit reached"},
    {sak::error_code::filesystem_error, "Filesystem error"},
    {sak::error_code::crypto_error, "Cryptographic error"},
    {sak::error_code::decrypt_failed, "Decryption failed"},
    {sak::error_code::invalid_format, "Invalid format"},
    // Generic
    {sak::error_code::unknown_error, "Unclassified error (no specific error code was set)"},
    {sak::error_code::not_implemented, "Not implemented"},
    {sak::error_code::internal_error, "Internal error"},
    {sak::error_code::assertion_failed, "Assertion failed"},
    {sak::error_code::invalid_operation, "Invalid operation"},
    {sak::error_code::partial_failure, "Partial failure"},
};

void ErrorCodeTests::allCodesHaveNames() {
    // Exact-message pin for every declared code. QCOMPARE against the independent literal
    // subsumes the old !empty() and != "Undefined error" checks (an empty or fallback text
    // also fails equality) and additionally catches a typo or a cross-mapped message.
    for (const auto& entry : kAllErrorCodes) {
        QCOMPARE(sak::to_string(entry.code), entry.expected);
    }
}

void ErrorCodeTests::successIsZero() {
    QCOMPARE(static_cast<int>(sak::error_code::success), 0);
    QCOMPARE(sak::to_string(sak::error_code::success), std::string_view("Success"));
}

void ErrorCodeTests::specificCodes() {
    using sak::error_code;
    using sak::to_string;

    QCOMPARE(to_string(error_code::file_not_found), std::string_view("File not found"));
    QCOMPARE(to_string(error_code::internal_error), std::string_view("Internal error"));
    QCOMPARE(to_string(error_code::invalid_argument), std::string_view("Invalid argument"));
    QCOMPARE(to_string(error_code::invalid_operation), std::string_view("Invalid operation"));
    QCOMPARE(to_string(error_code::partial_failure), std::string_view("Partial failure"));
    QCOMPARE(to_string(error_code::crypto_error), std::string_view("Cryptographic error"));
    QCOMPARE(to_string(error_code::decrypt_failed), std::string_view("Decryption failed"));
    QCOMPARE(to_string(error_code::invalid_format), std::string_view("Invalid format"));
}

void ErrorCodeTests::noUndefinedForKnownCodes() {
    // Test that an unknown numeric value falls through to "Undefined error"
    auto unknown = static_cast<sak::error_code>(99'999);
    QCOMPARE(sak::to_string(unknown), std::string_view("Undefined error"));
}

QTEST_MAIN(ErrorCodeTests)
#include "test_error_codes.moc"
