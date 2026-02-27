// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_error_codes.cpp
/// @brief Unit tests for error_code enum to_string completeness

#include <QtTest/QtTest>

#include "sak/error_codes.h"

#include <string_view>

class ErrorCodeTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void allCodesHaveNames();
    void successIsZero();
    void specificCodes();
    void noUndefinedForKnownCodes();
};

void ErrorCodeTests::allCodesHaveNames()
{
    // Verify that every declared error_code has a non-empty, non-"Undefined"
    // string representation. This prevents forgotten switch cases.
    using sak::error_code;
    using sak::to_string;

    const error_code codes[] = {
        error_code::success,
        // File system
        error_code::file_not_found,
        error_code::permission_denied,
        error_code::path_too_long,
        error_code::invalid_path,
        error_code::disk_full,
        error_code::file_already_exists,
        error_code::directory_not_empty,
        error_code::is_directory,
        error_code::not_a_directory,
        error_code::file_too_large,
        error_code::invalid_filename,
        error_code::circular_reference,
        error_code::symlink_loop,
        // I/O
        error_code::read_error,
        error_code::write_error,
        error_code::seek_error,
        error_code::truncate_error,
        error_code::flush_error,
        error_code::lock_error,
        error_code::invalid_argument,
        // Hash
        error_code::hash_calculation_failed,
        error_code::hash_mismatch,
        error_code::verification_failed,
        error_code::corrupted_data,
        // Config
        error_code::invalid_configuration,
        error_code::missing_required_field,
        error_code::parse_error,
        error_code::unsupported_version,
        // Platform
        error_code::platform_not_supported,
        error_code::permission_update_failed,
        error_code::registry_access_denied,
        error_code::plist_parse_error,
        error_code::elevation_required,
        error_code::elevation_failed,
        error_code::environment_error,
        error_code::execution_failed,
        error_code::not_found,
        // Threading
        error_code::thread_creation_failed,
        error_code::operation_cancelled,
        error_code::timeout,
        error_code::deadlock_detected,
        // Memory
        error_code::out_of_memory,
        error_code::allocation_failed,
        error_code::buffer_overflow,
        // Scanner
        error_code::scan_failed,
        error_code::organization_failed,
        error_code::duplicate_resolution_failed,
        error_code::license_scan_failed,
        error_code::backup_failed,
        // Network
        error_code::network_unavailable,
        error_code::connection_failed,
        error_code::transfer_failed,
        error_code::network_timeout,
        error_code::protocol_error,
        error_code::authentication_failed,
        // Security
        error_code::validation_failed,
        error_code::path_traversal_attempt,
        error_code::invalid_file,
        error_code::integer_overflow,
        error_code::insufficient_disk_space,
        error_code::insufficient_memory,
        error_code::resource_limit_reached,
        error_code::filesystem_error,
        error_code::crypto_error,
        error_code::decrypt_failed,
        error_code::invalid_format,
        // Generic
        error_code::unknown_error,
        error_code::not_implemented,
        error_code::internal_error,
        error_code::assertion_failed,
        error_code::invalid_operation,
        error_code::partial_failure,
    };

    for (const auto ec : codes) {
        const auto name = to_string(ec);
        QVERIFY2(!name.empty(),
                 qPrintable(QString("error_code %1 has empty name")
                                .arg(static_cast<int>(ec))));
        QVERIFY2(name != "Undefined error",
                 qPrintable(QString("error_code %1 maps to 'Undefined error'")
                                .arg(static_cast<int>(ec))));
    }
}

void ErrorCodeTests::successIsZero()
{
    QCOMPARE(static_cast<int>(sak::error_code::success), 0);
    QCOMPARE(sak::to_string(sak::error_code::success), std::string_view("Success"));
}

void ErrorCodeTests::specificCodes()
{
    using sak::error_code;
    using sak::to_string;

    QCOMPARE(to_string(error_code::file_not_found),     std::string_view("File not found"));
    QCOMPARE(to_string(error_code::internal_error),     std::string_view("Internal error"));
    QCOMPARE(to_string(error_code::invalid_argument),   std::string_view("Invalid argument"));
    QCOMPARE(to_string(error_code::invalid_operation),  std::string_view("Invalid operation"));
    QCOMPARE(to_string(error_code::partial_failure),    std::string_view("Partial failure"));
    QCOMPARE(to_string(error_code::crypto_error),       std::string_view("Cryptographic error"));
    QCOMPARE(to_string(error_code::decrypt_failed),     std::string_view("Decryption failed"));
    QCOMPARE(to_string(error_code::invalid_format),     std::string_view("Invalid format"));
}

void ErrorCodeTests::noUndefinedForKnownCodes()
{
    // Test that an unknown numeric value falls through to "Undefined error"
    auto unknown = static_cast<sak::error_code>(99999);
    QCOMPARE(sak::to_string(unknown), std::string_view("Undefined error"));
}

QTEST_MAIN(ErrorCodeTests)
#include "test_error_codes.moc"
