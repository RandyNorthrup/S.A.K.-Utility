// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_logger.cpp
/// @brief Unit tests for the thread-safe logging system
///
/// IMPORTANT: sak::logger is a Meyer's singleton. Once initialized, calling
/// initialize() again fails because the internal ofstream is already open.
/// All tests share a single initialization performed in initTestCase().
/// Each test uses unique marker strings for content verification.

#include <QtTest/QtTest>

#include "sak/logger.h"
#include "sak/error_codes.h"

#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

class LoggerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // Initialization (verified against the single init)
    void initialize_validDir();
    void initialize_createsDir();
    void initialize_reInitFails();

    // Level control
    void setLevel_getLevel();
    void levelFiltering_belowMinNotWritten();
    void levelFiltering_atMinWritten();

    // Log output
    void log_writesToFile();
    void log_multipleMessages();

    // Console output toggle
    void consoleOutput_toggle();

    // flush
    void flush_writesData();

    // getLogFile
    void getLogFile_afterInit();

    // isInitialized
    void isInitialized_afterInit();

    // Thread safety
    void concurrentWrites_noCorruption();

    // to_string for log_level
    void logLevel_toString();

private:
    /// @brief Reads all content from the singleton log file
    std::string readLogContent();

    QTemporaryDir m_logDir;
    std::filesystem::path m_logSubDir;
};

// ============================================================================
// Setup â€” single initialization for the entire test run
// ============================================================================

void LoggerTests::initTestCase()
{
    QVERIFY(m_logDir.isValid());

    // Use a subdirectory that doesn't exist yet â€” verifies initialize creates it
    m_logSubDir = std::filesystem::path(m_logDir.path().toStdWString()) / "log_subdir";
    QVERIFY(!std::filesystem::exists(m_logSubDir));

    auto& log = sak::logger::instance();
    auto result = log.initialize(m_logSubDir, "test_logger");
    QVERIFY2(result.has_value(), "Logger must initialize for tests to proceed");
    log.setLevel(sak::log_level::debug);
    log.setConsoleOutput(false);
}

std::string LoggerTests::readLogContent()
{
    auto& log = sak::logger::instance();
    log.flush();
    auto logFile = log.getLogFile();
    std::ifstream file(logFile);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

// ============================================================================
// Initialization
// ============================================================================

void LoggerTests::initialize_validDir()
{
    auto& log = sak::logger::instance();
    QVERIFY(log.isInitialized());
}

void LoggerTests::initialize_createsDir()
{
    // The subdirectory should have been created during initTestCase
    QVERIFY(std::filesystem::exists(m_logSubDir));
    QVERIFY(std::filesystem::is_directory(m_logSubDir));
}

void LoggerTests::initialize_reInitFails()
{
    // Re-initializing a singleton logger should fail gracefully
    auto& log = sak::logger::instance();
    auto result = log.initialize(
        std::filesystem::path("Z:\\InvalidDrive\\NoDir"), "reinit");
    QVERIFY(!result.has_value());
    // Original initialization should still be active
    QVERIFY(log.isInitialized());
}

// ============================================================================
// Level Control
// ============================================================================

void LoggerTests::setLevel_getLevel()
{
    auto& log = sak::logger::instance();
    log.setLevel(sak::log_level::warning);
    QCOMPARE(log.getLevel(), sak::log_level::warning);

    log.setLevel(sak::log_level::debug);
    QCOMPARE(log.getLevel(), sak::log_level::debug);
}

void LoggerTests::levelFiltering_belowMinNotWritten()
{
    auto& log = sak::logger::instance();
    log.setLevel(sak::log_level::error);

    log.log(sak::log_level::debug, "FILTER_MARKER_SHOULD_NOT_APPEAR_DEBUG");
    log.log(sak::log_level::info, "FILTER_MARKER_SHOULD_NOT_APPEAR_INFO");

    std::string content = readLogContent();

    // Debug/info messages should NOT be in the file when min level is error
    QVERIFY(content.find("FILTER_MARKER_SHOULD_NOT_APPEAR_DEBUG") == std::string::npos);
    QVERIFY(content.find("FILTER_MARKER_SHOULD_NOT_APPEAR_INFO") == std::string::npos);

    // Restore level for subsequent tests
    log.setLevel(sak::log_level::debug);
}

void LoggerTests::levelFiltering_atMinWritten()
{
    auto& log = sak::logger::instance();
    log.setLevel(sak::log_level::warning);

    log.log(sak::log_level::warning, "AT_MIN_MARKER_WARNING_VISIBLE");
    log.log(sak::log_level::error, "AT_MIN_MARKER_ERROR_VISIBLE");

    std::string content = readLogContent();
    QVERIFY(content.find("AT_MIN_MARKER_WARNING_VISIBLE") != std::string::npos);
    QVERIFY(content.find("AT_MIN_MARKER_ERROR_VISIBLE") != std::string::npos);

    log.setLevel(sak::log_level::debug);
}

// ============================================================================
// Log Output
// ============================================================================

void LoggerTests::log_writesToFile()
{
    auto& log = sak::logger::instance();
    log.log(sak::log_level::info, "WRITE_TEST_ENTRY_UNIQUE_98765");

    std::string content = readLogContent();
    QVERIFY(content.find("WRITE_TEST_ENTRY_UNIQUE_98765") != std::string::npos);
}

void LoggerTests::log_multipleMessages()
{
    auto& log = sak::logger::instance();

    for (int i = 0; i < 10; ++i) {
        log.log(sak::log_level::info, "MULTI_MSG_MARKER_ZZZZ");
    }

    std::string content = readLogContent();

    int count = 0;
    std::string::size_type pos = 0;
    while ((pos = content.find("MULTI_MSG_MARKER_ZZZZ", pos)) != std::string::npos) {
        ++count;
        pos += 21;
    }
    QCOMPARE(count, 10);
}

// ============================================================================
// Console Output
// ============================================================================

void LoggerTests::consoleOutput_toggle()
{
    auto& log = sak::logger::instance();
    log.setConsoleOutput(true);
    log.setConsoleOutput(false);
    QVERIFY(true); // No crash
}

// ============================================================================
// Flush
// ============================================================================

void LoggerTests::flush_writesData()
{
    auto& log = sak::logger::instance();
    log.log(sak::log_level::info, "FLUSH_VERIFY_MARKER");
    log.flush();

    auto logFile = log.getLogFile();
    QVERIFY(std::filesystem::file_size(logFile) > 0);
}

// ============================================================================
// getLogFile
// ============================================================================

void LoggerTests::getLogFile_afterInit()
{
    auto& log = sak::logger::instance();
    auto logFile = log.getLogFile();
    QVERIFY(!logFile.empty());
    QVERIFY(logFile.filename().string().find("test_logger") != std::string::npos);
}

// ============================================================================
// isInitialized
// ============================================================================

void LoggerTests::isInitialized_afterInit()
{
    auto& log = sak::logger::instance();
    QVERIFY(log.isInitialized());
}

// ============================================================================
// Thread Safety
// ============================================================================

void LoggerTests::concurrentWrites_noCorruption()
{
    auto& log = sak::logger::instance();

    constexpr int THREADS = 4;
    constexpr int MSGS_PER_THREAD = 25;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&log]() {
            for (int m = 0; m < MSGS_PER_THREAD; ++m) {
                log.log(sak::log_level::info, "CONCURRENT_WRITE_MARKER");
            }
        });
    }

    for (auto& t : threads)
        t.join();

    std::string content = readLogContent();

    int count = 0;
    std::string::size_type pos = 0;
    while ((pos = content.find("CONCURRENT_WRITE_MARKER", pos)) != std::string::npos) {
        ++count;
        pos += 23;
    }
    QCOMPARE(count, THREADS * MSGS_PER_THREAD);
}

// ============================================================================
// to_string
// ============================================================================

void LoggerTests::logLevel_toString()
{
    QCOMPARE(sak::to_string(sak::log_level::debug), std::string_view("DEBUG"));
    QCOMPARE(sak::to_string(sak::log_level::info), std::string_view("INFO"));
    QCOMPARE(sak::to_string(sak::log_level::warning), std::string_view("WARNING"));
    QCOMPARE(sak::to_string(sak::log_level::error), std::string_view("ERROR"));
    QCOMPARE(sak::to_string(sak::log_level::critical), std::string_view("CRITICAL"));
}

QTEST_GUILESS_MAIN(LoggerTests)
#include "test_logger.moc"
