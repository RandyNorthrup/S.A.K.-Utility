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
    void log_escapesControlBytesInMessage();
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

    // Class (F): existence is not content. The only operator-facing statement of
    // WHERE entries are going is the record initialize() writes into the stream
    // it opened ("Logger initialized: <path>"), and it must name THAT file --
    // announcing the directory, a stale member, or nothing at all still leaves a
    // file that exists under the right parent. Both sides here go through
    // path::string(), so the comparison is byte-exact on any host.
    const std::string bootContent = readLogContent();
    QVERIFY2(bootContent.find("Logger initialized: " + logFile.string()) != std::string::npos,
             "initialize() must announce, inside the file it opened, that exact path");
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

    // Class (A): a refusal reported through !has_value() alone is satisfied by ANY
    // of initialize()'s distinct failure modes -- not_a_directory, permission_denied,
    // unknown_error from ensureLogDirectory, or the later stream-open write_error --
    // so the guard this fixture is aimed at can be dead and the test stays green.
    // Pin the REASON with a deterministic, machine-invariant fixture: a path that IS
    // a regular file must be refused as not_a_directory.
    QTemporaryDir occupied;
    QVERIFY(occupied.isValid());
    const std::filesystem::path fileAsDir = std::filesystem::path(occupied.path().toStdWString()) /
                                            "iam_a_file";
    {
        std::ofstream f(fileAsDir);
        f << "x";
    }
    const auto fileResult = log.initialize(fileAsDir, "reinit");
    QVERIFY(!fileResult.has_value());
    QCOMPARE(fileResult.error(), sak::error_code::not_a_directory);
    // Original initialization should still be active
    QVERIFY(log.isInitialized());
    // Fail closed: a rejected initialize() must leave the sink where it was,
    // not repoint getLogFile() at a file it never opened.
    QVERIFY2(log.getLogFile() == originalFile, "a failed re-init must not repoint the log sink");

    // Class (N): "the sink survived the refusal" has two independent sources --
    // the reported path (m_log_file, what getLogFile() returns) and the stream
    // that writes actually go to (m_file_stream). The check above only sees the
    // first. Production itself documents the divergence (logger.cpp:116-121), and
    // isInitialized() cannot see it either: the ensureLogDirectory early return
    // (logger.cpp:105-107) never clears m_initialized. Observe the STREAM: a
    // rejected initialize() that closed it would leave both checks above green
    // while writeEntryToFile silently drops every later entry (logger.cpp:229-231).
    log.log(sak::log_level::error, "REINIT_REFUSED_SINK_STILL_LIVE_4417");
    log.flush();
    std::ifstream survivor(originalFile);
    const std::string survivorContent((std::istreambuf_iterator<char>(survivor)),
                                      std::istreambuf_iterator<char>());
    QCOMPARE(countOccurrences(survivorContent, "REINIT_REFUSED_SINK_STILL_LIVE_4417"), 1);
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

    // Two residual gaps. (1) Only levels FAR below the threshold are proved
    // filtered, so an off-by-one in `level < m_min_level` that lets the level
    // IMMEDIATELY below through stays green. (2) "marker absent" is equally
    // satisfied by a logger that writes nothing at all, so the same window needs
    // a live-sink control at the threshold itself.
    log.log(sak::log_level::warning, "FILTER_MARKER_SHOULD_NOT_APPEAR_WARNING");
    log.log(sak::log_level::error, "FILTER_CONTROL_ERROR_MUST_APPEAR_5512");
    const std::string boundary = readLogContent();
    QVERIFY(boundary.find("FILTER_MARKER_SHOULD_NOT_APPEAR_WARNING") == std::string::npos);
    QCOMPARE(countOccurrences(boundary, "FILTER_CONTROL_ERROR_MUST_APPEAR_5512"), 1);

    // A second, INDEPENDENT copy of this filter lives in the arg-formatted
    // template overload (include/sak/logger.h:282); the calls above all bind the
    // no-args overload (src/core/logger.cpp:411), so deleting the template's
    // early exit leaks below-threshold formatted entries and stays green. A
    // three-argument call binds the template, so drive that copy at the same
    // boundary, with its own live-sink control.
    log.log(sak::log_level::warning, "FILTER_MARKER_FMT_SHOULD_NOT_APPEAR_{}", 5513);
    log.log(sak::log_level::error, "FILTER_CONTROL_FMT_MUST_APPEAR_{}", 5514);
    const std::string formatted = readLogContent();
    QVERIFY(formatted.find("FILTER_MARKER_FMT_SHOULD_NOT_APPEAR_5513") == std::string::npos);
    QCOMPARE(countOccurrences(formatted, "FILTER_CONTROL_FMT_MUST_APPEAR_5514"), 1);

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

    // Marker presence leaves the record SHAPE unpinned. Production formats every
    // entry as "[<ts>] [<LEVEL>] [<file>:<line>:<function>] <message>\n", so a
    // build that dropped the level tag or the source location -- or stamped one
    // hardcoded level on every entry -- still contains this marker. Log a second
    // entry at a DIFFERENT severity and pin the header of each record.
    log.log(sak::log_level::critical, "WRITE_TEST_ENTRY_CRITICAL_98766");
    const std::string shaped = readLogContent();

    const auto infoAt = shaped.find("WRITE_TEST_ENTRY_UNIQUE_98765");
    QVERIFY(infoAt != std::string::npos);
    const auto infoNl = shaped.rfind('\n', infoAt);
    const std::string::size_type infoBegin =
        (infoNl == std::string::npos) ? std::string::size_type{0} : infoNl + 1;
    const std::string infoRecord = shaped.substr(infoBegin, infoAt - infoBegin);
    QVERIFY2(infoRecord.starts_with("["), "an entry must begin its own line");

    // Class (F): the needle below deliberately begins at "] ", so the timestamp
    // field is proved only to EXIST. getTimestamp()'s own catch arm returns the
    // literal "TIMESTAMP_ERROR" (logger.cpp:336), and dropping the date from
    // std::format("{:%Y-%m-%d %H:%M:%S}", now) (logger.cpp:332) satisfies every
    // other assertion in this file. Pin the fixed leading 19 characters
    // positionally -- the sub-second tail is platform width (MSVC system_clock
    // ticks at 100ns, so 7 digits), but "YYYY-MM-DD HH:MM:SS" is not.
    const auto infoStampEnd = infoRecord.find(']');
    QVERIFY(infoStampEnd != std::string::npos);
    const std::string infoStamp = infoRecord.substr(1, infoStampEnd - 1);
    QVERIFY2(infoStamp.size() >= 19, "a record timestamp must carry a full date and time");
    for (std::string::size_type i = 0; i < 19; ++i) {
        const char c = infoStamp[i];
        const bool stampShaped = (i == 4 || i == 7)     ? (c == '-')
                                 : (i == 10)            ? (c == ' ')
                                 : (i == 13 || i == 16) ? (c == ':')
                                                        : (c >= '0' && c <= '9');
        QVERIFY2(stampShaped, "a record timestamp must render as YYYY-MM-DD HH:MM:SS");
    }

    QCOMPARE(countOccurrences(infoRecord, "] [INFO] [test_logger.cpp:"), 1);

    const auto critAt = shaped.find("WRITE_TEST_ENTRY_CRITICAL_98766");
    QVERIFY(critAt != std::string::npos);
    const auto critNl = shaped.rfind('\n', critAt);
    const std::string::size_type critBegin =
        (critNl == std::string::npos) ? std::string::size_type{0} : critNl + 1;
    const std::string critRecord = shaped.substr(critBegin, critAt - critBegin);
    QCOMPARE(countOccurrences(critRecord, "] [CRITICAL] [test_logger.cpp:"), 1);
}

void LoggerTests::log_escapesControlBytesInMessage() {
    auto& log = sak::logger::instance();

    // The record shape pinned above is only enforceable because production escapes control
    // bytes before formatting (sanitizeLogText, src/core/logger.cpp:54-66, called at :200 --
    // CWE-117 log forging). No fixture in this file contains a byte below 0x20, so that call
    // can be deleted and every assertion here still passes. Hand log() a message carrying a
    // raw newline plus a well-formed forged header and require it to stay INSIDE one record.
    log.log(sak::log_level::info,
            "INJECT_TEST_ENTRY_98767\n[2000-01-01 00:00:00.000] [CRITICAL] [evil.cpp:1:forge] "
            "FORGED_TEST_ENTRY_98768");
    const std::string injected = readLogContent();

    const auto injAt = injected.find("INJECT_TEST_ENTRY_98767");
    QVERIFY(injAt != std::string::npos);
    const auto injNl = injected.rfind('\n', injAt);
    const std::string::size_type injBegin = (injNl == std::string::npos) ? std::string::size_type{0}
                                                                         : injNl + 1;
    const auto injEnd = injected.find('\n', injAt);
    QVERIFY(injEnd != std::string::npos);
    const std::string injRecord = injected.substr(injBegin, injEnd - injBegin);
    QVERIFY2(injRecord.find("INJECT_TEST_ENTRY_98767\\n[2000-01-01") != std::string::npos,
             "a raw newline must be escaped in place, not emitted as a record break");
    QVERIFY2(injRecord.find("FORGED_TEST_ENTRY_98768") != std::string::npos,
             "injected text must stay inside the single record it was logged in");
    QCOMPARE(countOccurrences(injRecord, "] [INFO] [test_logger.cpp:"), 1);
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
    // Pin the deterministic name shape rather than a mere non-empty / substring check.
    // Prefix + extension alone leave the timestamp -- the only varying part, and the whole
    // reason the name is unique -- unchecked, so truncating the stamp in logger.cpp:113
    // would stay green. Production emits "<prefix>_YYYY-MM-DD_HH-MM-SS[.fraction].log"
    // (logger.cpp:113-114; %S on system_clock carries a sub-second fraction here).
    const std::string kPrefix = "test_logger_";
    QVERIFY(logFile.filename().string().starts_with(kPrefix));
    QCOMPARE(logFile.extension().string(), std::string(".log"));

    const std::string stamp = logFile.stem().string().substr(kPrefix.size());
    const std::string shape = "DDDD-DD-DD_DD-DD-DD";  // D = decimal digit
    QVERIFY2(stamp.size() >= shape.size(),
             "log name must carry a full YYYY-MM-DD_HH-MM-SS stamp after the prefix");
    for (std::string::size_type i = 0; i < shape.size(); ++i) {
        const bool ok = (shape[i] == 'D') ? (stamp[i] >= '0' && stamp[i] <= '9')
                                          : (stamp[i] == shape[i]);
        QVERIFY2(ok, "log name timestamp must be YYYY-MM-DD_HH-MM-SS");
    }
    const std::string frac = stamp.substr(shape.size());
    QVERIFY2(frac.empty() || (frac.size() > 1 && frac.front() == '.' &&
                              frac.find_first_not_of("0123456789", 1) == std::string::npos),
             "only a fractional-second field may follow the timestamp");
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

    // Class (B): "no corruption" is never actually checked -- counting markers
    // cannot see records SPLICED into one another, which is exactly what losing
    // the write lock produces. Production writes the message last, immediately
    // before the single '\n', and starts every record with '[', so each marker
    // must sit at end-of-record inside a record carrying exactly one header.
    constexpr std::string::size_type MARKER_LEN = 23;
    for (std::string::size_type p = content.find("CONCURRENT_WRITE_MARKER"); p != std::string::npos;
         p = content.find("CONCURRENT_WRITE_MARKER", p + MARKER_LEN)) {
        const auto recordEnd = p + MARKER_LEN;
        QVERIFY2(recordEnd < content.size() &&
                     (content[recordEnd] == '\n' || content[recordEnd] == '\r'),
                 "a concurrent record must terminate right after its message");
        const auto nl = content.rfind('\n', p);
        const std::string::size_type begin = (nl == std::string::npos) ? std::string::size_type{0}
                                                                       : nl + 1;
        const std::string record = content.substr(begin, recordEnd - begin);
        QVERIFY2(record.starts_with("["), "a concurrent record must begin its own line");
        QCOMPARE(countOccurrences(record, "] [INFO] [test_logger.cpp:"), 1);
        QCOMPARE(countOccurrences(record, "CONCURRENT_WRITE_MARKER"), 1);
    }
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

    // Class (G): the catalog is pinned by MEMBERSHIP only. Production depends on
    // its ORDER -- log() drops entries via `level < m_min_level`, and
    // writeEntryToFile / writeEntryToConsole force-flush and route to stderr via
    // `level >= log_level::error`. Pin the ranking so a reordering cannot keep
    // every string above correct while silently changing severity semantics.
    static_assert(sak::log_level::debug < sak::log_level::info);
    static_assert(sak::log_level::info < sak::log_level::warning);
    static_assert(sak::log_level::warning < sak::log_level::error);
    static_assert(sak::log_level::error < sak::log_level::critical);

    // The default arm is a real production path: a level outside the catalog must
    // be labelled UNKNOWN, not mislabelled as a genuine severity.
    QCOMPARE(sak::to_string(static_cast<sak::log_level>(7)), std::string_view("UNKNOWN"));
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
