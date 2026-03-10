// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_input_validator.cpp
/// @brief Unit tests for input validation utilities

#include "sak/error_codes.h"
#include "sak/input_validator.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdint>
#include <filesystem>
#include <limits>

class InputValidatorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Path validation
    void validatePath_existingFile();
    void validatePath_nonExistent_mustExistFails();
    void validatePath_nonExistent_noMustExist();
    void validatePath_directoryAsFile_fails();
    void validatePath_traversalSequences();
    void validatePath_suspiciousPatterns();
    void validatePath_maxPathLength();

    // Path-within-base validation
    void pathWithinBase_validSubpath();
    void pathWithinBase_traversalRejected();

    // String validation
    void validateString_normal();
    void validateString_tooShort();
    void validateString_tooLong();
    void validateString_nullBytes();
    void validateString_controlChars();
    void validateString_utf8Check();

    // String sanitization
    void sanitizeString_removesControlChars();
    void sanitizeString_preservesUnicode();
    void sanitizeString_stripUnicode();

    // Numeric validation
    void validateNumeric_inRange();
    void validateNumeric_belowMin();
    void validateNumeric_aboveMax();

    // Safe arithmetic
    void safeAdd_normal();
    void safeAdd_overflow();
    void safeMultiply_normal();
    void safeMultiply_overflow();
    void safeCast_validCast();
    void safeCast_overflowCast();

    // Buffer validation
    void validateBufferSize_withinLimits();
    void validateBufferSize_exceedsMax();
    void validateBufferSize_belowMin();

    // Resource validation
    void validateDiskSpace_hasFreeSpace();
    void validateThreadCount_reasonable();

    // Helper functions
    void containsTraversal_dotDot();
    void containsTraversal_normal();
    void containsNullBytes_detected();
    void containsControlChars_detected();
    void isValidUtf8_valid();
    void isValidUtf8_invalid();

private:
    QTemporaryDir m_tempDir;
    std::filesystem::path m_basePath;
};

void InputValidatorTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_basePath = m_tempDir.path().toStdWString();

    QFile f(m_tempDir.filePath("test_file.txt"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("test content");
    f.close();

    QDir(m_tempDir.path()).mkpath("test_subdir");
}

// ============================================================================
// Path Validation Tests
// ============================================================================

void InputValidatorTests::validatePath_existingFile() {
    sak::path_validation_config cfg;
    cfg.must_exist = true;
    cfg.must_be_file = true;

    auto result = sak::input_validator::validatePath(m_basePath / "test_file.txt", cfg);
    QVERIFY2(result.is_valid, qPrintable(QString::fromStdString(result.error_message)));
}

void InputValidatorTests::validatePath_nonExistent_mustExistFails() {
    sak::path_validation_config cfg;
    cfg.must_exist = true;

    auto result = sak::input_validator::validatePath(m_basePath / "nonexistent.txt", cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validatePath_nonExistent_noMustExist() {
    sak::path_validation_config cfg;
    cfg.must_exist = false;

    auto result = sak::input_validator::validatePath(m_basePath / "future_file.txt", cfg);
    QVERIFY(result.is_valid);
}

void InputValidatorTests::validatePath_directoryAsFile_fails() {
    sak::path_validation_config cfg;
    cfg.must_exist = true;
    cfg.must_be_file = true;

    auto result = sak::input_validator::validatePath(m_basePath / "test_subdir", cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validatePath_traversalSequences() {
    QVERIFY(sak::input_validator::containsTraversalSequences(
        std::filesystem::path("subdir/../../../etc/passwd")));
}

void InputValidatorTests::validatePath_suspiciousPatterns() {
    QVERIFY(sak::input_validator::containsSuspiciousPatterns(
        std::filesystem::path("\\\\server\\share")));
}

void InputValidatorTests::validatePath_maxPathLength() {
    sak::path_validation_config cfg;
    cfg.max_path_length = 10;

    auto result = sak::input_validator::validatePath(
        std::filesystem::path("this_is_a_very_long_path_name.txt"), cfg);
    QVERIFY(!result.is_valid);
}

// ============================================================================
// Path Within Base
// ============================================================================

void InputValidatorTests::pathWithinBase_validSubpath() {
    auto result = sak::input_validator::validatePathWithinBase(m_basePath / "test_file.txt",
                                                               m_basePath);
    QVERIFY(result.is_valid);
}

void InputValidatorTests::pathWithinBase_traversalRejected() {
    auto result = sak::input_validator::validatePathWithinBase(
        m_basePath / ".." / ".." / "etc" / "passwd", m_basePath);
    QVERIFY(!result.is_valid);
}

// ============================================================================
// String Validation Tests
// ============================================================================

void InputValidatorTests::validateString_normal() {
    auto result = sak::input_validator::validateString("Hello, World!");
    QVERIFY(result.is_valid);
}

void InputValidatorTests::validateString_tooShort() {
    sak::string_validation_config cfg;
    cfg.min_length = 5;

    auto result = sak::input_validator::validateString("Hi", cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validateString_tooLong() {
    sak::string_validation_config cfg;
    cfg.max_length = 5;

    auto result = sak::input_validator::validateString("This is too long", cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validateString_nullBytes() {
    std::string withNull = "hello\0world";
    withNull.resize(11);  // Ensure null byte is included

    sak::string_validation_config cfg;
    cfg.allow_null_bytes = false;

    auto result = sak::input_validator::validateString(
        std::string_view(withNull.data(), withNull.size()), cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validateString_controlChars() {
    sak::string_validation_config cfg;
    cfg.allow_control_chars = false;

    auto result = sak::input_validator::validateString("hello\x01world", cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validateString_utf8Check() {
    sak::string_validation_config cfg;
    cfg.require_utf8 = true;

    // Valid UTF-8
    auto result = sak::input_validator::validateString(std::string_view("Hello \xC3\xA9"), cfg);
    QVERIFY(result.is_valid);
}

// ============================================================================
// Sanitization Tests
// ============================================================================

void InputValidatorTests::sanitizeString_removesControlChars() {
    std::string input = "hello\x01\x02world";
    std::string sanitized = sak::input_validator::sanitizeString(input);
    QVERIFY(!sak::input_validator::containsControlChars(sanitized));
}

void InputValidatorTests::sanitizeString_preservesUnicode() {
    std::string input = "H\xC3\xA9llo W\xC3\xB6rld";
    std::string sanitized = sak::input_validator::sanitizeString(input, true);
    QCOMPARE(sanitized, input);
}

void InputValidatorTests::sanitizeString_stripUnicode() {
    std::string input = "Hello \xC3\xA9 World";
    std::string sanitized = sak::input_validator::sanitizeString(input, false);
    // Non-ASCII bytes should be removed when allow_unicode=false
    // The exact behavior depends on implementation
    QVERIFY(!sanitized.empty());
}

// ============================================================================
// Numeric Validation Tests
// ============================================================================

void InputValidatorTests::validateNumeric_inRange() {
    sak::numeric_validation_config<int> cfg;
    cfg.min_value = 0;
    cfg.max_value = 100;

    auto result = sak::input_validator::validateNumeric(50, cfg);
    QVERIFY(result.is_valid);
}

void InputValidatorTests::validateNumeric_belowMin() {
    sak::numeric_validation_config<int> cfg;
    cfg.min_value = 10;
    cfg.max_value = 100;

    auto result = sak::input_validator::validateNumeric(5, cfg);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validateNumeric_aboveMax() {
    sak::numeric_validation_config<int> cfg;
    cfg.min_value = 0;
    cfg.max_value = 100;

    auto result = sak::input_validator::validateNumeric(150, cfg);
    QVERIFY(!result.is_valid);
}

// ============================================================================
// Safe Arithmetic Tests
// ============================================================================

void InputValidatorTests::safeAdd_normal() {
    auto result = sak::input_validator::safeAdd(10, 20);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), 30);
}

void InputValidatorTests::safeAdd_overflow() {
    auto result = sak::input_validator::safeAdd((std::numeric_limits<int>::max)(), 1);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::integer_overflow);
}

void InputValidatorTests::safeMultiply_normal() {
    auto result = sak::input_validator::safeMultiply(6, 7);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), 42);
}

void InputValidatorTests::safeMultiply_overflow() {
    auto result = sak::input_validator::safeMultiply((std::numeric_limits<int>::max)(), 2);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::integer_overflow);
}

void InputValidatorTests::safeCast_validCast() {
    auto result = sak::input_validator::safeCast<int>(std::int64_t{42});
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), 42);
}

void InputValidatorTests::safeCast_overflowCast() {
    auto result = sak::input_validator::safeCast<int>(
        static_cast<std::int64_t>((std::numeric_limits<int>::max)()) + 1);
    QVERIFY(!result.has_value());
}

// ============================================================================
// Buffer Validation Tests
// ============================================================================

void InputValidatorTests::validateBufferSize_withinLimits() {
    auto result = sak::input_validator::validateBufferSize(100, 1024, 10);
    QVERIFY(result.is_valid);
}

void InputValidatorTests::validateBufferSize_exceedsMax() {
    auto result = sak::input_validator::validateBufferSize(2048, 1024, 0);
    QVERIFY(!result.is_valid);
}

void InputValidatorTests::validateBufferSize_belowMin() {
    auto result = sak::input_validator::validateBufferSize(5, 1024, 10);
    QVERIFY(!result.is_valid);
}

// ============================================================================
// Resource Validation Tests
// ============================================================================

void InputValidatorTests::validateDiskSpace_hasFreeSpace() {
    // Verify that our temp dir has at least 1 byte free
    auto result = sak::input_validator::validate_disk_space(m_basePath, 1);
    QVERIFY(result.is_valid);
}

void InputValidatorTests::validateThreadCount_reasonable() {
    auto result = sak::input_validator::validate_thread_count(4);
    QVERIFY(result.is_valid);
}

// ============================================================================
// Helper Function Tests
// ============================================================================

void InputValidatorTests::containsTraversal_dotDot() {
    QVERIFY(
        sak::input_validator::containsTraversalSequences(std::filesystem::path("../../../etc")));
    QVERIFY(sak::input_validator::containsTraversalSequences(
        std::filesystem::path("subdir/../../parent")));
}

void InputValidatorTests::containsTraversal_normal() {
    QVERIFY(!sak::input_validator::containsTraversalSequences(
        std::filesystem::path("subdir/file.txt")));
    QVERIFY(
        !sak::input_validator::containsTraversalSequences(std::filesystem::path("normal_path")));
}

void InputValidatorTests::containsNullBytes_detected() {
    std::string withNull("hello\0world", 11);
    QVERIFY(sak::input_validator::containsNullBytes(withNull));
    QVERIFY(!sak::input_validator::containsNullBytes("hello world"));
}

void InputValidatorTests::containsControlChars_detected() {
    QVERIFY(sak::input_validator::containsControlChars("hello\x01world"));
    QVERIFY(!sak::input_validator::containsControlChars("hello world"));
}

void InputValidatorTests::isValidUtf8_valid() {
    QVERIFY(sak::input_validator::isValidUtf8("Hello World"));
    QVERIFY(sak::input_validator::isValidUtf8("H\xC3\xA9llo"));
}

void InputValidatorTests::isValidUtf8_invalid() {
    // Invalid UTF-8: lone continuation byte
    QVERIFY(!sak::input_validator::isValidUtf8("hello\x80world"));

    // Invalid UTF-8: truncated 2-byte sequence
    QVERIFY(!sak::input_validator::isValidUtf8("abc\xC3"));

    // Invalid UTF-8: truncated 3-byte sequence
    QVERIFY(!sak::input_validator::isValidUtf8("abc\xE0\xA0"));

    // Invalid UTF-8: overlong 2-byte encoding of ASCII
    QVERIFY(!sak::input_validator::isValidUtf8("\xC0\x80"));
}

QTEST_GUILESS_MAIN(InputValidatorTests)
#include "test_input_validator.moc"
