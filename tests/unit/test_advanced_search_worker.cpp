// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_search_worker.cpp
/// @brief Unit tests for AdvancedSearchWorker -- regex compilation, text search,
///        metadata, archive, binary, dispatch logic, cancellation

#include "sak/advanced_search_types.h"
#include "sak/advanced_search_worker.h"
#include "sak/error_codes.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <limits>

#include <zlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

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

    // -- Regex Compilation (via execute()) --
    void emptyPattern_returnsError();
    void plainText_findsMatch();
    void plainText_caseInsensitive();
    void plainText_caseSensitive();
    void wholeWord_matchesBoundary();
    void wholeWord_rejectsPartial();
    void regexMode_specialChars();
    void regexEscape_literalDots();

    // -- Text Content Search --
    void textSearch_singleMatch();
    void textSearch_multipleMatches();
    void textSearch_contextLines();
    void textSearch_lineNumbers();
    void textSearch_noMatchReturnsEmpty();
    void textSearch_binaryFileSkipped();
    void textSearch_maxResults();

    // -- File Extension Filter --
    void extensionFilter_matchesIncluded();
    void extensionFilter_excludesOthers();
    void extensionFilter_emptyMatchesAll();

    // -- Exclusion Patterns --
    void excludePatterns_gitExcluded();
    void excludePatterns_customExclude();

    // -- File Size Limit --
    void fileSizeLimit_skipsOversized();

    // -- Single File Search --
    void singleFileSearch_worksWithFilePath();

    // -- Batch Emission --
    void batchEmission_emitsResults();

    // -- Hex/Binary Search --
    void hexSearch_findsPattern();
    void hexSearch_exclusiveMode();

    // -- Image Metadata Search --
    void imageMetadataSearch_findsEmbeddedMetadata();
    void imageMetadataSearch_matchesByTagName();
    void imageMetadataSearch_matchesDimensions();
    void imageMetadataSearch_noMatchReturnsEmpty();
    void imageMetadataSearch_jpegExifExtraction();
    void imageMetadataSearch_directorySearch();

    // -- Diagnostic: Real directory --
    void imageMetadataSearch_realDirectory();

    // -- File Metadata Search --
    void fileMetadataSearch_findsFileInfo();
    void fileMetadataSearch_hasMetadataFormat();

    // -- Archive Search --
    void archiveSearch_validZip();
    void archiveSearch_zipEntryNames();
    void archiveSearch_deflateCompressed();

    // -- Cancellation --
    void cancellation_stopsEarly();

    // -- Progress Reporting --
    void progress_emitsProgressSignal();

    // -- Malformed-media hardening --
    void malformedMedia_doesNotCrash();

    // -- B6-22: surface unreadable files + bad exclude regex --
    void firstInvalidExcludePattern_detectsBadRegex();
    void invalidExcludePattern_failsClosed();
    void unreadableFile_isCounted();

    // -- Codex review 3 (search) --
    void missingRoot_failsClosed();
    void binarySearch_byteExactOffsetsAndArbitraryBytes();
    void negativeContextLines_noCrash();
    void archiveUnreadable_isCounted();

    // -- Codex review 5 (search: regex budget, incompleteness, zip, filters) --
    void overLongLine_notScannedAndMarkedIncomplete();
    void incompleteScan_reportedAsIncompleteNotComplete();
    void targetReadFailure_marksScanIncomplete();
    void dataDescriptorZip_reportedIncomplete();
    void singleFileSearch_appliesSkipFilters();
    void requiresNetworkProbe_detectsMappedNetworkDrive();
    void classifyNetworkReadStep_timeoutIsFailureNotShortRead();
    void networkReadTimeoutMs_clampsConfiguredSeconds();

private:
    void createTextFixtures();
    void createBinaryAndMetadataFixtures();
    void createArchiveFixtures();

    /// @brief Create a text file with given content in the temp dir
    void createTestFile(const QString& name, const QString& content);

    /// @brief Create a subdirectory in the temp dir
    void createSubDir(const QString& name);

    /// @brief Create a minimal valid ZIP file with one stored text entry
    QByteArray createMinimalZip(const QString& entryName, const QByteArray& entryData);

    /// @brief Create a minimal valid ZIP file with one deflate-compressed entry
    QByteArray createDeflateZip(const QString& entryName, const QByteArray& entryData);

    /// @brief Create a streaming ("data descriptor") ZIP: general-purpose bit 3
    ///        set, zero sizes in the local file header, real sizes in a trailing
    ///        data descriptor and in the central directory.
    static QByteArray createDataDescriptorZip(const QString& entryName,
                                              const QByteArray& entryData);

    /// @brief Run a worker synchronously and return results
    QVector<SearchMatch> runWorker(const SearchConfig& config);

    /// @brief Build a minimal JPEG with embedded EXIF metadata
    static QByteArray buildExifJpeg();

    QTemporaryDir m_temp_dir;
};

// ============================================================================
// Setup / Teardown
// ============================================================================

void AdvancedSearchWorkerTests::initTestCase() {
    QVERIFY(m_temp_dir.isValid());
    createTextFixtures();
    createBinaryAndMetadataFixtures();
    createArchiveFixtures();
}

void AdvancedSearchWorkerTests::createTextFixtures() {
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

    createTestFile("large_match.txt", QString("prefix target suffix\n").repeated(100));

    createSubDir("subdir");
    createTestFile("subdir/nested.txt", "Nested file content\nWith target text\n");

    createSubDir(".git");
    createTestFile(".git/config", "git config file\nHello inside git\n");

    createTestFile("special.txt",
                   "File has dots: config.ini\n"
                   "And parens: func(arg)\n"
                   "Plus stars: rating***\n");
}

void AdvancedSearchWorkerTests::createBinaryAndMetadataFixtures() {
    createTestFile("fake.bin",
                   QString(QByteArray(100, '\0')) + "hidden_text_marker" +
                       QString(QByteArray(100, '\0')));
    {
        QImage image(32, 24, QImage::Format_ARGB32);
        image.fill(Qt::darkGreen);
        image.setText("Comment", "CameraTrip");
        QVERIFY(image.save(m_temp_dir.path() + "/test.png", "PNG"));
    }

    // Create a synthetic JPEG with real EXIF metadata
    // (bypasses Qt JPEG plugin which may not be available in tests)
    {
        const QByteArray exif_jpeg = buildExifJpeg();
        QFile file(m_temp_dir.path() + "/exif_test.jpg");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(exif_jpeg);
        file.close();
    }
}

void AdvancedSearchWorkerTests::createArchiveFixtures() {
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

void AdvancedSearchWorkerTests::cleanupTestCase() {
    // QTemporaryDir auto-cleans
}

// ============================================================================
// Helper Methods
// ============================================================================

void AdvancedSearchWorkerTests::createTestFile(const QString& name, const QString& content) {
    const QString path = m_temp_dir.path() + "/" + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QFAIL(qPrintable(QString("Failed to create test file: %1").arg(path)));
    }
    file.write(content.toUtf8());
    file.close();
}

void AdvancedSearchWorkerTests::createSubDir(const QString& name) {
    const QString path = m_temp_dir.path() + "/" + name;
    QDir().mkpath(path);
}

QByteArray AdvancedSearchWorkerTests::createMinimalZip(const QString& entryName,
                                                       const QByteArray& entryData) {
    QByteArray zip;
    const QByteArray nameBytes = entryName.toUtf8();

    // -- Local File Header --
    zip.append("\x50\x4B\x03\x04", 4);
    zip.append("\x14\x00", 2);
    zip.append("\x00\x00", 2);
    zip.append("\x00\x00", 2);
    zip.append("\x00\x00", 2);
    zip.append("\x00\x00", 2);
    zip.append("\x00\x00\x00\x00", 4);
    quint32 size = static_cast<quint32>(entryData.size());
    zip.append(reinterpret_cast<const char*>(&size), 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);
    quint16 nameLen = static_cast<quint16>(nameBytes.size());
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append("\x00\x00", 2);
    zip.append(nameBytes);
    zip.append(entryData);

    // -- Central Directory Header --
    zip.append("\x50\x4B\x01\x02", 4);
    zip.append("\x14\x00", 2);
    zip.append("\x14\x00", 2);
    zip.append("\x00\x00", 2);
    zip.append("\x00\x00", 2);
    zip.append("\x00\x00\x00\x00", 4);
    zip.append("\x00\x00\x00\x00", 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append(QByteArray(12, '\0'));
    quint32 zero = 0;
    zip.append(reinterpret_cast<const char*>(&zero), 4);
    zip.append(nameBytes);

    // -- End of Central Directory --
    quint32 cdOffset = static_cast<quint32>(30 + nameBytes.size() + entryData.size());
    quint32 cdSize = static_cast<quint32>(46 + nameBytes.size());
    zip.append("\x50\x4B\x05\x06", 4);
    zip.append("\x00\x00\x00\x00", 4);
    zip.append("\x01\x00\x01\x00", 4);
    zip.append(reinterpret_cast<const char*>(&cdSize), 4);
    zip.append(reinterpret_cast<const char*>(&cdOffset), 4);
    zip.append("\x00\x00", 2);

    return zip;
}

QByteArray AdvancedSearchWorkerTests::createDeflateZip(const QString& entryName,
                                                       const QByteArray& entryData) {
    QByteArray compressed;
    compressed.resize(compressBound(static_cast<uLong>(entryData.size())));

    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(entryData.constData()));
    strm.avail_in = static_cast<uInt>(entryData.size());
    strm.next_out = reinterpret_cast<Bytef*>(compressed.data());
    strm.avail_out = static_cast<uInt>(compressed.size());

    // -MAX_WBITS -> raw deflate (what ZIP uses)
    deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    deflate(&strm, Z_FINISH);
    deflateEnd(&strm);
    compressed.resize(static_cast<qsizetype>(strm.total_out));

    quint32 crc = static_cast<quint32>(crc32(crc32(0, Z_NULL, 0),
                                             reinterpret_cast<const Bytef*>(entryData.constData()),
                                             static_cast<uInt>(entryData.size())));

    const QByteArray nameBytes = entryName.toUtf8();
    quint16 nameLen = static_cast<quint16>(nameBytes.size());
    quint32 compSize = static_cast<quint32>(compressed.size());
    quint32 uncompSize = static_cast<quint32>(entryData.size());

    QByteArray zip;

    // -- Local File Header --
    zip.append("\x50\x4B\x03\x04", 4);  // Signature
    zip.append("\x14\x00", 2);          // Version needed (2.0)
    zip.append("\x00\x00", 2);          // Flags
    zip.append("\x08\x00", 2);          // Compression: deflate (8)
    zip.append("\x00\x00\x00\x00", 4);  // Time, Date
    zip.append(reinterpret_cast<const char*>(&crc), 4);
    zip.append(reinterpret_cast<const char*>(&compSize), 4);
    zip.append(reinterpret_cast<const char*>(&uncompSize), 4);
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append("\x00\x00", 2);  // Extra field length
    zip.append(nameBytes);
    zip.append(compressed);

    // -- Central Directory Header --
    zip.append("\x50\x4B\x01\x02", 4);
    zip.append("\x14\x00", 2);          // Version made by
    zip.append("\x14\x00", 2);          // Version needed
    zip.append("\x00\x00", 2);          // Flags
    zip.append("\x08\x00", 2);          // Compression: deflate (8)
    zip.append("\x00\x00\x00\x00", 4);  // Time, Date
    zip.append(reinterpret_cast<const char*>(&crc), 4);
    zip.append(reinterpret_cast<const char*>(&compSize), 4);
    zip.append(reinterpret_cast<const char*>(&uncompSize), 4);
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append(QByteArray(12, '\0'));                     // Extra, Comment, Disk, attrs
    quint32 zero = 0;
    zip.append(reinterpret_cast<const char*>(&zero), 4);  // Local header offset
    zip.append(nameBytes);

    // -- End of Central Directory --
    quint32 cdOffset = static_cast<quint32>(30 + nameBytes.size() + compressed.size());
    quint32 cdSize = static_cast<quint32>(46 + nameBytes.size());
    zip.append("\x50\x4B\x05\x06", 4);
    zip.append("\x00\x00\x00\x00", 4);  // Disk numbers
    zip.append("\x01\x00\x01\x00", 4);  // Total entries
    zip.append(reinterpret_cast<const char*>(&cdSize), 4);
    zip.append(reinterpret_cast<const char*>(&cdOffset), 4);
    zip.append("\x00\x00", 2);  // Comment length

    return zip;
}

QByteArray AdvancedSearchWorkerTests::createDataDescriptorZip(const QString& entryName,
                                                              const QByteArray& entryData) {
    const QByteArray nameBytes = entryName.toUtf8();
    const quint16 nameLen = static_cast<quint16>(nameBytes.size());
    const quint32 size = static_cast<quint32>(entryData.size());
    const quint32 crc =
        static_cast<quint32>(crc32(crc32(0, Z_NULL, 0),
                                   reinterpret_cast<const Bytef*>(entryData.constData()),
                                   static_cast<uInt>(entryData.size())));
    const quint32 zero = 0;

    QByteArray zip;

    // Local File Header: flag bit 3 set, so the sizes here are ZERO and the real
    // ones only appear in the trailing data descriptor.
    zip.append("\x50\x4B\x03\x04", 4);
    zip.append("\x14\x00", 2);                            // Version needed
    zip.append("\x08\x00", 2);                            // Flags: bit 3 (data descriptor)
    zip.append("\x00\x00", 2);                            // Compression: stored
    zip.append("\x00\x00\x00\x00", 4);                    // Time, Date
    zip.append(reinterpret_cast<const char*>(&zero), 4);  // CRC (in the descriptor)
    zip.append(reinterpret_cast<const char*>(&zero), 4);  // Compressed size
    zip.append(reinterpret_cast<const char*>(&zero), 4);  // Uncompressed size
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append("\x00\x00", 2);                            // Extra field length
    zip.append(nameBytes);
    zip.append(entryData);

    // Data Descriptor
    zip.append("\x50\x4B\x07\x08", 4);
    zip.append(reinterpret_cast<const char*>(&crc), 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);

    const quint32 cdOffset = static_cast<quint32>(zip.size());

    // Central Directory Header (carries the real sizes)
    zip.append("\x50\x4B\x01\x02", 4);
    zip.append("\x14\x00", 2);          // Version made by
    zip.append("\x14\x00", 2);          // Version needed
    zip.append("\x08\x00", 2);          // Flags
    zip.append("\x00\x00", 2);          // Compression: stored
    zip.append("\x00\x00\x00\x00", 4);  // Time, Date
    zip.append(reinterpret_cast<const char*>(&crc), 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);
    zip.append(reinterpret_cast<const char*>(&size), 4);
    zip.append(reinterpret_cast<const char*>(&nameLen), 2);
    zip.append(QByteArray(12, '\0'));                     // Extra, Comment, Disk, attrs
    zip.append(reinterpret_cast<const char*>(&zero), 4);  // Local header offset
    zip.append(nameBytes);
    const quint32 cdSize = static_cast<quint32>(zip.size()) - cdOffset;

    // End of Central Directory
    zip.append("\x50\x4B\x05\x06", 4);
    zip.append("\x00\x00\x00\x00", 4);  // Disk numbers
    zip.append("\x01\x00\x01\x00", 4);  // Total entries
    zip.append(reinterpret_cast<const char*>(&cdSize), 4);
    zip.append(reinterpret_cast<const char*>(&cdOffset), 4);
    zip.append("\x00\x00", 2);  // Comment length

    return zip;
}

QVector<SearchMatch> AdvancedSearchWorkerTests::runWorker(const SearchConfig& config) {
    QVector<SearchMatch> allMatches;
    AdvancedSearchWorker worker(config);

    QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);
    QSignalSpy finishedSpy(&worker, qOverload<>(&WorkerBase::finished));
    QSignalSpy failedSpy(&worker, &WorkerBase::failed);

    worker.start();
    // A worker that has not finished must not fall through to read partial results, nor be
    // destroyed while its QThread is still running (a qFatal). A bare worker.wait(10s) whose
    // return is ignored did exactly that. On timeout, request the cooperative stop, join, and
    // FAIL the test -- QTest::qVerify (not QVERIFY) because this helper returns a value and
    // QVERIFY's early return is illegal in a non-void function.
    if (!worker.wait(10'000)) {
        worker.requestStop();
        worker.wait();
        QTest::qVerify(false,
                       "worker.wait(10000)",
                       "AdvancedSearchWorker did not finish within 10s",
                       __FILE__,
                       __LINE__);
    }

    for (int i = 0; i < resultsSpy.count(); ++i) {
        const auto matches = resultsSpy[i][0].value<QVector<SearchMatch>>();
        allMatches.append(matches);
    }

    return allMatches;
}

QByteArray AdvancedSearchWorkerTests::buildExifJpeg() {
    // Construct a minimal valid JPEG with an APP1 EXIF segment containing:
    //   Software  = "TestCamera"
    //   DateTime  = "2025:01:15 10:30:00"
    // This bypasses the Qt JPEG image plugin entirely.

    // -- TIFF body (little-endian) --
    QByteArray tiff;
    tiff.append("II", 2);                // Byte order mark
    tiff.append("\x2A\x00", 2);          // TIFF magic (42 LE)
    tiff.append("\x08\x00\x00\x00", 4);  // IFD0 offset = 8

    // IFD0: 2 entries
    tiff.append("\x02\x00", 2);  // Entry count

    // Entry 0 -- Software (tag 0x0131), ASCII, 11 bytes at offset 38
    tiff.append("\x31\x01", 2);          // Tag
    tiff.append("\x02\x00", 2);          // Type (ASCII)
    tiff.append("\x0B\x00\x00\x00", 4);  // Count = 11
    tiff.append("\x26\x00\x00\x00", 4);  // Offset = 38

    // Entry 1 -- DateTime (tag 0x0132), ASCII, 20 bytes at offset 49
    tiff.append("\x32\x01", 2);          // Tag
    tiff.append("\x02\x00", 2);          // Type (ASCII)
    tiff.append("\x14\x00\x00\x00", 4);  // Count = 20
    tiff.append("\x31\x00\x00\x00", 4);  // Offset = 49

    // No next IFD
    tiff.append("\x00\x00\x00\x00", 4);

    // String data (offsets 38 and 49 within TIFF)
    tiff.append("TestCamera\0", 11);           // Offset 38
    tiff.append("2025:01:15 10:30:00\0", 20);  // Offset 49

    // -- Wrap as JPEG APP1 --
    const auto app1_len = static_cast<uint16_t>(2 + 6 + tiff.size());

    QByteArray jpeg;
    jpeg.append("\xFF\xD8", 2);  // SOI
    jpeg.append("\xFF\xE1", 2);  // APP1 marker
    jpeg.append(static_cast<char>((app1_len >> 8) & 0xFF));
    jpeg.append(static_cast<char>(app1_len & 0xFF));
    jpeg.append("Exif\0\0", 6);  // EXIF header
    jpeg.append(tiff);
    jpeg.append("\xFF\xDA", 2);  // SOS (terminates scan)
    jpeg.append("\x00\x02", 2);  // Minimal SOS length
    jpeg.append("\xFF\xD9", 2);  // EOI

    return jpeg;
}

// ============================================================================
// Regex Compilation Tests
// ============================================================================

void AdvancedSearchWorkerTests::emptyPattern_returnsError() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "";

    AdvancedSearchWorker worker(config);
    QSignalSpy failSpy(&worker, &WorkerBase::failed);

    worker.start();
    QVERIFY(worker.wait(5000));

    QCOMPARE(failSpy.count(), 1);
    QCOMPARE(failSpy[0][0].toInt(), static_cast<int>(sak::error_code::invalid_argument));
}

void AdvancedSearchWorkerTests::plainText_findsMatch() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 3);  // "Hello" appears 3 times
}

void AdvancedSearchWorkerTests::plainText_caseInsensitive() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "hello";
    config.case_sensitive = false;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 3);  // Case-insensitive should find all "Hello"
}

void AdvancedSearchWorkerTests::plainText_caseSensitive() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "hello";  // lowercase
    config.case_sensitive = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);  // No lowercase "hello" in file
}

void AdvancedSearchWorkerTests::wholeWord_matchesBoundary() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.whole_word = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 3);
}

void AdvancedSearchWorkerTests::wholeWord_rejectsPartial() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hell";  // Partial word
    config.whole_word = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);  // "Hell" is not a whole word in the file
}

void AdvancedSearchWorkerTests::regexMode_specialChars() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello.*line";
    config.use_regex = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 2);
}

void AdvancedSearchWorkerTests::regexEscape_literalDots() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/special.txt";
    config.pattern = "config.ini";  // Non-regex: dots should be literal
    config.use_regex = false;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
}

// ============================================================================
// Text Content Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::textSearch_singleMatch() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/code.cpp";
    config.pattern = "iostream";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QVERIFY(matches[0].line_content.contains("iostream"));
}

void AdvancedSearchWorkerTests::textSearch_multipleMatches() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 3);
}

void AdvancedSearchWorkerTests::textSearch_contextLines() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "line 2";
    config.context_lines = 1;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    // Exact pins, not <= 1: the match is at line index 1, so with context_lines=1 the before/after
    // windows are each EXACTLY one known line. `<= 1` was satisfied by 0 too, so a regression that
    // emptied either context loop shipped green; pin size AND content so a dead or wrong-line loop
    // fails here.
    QCOMPARE(matches[0].context_before.size(), 1);
    QCOMPARE(matches[0].context_before.first(), QStringLiteral("Hello World"));
    QCOMPARE(matches[0].context_after.size(), 1);
    QCOMPARE(matches[0].context_after.first(), QStringLiteral("Hello again on line 3"));
}

void AdvancedSearchWorkerTests::textSearch_lineNumbers() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "line 2";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches[0].line_number, 2);  // "This is line 2" is on line 2
}

void AdvancedSearchWorkerTests::textSearch_noMatchReturnsEmpty() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = "nonexistent_pattern_xyz";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);
}

void AdvancedSearchWorkerTests::textSearch_binaryFileSkipped() {
    // Image extensions should be skipped for text content search
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "PNG";
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // PNG file is in kBinaryImageExts, so text search is skipped
    QCOMPARE(matches.size(), 0);
}

void AdvancedSearchWorkerTests::textSearch_maxResults() {
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

void AdvancedSearchWorkerTests::extensionFilter_matchesIncluded() {
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

void AdvancedSearchWorkerTests::extensionFilter_excludesOthers() {
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

void AdvancedSearchWorkerTests::extensionFilter_emptyMatchesAll() {
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.file_extensions.clear();  // No filter
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should find matches from multiple file types
    QSet<QString> extensions;
    for (const auto& m : matches) {
        extensions.insert(QFileInfo(m.file_path).suffix());
    }
    QVERIFY(extensions.size() >= 2);  // At least .txt and .cpp
}

// ============================================================================
// Exclusion Tests
// ============================================================================

void AdvancedSearchWorkerTests::excludePatterns_gitExcluded() {
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

void AdvancedSearchWorkerTests::excludePatterns_customExclude() {
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

void AdvancedSearchWorkerTests::fileSizeLimit_skipsOversized() {
    // max_file_size is only enforced in directory-recursive search,
    // so search the temp dir (not a single file) to exercise the limit.
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.max_file_size = 1;  // 1 byte -- every file is too large
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);
}

// ============================================================================
// Single File Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::singleFileSearch_worksWithFilePath() {
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

void AdvancedSearchWorkerTests::batchEmission_emitsResults() {
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);

    worker.start();
    QVERIFY(worker.wait(10'000));

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

void AdvancedSearchWorkerTests::hexSearch_findsPattern() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/fake.bin";
    config.pattern = "hidden_text_marker";
    config.hex_search = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY(matches.size() >= 1);
}

void AdvancedSearchWorkerTests::hexSearch_exclusiveMode() {
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

void AdvancedSearchWorkerTests::imageMetadataSearch_findsEmbeddedMetadata() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "CameraTrip";
    config.search_image_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should find the embedded image metadata value.
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

    // Image metadata mode should not rely on generic filesystem fields.
    for (const auto& m : matches) {
        QVERIFY2(!m.line_content.contains("FileName:"),
                 "Image metadata matches should not come from FileName field");
    }
}

void AdvancedSearchWorkerTests::imageMetadataSearch_matchesByTagName() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "Comment";
    config.search_image_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // "Comment" is a metadata key name -- key matching should find it
    QVERIFY2(matches.size() >= 1, "Searching by metadata tag name should produce matches");

    bool found_key_match = false;
    for (const auto& m : matches) {
        if (m.line_content.startsWith("[Metadata]") && m.line_content.contains("Comment")) {
            found_key_match = true;
            break;
        }
    }
    QVERIFY2(found_key_match, "Should match the metadata key name 'Comment'");
}

void AdvancedSearchWorkerTests::imageMetadataSearch_matchesDimensions() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "32x24";
    config.search_image_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // supplementWithImageReader adds Dimensions = "32x24"
    QVERIFY2(matches.size() >= 1, "Image metadata should include dimensions from QImageReader");

    bool found_dimensions = false;
    for (const auto& m : matches) {
        if (m.line_content.contains("Dimensions") && m.line_content.contains("32x24")) {
            found_dimensions = true;
            break;
        }
    }
    QVERIFY2(found_dimensions, "Should find Dimensions metadata field");
}

void AdvancedSearchWorkerTests::imageMetadataSearch_noMatchReturnsEmpty() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.png";
    config.pattern = "TotallyNonexistentMetadataValue12345";
    config.search_image_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 0);
}

void AdvancedSearchWorkerTests::imageMetadataSearch_jpegExifExtraction() {
    // Search by EXIF Software tag value
    {
        SearchConfig config;
        config.root_path = m_temp_dir.path() + "/exif_test.jpg";
        config.pattern = "TestCamera";
        config.search_image_metadata = true;
        config.exclude_patterns.clear();

        auto matches = runWorker(config);
        QVERIFY2(matches.size() >= 1, "JPEG EXIF Software tag should be searchable");

        bool found_software = false;
        for (const auto& m : matches) {
            if (m.line_content.contains("Software") && m.line_content.contains("TestCamera")) {
                found_software = true;
                break;
            }
        }
        QVERIFY2(found_software, "Should find Software value in JPEG EXIF metadata");
    }

    // Search by EXIF DateTime value (year portion)
    {
        SearchConfig config;
        config.root_path = m_temp_dir.path() + "/exif_test.jpg";
        config.pattern = "2025";
        config.search_image_metadata = true;
        config.exclude_patterns.clear();

        auto matches = runWorker(config);
        QVERIFY2(matches.size() >= 1, "JPEG EXIF DateTime should be searchable by year");

        bool found_date = false;
        for (const auto& m : matches) {
            if (m.line_content.contains("DateTime") && m.line_content.contains("2025")) {
                found_date = true;
                break;
            }
        }
        QVERIFY2(found_date, "Should find DateTime value in JPEG EXIF metadata");
    }

    // Search by EXIF tag name
    {
        SearchConfig config;
        config.root_path = m_temp_dir.path() + "/exif_test.jpg";
        config.pattern = "DateTime";
        config.search_image_metadata = true;
        config.exclude_patterns.clear();

        auto matches = runWorker(config);
        QVERIFY2(matches.size() >= 1, "JPEG EXIF tag names should be searchable");
    }
}

void AdvancedSearchWorkerTests::imageMetadataSearch_directorySearch() {
    // Search the entire temp directory for EXIF metadata value
    // This verifies that directory iteration dispatches to image
    // metadata search correctly for image files
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "TestCamera";
    config.search_image_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY2(matches.size() >= 1, "Directory search should find EXIF metadata in JPEG files");

    // Verify the match came from the JPEG file
    bool from_jpeg = false;
    for (const auto& m : matches) {
        if (m.file_path.endsWith(".jpg") && m.line_content.contains("TestCamera")) {
            from_jpeg = true;
            break;
        }
    }
    QVERIFY2(from_jpeg, "Directory search should find metadata in JPEG files");
}

void AdvancedSearchWorkerTests::imageMetadataSearch_realDirectory() {
    // Diagnostic test: verify EXIF metadata search works on real JPEGs.
    // Requires SAK_EXIF_SAMPLE_DIR to point to an EXIF sample directory.
    const QString real_dir = qEnvironmentVariable("SAK_EXIF_SAMPLE_DIR").trimmed();
    if (real_dir.isEmpty()) {
        QSKIP("Set SAK_EXIF_SAMPLE_DIR to run this diagnostic test");
    }
    if (!QDir(real_dir).exists()) {
        QSKIP("exif-samples-master test directory not available");
    }

    SearchConfig config;
    config.root_path = real_dir;
    config.pattern = "Canon";
    config.search_image_metadata = true;
    config.exclude_patterns.clear();
    config.max_results = 0;

    auto matches = runWorker(config);

    int metadata_count = 0;
    for (const auto& match : matches) {
        if (match.line_content.startsWith("[Metadata]")) {
            ++metadata_count;
        }
    }
    QVERIFY2(metadata_count >= 1, "Should find [Metadata] EXIF matches in Canon JPEG files");
}

// ============================================================================
// File Metadata Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::fileMetadataSearch_findsFileInfo() {
    // Create a test file with a metadata extension
    createTestFile("test_meta.json", R"({"key": "value"})");

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_meta.json";
    config.pattern = "json";  // Should match FileName or FileType
    config.search_file_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // Should find filesystem metadata
    QVERIFY(matches.size() >= 1);
}

void AdvancedSearchWorkerTests::fileMetadataSearch_hasMetadataFormat() {
    createTestFile("format_check.json", R"({"data": 123})");

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/format_check.json";
    config.pattern = "FileType";
    config.search_file_metadata = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QVERIFY2(matches.size() >= 1, "Searching by metadata key name should find FileType field");

    // Verify the metadata output format uses [Metadata] prefix
    bool has_metadata_prefix = false;
    for (const auto& m : matches) {
        if (m.line_content.startsWith("[Metadata]")) {
            has_metadata_prefix = true;
            break;
        }
    }
    QVERIFY2(has_metadata_prefix, "File metadata results should use [Metadata] prefix format");
}

// ============================================================================
// Archive Search Tests
// ============================================================================

void AdvancedSearchWorkerTests::archiveSearch_validZip() {
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
    QVERIFY2(hasArchivePath, "Archive matches should use path!/entry format");
}

void AdvancedSearchWorkerTests::archiveSearch_zipEntryNames() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test.zip";
    config.pattern = "readme";  // Should match the entry filename
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
    QVERIFY2(hasEntryMatch, "Should match archive entry filenames");
}

void AdvancedSearchWorkerTests::archiveSearch_deflateCompressed() {
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/deflate_test.zip";
    config.pattern = "SearchableToken";
    config.search_in_archives = true;
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    // The deflate-compressed ZIP contains "SearchableToken" inside compressed.txt
    QVERIFY2(matches.size() >= 1, "Deflate-compressed ZIP entry content should be searchable");

    // Verify it found the match inside the archive
    bool foundInArchive = false;
    for (const auto& m : matches) {
        if (m.file_path.contains("!/") && m.line_content.contains("SearchableToken")) {
            foundInArchive = true;
            break;
        }
    }
    QVERIFY2(foundInArchive, "Should find content match inside deflate-compressed archive entry");
}

// ============================================================================
// Cancellation Tests
// ============================================================================

void AdvancedSearchWorkerTests::cancellation_stopsEarly() {
    // Create many files to ensure search takes a while
    createSubDir("cancel_test");
    for (int i = 0; i < 100; ++i) {
        createTestFile(QString("cancel_test/file_%1.txt").arg(i),
                       QString("Content for cancel test file %1\nWith some target text\n").arg(i));
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

void AdvancedSearchWorkerTests::progress_emitsProgressSignal() {
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = "target";
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy progressSpy(&worker, &WorkerBase::progress);

    worker.start();
    QVERIFY(worker.wait(10'000));

    // Progress should be reported (at least the final report)
    QVERIFY(progressSpy.count() >= 1);
}

// ============================================================================
// Malformed-media hardening
// ============================================================================

namespace {

struct MalformedFixture {
    const char* name;
    QByteArray bytes;
    bool image;  // true -> image-metadata search, false -> file-metadata
};

// Each fixture carries an on-disk length/offset field with the high bit set;
// before the parser hardening these narrowed to negative ints and drove the
// metadata parsers to read far outside the buffer (crash/OOB).
QVector<MalformedFixture> buildMalformedMediaFixtures() {
    QByteArray png;  // PNG with a first chunk length of 0x80000000.
    png.append("\x89PNG\r\n\x1A\n", 8);
    png.append("\x80\x00\x00\x00", 4);
    png.append("tEXt", 4);
    png.append("Comment\0value", 13);

    QByteArray tiff;  // EXIF IFD entry whose value offset is 0xFFFFFFFF.
    tiff.append("II", 2);
    tiff.append("\x2A\x00", 2);
    tiff.append("\x08\x00\x00\x00", 4);
    tiff.append("\x01\x00", 2);          // 1 entry
    tiff.append("\x31\x01", 2);          // Software tag
    tiff.append("\x02\x00", 2);          // ASCII
    tiff.append("\x40\x00\x00\x00", 4);  // count = 64 (out of line)
    tiff.append("\xFF\xFF\xFF\xFF", 4);  // value offset = UINT32_MAX
    tiff.append("\x00\x00\x00\x00", 4);  // no next IFD
    const auto app1_len = static_cast<uint16_t>(2 + 6 + tiff.size());
    QByteArray jpeg;
    jpeg.append("\xFF\xD8", 2);
    jpeg.append("\xFF\xE1", 2);
    jpeg.append(static_cast<char>((app1_len >> 8) & 0xFF));
    jpeg.append(static_cast<char>(app1_len & 0xFF));
    jpeg.append("Exif\0\0", 6);
    jpeg.append(tiff);
    jpeg.append("\xFF\xD9", 2);

    QByteArray zip;                     // ZIP local header with compressed-size 0x80000000.
    zip.append("PK\x03\x04", 4);
    zip.append(QByteArray(14, '\0'));   // version..crc
    zip.append("\x00\x00\x00\x80", 4);  // compressed size (LE)
    zip.append("\x00\x00\x00\x80", 4);  // uncompressed size
    zip.append("\x08\x00", 2);          // name length = 8
    zip.append("\x00\x00", 2);          // extra length
    zip.append("core.xml", 8);

    QByteArray mp3;                     // ID3v2 with a first frame size field of 0x80000000.
    mp3.append("ID3", 3);
    mp3.append("\x03\x00", 2);          // version
    mp3.append("\x00", 1);              // flags
    mp3.append("\x00\x00\x40\x00", 4);  // synchsafe tag size (8192)
    mp3.append("TIT2", 4);              // frame id
    mp3.append("\x80\x00\x00\x00", 4);  // frame size = 0x80000000
    mp3.append("\x00\x00", 2);          // frame flags
    mp3.append(QByteArray(64, 'A'));

    return {{"bad.png", png, true},
            {"bad.jpg", jpeg, true},
            {"bad.docx", zip, false},
            {"bad.mp3", mp3, false}};
}

}  // namespace

void AdvancedSearchWorkerTests::malformedMedia_doesNotCrash() {
    for (const auto& fx : buildMalformedMediaFixtures()) {
        const QString path = m_temp_dir.path() + "/" + fx.name;
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(fx.bytes), static_cast<qint64>(fx.bytes.size()));
        f.close();

        SearchConfig config;
        config.root_path = path;
        config.pattern = "value";
        config.search_image_metadata = fx.image;
        config.search_file_metadata = !fx.image;
        config.exclude_patterns.clear();

        // The assertion that matters is that the worker returns at all: a
        // regression would crash the process or hang past the wait timeout.
        AdvancedSearchWorker worker(config);
        worker.start();
        QVERIFY2(worker.wait(10'000),
                 qPrintable(QStringLiteral("worker hung on malformed %1").arg(fx.name)));
    }
}

// ============================================================================
// B6-22: surface unreadable files + bad exclude regex
// ============================================================================

void AdvancedSearchWorkerTests::firstInvalidExcludePattern_detectsBadRegex() {
    // All valid -> nullopt.
    QVERIFY(!AdvancedSearchWorker::firstInvalidExcludePattern(
                 QStringList{QStringLiteral("\\.git"), QStringLiteral("node_modules")})
                 .has_value());
    // An unbalanced bracket is an invalid regex -> returned so the caller fails closed.
    const auto bad = AdvancedSearchWorker::firstInvalidExcludePattern(
        QStringList{QStringLiteral("valid"), QStringLiteral("[unterminated")});
    QVERIFY(bad.has_value());
    QCOMPARE(*bad, QStringLiteral("[unterminated"));
}

void AdvancedSearchWorkerTests::invalidExcludePattern_failsClosed() {
    SearchConfig config;
    config.root_path = m_temp_dir.path();
    config.pattern = QStringLiteral("Hello");
    config.exclude_patterns = QStringList{QStringLiteral("[unterminated")};

    AdvancedSearchWorker worker(config);
    QSignalSpy failedSpy(&worker, &WorkerBase::failed);
    worker.start();
    QVERIFY(worker.wait(10'000));

    // An invalid exclude regex must abort the search (fail closed), not silently
    // drop the exclusion and search the paths it was meant to exclude.
    QCOMPARE(failedSpy.count(), 1);
}

void AdvancedSearchWorkerTests::unreadableFile_isCounted() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A searchable file plus one we exclusively lock so the worker cannot read it.
    QFile readable(QDir(dir.path()).filePath(QStringLiteral("readable.txt")));
    QVERIFY(readable.open(QIODevice::WriteOnly));
    readable.write("Hello there");
    readable.close();

    const QString lockedPath = QDir(dir.path()).filePath(QStringLiteral("locked.txt"));
    QFile locked(lockedPath);
    QVERIFY(locked.open(QIODevice::WriteOnly));
    locked.write("Hello secret");
    locked.close();

    const std::wstring wpath = lockedPath.toStdWString();
    HANDLE handle = CreateFileW(wpath.c_str(),
                                GENERIC_READ,
                                0,  // exclusive
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);

    SearchConfig config;
    config.root_path = dir.path();
    config.pattern = QStringLiteral("Hello");

    AdvancedSearchWorker worker(config);
    worker.start();
    const bool finished = worker.wait(10'000);
    CloseHandle(handle);
    QVERIFY(finished);

    // The locked file could not be read -> counted, not a silent false negative.
    QCOMPARE(worker.filesUnreadable(), 1);
}

// ============================================================================
// Codex review 3 (search)
// ============================================================================

void AdvancedSearchWorkerTests::missingRoot_failsClosed() {
    // A nonexistent local directory root must fail closed, not fall through to
    // QDirIterator and report a clean "0 matches" (a false negative).
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/no_such_directory_xyz123";
    config.pattern = QStringLiteral("Hello");
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy failedSpy(&worker, &WorkerBase::failed);
    worker.start();
    QVERIFY(worker.wait(10'000));

    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy[0][0].toInt(), static_cast<int>(sak::error_code::file_not_found));
}

void AdvancedSearchWorkerTests::binarySearch_byteExactOffsetsAndArbitraryBytes() {
    // File: two bytes that are invalid UTF-8 (0xFF 0xFE) followed by "TOKEN".
    QByteArray bytes;
    bytes.append(static_cast<char>(0xFF));
    bytes.append(static_cast<char>(0xFE));
    bytes.append("TOKEN");
    const QString path = m_temp_dir.path() + "/bytes.bin";
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(bytes), static_cast<qint64>(bytes.size()));
        f.close();
    }

    // (a) The reported byte offset must equal the raw file offset (2), not the
    // UTF-8 re-encoded offset the old fromUtf8() path produced.
    {
        SearchConfig config;
        config.root_path = path;
        config.pattern = QStringLiteral("TOKEN");
        config.hex_search = true;
        config.exclude_patterns.clear();

        auto matches = runWorker(config);
        QCOMPARE(matches.size(), 1);
        QCOMPARE(matches[0].line_number, 2);  // byte offset of 'T'
    }

    // (b) An arbitrary non-UTF-8 byte must be matchable via a regex \xNN escape;
    // under the old UTF-8 decode 0xFF collapsed to U+FFFD and never matched.
    {
        SearchConfig config;
        config.root_path = path;
        config.pattern = QStringLiteral("\\xff");
        config.use_regex = true;
        config.hex_search = true;
        config.exclude_patterns.clear();

        auto matches = runWorker(config);
        QVERIFY(matches.size() >= 1);
        QCOMPARE(matches[0].line_number, 0);  // 0xFF is the first byte
    }
}

void AdvancedSearchWorkerTests::negativeContextLines_noCrash() {
    // INT_MIN context_lines used to make line_index - ctx overflow (UB). It must
    // now be clamped to 0: the search still succeeds with no context lines.
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/hello.txt";
    config.pattern = QStringLiteral("line 2");
    config.context_lines = std::numeric_limits<int>::min();
    config.exclude_patterns.clear();

    auto matches = runWorker(config);
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches[0].context_before.size(), 0);
    QCOMPARE(matches[0].context_after.size(), 0);
}

void AdvancedSearchWorkerTests::archiveUnreadable_isCounted() {
    // An archive that cannot be opened must be counted as unreadable, not
    // reported as an archive that genuinely holds no match.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString zipPath = QDir(dir.path()).filePath(QStringLiteral("locked.zip"));
    {
        QFile z(zipPath);
        QVERIFY(z.open(QIODevice::WriteOnly));
        z.write(createMinimalZip(QStringLiteral("readme.txt"), QByteArray("hello zip")));
        z.close();
    }

    const std::wstring wpath = zipPath.toStdWString();
    HANDLE handle = CreateFileW(
        wpath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);

    SearchConfig config;
    config.root_path = zipPath;
    config.pattern = QStringLiteral("hello");
    config.search_in_archives = true;
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    worker.start();
    const bool finished = worker.wait(10'000);
    CloseHandle(handle);
    QVERIFY(finished);

    QCOMPARE(worker.filesUnreadable(), 1);
}

// ============================================================================
// Codex review 5 (search): regex budget, incompleteness, zip, single-file filters
// ============================================================================

void AdvancedSearchWorkerTests::overLongLine_notScannedAndMarkedIncomplete() {
    // Qt exposes no PCRE2 match/time limit and the stop flag is only reachable
    // BETWEEN lines, so an unbounded subject line is an uninterruptible worker
    // hang. Over-limit lines must be skipped, and the skip surfaced.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("over_long_line.txt"));
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray filler(300'000, 'a');  // > the 256 KiB scan limit
        f.write(filler);
        f.write("NEEDLE");
        f.write("\nshort NEEDLE line\n");
        f.close();
    }

    SearchConfig config;
    config.root_path = path;
    config.pattern = QStringLiteral("NEEDLE");
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);
    worker.start();
    QVERIFY(worker.wait(10'000));

    QVector<SearchMatch> matches;
    for (int i = 0; i < resultsSpy.count(); ++i) {
        matches += resultsSpy[i][0].value<QVector<SearchMatch>>();
    }

    // Only the short second line may be scanned.
    QCOMPARE(matches.size(), 1);
    QCOMPARE(matches[0].line_number, 2);
    // ... and the omission is surfaced, never silent.
    QCOMPARE(worker.filesUnreadable(), 1);
    QVERIFY(worker.scanIncomplete());
}

void AdvancedSearchWorkerTests::incompleteScan_reportedAsIncompleteNotComplete() {
    // (a) Control: a directory the worker can fully read reports "Search complete".
    QTemporaryDir clean_dir;
    QVERIFY(clean_dir.isValid());
    {
        QFile f(QDir(clean_dir.path()).filePath(QStringLiteral("readable.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("Hello there");
        f.close();
    }

    SearchConfig clean_config;
    clean_config.root_path = clean_dir.path();
    clean_config.pattern = QStringLiteral("Hello");

    AdvancedSearchWorker clean_worker(clean_config);
    QSignalSpy clean_progress(&clean_worker, &WorkerBase::progress);
    clean_worker.start();
    QVERIFY(clean_worker.wait(10'000));
    QVERIFY(!clean_worker.scanIncomplete());
    QVERIFY(clean_progress.count() >= 1);
    const QString clean_final = clean_progress[clean_progress.count() - 1][2].toString();
    QVERIFY2(clean_final.startsWith(QStringLiteral("Search complete")), qPrintable(clean_final));

    // (b) A file the worker cannot open makes the run non-authoritative: the
    // terminal message must say INCOMPLETE, not "Search complete".
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString lockedPath = QDir(dir.path()).filePath(QStringLiteral("locked.txt"));
    {
        QFile locked(lockedPath);
        QVERIFY(locked.open(QIODevice::WriteOnly));
        locked.write("Hello secret");
        locked.close();
    }

    const std::wstring wpath = lockedPath.toStdWString();
    HANDLE handle = CreateFileW(
        wpath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);

    SearchConfig config;
    config.root_path = dir.path();
    config.pattern = QStringLiteral("Hello");

    AdvancedSearchWorker worker(config);
    QSignalSpy progress(&worker, &WorkerBase::progress);
    worker.start();
    const bool finished = worker.wait(10'000);
    CloseHandle(handle);
    QVERIFY(finished);

    QVERIFY(worker.scanIncomplete());
    QVERIFY(!worker.incompleteReasons().isEmpty());
    QVERIFY(progress.count() >= 1);
    const QString final_message = progress[progress.count() - 1][2].toString();
    QVERIFY2(final_message.startsWith(QStringLiteral("Search INCOMPLETE")),
             qPrintable(final_message));
}

void AdvancedSearchWorkerTests::targetReadFailure_marksScanIncomplete() {
    // A failed read through the file-system bridge used to be swallowed: no
    // match, no unreadable count, and a confident "Search complete".
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString lockedPath = QDir(dir.path()).filePath(QStringLiteral("locked.txt"));
    {
        QFile locked(lockedPath);
        QVERIFY(locked.open(QIODevice::WriteOnly));
        locked.write("Hello secret");
        locked.close();
    }

    const std::wstring wpath = lockedPath.toStdWString();
    HANDLE handle = CreateFileW(
        wpath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);

    SearchConfig config;
    config.pattern = QStringLiteral("Hello");
    config.exclude_patterns.clear();
    config.use_file_system_target = true;
    config.file_system_target = sak::FileManagementFileSystemBridge::localTarget(dir.path());
    config.root_path = config.file_system_target.root_path;
    QVERIFY(config.file_system_target.can_advanced_search);

    AdvancedSearchWorker worker(config);
    QSignalSpy progress(&worker, &WorkerBase::progress);
    worker.start();
    const bool finished = worker.wait(10'000);
    CloseHandle(handle);
    QVERIFY(finished);

    QCOMPARE(worker.filesUnreadable(), 1);
    QVERIFY(worker.scanIncomplete());
    QVERIFY(progress.count() >= 1);
    const QString final_message = progress[progress.count() - 1][2].toString();
    QVERIFY2(final_message.startsWith(QStringLiteral("Search INCOMPLETE")),
             qPrintable(final_message));
}

void AdvancedSearchWorkerTests::dataDescriptorZip_reportedIncomplete() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString zipPath = QDir(dir.path()).filePath(QStringLiteral("streamed.zip"));
    {
        QFile z(zipPath);
        QVERIFY(z.open(QIODevice::WriteOnly));
        z.write(createDataDescriptorZip(QStringLiteral("readme.txt"),
                                        QByteArray("Hello from a streamed ZIP entry")));
        z.close();
    }

    SearchConfig config;
    config.root_path = zipPath;
    config.pattern = QStringLiteral("Hello");
    config.search_in_archives = true;
    config.exclude_patterns.clear();

    AdvancedSearchWorker worker(config);
    worker.start();
    QVERIFY(worker.wait(10'000));

    // A streaming ZIP stores no sizes in its local headers, so the entry chain
    // cannot be walked past the first entry. That must be surfaced, not returned
    // as a partial match list indistinguishable from a full scan.
    QCOMPARE(worker.filesUnreadable(), 1);
    QVERIFY(worker.scanIncomplete());

    // Control: a well-formed ZIP whose chain ends exactly on the central directory
    // must NOT trip the premature-stop detection.
    SearchConfig ok_config;
    ok_config.root_path = m_temp_dir.path() + "/test.zip";
    ok_config.pattern = QStringLiteral("Hello");
    ok_config.search_in_archives = true;
    ok_config.exclude_patterns.clear();

    AdvancedSearchWorker ok_worker(ok_config);
    QSignalSpy okResults(&ok_worker, &AdvancedSearchWorker::resultsReady);
    ok_worker.start();
    QVERIFY(ok_worker.wait(10'000));
    QCOMPARE(ok_worker.filesUnreadable(), 0);
    QVERIFY(!ok_worker.scanIncomplete());
    QVERIFY(okResults.count() >= 1);
}

void AdvancedSearchWorkerTests::singleFileSearch_appliesSkipFilters() {
    // The explicitly-named single file must go through the SAME filters as the
    // directory walk. It previously ran only isExcluded, so max_file_size and the
    // extension filter were bypassed and an arbitrarily large file was read whole.

    // (a) max_file_size
    {
        SearchConfig config;
        config.root_path = m_temp_dir.path() + "/hello.txt";
        config.pattern = QStringLiteral("Hello");
        config.max_file_size = 1;  // 1 byte -- the fixture is over the limit
        config.exclude_patterns.clear();

        AdvancedSearchWorker worker(config);
        QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);
        worker.start();
        QVERIFY(worker.wait(10'000));

        QCOMPARE(resultsSpy.count(), 0);
        // The named file was never opened, so "0 matches" is not authoritative.
        QVERIFY(worker.scanIncomplete());
    }

    // (b) extension filter
    {
        SearchConfig config;
        config.root_path = m_temp_dir.path() + "/hello.txt";
        config.pattern = QStringLiteral("Hello");
        config.file_extensions = QStringList{QStringLiteral(".cpp")};
        config.exclude_patterns.clear();

        AdvancedSearchWorker worker(config);
        QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);
        worker.start();
        QVERIFY(worker.wait(10'000));

        QCOMPARE(resultsSpy.count(), 0);
        QVERIFY(worker.scanIncomplete());
    }

    // (c) Control: an unfiltered single file still searches and reports complete.
    {
        SearchConfig config;
        config.root_path = m_temp_dir.path() + "/hello.txt";
        config.pattern = QStringLiteral("Hello");
        config.exclude_patterns.clear();

        AdvancedSearchWorker worker(config);
        QSignalSpy resultsSpy(&worker, &AdvancedSearchWorker::resultsReady);
        worker.start();
        QVERIFY(worker.wait(10'000));

        QVERIFY(resultsSpy.count() >= 1);
        QVERIFY(!worker.scanIncomplete());
    }
}

void AdvancedSearchWorkerTests::requiresNetworkProbe_detectsMappedNetworkDrive() {
    // UNC roots were already routed through the bounded accessibility probe.
    QVERIFY(AdvancedSearchWorker::requiresNetworkProbe(QStringLiteral("\\\\server\\share"), 0));
    QVERIFY(AdvancedSearchWorker::requiresNetworkProbe(QStringLiteral("//server/share"), 0));

    // A drive letter mapped to an SMB share looks local, so it used to bypass the
    // probe entirely and an unresponsive share could block the whole walk.
    QVERIFY(AdvancedSearchWorker::requiresNetworkProbe(QStringLiteral("Z:\\dir\\file.txt"),
                                                       DRIVE_REMOTE));

    // Genuinely local roots must not pay for the probe.
    QVERIFY(
        !AdvancedSearchWorker::requiresNetworkProbe(QStringLiteral("C:\\Windows"), DRIVE_FIXED));
    QVERIFY(
        !AdvancedSearchWorker::requiresNetworkProbe(QStringLiteral("D:\\data"), DRIVE_REMOVABLE));
    QVERIFY(!AdvancedSearchWorker::requiresNetworkProbe(QStringLiteral("E:\\iso"), DRIVE_CDROM));

    // The live classifier agrees for the two ends it can decide without a mapping.
    QVERIFY(AdvancedSearchWorker::isNetworkPath(QStringLiteral("\\\\server\\share")));
    QVERIFY(!AdvancedSearchWorker::isNetworkPath(m_temp_dir.path()));
}

void AdvancedSearchWorkerTests::classifyNetworkReadStep_timeoutIsFailureNotShortRead() {
    using Step = AdvancedSearchWorker::NetworkReadStep;

    // A timeout is a FAILURE, whatever landed. Returning the partial buffer would
    // let a hung SMB server masquerade as a file that simply held no match.
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(false, true, 4096, 4096),
             Step::TimedOut);
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(false, true, 10, 4096), Step::TimedOut);
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(false, false, 0, 4096), Step::TimedOut);

    // A signalled-but-failed I/O is a failure, never a clean end of file.
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, false, 0, 4096), Step::Failed);
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, true, -1, 4096), Step::Failed);

    // A zero-length window is a caller bug, not a clean stop.
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, true, 0, 0), Step::Failed);
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, true, 0, -1), Step::Failed);

    // A file read comes up short only at end of file; what landed is real.
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, true, 0, 4096), Step::EndOfFile);
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, true, 100, 4096), Step::EndOfFile);

    // A full window keeps the read loop going.
    QCOMPARE(AdvancedSearchWorker::classifyNetworkReadStep(true, true, 4096, 4096), Step::Complete);
}

void AdvancedSearchWorkerTests::networkReadTimeoutMs_clampsConfiguredSeconds() {
    constexpr int kDefaultMs = sak::kAdvancedSearchNetworkTimeoutSec * 1000;

    QCOMPARE(AdvancedSearchWorker::networkReadTimeoutMs(5), 5000);

    // An unset or nonsensical budget takes the documented default; there is no
    // "wait forever" setting for a network read.
    QCOMPARE(AdvancedSearchWorker::networkReadTimeoutMs(0), kDefaultMs);
    QCOMPARE(AdvancedSearchWorker::networkReadTimeoutMs(-1), kDefaultMs);
    QCOMPARE(AdvancedSearchWorker::networkReadTimeoutMs(std::numeric_limits<int>::min()),
             kDefaultMs);

    // A huge configured value is clamped so seconds * 1000 cannot overflow int. Pin the ABSOLUTE
    // ceiling (kMaxNetworkTimeoutSec 300 * 1000 ms = 300000): `> 0` plus a self-referential compare
    // could not catch a silent bump of the max (e.g. 300 -> 3000) that decuples the per-read
    // timeout; the exact value does.
    const int clamped = AdvancedSearchWorker::networkReadTimeoutMs(std::numeric_limits<int>::max());
    QCOMPARE(clamped, 300'000);
    QCOMPARE(clamped, AdvancedSearchWorker::networkReadTimeoutMs(1'000'000));
}

QTEST_GUILESS_MAIN(AdvancedSearchWorkerTests)
#include "test_advanced_search_worker.moc"
