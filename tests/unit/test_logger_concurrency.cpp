// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_logger_concurrency.cpp
/// @brief The logger's lock: entries written from several threads at once never splice.
///
/// It writes through an in-memory sink, so it needs no file system: the WSL ThreadSanitizer row
/// (tests/tsan, scripts/run_tsan_wsl.ps1) builds this file with src/core/logger.cpp alone, and the
/// Windows suite runs it as an ordinary test. The production sink is FileLogSink.

#include "sak/logger.h"

#include <QtTest/QtTest>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

/// Every entry appended to one string the test reads afterwards.
class MemorySink final : public sak::LogSink {
public:
    explicit MemorySink(std::shared_ptr<std::string> text) : m_text(std::move(text)) {}
    [[nodiscard]] bool write(std::string_view entry) override {
        m_text->append(entry);
        return true;
    }
    [[nodiscard]] bool rotate() override { return true; }
    [[nodiscard]] std::filesystem::path location() const override { return "memory"; }

private:
    std::shared_ptr<std::string> m_text;
};

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int count = 0;
    for (std::string::size_type at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

}  // namespace

class LoggerConcurrencyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void concurrentWrites_noCorruption();
};

void LoggerConcurrencyTests::concurrentWrites_noCorruption() {
    auto& log = sak::logger::instance();
    log.setConsoleOutput(false);
    const auto text = std::make_shared<std::string>();
    QVERIFY(log.initialize(std::make_unique<MemorySink>(text)).has_value());

    constexpr int kThreads = 4;
    constexpr int kMessagesPerThread = 25;
    const std::string marker = "CONCURRENT_WRITE_MARKER";
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log]() {
            for (int m = 0; m < kMessagesPerThread; ++m) {
                log.log(sak::log_level::info, "CONCURRENT_WRITE_MARKER");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    const std::string content = *text;
    QCOMPARE(countOccurrences(content, marker), kThreads * kMessagesPerThread);
    // Counting markers cannot see records SPLICED into one another, which is exactly what losing
    // the write lock produces. Each record starts with '[' and ends with its message and '\n',
    // so every marker must close a record that carries exactly one header.
    for (std::string::size_type p = content.find(marker); p != std::string::npos;
         p = content.find(marker, p + marker.size())) {
        const auto record_end = p + marker.size();
        QVERIFY2(record_end < content.size() &&
                     (content[record_end] == '\n' || content[record_end] == '\r'),
                 "a concurrent record must terminate right after its message");
        const auto newline = content.rfind('\n', p);
        const std::string::size_type begin = newline == std::string::npos ? 0 : newline + 1;
        const std::string record = content.substr(begin, record_end - begin);
        QVERIFY2(record.starts_with("["), "a concurrent record must begin its own line");
        QCOMPARE(countOccurrences(record, "] [INFO] [test_logger_concurrency.cpp:"), 1);
        QCOMPARE(countOccurrences(record, marker), 1);
    }
}

QTEST_GUILESS_MAIN(LoggerConcurrencyTests)
#include "test_logger_concurrency.moc"
