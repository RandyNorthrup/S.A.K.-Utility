// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_logger.cpp
/// @brief Unit tests for the thread-safe logging system
///
/// IMPORTANT: sak::logger is a Meyer's singleton. Once initialized, calling
/// initialize() again fails because the internal ofstream is already open.
/// All tests share a single initialization performed in initTestCase().
/// Each test uses unique marker strings for content verification.

#include "sak/error_codes.h"
#include "sak/logger.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

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

    // Thread safety
    void concurrentWrites_noCorruption();

    // to_string for log_level
    void logLevel_toString();

    // B5-08: write-probe must not clobber and must leave no leftover.
    void ensureLogDirectory_leavesNoProbeFile();

    // Fail closed: a failed file write must not advance the rotation counter.
    void bytesToCommit_zeroOnStreamFailure();

private:
    /// @brief Reads all content from the singleton log file
    std::string readLogContent();

    /// @brief Counts non-overlapping occurrences of a marker in log content
    static int countOccurrences(const std::string& haystack, const std::string& needle);

    QTemporaryDir m_logDir;
    std::filesystem::path m_logSubDir;
};

// ============================================================================
// Setup -- single initialization for the entire test run
// ============================================================================

void LoggerTests::initTestCase() {
    QVERIFY(m_logDir.isValid());

    // Use a subdirectory that doesn't exist yet -- verifies initialize creates it
    m_logSubDir = std::filesystem::path(m_logDir.path().toStdWString()) / "log_subdir";
    QVERIFY(!std::filesystem::exists(m_logSubDir));

    auto& log = sak::logger::instance();
    auto result = log.initialize(m_logSubDir, "test_logger");
    QVERIFY2(result.has_value(), "Logger must initialize for tests to proceed");
    log.setLevel(sak::log_level::debug);
    log.setConsoleOutput(false);
}

std::string LoggerTests::readLogContent() {
    auto& log = sak::logger::instance();
    log.flush();
    auto logFile = log.getLogFile();
    std::ifstream file(logFile);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int LoggerTests::countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    std::string::size_type pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ============================================================================
// Initialization
// ============================================================================

void LoggerTests::initialize_validDir() {
    const auto& log = sak::logger::instance();
    QVERIFY(log.isInitialized());

    // The flag alone proves nothing: a successful initialize() must also have
    // opened a real file inside the directory it was handed.
    const auto logFile = log.getLogFile();
    QVERIFY2(logFile.parent_path() == m_logSubDir,
             "initialize() must place the log file in the directory it was given");
    QVERIFY(std::filesystem::exists(logFile));
}

void LoggerTests::initialize_createsDir() {
    // The subdirectory should have been created during initTestCase
    QVERIFY(std::filesystem::exists(m_logSubDir));
    QVERIFY(std::filesystem::is_directory(m_logSubDir));
}

void LoggerTests::initialize_reInitFails() {
    // Re-initializing a singleton logger should fail gracefully
    auto& log = sak::logger::instance();
    const auto originalFile = log.getLogFile();
    auto result = log.initialize(std::filesystem::path("Z:\\InvalidDrive\\NoDir"), "reinit");
    QVERIFY(!result.has_value());
    // Original initialization should still be active
    QVERIFY(log.isInitialized());
    // Fail closed: a rejected initialize() must leave the sink where it was,
    // not repoint getLogFile() at a file it never opened.
    QVERIFY2(log.getLogFile() == originalFile, "a failed re-init must not repoint the log sink");
}

// ============================================================================
// Level Control
// ============================================================================

void LoggerTests::setLevel_getLevel() {
    auto& log = sak::logger::instance();
    log.setLevel(sak::log_level::warning);
    QCOMPARE(log.getLevel(), sak::log_level::warning);

    log.setLevel(sak::log_level::debug);
    QCOMPARE(log.getLevel(), sak::log_level::debug);
}

void LoggerTests::levelFiltering_belowMinNotWritten() {
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

void LoggerTests::levelFiltering_atMinWritten() {
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

void LoggerTests::log_writesToFile() {
    auto& log = sak::logger::instance();
    log.log(sak::log_level::info, "WRITE_TEST_ENTRY_UNIQUE_98765");

    std::string content = readLogContent();
    QVERIFY(content.find("WRITE_TEST_ENTRY_UNIQUE_98765") != std::string::npos);
}

void LoggerTests::log_multipleMessages() {
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

void LoggerTests::consoleOutput_toggle() {
    auto& log = sak::logger::instance();

    // The console flag has no getter, so the file is the only observable from
    // here. What that does pin down is that console output is a SEPARATE sink:
    // enabling it must not duplicate the entry into the file, and disabling it
    // must not suppress the file write.
    log.setConsoleOutput(true);
    log.log(sak::log_level::info, "CONSOLE_ON_MARKER_7731");
    log.setConsoleOutput(false);
    log.log(sak::log_level::info, "CONSOLE_OFF_MARKER_7731");

    const std::string content = readLogContent();
    QCOMPARE(countOccurrences(content, "CONSOLE_ON_MARKER_7731"), 1);
    QCOMPARE(countOccurrences(content, "CONSOLE_OFF_MARKER_7731"), 1);
}

// ============================================================================
// Flush
// ============================================================================

void LoggerTests::flush_writesData() {
    auto& log = sak::logger::instance();
    log.log(sak::log_level::info, "FLUSH_VERIFY_MARKER");
    log.flush();

    // Read through an independent handle instead of readLogContent(), which
    // flushes again: the claim is that flush() itself made the buffered info
    // entry visible to another reader. A non-empty file proves nothing here --
    // earlier tests already filled it.
    std::ifstream file(log.getLogFile());
    const std::string content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    QVERIFY(content.find("FLUSH_VERIFY_MARKER") != std::string::npos);
}

// ============================================================================
// getLogFile
// ============================================================================

void LoggerTests::getLogFile_afterInit() {
    const auto& log = sak::logger::instance();
    auto logFile = log.getLogFile();
    // Pin the deterministic name shape rather than a mere non-empty / substring check: the log file
    // is "test_logger_<...>.log".
    QVERIFY(logFile.filename().string().starts_with("test_logger_"));
    QCOMPARE(logFile.extension().string(), std::string(".log"));
}

// ============================================================================
// Thread Safety
// ============================================================================

void LoggerTests::concurrentWrites_noCorruption() {
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

    for (auto& t : threads) {
        t.join();
    }

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

void LoggerTests::logLevel_toString() {
    QCOMPARE(sak::to_string(sak::log_level::debug), std::string_view("DEBUG"));
    QCOMPARE(sak::to_string(sak::log_level::info), std::string_view("INFO"));
    QCOMPARE(sak::to_string(sak::log_level::warning), std::string_view("WARNING"));
    QCOMPARE(sak::to_string(sak::log_level::error), std::string_view("ERROR"));
    QCOMPARE(sak::to_string(sak::log_level::critical), std::string_view("CRITICAL"));
}

// ============================================================================
// B5-08: write-permission probe hardening
// ============================================================================

void LoggerTests::ensureLogDirectory_leavesNoProbeFile() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const std::filesystem::path dir(tmp.path().toStdWString());

    // A pre-existing unrelated file must survive the probe untouched (the probe
    // uses a unique name and an exclusive create, so it never clobbers it).
    const std::filesystem::path victim = dir / "important.txt";
    {
        std::ofstream vf(victim);
        vf << "keep me";
    }

    const auto result = sak::logger::ensureLogDirectory(dir);
    QVERIFY2(result.has_value(), "a writable directory must probe successfully");

    // The victim file is intact.
    QVERIFY(std::filesystem::exists(victim));
    std::ifstream vin(victim);
    const std::string content((std::istreambuf_iterator<char>(vin)),
                              std::istreambuf_iterator<char>());
    QCOMPARE(content, std::string("keep me"));

    // The directory now contains EXACTLY the victim file: the probe created a uniquely-named file
    // and cleaned it up, leaving no ".test_write" and no ".sak_log_probe_*" behind. The old check
    // for a fixed ".test_write" name was vacuous -- the code never creates that name.
    std::vector<std::string> survivors;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        survivors.push_back(entry.path().filename().string());
    }
    QCOMPARE(survivors.size(), static_cast<size_t>(1));
    QCOMPARE(survivors.front(), std::string("important.txt"));
}

// ============================================================================
// Fail-closed write accounting
// ============================================================================

// A good write commits the full entry size; a write that left the stream in a
// bad state (full disk, revoked handle) must commit zero so m_bytes_written
// never counts bytes that were not durably written.
void LoggerTests::bytesToCommit_zeroOnStreamFailure() {
    QCOMPARE(sak::logger::bytesToCommit(true, 128), std::size_t{128});
    QCOMPARE(sak::logger::bytesToCommit(false, 128), std::size_t{0});
    QCOMPARE(sak::logger::bytesToCommit(true, 0), std::size_t{0});
}

QTEST_GUILESS_MAIN(LoggerTests)
#include "test_logger.moc"
