// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_worker.cpp
/// @brief Implements directory-recursive file content search on a worker thread

#include "sak/advanced_search_worker.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTextStream>

#include <algorithm>
#include <cstring>

#include <zlib.h>

namespace sak {

namespace {
constexpr qsizetype kMaxTextSearchLines = 500'000;
constexpr int kByteIndex2 = 2;
constexpr int kByteIndex3 = 3;
constexpr int kByteShift8 = 8;
constexpr int kByteShift16 = 16;
constexpr int kByteShift24 = 24;
constexpr int kExifTagHexWidth = 4;
constexpr int kExifShortBytes = 2;
constexpr int kExifLongBytes = 4;
constexpr int kExifRationalBytes = 8;
constexpr int kExifInlineValueBytes = 4;
constexpr int kExifValueOffset = 8;
constexpr int kExifEntryCountBytes = 2;
constexpr int kExifIfdEntrySize = 12;
constexpr int kExifEntryTypeOffset = 2;
constexpr int kExifEntryCountOffset = 4;
constexpr int kExifEntryValueOffset = 8;
constexpr int kJpegHeaderMinBytes = 4;
constexpr int kJpegScanStartOffset = 2;
constexpr int kJpegSegmentHeaderBytes = 4;
constexpr uint8_t kJpegMarkerPrefix = 0xFF;
constexpr uint8_t kJpegMarkerSoi = 0xD8;
constexpr int kPngSignatureBytes = 8;
constexpr int kPngChunkOverheadBytes = 12;
constexpr int kPngChunkLengthHighWordFactor = 65'536;
constexpr int kZipSignatureByte0 = 0;
constexpr int kZipSignatureByte1 = 1;
constexpr int kZipSignatureByte2 = 2;
constexpr int kZipSignatureByte3 = 3;
constexpr uint8_t kZipSignaturePk0 = 0x50;
constexpr uint8_t kZipSignaturePk1 = 0x4B;
constexpr uint8_t kZipSignatureLocal2 = 0x03;
constexpr uint8_t kZipSignatureLocal3 = 0x04;
constexpr int kId3SignatureByte2 = 2;
constexpr int kUtf16CodeUnitBytes = 2;
constexpr int kId3SizeByte0 = 6;
constexpr int kId3SizeByte1 = 7;
constexpr int kId3SizeByte2 = 8;
constexpr int kId3SizeByte3 = 9;
constexpr qint64 kDefaultBinarySearchBytes = 100LL * sak::kBytesPerMB;
constexpr int kTargetSearchBrowseMaxEntries = 10'000;
}  // namespace

// -- Zlib Inflate Helper ------------------------------------------------------

/// @brief Decompress a deflate-compressed (method 8) ZIP entry using zlib.
/// @param compressedData  The raw deflated bytes from the ZIP entry.
/// @param expectedSize    The uncompressed size from the ZIP header.
/// @return The decompressed bytes, or an empty QByteArray on failure.
[[nodiscard]] static QByteArray inflateZipEntry(const QByteArray& compressedData,
                                                uint32_t expectedSize) {
    if (compressedData.isEmpty() || expectedSize == 0) {
        return {};
    }

    // Cap at 64 MiB to prevent zip-bomb decompression
    constexpr uint32_t kMaxInflateSize = 64u * 1024u * 1024u;
    if (expectedSize > kMaxInflateSize) {
        return {};
    }

    QByteArray output;
    output.resize(static_cast<qsizetype>(expectedSize));

    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressedData.constData()));
    strm.avail_in = static_cast<uInt>(compressedData.size());
    strm.next_out = reinterpret_cast<Bytef*>(output.data());
    strm.avail_out = static_cast<uInt>(expectedSize);

    // -MAX_WBITS -> raw deflate (no zlib/gzip header), which is what ZIP uses
    int ret = inflateInit2(&strm, -MAX_WBITS);
    if (ret != Z_OK) {
        return {};
    }

    ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        return {};
    }

    output.resize(static_cast<qsizetype>(strm.total_out));
    return output;
}

// -- Construction ------------------------------------------------------------

AdvancedSearchWorker::AdvancedSearchWorker(SearchConfig config, QObject* parent)
    : WorkerBase(parent), m_config(std::move(config)) {}

// -- Regex Compilation -------------------------------------------------------

auto AdvancedSearchWorker::compileRegex() const -> std::expected<QRegularExpression, QString> {
    if (m_config.pattern.isEmpty()) {
        return std::unexpected(QStringLiteral("Search pattern is empty"));
    }

    QString regexPattern = m_config.pattern;

    // Escape if not using regex mode
    if (!m_config.use_regex) {
        regexPattern = QRegularExpression::escape(regexPattern);
    }

    // Wrap for whole-word matching
    if (m_config.whole_word) {
        regexPattern = QString(R"(\b%1\b)").arg(regexPattern);
    }

    QRegularExpression::PatternOptions opts = QRegularExpression::DontCaptureOption;

    if (!m_config.case_sensitive) {
        opts |= QRegularExpression::CaseInsensitiveOption;
    }

    QRegularExpression regex(regexPattern, opts);

    if (!regex.isValid()) {
        return std::unexpected(QString("Invalid regex: %1 at offset %2")
                                   .arg(regex.errorString())
                                   .arg(regex.patternErrorOffset()));
    }

    return regex;
}

// -- Exclusion & Filtering ---------------------------------------------------

bool AdvancedSearchWorker::isExcluded(const QString& path) const {
    return std::any_of(m_compiled_excludes.begin(),
                       m_compiled_excludes.end(),
                       [&path](const auto& excludeRegex) {
                           return excludeRegex.match(path).hasMatch();
                       });
}

bool AdvancedSearchWorker::matchesExtensionFilter(const QString& filePath) const {
    Q_ASSERT(!filePath.isEmpty());
    if (m_config.file_extensions.isEmpty()) {
        return true;  // No filter = accept all
    }

    const QString ext = QFileInfo(filePath).suffix().toLower();
    return std::any_of(m_config.file_extensions.begin(),
                       m_config.file_extensions.end(),
                       [&ext](const QString& filter) {
                           QString normalized = filter.trimmed().toLower();
                           if (normalized.startsWith('.')) {
                               normalized = normalized.mid(1);
                           }
                           return ext == normalized;
                       });
}

// -- Network Path Detection --------------------------------------------------

bool AdvancedSearchWorker::isNetworkPath(const QString& path) {
    return path.startsWith("\\\\") || path.startsWith("//");
}

bool AdvancedSearchWorker::checkNetworkPathAccessible(const QString& path) const {
    // Simple accessibility check -- try to list directory contents
    const QFileInfo info(path);
    return info.exists() && info.isReadable();
}

// -- Main Search Execution ---------------------------------------------------

auto AdvancedSearchWorker::prepareSearchConfig()
    -> std::expected<QRegularExpression, sak::error_code> {
    auto regexResult = compileRegex();
    if (!regexResult) {
        logError(
            "AdvancedSearchWorker: regex compilation "
            "failed: {}",
            regexResult.error().toStdString());
        return std::unexpected(sak::error_code::invalid_argument);
    }
    const auto& regex = regexResult.value();

    // Compile exclusion patterns once
    m_compiled_excludes.clear();
    for (const auto& pattern : m_config.exclude_patterns) {
        QRegularExpression excl(pattern, QRegularExpression::CaseInsensitiveOption);
        if (excl.isValid()) {
            m_compiled_excludes.append(excl);
        }
    }

    // Check network accessibility
    if (!m_config.use_file_system_target && isNetworkPath(m_config.root_path)) {
        if (!checkNetworkPathAccessible(m_config.root_path)) {
            logError(
                "AdvancedSearchWorker: network path "
                "not accessible: {}",
                m_config.root_path.toStdString());
            return std::unexpected(sak::error_code::network_unavailable);
        }
    }

    return regex;
}

bool AdvancedSearchWorker::shouldSkipFile(const QString& file_path) const {
    if (isExcluded(file_path)) {
        return true;
    }
    if (!matchesExtensionFilter(file_path)) {
        return true;
    }
    if (m_config.max_file_size > 0) {
        const QFileInfo file_info(file_path);
        if (file_info.size() > m_config.max_file_size) {
            return true;
        }
    }
    return false;
}

bool AdvancedSearchWorker::shouldSkipTargetFile(const FileManagementEntry& file) const {
    if (!file.regular_file) {
        return true;
    }
    if (isExcluded(file.path)) {
        return true;
    }
    if (!matchesExtensionFilter(file.path)) {
        return true;
    }
    return m_config.max_file_size > 0 &&
           static_cast<qint64>(file.size_bytes) > m_config.max_file_size;
}

bool AdvancedSearchWorker::processSearchFile(const QString& file_path,
                                             const QRegularExpression& regex,
                                             QVector<SearchMatch>& batch_matches,
                                             int& total_matches,
                                             int& total_files) {
    auto matches = searchFile(file_path, regex);
    if (matches.isEmpty()) {
        return true;
    }

    Q_EMIT fileSearched(file_path, matches.size());
    batch_matches.append(matches);
    total_matches += matches.size();
    total_files++;

    if (m_config.max_results > 0 && total_matches >= m_config.max_results) {
        logInfo(
            "AdvancedSearchWorker: max results limit "
            "({}) reached",
            m_config.max_results);
        return false;
    }
    return true;
}

void AdvancedSearchWorker::runDirectorySearch(const QRegularExpression& regex,
                                              int& total_matches,
                                              int& total_files) {
    QDirIterator it(m_config.root_path,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    int file_count = 0;
    QVector<SearchMatch> batch_matches;

    while (it.hasNext()) {
        if (checkStop()) {
            logInfo(
                "AdvancedSearchWorker: search cancelled "
                "after {} files",
                file_count);
            return;
        }

        const QString file_path = it.next();
        if (shouldSkipFile(file_path)) {
            continue;
        }

        if (!processSearchFile(file_path, regex, batch_matches, total_matches, total_files)) {
            break;
        }

        file_count++;

        if (file_count % kBatchSize == 0 && !batch_matches.isEmpty()) {
            Q_EMIT resultsReady(batch_matches);
            batch_matches.clear();
        }

        constexpr int kProgressInterval = 100;
        if (file_count % kProgressInterval == 0) {
            reportProgress(file_count,
                           0,
                           QString("Searching... %1 files scanned, "
                                   "%2 matches found")
                               .arg(file_count)
                               .arg(total_matches));
        }
    }

    if (!batch_matches.isEmpty()) {
        Q_EMIT resultsReady(batch_matches);
    }

    reportProgress(file_count,
                   file_count,
                   QString("Search complete: %1 matches in %2 files "
                           "(%3 files scanned)")
                       .arg(total_matches)
                       .arg(total_files)
                       .arg(file_count));
}

bool AdvancedSearchWorker::searchTargetDirectory(const QString& directory_path,
                                                 const QRegularExpression& regex,
                                                 QVector<SearchMatch>& batch_matches,
                                                 int& total_matches,
                                                 int& total_files,
                                                 int& file_count) {
    if (checkStop()) {
        return false;
    }

    const auto listing = FileManagementFileSystemBridge::listDirectory(
        m_config.file_system_target, directory_path, kTargetSearchBrowseMaxEntries);
    if (!listing.ok) {
        logWarning("AdvancedSearchWorker: target listing failed at '{}': {}",
                   directory_path.toStdString(),
                   listing.blockers.join(QStringLiteral("; ")).toStdString());
        return true;
    }

    for (const auto& entry : listing.entries) {
        if (checkStop()) {
            return false;
        }
        if (entry.regular_file) {
            ++file_count;
            if (!shouldSkipTargetFile(entry) &&
                !processTargetFile(entry, regex, batch_matches, total_matches, total_files)) {
                return false;
            }
            if (file_count % kBatchSize == 0 && !batch_matches.isEmpty()) {
                Q_EMIT resultsReady(batch_matches);
                batch_matches.clear();
            }
            constexpr int kProgressInterval = 100;
            if (file_count % kProgressInterval == 0) {
                reportProgress(file_count,
                               0,
                               QString("Searching target... %1 files scanned, %2 matches found")
                                   .arg(file_count)
                                   .arg(total_matches));
            }
        } else if (entry.directory) {
            if (!searchTargetDirectory(entry.path,
                                       regex,
                                       batch_matches,
                                       total_matches,
                                       total_files,
                                       file_count)) {
                return false;
            }
        }
    }
    return true;
}

bool AdvancedSearchWorker::processTargetFile(const FileManagementEntry& file,
                                             const QRegularExpression& regex,
                                             QVector<SearchMatch>& batch_matches,
                                             int& total_matches,
                                             int& total_files) {
    auto matches = searchTargetFile(file, regex);
    if (matches.isEmpty()) {
        return true;
    }

    Q_EMIT fileSearched(file.path, matches.size());
    batch_matches.append(matches);
    total_matches += matches.size();
    total_files++;

    if (m_config.max_results > 0 && total_matches >= m_config.max_results) {
        logInfo("AdvancedSearchWorker: max results limit ({}) reached", m_config.max_results);
        return false;
    }
    return true;
}

void AdvancedSearchWorker::runFileSystemTargetSearch(const QRegularExpression& regex,
                                                     int& total_matches,
                                                     int& total_files) {
    int file_count = 0;
    QVector<SearchMatch> batch_matches;
    searchTargetDirectory(m_config.root_path.trimmed().isEmpty() ? QStringLiteral("/")
                                                                : m_config.root_path,
                          regex,
                          batch_matches,
                          total_matches,
                          total_files,
                          file_count);

    if (!batch_matches.isEmpty()) {
        Q_EMIT resultsReady(batch_matches);
    }
    reportProgress(file_count,
                   file_count,
                   QString("Search complete: %1 matches in %2 files (%3 files scanned)")
                       .arg(total_matches)
                       .arg(total_files)
                       .arg(file_count));
}

auto AdvancedSearchWorker::execute() -> std::expected<void, sak::error_code> {
    logInfo("AdvancedSearchWorker: starting search for '{}' in '{}'",
            m_config.pattern.toStdString(),
            m_config.root_path.toStdString());

    auto regexResult = prepareSearchConfig();
    if (!regexResult) {
        return std::unexpected(regexResult.error());
    }
    const auto& regex = regexResult.value();

    int totalMatches = 0;
    int totalFiles = 0;

    if (m_config.use_file_system_target) {
        if (!m_config.file_system_target.can_advanced_search) {
            logError("AdvancedSearchWorker: target does not support advanced search: {}",
                     m_config.file_system_target.label.toStdString());
            return std::unexpected(sak::error_code::invalid_argument);
        }
        runFileSystemTargetSearch(regex, totalMatches, totalFiles);
        logInfo(
            "AdvancedSearchWorker: target search complete -- "
            "{} matches in {} files",
            totalMatches,
            totalFiles);
        return {};
    }

    // Single file search -- no directory iteration needed
    if (QFileInfo(m_config.root_path).isFile()) {
        if (!isExcluded(m_config.root_path)) {
            auto matches = searchFile(m_config.root_path, regex);
            if (!matches.isEmpty()) {
                totalMatches += matches.size();
                totalFiles++;
                Q_EMIT fileSearched(m_config.root_path, matches.size());
                Q_EMIT resultsReady(matches);
            }
        }
        reportProgress(
            1,
            1,
            QString("Search complete: %1 matches in %2 files").arg(totalMatches).arg(totalFiles));
        return {};
    }

    runDirectorySearch(regex, totalMatches, totalFiles);

    logInfo(
        "AdvancedSearchWorker: search complete -- "
        "{} matches in {} files",
        totalMatches,
        totalFiles);

    return {};
}

// -- File Search Dispatcher --------------------------------------------------

bool shouldSearchText(const QString& ext, bool handled_as_special) {
    if (handled_as_special && kArchiveExtensions.contains(ext)) {
        return false;
    }
    static const QSet<QString> kBinaryImageExts = {
        "jpg", "jpeg", "png", "tiff", "tif", "gif", "bmp", "webp", "heic", "heif"};
    return !kBinaryImageExts.contains(ext);
}

QVector<SearchMatch> AdvancedSearchWorker::searchFile(const QString& filePath,
                                                      const QRegularExpression& regex) {
    QVector<SearchMatch> matches;
    const QString ext = QFileInfo(filePath).suffix().toLower();

    bool handled_as_special = false;

    if (m_config.search_image_metadata && kImageExtensions.contains(ext)) {
        matches.append(searchImageMetadata(filePath, regex));
        handled_as_special = true;
    }

    if (m_config.search_file_metadata && kFileMetadataExtensions.contains(ext)) {
        matches.append(searchFileMetadata(filePath, regex));
        handled_as_special = true;
    }

    if (m_config.search_in_archives && kArchiveExtensions.contains(ext)) {
        matches.append(searchArchive(filePath, regex));
        handled_as_special = true;
    }

    if (m_config.hex_search) {
        matches.append(searchBinary(filePath, regex));
        return matches;
    }

    if (shouldSearchText(ext, handled_as_special)) {
        matches.append(searchTextContent(filePath, regex));
    }

    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchTargetFile(const FileManagementEntry& file,
                                                            const QRegularExpression& regex) {
    QVector<SearchMatch> matches;
    if (m_config.search_file_metadata) {
        QMap<QString, QString> metadata;
        metadata.insert(QStringLiteral("FileName"), file.name);
        metadata.insert(QStringLiteral("FilePath"), file.path);
        metadata.insert(QStringLiteral("FileSize"), QStringLiteral("%1 bytes").arg(file.size_bytes));
        metadata.insert(QStringLiteral("FileType"), QFileInfo(file.path).suffix().toUpper());
        matches.append(collectMetadataMatches(file.path, regex, metadata));
    }

    const uint64_t readLimit =
        m_config.max_file_size > 0 ? static_cast<uint64_t>(m_config.max_file_size)
                                   : static_cast<uint64_t>(kDefaultBinarySearchBytes);
    const auto read =
        FileManagementFileSystemBridge::readFile(m_config.file_system_target, file.path, readLimit);
    if (!read.ok) {
        return matches;
    }

    if (m_config.hex_search) {
        matches.append(searchBinaryBytes(file.path, read.data, regex));
    } else if (shouldSearchText(QFileInfo(file.path).suffix().toLower(), false)) {
        matches.append(searchTextBytes(file.path, read.data, regex));
    }
    return matches;
}

// -- Text Content Search -----------------------------------------------------

SearchMatch AdvancedSearchWorker::buildContextMatch(
    const QString& file_path,
    const QStringList& lines,
    int line_index,
    const QRegularExpressionMatch& regex_match) const {
    SearchMatch match;
    match.file_path = file_path;
    match.line_number = line_index + 1;
    match.line_content = lines[line_index];
    match.match_start = static_cast<int>(regex_match.capturedStart());
    match.match_end = static_cast<int>(regex_match.capturedEnd());

    const int ctx = m_config.context_lines;
    for (int j = std::max(0, line_index - ctx); j < line_index; ++j) {
        match.context_before.append(lines[j]);
    }
    const int last = static_cast<int>(lines.size()) - 1;
    const int end = std::min(last, line_index + ctx);
    for (int j = line_index + 1; j <= end; ++j) {
        match.context_after.append(lines[j]);
    }
    return match;
}

QVector<SearchMatch> AdvancedSearchWorker::searchTextContent(const QString& filePath,
                                                             const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return matches;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    // Read all lines for context window support
    QStringList lines;
    while (!stream.atEnd()) {
        lines.append(stream.readLine());

        // Safety limit: skip very large files that somehow passed the size check
        if (lines.size() > kMaxTextSearchLines) {
            logWarning("AdvancedSearchWorker: file '{}' exceeds 500k lines, skipping",
                       filePath.toStdString());
            return matches;
        }
    }

    // Search each line for matches
    for (int i = 0; i < lines.size(); ++i) {
        if (checkStop()) {
            return matches;
        }

        const QString& line = lines[i];
        auto match_iter = regex.globalMatch(line);

        while (match_iter.hasNext()) {
            auto regex_match = match_iter.next();
            matches.append(buildContextMatch(filePath, lines, i, regex_match));

            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }
    }

    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchTextBytes(const QString& file_path,
                                                           const QByteArray& data,
                                                           const QRegularExpression& regex) const {
    QVector<SearchMatch> matches;
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
    if (lines.size() > kMaxTextSearchLines) {
        logWarning("AdvancedSearchWorker: virtual file '{}' exceeds 500k lines, skipping",
                   file_path.toStdString());
        return matches;
    }

    for (int i = 0; i < lines.size(); ++i) {
        if (checkStop()) {
            return matches;
        }
        auto match_iter = regex.globalMatch(lines.at(i));
        while (match_iter.hasNext()) {
            const auto regex_match = match_iter.next();
            matches.append(buildContextMatch(file_path, lines, i, regex_match));
            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }
    }
    return matches;
}

// -- Image Metadata Search ---------------------------------------------------

namespace {

/// @brief Read a 16-bit big-endian unsigned integer from raw bytes
[[nodiscard]] inline uint16_t readBE16(const char* data) {
    return static_cast<uint16_t>((static_cast<uint8_t>(data[0]) << kByteShift8) |
                                 static_cast<uint8_t>(data[1]));
}

/// @brief Read a 32-bit unsigned integer respecting byte order
[[nodiscard]] inline uint32_t readU32(const char* data, bool littleEndian) {
    if (littleEndian) {
        return static_cast<uint32_t>(static_cast<uint8_t>(data[0])) |
               (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << kByteShift8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(data[kByteIndex2])) << kByteShift16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(data[kByteIndex3])) << kByteShift24);
    }
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[0])) << kByteShift24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << kByteShift16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(data[kByteIndex2])) << kByteShift8) |
           static_cast<uint32_t>(static_cast<uint8_t>(data[kByteIndex3]));
}

/// @brief Read a 16-bit unsigned integer respecting byte order
[[nodiscard]] inline uint16_t readU16(const char* data, bool littleEndian) {
    if (littleEndian) {
        return static_cast<uint16_t>(static_cast<uint8_t>(data[0]) |
                                     (static_cast<uint8_t>(data[1]) << kByteShift8));
    }
    return static_cast<uint16_t>((static_cast<uint8_t>(data[0]) << kByteShift8) |
                                 static_cast<uint8_t>(data[1]));
}

struct ExifTagEntry {
    uint16_t tag_id;
    const char* name;
};

static constexpr ExifTagEntry kExifTagNames[] = {
    // IFD0 / IFD1 tags
    {0x010E, "ImageDescription"},
    {0x010F, "CameraMake"},
    {0x0110, "CameraModel"},
    {0x0112, "Orientation"},
    {0x011A, "XResolution"},
    {0x011B, "YResolution"},
    {0x0128, "ResolutionUnit"},
    {0x0131, "Software"},
    {0x0132, "DateTime"},
    {0x013B, "Artist"},
    {0x0213, "YCbCrPositioning"},
    {0x8298, "Copyright"},
    // Sub-IFD pointers
    {0x8769, "ExifOffset"},
    {0x8825, "GPSInfo"},
    // Exif sub-IFD tags
    {0x829A, "ExposureTime"},
    {0x829D, "FNumber"},
    {0x8827, "ISOSpeed"},
    {0x9000, "ExifVersion"},
    {0x9003, "DateTimeOriginal"},
    {0x9004, "DateTimeDigitized"},
    {0x9101, "ComponentsConfig"},
    {0x9102, "CompressedBitsPerPixel"},
    {0x9201, "ShutterSpeed"},
    {0x9202, "Aperture"},
    {0x9203, "Brightness"},
    {0x9204, "ExposureBias"},
    {0x9205, "MaxAperture"},
    {0x9206, "SubjectDistance"},
    {0x9207, "MeteringMode"},
    {0x9208, "LightSource"},
    {0x9209, "Flash"},
    {0x920A, "FocalLength"},
    {0x9286, "UserComment"},
    {0x927C, "MakerNote"},
    {0xA001, "ColorSpace"},
    {0xA002, "PixelXDimension"},
    {0xA003, "PixelYDimension"},
    {0xA005, "InteropOffset"},
    {0xA210, "FocalPlaneResUnit"},
    {0xA217, "SensingMethod"},
    {0xA300, "FileSource"},
    {0xA301, "SceneType"},
    {0xA401, "CustomRendered"},
    {0xA402, "ExposureMode"},
    {0xA403, "WhiteBalance"},
    {0xA404, "DigitalZoomRatio"},
    {0xA405, "FocalLengthIn35mm"},
    {0xA406, "SceneCaptureType"},
    {0xA407, "GainControl"},
    {0xA408, "Contrast"},
    {0xA409, "Saturation"},
    {0xA40A, "Sharpness"},
    {0xA420, "ImageUniqueID"},
    {0xA432, "LensInfo"},
    {0xA433, "LensMake"},
    {0xA434, "LensModel"},
    {0xA435, "LensSerialNumber"},
    // GPS sub-IFD tags
    {0x0000, "GPSVersionID"},
    {0x0001, "GPSLatitudeRef"},
    {0x0002, "GPSLatitude"},
    {0x0003, "GPSLongitudeRef"},
    {0x0004, "GPSLongitude"},
    {0x0005, "GPSAltitudeRef"},
    {0x0006, "GPSAltitude"},
    {0x0007, "GPSTimeStamp"},
    {0x0008, "GPSSatellites"},
    {0x0009, "GPSStatus"},
    {0x000A, "GPSMeasureMode"},
    {0x000B, "GPSDOP"},
    {0x000C, "GPSSpeedRef"},
    {0x000D, "GPSSpeed"},
    {0x000E, "GPSTrackRef"},
    {0x000F, "GPSTrack"},
    {0x0010, "GPSImgDirRef"},
    {0x0011, "GPSImgDirection"},
    {0x0012, "GPSMapDatum"},
    {0x001D, "GPSDateStamp"},
};

/// @brief EXIF tag ID -> human-readable name mapping
[[nodiscard]] QString exifTagName(uint16_t tag) {
    auto it = std::find_if(std::begin(kExifTagNames),
                           std::end(kExifTagNames),
                           [tag](const auto& entry) { return entry.tag_id == tag; });
    if (it != std::end(kExifTagNames)) {
        return QStringLiteral("%1").arg(QLatin1String(it->name));
    }
    return QString("Tag_0x%1").arg(tag, kExifTagHexWidth, sak::kHexBase, QChar('0'));
}

int exifTypeUnitSize(uint16_t type) {
    struct TypeSize {
        uint16_t type;
        int size;
    };
    static constexpr TypeSize kSizes[] = {
        {1, 1},
        {2, 1},
        {3, 2},
        {4, 4},
        {5, 8},
        {7, 1},
        {9, 4},
        {10, 8},
    };
    auto it = std::find_if(std::begin(kSizes), std::end(kSizes), [type](const auto& entry) {
        return entry.type == type;
    });
    if (it != std::end(kSizes)) {
        return it->size;
    }
    return 0;
}

QString extractExifRational(const char* value_ptr, bool little_endian) {
    const uint32_t num = readU32(value_ptr, little_endian);
    const uint32_t den = readU32(value_ptr + kExifLongBytes, little_endian);
    if (den != 0) {
        return QString("%1/%2").arg(num).arg(den);
    }
    return {};
}

constexpr uint16_t kExifTypeAscii = 2;
constexpr uint16_t kExifTypeShort = 3;
constexpr uint16_t kExifTypeLong = 4;
constexpr uint16_t kExifTypeRational = 5;
constexpr uint16_t kExifTypeUndefined = 7;
constexpr uint32_t kMaxUndefinedLen = 64;
constexpr uint32_t kMaxMultiValues = 8;

template <typename ReadFn>
QString formatExifMultiValues(int type_size, uint32_t count, const char* valuePtr, ReadFn reader) {
    if (count == 0) {
        return {};
    }
    const uint32_t limit = std::min(count, kMaxMultiValues);
    QStringList parts;
    for (uint32_t idx = 0; idx < limit; ++idx) {
        parts.append(QString::number(reader(valuePtr + idx * type_size)));
    }
    return parts.join(QStringLiteral(", "));
}

QString formatExifRationals(uint32_t count, const char* valuePtr, bool littleEndian) {
    if (count == 0) {
        return {};
    }
    const uint32_t limit = std::min(count, kMaxMultiValues);
    QStringList parts;
    for (uint32_t idx = 0; idx < limit; ++idx) {
        const QString rat = extractExifRational(valuePtr + idx * kExifRationalBytes, littleEndian);
        if (!rat.isEmpty()) {
            parts.append(rat);
        }
    }
    return parts.join(QStringLiteral(", "));
}

QString extractExifValue(uint16_t type, uint32_t count, const char* valuePtr, bool littleEndian) {
    switch (type) {
    case kExifTypeAscii:
        if (count > 0) {
            return QString::fromLatin1(valuePtr, static_cast<int>(count - 1)).trimmed();
        }
        return {};
    case kExifTypeShort:
        return formatExifMultiValues(
            kExifShortBytes, count, valuePtr, [littleEndian](const char* p) {
                return readU16(p, littleEndian);
            });
    case kExifTypeLong:
        return formatExifMultiValues(
            kExifLongBytes, count, valuePtr, [littleEndian](const char* p) {
                return readU32(p, littleEndian);
            });
    case kExifTypeRational:
        return formatExifRationals(count, valuePtr, littleEndian);
    case kExifTypeUndefined:
        if (count <= kMaxUndefinedLen) {
            return QString::fromLatin1(valuePtr, static_cast<int>(count)).trimmed();
        }
        break;
    default:
        break;
    }
    return {};
}

constexpr uint16_t kExifIfdTag = 0x8769;
constexpr uint16_t kGpsIfdTag = 0x8825;

bool isSubIfdTag(uint16_t tag, uint16_t type, uint32_t count) {
    return (tag == kExifIfdTag || tag == kGpsIfdTag) && type == kExifTypeLong && count == 1;
}

const char* resolveExifValuePtr(
    const char* entry, const char* base, uint32_t total_bytes, int data_size, bool little_endian) {
    if (total_bytes <= kExifInlineValueBytes) {
        return entry + kExifValueOffset;
    }
    const uint32_t offset = readU32(entry + 8, little_endian);
    if (static_cast<int>(offset + total_bytes) > data_size) {
        return nullptr;
    }
    return base + offset;
}

void parseExifIFD(const QByteArray& tiffData,
                  uint32_t ifdOffset,
                  bool littleEndian,
                  QMap<QString, QString>& metadata,
                  int depth = 0) {
    constexpr int kMaxDepth = 4;
    if (depth > kMaxDepth) {
        return;
    }

    const int data_size = tiffData.size();
    if (static_cast<int>(ifdOffset + kExifEntryCountBytes) > data_size) {
        return;
    }

    const char* base = tiffData.constData();
    const uint16_t entry_count = readU16(base + ifdOffset, littleEndian);
    for (uint16_t i = 0; i < entry_count; ++i) {
        const uint32_t entry_offset = ifdOffset + kExifEntryCountBytes + (i * kExifIfdEntrySize);
        if (static_cast<int>(entry_offset + kExifIfdEntrySize) > data_size) {
            break;
        }

        const char* entry = base + entry_offset;
        const uint16_t tag = readU16(entry, littleEndian);
        const uint16_t type = readU16(entry + kExifEntryTypeOffset, littleEndian);
        const uint32_t count = readU32(entry + kExifEntryCountOffset, littleEndian);

        const int unit_size = exifTypeUnitSize(type);
        if (unit_size == 0) {
            continue;
        }

        if (isSubIfdTag(tag, type, count)) {
            const uint32_t sub_offset = readU32(entry + kExifEntryValueOffset, littleEndian);
            parseExifIFD(tiffData, sub_offset, littleEndian, metadata, depth + 1);
            continue;
        }

        const uint32_t total_bytes = count * static_cast<uint32_t>(unit_size);
        const char* value_ptr =
            resolveExifValuePtr(entry, base, total_bytes, data_size, littleEndian);
        if (!value_ptr) {
            continue;
        }

        const QString value = extractExifValue(type, count, value_ptr, littleEndian);
        if (!value.isEmpty()) {
            metadata.insert(exifTagName(tag), value);
        }
    }
}

/// @brief Extract EXIF metadata from a JPEG file
bool isJpegSoiMarker(const QByteArray& data) {
    return data.size() >= kJpegHeaderMinBytes &&
           static_cast<uint8_t>(data[0]) == kJpegMarkerPrefix &&
           static_cast<uint8_t>(data[1]) == kJpegMarkerSoi;
}

void processApp1Segment(const QByteArray& file_data,
                        int offset,
                        uint16_t seg_len,
                        QMap<QString, QString>& metadata) {
    constexpr int kExifHeaderSize = 6;
    constexpr int kMinTiffSize = 8;
    constexpr int kSegLenFieldSize = 2;
    const int exif_start = offset + kJpegSegmentHeaderBytes;
    if (file_data.mid(exif_start, kExifHeaderSize) != QByteArray("Exif\0\0", kExifHeaderSize)) {
        return;
    }

    // JPEG APP1 seg_len includes the 2-byte length field itself,
    // then "Exif\0\0" (6 bytes), then the TIFF payload.
    if (seg_len <= kSegLenFieldSize + kExifHeaderSize) {
        return;
    }

    const int tiff_start = exif_start + kExifHeaderSize;
    const int tiff_len = static_cast<int>(seg_len) - kSegLenFieldSize - kExifHeaderSize;
    const QByteArray tiff_data = file_data.mid(tiff_start, tiff_len);
    if (tiff_data.size() < kMinTiffSize) {
        return;
    }
    const bool little_endian = (tiff_data[0] == 'I' && tiff_data[1] == 'I');
    const uint32_t ifd0_offset = readU32(tiff_data.constData() + kExifEntryCountOffset,
                                         little_endian);
    parseExifIFD(tiff_data, ifd0_offset, little_endian, metadata);
}

[[nodiscard]] QMap<QString, QString> extractJpegExif(const QByteArray& fileData) {
    QMap<QString, QString> metadata;

    if (!isJpegSoiMarker(fileData)) {
        return metadata;
    }

    constexpr uint8_t kMarkerSos = 0xDA;
    constexpr uint8_t kMarkerEoi = 0xD9;
    constexpr uint8_t kMarkerApp1 = 0xE1;
    constexpr int kMinApp1Len = 8;

    int offset = kJpegScanStartOffset;
    while (offset + kJpegSegmentHeaderBytes < fileData.size()) {
        if (static_cast<uint8_t>(fileData[offset]) != kJpegMarkerPrefix) {
            break;
        }

        const uint8_t marker = static_cast<uint8_t>(fileData[offset + 1]);
        if (marker == kMarkerSos || marker == kMarkerEoi) {
            break;
        }

        const uint16_t seg_len = readBE16(fileData.constData() + offset + kExifEntryCountBytes);

        if (marker == kMarkerApp1 && seg_len > kMinApp1Len) {
            processApp1Segment(fileData, offset, seg_len, metadata);
        }

        offset += kExifEntryCountBytes + seg_len;
    }

    return metadata;
}

/// @brief Extract PNG text metadata chunks (tEXt, iTXt, zTXt)
void parseTEXtChunk(const QByteArray& chunk_data, QMap<QString, QString>& metadata) {
    const int null_pos = chunk_data.indexOf('\0');
    if (null_pos > 0) {
        const QString key = QString::fromLatin1(chunk_data.left(null_pos));
        const QString val = QString::fromLatin1(chunk_data.mid(null_pos + 1));
        metadata.insert(key, val);
    }
}

void parseITXtChunk(const QByteArray& chunk_data, QMap<QString, QString>& metadata) {
    const int null_pos = chunk_data.indexOf('\0');
    if (null_pos <= 0) {
        return;
    }
    const QString key = QString::fromLatin1(chunk_data.left(null_pos));
    constexpr int kNullsToSkip = 3;
    int text_start = null_pos + 1;
    for (int null_count = 0; null_count < kNullsToSkip && text_start < chunk_data.size();
         ++text_start) {
        if (chunk_data[text_start] == '\0') {
            ++null_count;
        }
    }
    if (text_start < chunk_data.size()) {
        const QString val = QString::fromUtf8(chunk_data.mid(text_start));
        metadata.insert(key, val);
    }
}

[[nodiscard]] QMap<QString, QString> extractPngMetadata(const QByteArray& fileData) {
    QMap<QString, QString> metadata;

    if (fileData.size() < kPngSignatureBytes) {
        return metadata;
    }
    static const char kPngSig[] = "\x89PNG\r\n\x1A\n";
    if (std::memcmp(fileData.constData(), kPngSig, kPngSignatureBytes) != 0) {
        return metadata;
    }

    int offset = kPngSignatureBytes;
    while (offset + kPngChunkOverheadBytes <= fileData.size()) {
        const uint32_t chunk_len = readBE16(fileData.constData() + offset) *
                                       kPngChunkLengthHighWordFactor +
                                   readBE16(fileData.constData() + offset + kExifEntryCountBytes);
        const QByteArray chunk_type = fileData.mid(offset + kExifLongBytes, kExifLongBytes);
        const int chunk_len_int = static_cast<int>(chunk_len);

        if (chunk_type == "IEND") {
            break;
        }

        if (chunk_len_int > 0) {
            const QByteArray chunk_data = fileData.mid(offset + kExifValueOffset, chunk_len_int);
            if (chunk_type == "tEXt") {
                parseTEXtChunk(chunk_data, metadata);
            } else if (chunk_type == "iTXt") {
                parseITXtChunk(chunk_data, metadata);
            }
        }

        offset += kPngChunkOverheadBytes + chunk_len_int;
    }

    return metadata;
}

struct MetadataMatchContext {
    const QString& file_path;
    const QString& key;
    const QString& value;
    int field_index;
};

[[nodiscard]] SearchMatch makeMetadataMatch(const MetadataMatchContext& ctx,
                                            const QRegularExpressionMatch& regex_match,
                                            bool match_in_key = false) {
    SearchMatch match;
    match.file_path = ctx.file_path;
    match.line_number = ctx.field_index;
    match.line_content = QString("[Metadata] %1: %2").arg(ctx.key, ctx.value);

    if (match_in_key) {
        constexpr int kMetadataPrefix = 11;
        match.match_start = kMetadataPrefix + static_cast<int>(regex_match.capturedStart());
        match.match_end = kMetadataPrefix + static_cast<int>(regex_match.capturedEnd());
    } else {
        const int prefix = QString("[Metadata] %1: ").arg(ctx.key).length();
        match.match_start = prefix + static_cast<int>(regex_match.capturedStart());
        match.match_end = prefix + static_cast<int>(regex_match.capturedEnd());
    }

    return match;
}

bool collectFieldMatches(const MetadataMatchContext& ctx,
                         const QRegularExpression& regex,
                         int max_results,
                         QVector<SearchMatch>& matches) {
    auto match_iter = regex.globalMatch(ctx.value);
    while (match_iter.hasNext()) {
        auto regex_match = match_iter.next();
        matches.append(makeMetadataMatch(ctx, regex_match));
        if (max_results > 0 && matches.size() >= max_results) {
            return true;
        }
    }

    auto key_iter = regex.globalMatch(ctx.key);
    while (key_iter.hasNext()) {
        auto regex_match = key_iter.next();
        matches.append(makeMetadataMatch(ctx, regex_match, true));
        if (max_results > 0 && matches.size() >= max_results) {
            return true;
        }
    }

    return false;
}

}  // anonymous namespace

/// @brief Extract TIFF metadata (IFD tags) from a raw TIFF file.
///        TIFF files use the same IFD structure as JPEG EXIF.
[[nodiscard]] QMap<QString, QString> extractTiffMetadata(const QByteArray& fileData) {
    QMap<QString, QString> metadata;
    constexpr int kMinTiffSize = 8;
    if (fileData.size() < kMinTiffSize) {
        return metadata;
    }
    const bool little_endian = (fileData[0] == 'I' && fileData[1] == 'I');
    const bool big_endian = (fileData[0] == 'M' && fileData[1] == 'M');
    if (!little_endian && !big_endian) {
        return metadata;
    }
    const uint32_t ifd0_offset = readU32(fileData.constData() + 4, little_endian);
    parseExifIFD(fileData, ifd0_offset, little_endian, metadata);
    return metadata;
}

void gatherFormatMetadata(const QByteArray& file_data,
                          const QString& ext,
                          QMap<QString, QString>& metadata) {
    if (ext == "jpg" || ext == "jpeg") {
        metadata = extractJpegExif(file_data);
    } else if (ext == "png") {
        metadata = extractPngMetadata(file_data);
    } else if (ext == "tiff" || ext == "tif") {
        metadata = extractTiffMetadata(file_data);
    }
}

void supplementWithImageReader(const QString& file_path, QMap<QString, QString>& metadata) {
    QImageReader reader(file_path);
    reader.setAutoDetectImageFormat(true);
    const QStringList keys = reader.textKeys();
    for (const auto& key : keys) {
        if (!metadata.contains(key)) {
            metadata.insert(key, reader.text(key));
        }
    }
    const QSize img_size = reader.size();
    if (img_size.isValid()) {
        metadata.insert("Width", QString::number(img_size.width()));
        metadata.insert("Height", QString::number(img_size.height()));
        metadata.insert("Dimensions",
                        QString("%1x%2").arg(img_size.width()).arg(img_size.height()));
    }
}

QVector<SearchMatch> AdvancedSearchWorker::searchImageMetadata(const QString& filePath,
                                                               const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    constexpr qint64 kMaxMetadataRead = 256 * 1024;
    const QByteArray fileData = file.read(std::min(file.size(), kMaxMetadataRead));
    file.close();

    if (checkStop()) {
        return matches;
    }

    const QString ext = QFileInfo(filePath).suffix().toLower();
    QMap<QString, QString> metadata;
    gatherFormatMetadata(fileData, ext, metadata);
    supplementWithImageReader(filePath, metadata);

    // cppcheck-suppress knownConditionTrueFalse ; atomic stop flag checked across threads
    if (checkStop()) {
        return matches;
    }

    int field_index = 1;
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
        // cppcheck-suppress knownConditionTrueFalse ; atomic stop flag checked across threads
        if (checkStop()) {
            return matches;
        }

        const MetadataMatchContext ctx{filePath, it.key(), it.value(), field_index};
        if (collectFieldMatches(ctx, regex, m_config.max_results, matches)) {
            return matches;
        }
        ++field_index;
    }

    return matches;
}

// -- File Metadata Search ----------------------------------------------------

namespace {

/// @brief Extract metadata from ZIP-based document formats (docx, xlsx, pptx,
///        odt, ods, odp, epub) by parsing the ZIP central directory and reading
///        well-known metadata XML files.
///
/// ZIP-based Office documents store metadata in:
///   - docProps/core.xml    (Dublin Core: title, subject, creator, keywords)
///   - docProps/app.xml     (Application: company, version, template)
///   - META-INF/container.xml (EPUB container)
///   - content.opf / OEBPS/content.opf (EPUB metadata)
///
/// ODF documents store metadata in meta.xml.
///
/// We parse ZIP Local File Headers to find and extract these specific entries.

bool isZipLocalFileHeader(const QByteArray& data, int offset) {
    return static_cast<uint8_t>(data[offset + kZipSignatureByte0]) == kZipSignaturePk0 &&
           static_cast<uint8_t>(data[offset + kZipSignatureByte1]) == kZipSignaturePk1 &&
           static_cast<uint8_t>(data[offset + kZipSignatureByte2]) == kZipSignatureLocal2 &&
           static_cast<uint8_t>(data[offset + kZipSignatureByte3]) == kZipSignatureLocal3;
}

bool isMetadataTarget(const QString& entry_name) {
    static const QStringList kMetadataFiles = {
        "docprops/core.xml", "docprops/app.xml", "meta.xml", "content.opf", "oebps/content.opf"};
    const QString lower = entry_name.toLower();
    return std::any_of(kMetadataFiles.begin(),
                       kMetadataFiles.end(),
                       [&lower](const QString& target) { return lower.endsWith(target); });
}

constexpr auto kXmlMetadataTagPattern = "<(?:dc:|cp:|dcterms:|meta:)?(\\w+)[^>]*>([^<]+)</";
constexpr auto kPdfInfoPattern = "/(\\w+)\\s*\\(([^)]{0,500})\\)";
constexpr int kId3HeaderSize = 10;
constexpr int kId3FrameIdSize = 4;
constexpr int kId3FrameHeaderSize = 10;
constexpr int kId3FrameSizeOffset = 4;
constexpr int kId3FrameDataOffset = 11;
constexpr std::uint32_t kId3FrameValueMaxBytes = 1000;
constexpr std::uint8_t kId3SynchsafeMask = 0x7F;
constexpr std::uint32_t kId3SynchsafeByte3Shift = 21;
constexpr std::uint32_t kId3SynchsafeByte2Shift = 14;
constexpr std::uint32_t kId3SynchsafeByte1Shift = 7;
constexpr std::uint32_t kByte3Shift = 24;
constexpr std::uint32_t kByte2Shift = 16;
constexpr std::uint32_t kByte1Shift = 8;
constexpr std::uint8_t kId3EncodingLatin1 = 0;
constexpr std::uint8_t kId3EncodingUtf16 = 1;
constexpr std::uint8_t kId3EncodingUtf16Be = 2;
constexpr std::uint8_t kId3EncodingUtf8 = 3;
constexpr qint64 kMaxMetadataRead = 512 * 1024;
constexpr qint64 kMaxZipMetadataRead = 10LL * 1024 * 1024;
constexpr qint64 kMaxArchiveSize = 100LL * 1024 * 1024;
constexpr int kZipLocalHeaderSize = 30;
constexpr int kZipMethodOffset = 8;
constexpr int kZipCompressedSizeOffset = 18;
constexpr int kZipUncompressedSizeOffset = 22;
constexpr int kZipNameLengthOffset = 26;
constexpr int kZipExtraLengthOffset = 28;
constexpr std::uint16_t kZipMethodStored = 0;
constexpr std::uint16_t kZipMethodDeflated = 8;
constexpr std::uint32_t kMaxArchiveTextEntrySize = 10u * 1024u * 1024u;
constexpr auto kArchiveEntryPrefix = "[Archive Entry] ";

void extractXmlTags(const QString& xml_text, QMap<QString, QString>& metadata) {
    static const QRegularExpression tagPattern(QString::fromLatin1(kXmlMetadataTagPattern),
                                               QRegularExpression::CaseInsensitiveOption);

    constexpr int kMaxValueLength = 1000;
    auto tag_iter = tagPattern.globalMatch(xml_text);
    while (tag_iter.hasNext()) {
        auto tag_match = tag_iter.next();
        const QString key = tag_match.captured(1).trimmed();
        const QString value = tag_match.captured(2).trimmed();
        if (!key.isEmpty() && !value.isEmpty() && value.length() < kMaxValueLength) {
            const QString display_key = key[0].toUpper() + key.mid(1);
            metadata.insert(display_key, value);
        }
    }
}

QByteArray decompressZipEntry(
    const QByteArray& file_data, int offset, int data_start, int entry_size, uint16_t comp_method) {
    if (comp_method == 0) {
        return file_data.mid(data_start, entry_size);
    }
    const uint32_t uncomp_size = readU32(file_data.constData() + offset + 22, true);
    return inflateZipEntry(file_data.mid(data_start, entry_size), uncomp_size);
}

[[nodiscard]] QMap<QString, QString> extractZipXmlMetadata(const QByteArray& fileData) {
    QMap<QString, QString> metadata;
    int offset = 0;
    const int data_size = fileData.size();

    while (offset + kZipLocalHeaderSize < data_size) {
        if (!isZipLocalFileHeader(fileData, offset)) {
            break;
        }

        const uint16_t name_len = readU16(fileData.constData() + offset + 26, true);
        const uint16_t extra_len = readU16(fileData.constData() + offset + 28, true);
        const uint32_t comp_size = readU32(fileData.constData() + offset + 18, true);
        const uint16_t comp_method = readU16(fileData.constData() + offset + 8, true);

        if (offset + kZipLocalHeaderSize + name_len > data_size) {
            break;
        }

        const QString entry_name = QString::fromLatin1(fileData.constData() + offset + 30,
                                                       name_len);
        const int data_start = offset + 30 + name_len + extra_len;
        const int entry_size = static_cast<int>(comp_size);

        if (isMetadataTarget(entry_name) &&
            (comp_method == kZipMethodStored || comp_method == kZipMethodDeflated) &&
            data_start + entry_size <= data_size) {
            QByteArray xml_data =
                decompressZipEntry(fileData, offset, data_start, entry_size, comp_method);
            if (!xml_data.isEmpty()) {
                extractXmlTags(QString::fromUtf8(xml_data), metadata);
            }
        }

        offset = data_start + entry_size;
    }

    return metadata;
}

/// @brief Extract basic metadata from a PDF file
///        Parses the PDF /Info dictionary for Title, Author, Subject, etc.
[[nodiscard]] QMap<QString, QString> extractPdfMetadata(const QByteArray& fileData) {
    QMap<QString, QString> metadata;

    // PDF Info dictionary entries look like: /Title (Some Title)
    // or /Title <hex string>
    static const QRegularExpression infoPattern(QString::fromLatin1(kPdfInfoPattern),
                                                QRegularExpression::MultilineOption);

    // Only scan the last 4KB of the file (xref/trailer area) plus first 4KB
    // to find the Info dictionary reference, then search relevant sections
    constexpr qsizetype kPdfScanMaxBytes = 32 * 1024;
    const qsizetype scanSizeBytes = std::min(kPdfScanMaxBytes, fileData.size());
    const int scanSize = static_cast<int>(scanSizeBytes);
    const QString pdfText = QString::fromLatin1(fileData.left(scanSize));

    auto matchIter = infoPattern.globalMatch(pdfText);
    while (matchIter.hasNext()) {
        auto m = matchIter.next();
        const QString key = m.captured(1);
        const QString value = m.captured(2).trimmed();

        // Only capture known PDF info keys
        static const QSet<QString> kPdfInfoKeys = {"Title",
                                                   "Author",
                                                   "Subject",
                                                   "Keywords",
                                                   "Creator",
                                                   "Producer",
                                                   "CreationDate",
                                                   "ModDate"};

        if (kPdfInfoKeys.contains(key) && !value.isEmpty()) {
            metadata.insert(key, value);
        }
    }

    return metadata;
}

bool hasId3v2Header(const QByteArray& file_data) {
    return file_data.size() >= kId3HeaderSize && file_data[0] == 'I' && file_data[1] == 'D' &&
           file_data[kId3SignatureByte2] == '3';
}

std::uint32_t readId3TagSize(const QByteArray& file_data) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_data[kId3SizeByte0]) &
                                       kId3SynchsafeMask)
            << kId3SynchsafeByte3Shift) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_data[kId3SizeByte1]) &
                                       kId3SynchsafeMask)
            << kId3SynchsafeByte2Shift) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_data[kId3SizeByte2]) &
                                       kId3SynchsafeMask)
            << kId3SynchsafeByte1Shift) |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_data[kId3SizeByte3]) &
                                      kId3SynchsafeMask);
}

std::uint32_t readId3FrameSize(const QByteArray& file_data, int offset) {
    const int size_offset = offset + kId3FrameSizeOffset;
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_data[size_offset]))
            << kByte3Shift) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(file_data[size_offset + 1]))
            << kByte2Shift) |
           (static_cast<std::uint32_t>(
                static_cast<std::uint8_t>(file_data[size_offset + kByteIndex2]))
            << kByte1Shift) |
           static_cast<std::uint32_t>(
               static_cast<std::uint8_t>(file_data[size_offset + kByteIndex3]));
}

const QMap<QByteArray, QString>& id3FrameNames() {
    static const QMap<QByteArray, QString> kFrameNames = {{"TIT2", "Title"},
                                                          {"TPE1", "Artist"},
                                                          {"TALB", "Album"},
                                                          {"TYER", "Year"},
                                                          {"TDRC", "RecordingDate"},
                                                          {"TRCK", "Track"},
                                                          {"TCON", "Genre"},
                                                          {"COMM", "Comment"},
                                                          {"TPE2", "AlbumArtist"},
                                                          {"TCOM", "Composer"},
                                                          {"TPUB", "Publisher"},
                                                          {"TCOP", "Copyright"}};
    return kFrameNames;
}

bool canReadId3Frame(const QByteArray& file_data,
                     int offset,
                     int max_offset,
                     const QByteArray& frame_id,
                     std::uint32_t frame_size) {
    if (frame_size == 0 || frame_id[0] == '\0') {
        return false;
    }
    if (offset + kId3FrameHeaderSize + static_cast<int>(frame_size) > max_offset) {
        return false;
    }
    return offset + kId3FrameDataOffset <= file_data.size();
}

bool shouldCaptureId3Frame(const QByteArray& frame_id, std::uint32_t frame_size) {
    return id3FrameNames().contains(frame_id) && frame_size > 1 &&
           frame_size < kId3FrameValueMaxBytes;
}

QString decodeId3FrameValue(const QByteArray& frame_data, std::uint8_t encoding) {
    switch (encoding) {
    case kId3EncodingLatin1:
        return QString::fromLatin1(frame_data).trimmed();
    case kId3EncodingUtf16:
    case kId3EncodingUtf16Be:
        return QString::fromUtf16(reinterpret_cast<const char16_t*>(frame_data.constData()),
                                  frame_data.size() / kUtf16CodeUnitBytes)
            .trimmed();
    case kId3EncodingUtf8:
        return QString::fromUtf8(frame_data).trimmed();
    default:
        return QString::fromLatin1(frame_data).trimmed();
    }
}

void captureId3Frame(const QByteArray& file_data,
                     int offset,
                     const QByteArray& frame_id,
                     std::uint32_t frame_size,
                     QMap<QString, QString>& metadata) {
    const auto encoding = static_cast<std::uint8_t>(file_data[offset + kId3FrameHeaderSize]);
    const QByteArray frame_data = file_data.mid(offset + kId3FrameDataOffset,
                                                static_cast<int>(frame_size - 1));
    QString value = decodeId3FrameValue(frame_data, encoding).remove(QChar('\0'));
    if (!value.isEmpty()) {
        metadata.insert(id3FrameNames()[frame_id], value);
    }
}

/// @brief Extract ID3v2 metadata from MP3 files
[[nodiscard]] QMap<QString, QString> extractMp3Metadata(const QByteArray& fileData) {
    QMap<QString, QString> metadata;

    if (!hasId3v2Header(fileData)) {
        return metadata;
    }

    // ID3v2 size is stored as synchsafe integer (4 bytes, 7 bits each)
    const uint32_t tagSize = readId3TagSize(fileData);
    const auto tagSizeBytes = static_cast<qsizetype>(tagSize + kId3HeaderSize);
    const qsizetype maxOffsetBytes = std::min(tagSizeBytes, fileData.size());
    const int maxOffset = static_cast<int>(maxOffsetBytes);

    int offset = kId3HeaderSize;
    while (offset + kId3FrameHeaderSize < maxOffset) {
        const QByteArray frameId = fileData.mid(offset, kId3FrameIdSize);
        const uint32_t frameSize = readId3FrameSize(fileData, offset);

        if (!canReadId3Frame(fileData, offset, maxOffset, frameId, frameSize)) {
            break;
        }

        if (shouldCaptureId3Frame(frameId, frameSize)) {
            captureId3Frame(fileData, offset, frameId, frameSize, metadata);
        }

        offset += kId3FrameHeaderSize + static_cast<int>(frameSize);
    }

    return metadata;
}

bool isZipMetadataExtension(const QString& ext) {
    static const QSet<QString> kZipMetadataExtensions = {
        "docx", "xlsx", "pptx", "odt", "ods", "odp", "epub"};
    return kZipMetadataExtensions.contains(ext);
}

QMap<QString, QString> extractZipMetadataFromFile(const QString& file_path) {
    QFile full_file(file_path);
    if (!full_file.open(QIODevice::ReadOnly)) {
        return {};
    }

    if (full_file.size() > kMaxZipMetadataRead) {
        return {};
    }

    return extractZipXmlMetadata(full_file.readAll());
}

QMap<QString, QString> extractMetadataForExtension(const QString& file_path,
                                                   const QString& ext,
                                                   const QByteArray& file_data) {
    if (ext == "pdf") {
        return extractPdfMetadata(file_data);
    }
    if (isZipMetadataExtension(ext)) {
        return extractZipMetadataFromFile(file_path);
    }
    if (ext == "mp3") {
        return extractMp3Metadata(file_data);
    }
    return {};
}

void appendFilesystemMetadata(const QString& file_path,
                              const QString& ext,
                              QMap<QString, QString>& metadata) {
    const QFileInfo info(file_path);
    metadata.insert("FileName", info.fileName());
    metadata.insert("FileSize", QString("%1 bytes").arg(info.size()));
    metadata.insert("FileType", ext.toUpper());
    metadata.insert("LastModified", info.lastModified().toString(Qt::ISODate));
    metadata.insert("Created", info.birthTime().toString(Qt::ISODate));
}

bool isArchiveTextExtension(const QString& ext) {
    static const QSet<QString> kArchiveTextExts = {
        "txt",  "md",  "csv", "json",  "xml",  "html", "htm", "css",  "js",  "py",
        "cpp",  "c",   "h",   "hpp",   "java", "rs",   "go",  "ts",   "tsx", "jsx",
        "rb",   "pl",  "sh",  "bat",   "ps1",  "yaml", "yml", "toml", "ini", "cfg",
        "conf", "log", "sql", "xhtml", "svg",  "opf",  "ncx"};
    return kArchiveTextExts.contains(ext);
}

bool isArchiveTextCompression(std::uint16_t method) {
    return method == kZipMethodStored || method == kZipMethodDeflated;
}

}  // anonymous namespace

QVector<SearchMatch> AdvancedSearchWorker::searchFileMetadata(const QString& filePath,
                                                              const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    const QByteArray fileData = file.read(std::min(file.size(), kMaxMetadataRead));
    file.close();

    if (checkStop()) {
        return matches;
    }

    const QString ext = QFileInfo(filePath).suffix().toLower();
    QMap<QString, QString> metadata = extractMetadataForExtension(filePath, ext, fileData);
    appendFilesystemMetadata(filePath, ext, metadata);

    // cppcheck-suppress knownConditionTrueFalse ; atomic stop flag checked across threads
    if (checkStop()) {
        return matches;
    }

    return collectMetadataMatches(filePath, regex, metadata);
}

QVector<SearchMatch> AdvancedSearchWorker::collectMetadataMatches(
    const QString& file_path,
    const QRegularExpression& regex,
    const QMap<QString, QString>& metadata) const {
    QVector<SearchMatch> matches;
    int fieldIndex = 1;
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
        // cppcheck-suppress knownConditionTrueFalse ; atomic stop flag checked across threads
        if (checkStop()) {
            return matches;
        }

        const QString& value = it.value();
        const MetadataMatchContext ctx{file_path, it.key(), value, fieldIndex};

        if (collectFieldMatches(ctx, regex, m_config.max_results, matches)) {
            return matches;
        }

        ++fieldIndex;
    }

    return matches;
}

// -- Archive Content Search ---------------------------------------------------

std::optional<AdvancedSearchWorker::ArchiveEntry> AdvancedSearchWorker::readArchiveEntry(
    const QByteArray& archive_data, const QString& file_path, int offset) const {
    const int data_size = archive_data.size();
    if (offset + kZipLocalHeaderSize > data_size || !isZipLocalFileHeader(archive_data, offset)) {
        return std::nullopt;
    }

    const uint16_t compMethod = readU16(archive_data.constData() + offset + kZipMethodOffset, true);
    const uint32_t compSize = readU32(archive_data.constData() + offset + kZipCompressedSizeOffset,
                                      true);
    const uint32_t uncompSize =
        readU32(archive_data.constData() + offset + kZipUncompressedSizeOffset, true);
    const uint16_t nameLen = readU16(archive_data.constData() + offset + kZipNameLengthOffset,
                                     true);
    const uint16_t extraLen = readU16(archive_data.constData() + offset + kZipExtraLengthOffset,
                                      true);

    if (offset + kZipLocalHeaderSize + nameLen > data_size) {
        return std::nullopt;
    }

    ArchiveEntry entry;
    entry.name = QString::fromUtf8(archive_data.constData() + offset + kZipLocalHeaderSize,
                                   nameLen);
    entry.path = QString("%1!/%2").arg(file_path, entry.name);
    entry.data_start = offset + kZipLocalHeaderSize + nameLen + extraLen;
    entry.entry_size = static_cast<int>(compSize);
    entry.next_offset = entry.data_start + entry.entry_size;
    entry.compression_method = compMethod;
    entry.uncompressed_size = uncompSize;
    return entry;
}

bool AdvancedSearchWorker::appendArchiveNameMatches(const ArchiveEntry& entry,
                                                    const QRegularExpression& regex,
                                                    QVector<SearchMatch>& matches) const {
    const QString archive_entry_prefix = QString::fromLatin1(kArchiveEntryPrefix);
    auto nameMatchIter = regex.globalMatch(entry.name);
    while (nameMatchIter.hasNext()) {
        auto regexMatch = nameMatchIter.next();

        SearchMatch match;
        match.file_path = entry.path;
        match.line_number = 0;
        match.line_content = QString("%1%2").arg(archive_entry_prefix, entry.name);
        match.match_start = archive_entry_prefix.length() +
                            static_cast<int>(regexMatch.capturedStart());
        match.match_end = archive_entry_prefix.length() +
                          static_cast<int>(regexMatch.capturedEnd());
        matches.append(match);

        if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
            return true;
        }
    }
    return false;
}

bool AdvancedSearchWorker::appendArchiveLineMatches(const QString& archive_path,
                                                    const QStringList& lines,
                                                    const QRegularExpression& regex,
                                                    QVector<SearchMatch>& matches) const {
    for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        if (checkStop()) {
            return true;
        }

        const QString& line = lines[lineIdx];
        auto matchIter = regex.globalMatch(line);
        while (matchIter.hasNext()) {
            auto regexMatch = matchIter.next();

            SearchMatch match;
            match.file_path = archive_path;
            match.line_number = lineIdx + 1;
            match.line_content = line;
            match.match_start = static_cast<int>(regexMatch.capturedStart());
            match.match_end = static_cast<int>(regexMatch.capturedEnd());

            for (int j = std::max(0, lineIdx - m_config.context_lines); j < lineIdx; ++j) {
                match.context_before.append(lines[j]);
            }
            for (int j = lineIdx + 1; j <= std::min(static_cast<int>(lines.size()) - 1,
                                                    lineIdx + m_config.context_lines);
                 ++j) {
                match.context_after.append(lines[j]);
            }

            matches.append(match);
            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return true;
            }
        }
    }
    return false;
}

bool AdvancedSearchWorker::appendArchiveContentMatches(const QByteArray& archive_data,
                                                       const ArchiveEntry& entry,
                                                       const QRegularExpression& regex,
                                                       QVector<SearchMatch>& matches) const {
    const QString entryExt = QFileInfo(entry.name).suffix().toLower();
    if (!isArchiveTextCompression(entry.compression_method) || !isArchiveTextExtension(entryExt)) {
        return false;
    }
    if (entry.data_start + entry.entry_size > archive_data.size() || entry.uncompressed_size == 0 ||
        entry.uncompressed_size >= kMaxArchiveTextEntrySize) {
        return false;
    }

    QByteArray entryData;
    if (entry.compression_method == kZipMethodStored) {
        entryData = archive_data.mid(entry.data_start, entry.entry_size);
    } else {
        entryData = inflateZipEntry(archive_data.mid(entry.data_start, entry.entry_size),
                                    entry.uncompressed_size);
    }

    if (entryData.isEmpty()) {
        return false;
    }

    return appendArchiveLineMatches(
        entry.path, QString::fromUtf8(entryData).split('\n'), regex, matches);
}

QVector<SearchMatch> AdvancedSearchWorker::searchArchive(const QString& filePath,
                                                         const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    if (file.size() > kMaxArchiveSize) {
        logWarning("AdvancedSearchWorker: archive '{}' too large ({} bytes), skipping",
                   filePath.toStdString(),
                   file.size());
        return matches;
    }

    const QByteArray archiveData = file.readAll();
    file.close();

    if (checkStop()) {
        return matches;
    }

    int offset = 0;
    const int dataSize = archiveData.size();
    while (offset + kZipLocalHeaderSize < dataSize) {
        if (checkStop()) {
            return matches;
        }

        const std::optional<ArchiveEntry> entry = readArchiveEntry(archiveData, filePath, offset);
        if (!entry.has_value()) {
            break;
        }

        if (appendArchiveNameMatches(*entry, regex, matches)) {
            return matches;
        }

        if (appendArchiveContentMatches(archiveData, *entry, regex, matches)) {
            return matches;
        }

        offset = entry->next_offset;
    }

    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchBinary(const QString& filePath,
                                                        const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    // Size guard -- refuse to load extremely large files into memory
    const qint64 maxBinarySize = m_config.max_file_size > 0 ? m_config.max_file_size
                                                            : kDefaultBinarySearchBytes;
    if (file.size() > maxBinarySize) {
        logWarning("AdvancedSearchWorker: binary file '{}' exceeds size limit ({} bytes), skipping",
                   filePath.toStdString(),
                   file.size());
        return matches;
    }

    // Read file as raw bytes
    const QByteArray content = file.readAll();
    file.close();

    // Search as UTF-8 text (binary files may contain text segments)
    const QString textContent = QString::fromUtf8(content);
    auto matchIter = regex.globalMatch(textContent);

    while (matchIter.hasNext()) {
        auto regexMatch = matchIter.next();

        SearchMatch match;
        match.file_path = filePath;
        match.line_number = static_cast<int>(regexMatch.capturedStart());  // byte offset
        match.line_content = regexMatch.captured();
        match.match_start = 0;
        match.match_end = static_cast<int>(regexMatch.capturedLength());

        // Provide hex context (16 bytes before and after)
        const int start = std::max(0, static_cast<int>(regexMatch.capturedStart()) - 16);
        const int end = std::min(static_cast<int>(content.size()),
                                 static_cast<int>(regexMatch.capturedEnd()) + 16);
        const QByteArray context = content.mid(start, end - start);
        match.context_before.append(QString("Hex: %1").arg(QString(context.toHex(' '))));

        matches.append(match);

        if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
            break;
        }
    }

    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchBinaryBytes(
    const QString& file_path,
    const QByteArray& data,
    const QRegularExpression& regex) const {
    QVector<SearchMatch> matches;
    const QString textContent = QString::fromUtf8(data);
    auto matchIter = regex.globalMatch(textContent);
    while (matchIter.hasNext()) {
        const auto regexMatch = matchIter.next();

        SearchMatch match;
        match.file_path = file_path;
        match.line_number = static_cast<int>(regexMatch.capturedStart());
        match.line_content = regexMatch.captured();
        match.match_start = 0;
        match.match_end = static_cast<int>(regexMatch.capturedLength());

        constexpr int kBinaryContextBytes = 16;
        const int start =
            std::max(0, static_cast<int>(regexMatch.capturedStart()) - kBinaryContextBytes);
        const int end = std::min(static_cast<int>(data.size()),
                                 static_cast<int>(regexMatch.capturedEnd()) +
                                     kBinaryContextBytes);
        match.context_before.append(QString("Hex: %1").arg(QString(data.mid(start, end - start).toHex(' '))));
        matches.append(match);

        if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
            break;
        }
    }
    return matches;
}

}  // namespace sak
