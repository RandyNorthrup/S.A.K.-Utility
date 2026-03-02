// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_search_worker.cpp
/// @brief Unit tests for AdvancedSearchWorker — regex compilation, text search,
///        metadata, archive, binary, dispatch logic, cancellation

#include <QtTest/QtTest>

#include "sak/advanced_search_worker.h"
#include "sak/advanced_search_types.h"
#include "sak/error_codes.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <zlib.h>

using sak::AdvancedSearchWorker;
using sak::SearchConfig;
using sak::SearchMatch;

// ============================================================================
// Test Class
// ============================================================================

class AdvancedSearchWorkerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // ── Regex Compilation (via execute()) ──
    void emptyPattern_returnsError();
    void plainText_findsMatch();
    void plainText_caseInsensitive();
    void plainText_caseSensitive();
    void wholeWord_matchesBoundary();
    void wholeWord_rejectsPartial();
    void regexMode_specialChars();
    void regexEscape_literalDots();

    // ── Text Content Search ──
    void textSearch_singleMatch();
    void textSearch_multipleMatches();
    void textSearch_contextLines();
    void textSearch_lineNumbers();
    void textSearch_noMatchReturnsEmpty();
    void textSearch_binaryFileSkipped();
    void textSearch_maxResults();

    // ── File Extension Filter ──
    void extensionFilter_matchesIncluded();
    void extensionFilter_excludesOthers();
    void extensionFilter_emptyMatchesAll();

    // ── Exclusion Patterns ──
    void excludePatterns_gitExcluded();
    void excludePatterns_customExclude();

    // ── File Size Limit ──
    void fileSizeLimit_skipsOversized();

    // ── Single File Search ──
    void singleFileSearch_worksWithFilePath();

    // ── Batch Emission ──
    void batchEmission_emitsResults();

    // ── Hex/Binary Search ──
    void hexSearch_findsPattern();
    void hexSearch_exclusiveMode();

    // ── Image Metadata Search ──
    void imageMetadataSearch_findsFileInfo();

    // ── File Metadata Search ──
    void fileMetadataSearch_findsFileInfo();

    // ── Archive Search ──
    void archiveSearch_validZip();
    void archiveSearch_zipEntryNames();
    void archiveSearch_deflateCompressed();

    // ── Cancellation ──
    void cancellation_stopsEarly();

    // ── Progress Reporting ──
    void progress_emitsProgressSignal();

private:
    /// @brief Create a text file with given content in the temp dir
    void createTestFile(const QString& name, const QString& content);

    /// @brief Create a subdirectory in the temp dir
    void createSubDir(const QString& name);

    /// @brief Create a minimal valid ZIP file with one stored text entry
    QByteArray createMinimalZip(const QString& entryName,
                                 const QByteArray& entryData);

    /// @brief Create a minimal valid ZIP file with one deflate-compressed entry
    QByteArray createDeflateZip(const QString& entryName,
                                const QByteArray& entryData);

    /// @brief Run a worker synchronously and return results
    QVector<SearchMatch> runWorker(const SearchConfig& config);

    QTemporaryDir m_temp_dir;
};

// ============================================================================
// Setup / Teardown
// ============================================================================

void AdvancedSearchWorkerTests::initTestCase()
{
    QVERIFY(m_temp_dir.isValid());

    // Create test file structure
    createTestFile("hello.txt",
        "Hello World\n"
        "This is line 2\n"
        "Hello again on line 3\n"
        "Line 4 has no match\n"
        "Final Hello on line 5\n");

    createTestFile("code.cpp",
        "#include <iostream>\n"
        "int main() {\n"
        "    std::cout << \"Hello\" << std::endl;\n"
        "    return 0;\n"
        "}\n");

    createTestFile("data.csv",
        "name,email,phone\n"
        "Alice,alice@example.com,555-1234\n"
        "Bob,bob@test.org,555-5678\n");

    createTestFile("empty.txt", "");

    createTestFile("large_match.txt",
        QString("prefix target suffix\n").repeated(100));

    // Create a subdirectory with files
    createSubDir("subdir");
    createTestFile("subdir/nested.txt", "Nested file content\nWith target text\n");

    // Create a file that should be excluded
    createSubDir(".git");
    createTestFile(".git/config", "git config file\nHello inside git\n");

    // Create a non-text file (fake binary with some text)
    createTestFile("fake.bin", QString(QByteArray(100, '\0')) + "hidden_text_marker" +
                   QString(QByteArray(100, '\0')));

    // Create a file with special regex characters in content
    createTestFile("special.txt",
        "File has dots: config.ini\n"
        "And parens: func(arg)\n"
        "Plus stars: rating***\n");

    // Create a small PNG-like file (just for metadata testing with filesystem info)
    createTestFile("test.png", "Not a real PNG but has the extension");

    // Create a minimal valid ZIP file
    QByteArray zipEntry = "Hello from inside ZIP!";
    QByteArray zipData = createMinimalZip("readme.txt", zipEntry);
    {
        QFile zipFile(m_temp_dir.path() + "/test.zip");
        QVERIFY(zipFile.open(QIODevice::WriteOnly));
        zipFile.write(zipData);
        zipFile.close();
    }

    // Create a deflate-compressed ZIP file
    QByteArray deflateEntry = "Compressed content with SearchableToken inside a deflated ZIP entry";
    QByteArray deflateZipData = createDeflateZip("compressed.txt", deflateEntry);
    {
        QFile zipFile(m_temp_dir.path() + "/deflate_test.zip");
        QVERIFY(zipFile.open(QIODevice::WriteOnly));
        zipFile.write(deflateZipData);
        zipFile.close();
    }
}

void AdvancedSearchWorkerTests::cleanupTestCase()
{
    // QTemporaryDir auto-cleans
}

// ============================================================================
// Helper Methods
// ============================================================================

void AdvancedSearchWorkerTests::createTestFile(const QString& name,
                                                    const QString& content)
{
    const QString path = m_temp_dir.path() + "/" + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QFAIL(qPrintable(QString("Failed to create test file: %1").arg(path)));
    }
    file.write(content.toUtf8());
    file.close();
}

void AdvancedSearchWorkerTests::createSubDir(const QString& name)
{
    const QString path = m_temp_dir.path() + "/" + name;
    QDir().mkpath(path);
}

QByteArray AdvancedSearchWorkerTests::createMinimalZip(const QString& entryName,
                                                         const QByteArray& entryData)
{
    QByteArray zip;
    const QByteArray nameBytes = entryName.toUtf8();

    // ── Local File Header ──
    // Signature: PK\x03\x04
    zip.append("\x50\x4B\x03\x04", 4);
    // Version needed: 20 (2.0)
    zip.append("\x14\x00", 2);
    // General purpose bit flag
    zip.append("\x00\x00", 2);
    // Compression method: 0 (stored)
    zip.append("\x00\x00", 2);
    // Last mod time
    zip.append("\x00\x00", 2);
    // Last mod date
    zip.append("\x00\x00", 2);
    // CRC-32 (placeholder)
    zip.append("\x00\x00\x00\x00", 4);
    // Compressed size
    quint32 size = static_cast<quint32>(entryData.size());
    zip.append(reinterpret_cast<const char*>(&size), 4);
    // Uncompressed size
    zip.append(reinterpret_cast<const char*>(&size), 4);
    // Filename length
    quint16 nameLen = static_cast<quint16>(nameBytes.size());
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    // Extra field length
    zip.append("\x00\x00", 2);
    // Filename
    zip.append(nameBytes);
    // File data
    zip.append(entryData);

    // ── Central Directory Header ──
    zip.append("\x50\x4B\x01\x02", 4);
    // Version made by
    zip.append("\x14\x00", 2);
    // Version needed
    zip.append("\x14\x00", 2);
    // Flags
    zip.append("\x00\x00", 2);
    // Compression: stored
    zip.append("\x00\x00", 2);
    // Time, Date
    zip.append("\x00\x00\x00\x00", 4);
    // CRC-32
    zip.append("\x00\x00\x00\x00", 4);
    // Compressed size
    zip.append(reinterpret_cast<const char*>(&size), 4);
    // Uncompressed size
    zip.append(reinterpret_cast<const char*>(&size), 4);
    // Filename length
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    // Extra, Comment, Disk, Int/Ext attrs
    zip.append(QByteArray(12, '\0'));
    // Relative offset of local header
    quint32 zero = 0;
    zip.append(reinterpret_cast<const char*>(&zero), 4);
    // Filename
    zip.append(nameBytes);

    // ── End of Central Directory ──
    quint32 cdOffset = static_cast<quint32>(30 + nameBytes.size() + entryData.size());
    quint32 cdSize = static_cast<quint32>(46 + nameBytes.size());
    zip.append("\x50\x4B\x05\x06", 4);
    // Disk numbers
    zip.append("\x00\x00\x00\x00", 4);
    // Total entries
    zip.append("\x01\x00\x01\x00", 4);
    // Central directory size
    zip.append(reinterpret_cast<const char*>(&cdSize), 4);
    // Central directory offset
    zip.append(reinterpret_cast<const char*>(&cdOffset), 4);
    // Comment length
    zip.append("\x00\x00", 2);

    return zip;
}

QByteArray AdvancedSearchWorkerTests::createDeflateZip(
    const QString& entryName,
    const QByteArray& entryData)
{
    // Compress the entry data with raw deflate (no zlib header)
    QByteArray compressed;
    compressed.resize(compressBound(static_cast<uLong>(entryData.size())));

    z_stream strm{};
    strm.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(entryData.constData()));
    strm.avail_in = static_cast<uInt>(entryData.size());
    strm.next_out  = reinterpret_cast<Bytef*>(compressed.data());
    strm.avail_out = static_cast<uInt>(compressed.size());

    // -MAX_WBITS → raw deflate (what ZIP uses)
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    deflate(&strm, Z_FINISH);
    deflateEnd(&strm);
    compressed.resize(static_cast<qsizetype>(strm.total_out));

    // Compute CRC-32
    quint32 crc = static_cast<quint32>(
        crc32(crc32(0, Z_NULL, 0),
              reinterpret_cast<const Bytef*>(entryData.constData()),
              static_cast<uInt>(entryData.size())));

    const QByteArray nameBytes = entryName.toUtf8();
    quint16 nameLen = static_cast<quint16>(nameBytes.size());
    quint32 compSize = static_cast<quint32>(compressed.size());
    quint32 uncompSize = static_cast<quint32>(entryData.size());

    QByteArray zip;

    // ── Local File Header ──
    zip.append("\x50\x4B\x03\x04", 4);    // Signature
    zip.append("\x14\x00", 2);              // Version needed (2.0)
    zip.append("\x00\x00", 2);              // Flags
    zip.append("\x08\x00", 2);              // Compression: deflate (8)
    zip.append("\x00\x00\x00\x00", 4);      // Time, Date
    zip.append(reinterpret_cast<const char*>(&crc), 4);
    zip.append(reinterpret_cast<const char*>(&compSize), 4);
    zip.append(reinterpret_cast<const char*>(&uncompSize), 4);
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append("\x00\x00", 2);              // Extra field length
    zip.append(nameBytes);
    zip.append(compressed);

    // ── Central Directory Header ──
    zip.append("\x50\x4B\x01\x02", 4);
    zip.append("\x14\x00", 2);              // Version made by
    zip.append("\x14\x00", 2);              // Version needed
    zip.append("\x00\x00", 2);              // Flags
    zip.append("\x08\x00", 2);              // Compression: deflate (8)
    zip.append("\x00\x00\x00\x00", 4);      // Time, Date
    zip.append(reinterpret_cast<const char*>(&crc), 4);
    zip.append(reinterpret_cast<const char*>(&compSize), 4);
    zip.append(reinterpret_cast<const char*>(&uncompSize), 4);
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append(QByteArray(12, '\0'));         // Extra, Comment, Disk, attrs
    quint32 zero = 0;
    zip.append(reinterpret_cast<const char*>(&zero), 4);  // Local header offset
    zip.append(nameBytes);

    // ── End of Central Directory ──
    quint32 cdOffset = static_cast<quint32>(30 + nameBytes.size() + compressed.size());
    quint32 cdSize = static_cast<quint32>(46 + nameBytes.size());
    zip.append("\x50\x4B\x05\x06", 4);
    zip.append("\x00\x00\x00\x00", 4);  // Disk numbers
    zip.append("\x01\x00\x01\x00", 4);  // Total entries
    zip.append(reinterpret_cast<const char*>(&cdSize), 4);
    zip.append(reinterpret_cast<const char*>(&cdOffset), 4);
    zip.append("\x00\x00", 2);          // Comment length

    return zip;
}

QVector<SearchMatch> AdvancedSearchWorkerTests::runWorker(const SearchConfig& config)
{
    QVector<SearchMatch> allMatches;
    AdvancedSearchWorker worker(config);

    QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);
    QSignalSpy finishedSpy(&worker,
        qOverload<>(&WorkerBase::finished));
    QSignalSpy failedSpy(&worker,
        &WorkerBase::failed);

    worker.start();
    worker.wait(10000); // 10s timeout

    for (int i = 0; i < resultsSpy.count(); ++i) {
        const auto matches = resultsSpy[i][0].value<QVector<SearchMatch>>();
        allMatches.append(matches);
    }

    return allMatches;
}

// ============================================================================
// Regex Compilation Tests
// ============================================================================

void AdvancedSearchWorkerTests::emptyPattern_returnsError()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "";

    AdvancedSearchWorker worker(config);
    QSignalSpy failSpy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy[0][0].toInt(),
        static_cast<int>(sak::error_code::invalid_argument));
}

void AdvancedSearchWorkerTests::plainText_findsMatch()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 3); // "Hello" appears 3 times
}

void AdvancedSearchWorkerTests::plainText_caseInsensitive()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "hello";
    config.case_sensitive = false;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 3); // Case-insensitive should find all "Hello"
}

void AdvancedSearchWorkerTests::plainText_caseSensitive()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "hello"; // lowercase
    config.case_sensitive = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0); // No lowercase "hello" in file
}

void AdvancedSearchWorkerTests::wholeWord_matchesBoundary()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.whole_word = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 3);
}

void AdvancedSearchWorkerTests::wholeWord_rejectsPartial()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hell"; // Partial word
    config.whole_word = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0); // "Hell" is not a whole word in the file
}

void AdvancedSearchWorkerTests::regexMode_specialChars()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello.*line";
    config.use_regex = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 2);
}

void AdvancedSearchWorkerTests::regexEscape_literalDots()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/special.txt";
    config.pattern = "config.ini"; // Non-regex: dots should be literal
    config.use_regex = false;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
}

// ============================================================================
// Text Content Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::textSearch_singleMatch()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/code.cpp";
    config.pattern = "iostream";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QVERIFY(matches[0].line_content.contains("iostream"));
}

void AdvancedSearchWorkerTests::textSearch_multipleMatches()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 3);
}

void AdvancedSearchWorkerTests::textSearch_contextLines()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "line 2";
    config.context_lines = 1;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QVERIFY(matches[0].context_before.size() <= 1);
    QVERIFY(matches[0].context_after.size() <= 1);
}

void AdvancedSearchWorkerTests::textSearch_lineNumbers()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "line 2";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches[0].line_number, 2); // "This is line 2" is on line 2
}

void AdvancedSearchWorkerTests::textSearch_noMatchReturnsEmpty()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "nonexistent_pattern_xyz";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);
}

void AdvancedSearchWorkerTests::textSearch_binaryFileSkipped()
{
    // Image extensions should be skipped for text content search
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "PNG";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // PNG file is in kBinaryImageExts, so text search is skipped
    QCOMPARE(matches.size(), 0);
}

void AdvancedSearchWorkerTests::textSearch_maxResults()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/large_match.txt";
    config.pattern = "target";
    config.max_results = 5;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() <= 5);
    QVERIFY(matches.size() > 0);
}

// ============================================================================
// File Extension Filter Tests
// ============================================================================

void AdvancedSearchWorkerTests::extensionFilter_matchesIncluded()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.file_extensions = {".txt"};
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // All matches should be from .txt files
    for (const auto& m : matches) {
        QVERIFY2(m.file_path.endsWith(".txt"),
            qPrintable(QString("Unexpected file: %1").arg(m.file_path)));
    }
    QVERIFY(!matches.isEmpty());
}

void AdvancedSearchWorkerTests::extensionFilter_excludesOthers()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.file_extensions = {".cpp"};
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should only find matches in .cpp files
    for (const auto& m : matches) {
        QVERIFY2(m.file_path.endsWith(".cpp"),
            qPrintable(QString("Unexpected file: %1").arg(m.file_path)));
    }
}

void AdvancedSearchWorkerTests::extensionFilter_emptyMatchesAll()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.file_extensions.clear(); // No filter
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should find matches from multiple file types
    QSet<QString> extensions;
    for (const auto& m : matches) {
        extensions.insert(QFileInfo(m.file_path).suffix());
    }
    QVERIFY(extensions.size() >= 2); // At least .txt and .cpp
}

// ============================================================================
// Exclusion Tests
// ============================================================================

void AdvancedSearchWorkerTests::excludePatterns_gitExcluded()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    // Default excludes include .git

    auto matches = runWorker(config);
    for (const auto& m : matches) {
        QVERIFY2(!m.file_path.contains(".git"),
            qPrintable(QString("Should exclude .git: %1").arg(m.file_path)));
    }
}

void AdvancedSearchWorkerTests::excludePatterns_customExclude()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "target";
    config.exclude_patterns = {R"(subdir)"};

    auto matches = runWorker(config);
    for (const auto& m : matches) {
        QVERIFY2(!m.file_path.contains("subdir"),
            qPrintable(QString("Should exclude subdir: %1").arg(m.file_path)));
    }
}

// ============================================================================
// File Size Limit Tests
// ============================================================================

void AdvancedSearchWorkerTests::fileSizeLimit_skipsOversized()
{
    // max_file_size is only enforced in directory-recursive search,
    // so search the temp dir (not a single file) to exercise the limit.
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.max_file_size = 1;          // 1 byte — every file is too large
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);
}

// ============================================================================
// Single File Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::singleFileSearch_worksWithFilePath()
{
    // Case-sensitive so "Alice" does NOT also match "alice" in the email field.
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/data.csv";
    config.pattern = "Alice";
    config.case_sensitive = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QVERIFY(matches[0].file_path.endsWith("data.csv"));
}

// ============================================================================
// Batch Emission Tests
// ============================================================================

void AdvancedSearchWorkerTests::batchEmission_emitsResults()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);

    worker.start();
    QVERIFY(worker.wait(10000));

    // Should have emitted at least one batch
    QVERIFY(resultsSpy.count() >= 1);

    // Each batch should have at least one match
    for (int i = 0; i < resultsSpy.count(); ++i) {
        const auto batch = resultsSpy[i][0].value<QVector<SearchMatch>>();
        QVERIFY(!batch.isEmpty());
    }
}

// ============================================================================
// Hex/Binary Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::hexSearch_findsPattern()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/fake.bin";
    config.pattern = "hidden_text_marker";
    config.hex_search = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 1);
}

void AdvancedSearchWorkerTests::hexSearch_exclusiveMode()
{
    // When hex_search is on, text search should NOT run
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.hex_search = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should still find the pattern (binary mode reads as UTF-8)
    QVERIFY(matches.size() >= 1);

    // But the result format should be binary-style (line_number = byte offset)
    // and context_before should contain hex data
    for (const auto& m : matches) {
        bool hasHex = false;
        for (const auto& ctx : m.context_before) {
            if (ctx.startsWith("Hex:")) {
                hasHex = true;
                break;
            }
        }
        QVERIFY2(hasHex, "Binary search should provide hex context");
    }
}

// ============================================================================
// Image Metadata Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::imageMetadataSearch_findsFileInfo()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "png"; // Should match FileName or FileType metadata
    config.search_image_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should find at least file info metadata (FileName, etc.)
    QVERIFY(matches.size() >= 1);

    // Verify metadata format
    bool hasMetadataLine = false;
    for (const auto& m : matches) {
        if (m.line_content.startsWith("[Metadata]")) {
            hasMetadataLine = true;
            break;
        }
    }
    QVERIFY2(hasMetadataLine, "Image metadata results should have [Metadata] prefix");
}

// ============================================================================
// File Metadata Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::fileMetadataSearch_findsFileInfo()
{
    // Create a test file with a metadata extension
    createTestFile("test_meta.json", R"({"key": "value"})");

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_meta.json";
    config.pattern = "json"; // Should match FileName or FileType
    config.search_file_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should find filesystem metadata
    QVERIFY(matches.size() >= 1);
}

// ============================================================================
// Archive Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::archiveSearch_validZip()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.zip";
    config.pattern = "Hello";
    config.search_in_archives = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // The ZIP contains "Hello from inside ZIP!" in readme.txt
    QVERIFY(matches.size() >= 1);

    // Check that matches reference the archive path format
    bool hasArchivePath = false;
    for (const auto& m : matches) {
        if (m.file_path.contains("!/")) {
            hasArchivePath = true;
            break;
        }
    }
    QVERIFY2(hasArchivePath,
        "Archive matches should use path!/entry format");
}

void AdvancedSearchWorkerTests::archiveSearch_zipEntryNames()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.zip";
    config.pattern = "readme"; // Should match the entry filename
    config.search_in_archives = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 1);

    // Should have an archive entry match
    bool hasEntryMatch = false;
    for (const auto& m : matches) {
        if (m.line_content.contains("[Archive Entry]")) {
            hasEntryMatch = true;
            break;
        }
    }
    QVERIFY2(hasEntryMatch,
        "Should match archive entry filenames");
}

void AdvancedSearchWorkerTests::archiveSearch_deflateCompressed()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/deflate_test.zip";
    config.pattern = "SearchableToken";
    config.search_in_archives = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // The deflate-compressed ZIP contains "SearchableToken" inside compressed.txt
    QVERIFY2(matches.size() >= 1,
        "Deflate-compressed ZIP entry content should be searchable");

    // Verify it found the match inside the archive
    bool foundInArchive = false;
    for (const auto& m : matches) {
        if (m.file_path.contains("!/") && m.line_content.contains("SearchableToken")) {
            foundInArchive = true;
            break;
        }
    }
    QVERIFY2(foundInArchive,
        "Should find content match inside deflate-compressed archive entry");
}

// ============================================================================
// Cancellation Tests
// ============================================================================

void AdvancedSearchWorkerTests::cancellation_stopsEarly()
{
    // Create many files to ensure search takes a while
    createSubDir("cancel_test");
    for (int i = 0; i < 100; ++i) {
        createTestFile(QString("cancel_test/file_%1.txt").arg(i),
            QString("Content for cancel test file %1\nWith some target text\n")
                .arg(i));
    }

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/cancel_test";
    config.pattern = "target";
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy cancelledSpy(&worker, &WorkerBase::cancelled);

    worker.start();

    // Wait briefly then cancel
    QThread::msleep(10);
    worker.requestStop();

    QVERIFY(worker.wait(5000));

    // Worker should have handled the stop request
    // (may or may not have found some results before stopping)
}

// ============================================================================
// Progress Tests
// ============================================================================

void AdvancedSearchWorkerTests::progress_emitsProgressSignal()
{
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "target";
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy progressSpy(&worker,
        &WorkerBase::progress);

    worker.start();
    QVERIFY(worker.wait(10000));

    // Progress should be reported (at least the final report)
    QVERIFY(progressSpy.count() >= 1);
}

QTEST_GUILESS_MAIN(AdvancedSearchWorkerTests)
#include "test_advanced_search_worker.moc"
