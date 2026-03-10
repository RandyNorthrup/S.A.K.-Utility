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

// All declared error_code values for exhaustive testing.
static constexpr sak::error_code kAllErrorCodes[] = {
    sak::error_code::success,
    // File system
    sak::error_code::file_not_found,
    sak::error_code::permission_denied,
    sak::error_code::path_too_long,
    sak::error_code::invalid_path,
    sak::error_code::disk_full,
    sak::error_code::file_already_exists,
    sak::error_code::directory_not_empty,
    sak::error_code::is_directory,
    sak::error_code::not_a_directory,
    sak::error_code::file_too_large,
    sak::error_code::invalid_filename,
    sak::error_code::circular_reference,
    sak::error_code::symlink_loop,
    // I/O
    sak::error_code::read_error,
    sak::error_code::write_error,
    sak::error_code::seek_error,
    sak::error_code::truncate_error,
    sak::error_code::flush_error,
    sak::error_code::lock_error,
    sak::error_code::invalid_argument,
    // Hash
    sak::error_code::hash_calculation_failed,
    sak::error_code::hash_mismatch,
    sak::error_code::verification_failed,
    sak::error_code::corrupted_data,
    // Config
    sak::error_code::invalid_configuration,
    sak::error_code::missing_required_field,
    sak::error_code::parse_error,
    sak::error_code::unsupported_version,
    // Platform
    sak::error_code::platform_not_supported,
    sak::error_code::permission_update_failed,
    sak::error_code::registry_access_denied,
    sak::error_code::plist_parse_error,
    sak::error_code::elevation_required,
    sak::error_code::elevation_failed,
    sak::error_code::environment_error,
    sak::error_code::execution_failed,
    sak::error_code::not_found,
    // Threading
    sak::error_code::thread_creation_failed,
    sak::error_code::operation_cancelled,
    sak::error_code::timeout,
    sak::error_code::deadlock_detected,
    // Memory
    sak::error_code::out_of_memory,
    sak::error_code::allocation_failed,
    sak::error_code::buffer_overflow,
    // Scanner
    sak::error_code::scan_failed,
    sak::error_code::organization_failed,
    sak::error_code::duplicate_resolution_failed,
    sak::error_code::license_scan_failed,
    sak::error_code::backup_failed,
    // Network
    sak::error_code::network_unavailable,
    sak::error_code::connection_failed,
    sak::error_code::transfer_failed,
    sak::error_code::network_timeout,
    sak::error_code::protocol_error,
    sak::error_code::authentication_failed,
    // Security
    sak::error_code::validation_failed,
    sak::error_code::path_traversal_attempt,
    sak::error_code::invalid_file,
    sak::error_code::integer_overflow,
    sak::error_code::insufficient_disk_space,
    sak::error_code::insufficient_memory,
    sak::error_code::resource_limit_reached,
    sak::error_code::filesystem_error,
    sak::error_code::crypto_error,
    sak::error_code::decrypt_failed,
    sak::error_code::invalid_format,
    // Generic
    sak::error_code::unknown_error,
    sak::error_code::not_implemented,
    sak::error_code::internal_error,
    sak::error_code::assertion_failed,
    sak::error_code::invalid_operation,
    sak::error_code::partial_failure,
};

void ErrorCodeTests::allCodesHaveNames() {
    for (const auto ec : kAllErrorCodes) {
        const auto name = sak::to_string(ec);
        QVERIFY2(!name.empty(),
                 qPrintable(QString("error_code %1 has empty name").arg(static_cast<int>(ec))));
        QVERIFY2(name != "Undefined error",
                 qPrintable(
                     QString("error_code %1 maps to 'Undefined error'").arg(static_cast<int>(ec))));
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
