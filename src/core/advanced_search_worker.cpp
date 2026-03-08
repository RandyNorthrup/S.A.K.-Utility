// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_worker.cpp
/// @brief Implements directory-recursive file content search on a worker thread

#include "sak/advanced_search_worker.h"
#include "sak/logger.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QTextStream>

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace sak {

// ── Zlib Inflate Helper ──────────────────────────────────────────────────────

/// @brief Decompress a deflate-compressed (method 8) ZIP entry using zlib.
/// @param compressedData  The raw deflated bytes from the ZIP entry.
/// @param expectedSize    The uncompressed size from the ZIP header.
/// @return The decompressed bytes, or an empty QByteArray on failure.
[[nodiscard]] static QByteArray inflateZipEntry(
    const QByteArray& compressedData,
    uint32_t expectedSize)
{
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
    strm.next_in  = reinterpret_cast<Bytef*>(
        const_cast<char*>(compressedData.constData()));
    strm.avail_in = static_cast<uInt>(compressedData.size());
    strm.next_out  = reinterpret_cast<Bytef*>(output.data());
    strm.avail_out = static_cast<uInt>(expectedSize);

    // -MAX_WBITS → raw deflate (no zlib/gzip header), which is what ZIP uses
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

// ── Construction ────────────────────────────────────────────────────────────

AdvancedSearchWorker::AdvancedSearchWorker(SearchConfig config, QObject* parent)
    : WorkerBase(parent)
    , m_config(std::move(config))
{
}

// ── Regex Compilation ───────────────────────────────────────────────────────

auto AdvancedSearchWorker::compileRegex() const
    -> std::expected<QRegularExpression, QString>
{
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

    QRegularExpression::PatternOptions opts =
        QRegularExpression::DontCaptureOption;

    if (!m_config.case_sensitive) {
        opts |= QRegularExpression::CaseInsensitiveOption;
    }

    QRegularExpression regex(regexPattern, opts);

    if (!regex.isValid()) {
        return std::unexpected(
            QString("Invalid regex: %1 at offset %2")
                .arg(regex.errorString())
                .arg(regex.patternErrorOffset()));
    }

    return regex;
}

// ── Exclusion & Filtering ───────────────────────────────────────────────────

bool AdvancedSearchWorker::isExcluded(const QString& path) const
{
    for (const auto& excludeRegex : m_compiled_excludes) {
        if (excludeRegex.match(path).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool AdvancedSearchWorker::matchesExtensionFilter(const QString& filePath) const
{
    if (m_config.file_extensions.isEmpty()) {
        return true; // No filter = accept all
    }

    const QString ext = QFileInfo(filePath).suffix().toLower();
    for (const auto& filter : m_config.file_extensions) {
        QString normalized = filter.trimmed().toLower();
        if (normalized.startsWith('.')) {
            normalized = normalized.mid(1);
        }
        if (ext == normalized) {
            return true;
        }
    }
    return false;
}

// ── Network Path Detection ──────────────────────────────────────────────────

bool AdvancedSearchWorker::isNetworkPath(const QString& path) const
{
    return path.startsWith("\\\\") || path.startsWith("//");
}

bool AdvancedSearchWorker::checkNetworkPathAccessible(const QString& path) const
{
    // Simple accessibility check — try to list directory contents
    const QFileInfo info(path);
    return info.exists() && info.isReadable();
}

// ── Main Search Execution ───────────────────────────────────────────────────

auto AdvancedSearchWorker::prepareSearchConfig()
    -> std::expected<QRegularExpression, sak::error_code>
{
    auto regexResult = compileRegex();
    if (!regexResult) {
        logError("AdvancedSearchWorker: regex compilation "
                 "failed: {}",
                 regexResult.error().toStdString());
        return std::unexpected(
            sak::error_code::invalid_argument);
    }
    const auto& regex = regexResult.value();

    // Compile exclusion patterns once
    m_compiled_excludes.clear();
    for (const auto& pattern : m_config.exclude_patterns) {
        QRegularExpression excl(
            pattern, QRegularExpression::CaseInsensitiveOption);
        if (excl.isValid()) {
            m_compiled_excludes.append(excl);
        }
    }

    // Check network accessibility
    if (isNetworkPath(m_config.root_path)) {
        if (!checkNetworkPathAccessible(m_config.root_path)) {
            logError(
                "AdvancedSearchWorker: network path "
                "not accessible: {}",
                m_config.root_path.toStdString());
            return std::unexpected(
                sak::error_code::network_unavailable);
        }
    }

    return regex;
}

void AdvancedSearchWorker::runDirectorySearch(
    const QRegularExpression& regex,
    int& total_matches, int& total_files)
{
    QDirIterator it(m_config.root_path,
                    QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    int fileCount = 0;
    QVector<SearchMatch> batchMatches;

    while (it.hasNext()) {
        if (checkStop()) {
            logInfo("AdvancedSearchWorker: search cancelled "
                    "after {} files", fileCount);
            return;
        }

        const QString filePath = it.next();

        if (isExcluded(filePath)) { continue; }
        if (!matchesExtensionFilter(filePath)) { continue; }

        const QFileInfo fileInfo(filePath);
        if (m_config.max_file_size > 0
            && fileInfo.size() > m_config.max_file_size) {
            continue;
        }

        auto matches = searchFile(filePath, regex);
        if (!matches.isEmpty()) {
            Q_EMIT fileSearched(filePath, matches.size());
            batchMatches.append(matches);
            total_matches += matches.size();
            total_files++;
        }

        fileCount++;

        if (m_config.max_results > 0
            && total_matches >= m_config.max_results) {
            logInfo("AdvancedSearchWorker: max results limit "
                    "({}) reached", m_config.max_results);
            break;
        }

        if (fileCount % kBatchSize == 0
            && !batchMatches.isEmpty()) {
            Q_EMIT resultsReady(batchMatches);
            batchMatches.clear();
        }

        constexpr int kProgressInterval = 100;
        if (fileCount % kProgressInterval == 0) {
            reportProgress(fileCount, 0,
                QString("Searching... %1 files scanned, "
                        "%2 matches found")
                    .arg(fileCount).arg(total_matches));
        }
    }

    if (!batchMatches.isEmpty()) {
        Q_EMIT resultsReady(batchMatches);
    }

    reportProgress(fileCount, fileCount,
        QString("Search complete: %1 matches in %2 files "
                "(%3 files scanned)")
            .arg(total_matches).arg(total_files).arg(fileCount));
}

auto AdvancedSearchWorker::execute() -> std::expected<void, sak::error_code>
{
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

    // Single file search — no directory iteration needed
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
        reportProgress(1, 1,
            QString("Search complete: %1 matches in %2 files")
                .arg(totalMatches).arg(totalFiles));
        return {};
    }

    runDirectorySearch(regex, totalMatches, totalFiles);

    logInfo("AdvancedSearchWorker: search complete — "
            "{} matches in {} files",
            totalMatches, totalFiles);

    return {};
}

// ── File Search Dispatcher ──────────────────────────────────────────────────

QVector<SearchMatch> AdvancedSearchWorker::searchFile(
    const QString& filePath,
    const QRegularExpression& regex)
{
    QVector<SearchMatch> matches;
    const QString ext = QFileInfo(filePath).suffix().toLower();

    bool handledAsSpecial = false;

    // Image metadata search
    if (m_config.search_image_metadata && kImageExtensions.contains(ext)) {
        matches.append(searchImageMetadata(filePath, regex));
        handledAsSpecial = true;
    }

    // File metadata search
    if (m_config.search_file_metadata && kFileMetadataExtensions.contains(ext)) {
        matches.append(searchFileMetadata(filePath, regex));
        handledAsSpecial = true;
    }

    // Archive search
    if (m_config.search_in_archives && kArchiveExtensions.contains(ext)) {
        matches.append(searchArchive(filePath, regex));
        handledAsSpecial = true;
    }

    // Binary/hex search — exclusive mode (binary files aren't text-searched)
    if (m_config.hex_search) {
        matches.append(searchBinary(filePath, regex));
        return matches;
    }

    // Text content search — always runs on non-special files, and also runs on
    // files that happen to have metadata extensions (e.g., search text in .json)
    if (!handledAsSpecial || !kArchiveExtensions.contains(ext)) {
        // Skip text search on pure binary image formats
        static const QSet<QString> kBinaryImageExts = {
            "jpg", "jpeg", "png", "tiff", "tif", "gif", "bmp", "webp"
        };
        if (!kBinaryImageExts.contains(ext)) {
            matches.append(searchTextContent(filePath, regex));
        }
    }

    return matches;
}

// ── Text Content Search ─────────────────────────────────────────────────────

QVector<SearchMatch> AdvancedSearchWorker::searchTextContent(
    const QString& filePath,
    const QRegularExpression& regex)
{
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
        if (lines.size() > 500000) {
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
        auto matchIter = regex.globalMatch(line);

        while (matchIter.hasNext()) {
            auto regexMatch = matchIter.next();

            SearchMatch match;
            match.file_path = filePath;
            match.line_number = i + 1; // 1-based
            match.line_content = line;
            match.match_start = static_cast<int>(regexMatch.capturedStart());
            match.match_end = static_cast<int>(regexMatch.capturedEnd());

            // Extract context lines before
            const int ctxBefore = m_config.context_lines;
            for (int j = std::max(0, i - ctxBefore); j < i; ++j) {
                match.context_before.append(lines[j]);
            }

            // Extract context lines after
            const int ctxAfter = m_config.context_lines;
            const int lastLine = static_cast<int>(lines.size()) - 1;
            const int end = std::min(lastLine, i + ctxAfter);
            for (int j = i + 1; j <= end; ++j) {
                match.context_after.append(lines[j]);
            }

            matches.append(match);

            // Check max results limit per file to avoid runaway
            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }
    }

    return matches;
}

// ── Image Metadata Search ───────────────────────────────────────────────────

namespace {

/// @brief Read a 16-bit big-endian unsigned integer from raw bytes
[[nodiscard]] inline uint16_t readBE16(const char* data)
{
    return static_cast<uint16_t>(
        (static_cast<uint8_t>(data[0]) << 8) |
         static_cast<uint8_t>(data[1]));
}

/// @brief Read a 32-bit unsigned integer respecting byte order
[[nodiscard]] inline uint32_t readU32(const char* data, bool littleEndian)
{
    if (littleEndian) {
        return static_cast<uint32_t>(static_cast<uint8_t>(data[0]))
             | (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) <<  8)
             | (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
    }
    return (static_cast<uint32_t>(static_cast<uint8_t>(data[0])) << 24)
         | (static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(data[2])) <<  8)
         |  static_cast<uint32_t>(static_cast<uint8_t>(data[3]));
}

/// @brief Read a 16-bit unsigned integer respecting byte order
[[nodiscard]] inline uint16_t readU16(const char* data, bool littleEndian)
{
    if (littleEndian) {
        return static_cast<uint16_t>(
            static_cast<uint8_t>(data[0]) |
            (static_cast<uint8_t>(data[1]) << 8));
    }
    return static_cast<uint16_t>(
        (static_cast<uint8_t>(data[0]) << 8) |
         static_cast<uint8_t>(data[1]));
}

/// @brief EXIF tag ID → human-readable name mapping
[[nodiscard]] QString exifTagName(uint16_t tag)
{
    switch (tag) {
        case 0x010E: return QStringLiteral("ImageDescription");
        case 0x010F: return QStringLiteral("CameraMake");
        case 0x0110: return QStringLiteral("CameraModel");
        case 0x0112: return QStringLiteral("Orientation");
        case 0x011A: return QStringLiteral("XResolution");
        case 0x011B: return QStringLiteral("YResolution");
        case 0x0128: return QStringLiteral("ResolutionUnit");
        case 0x0131: return QStringLiteral("Software");
        case 0x0132: return QStringLiteral("DateTime");
        case 0x013B: return QStringLiteral("Artist");
        case 0x0213: return QStringLiteral("YCbCrPositioning");
        case 0x8298: return QStringLiteral("Copyright");
        case 0x8769: return QStringLiteral("ExifOffset");
        case 0x8825: return QStringLiteral("GPSInfo");
        case 0x829A: return QStringLiteral("ExposureTime");
        case 0x829D: return QStringLiteral("FNumber");
        case 0x8827: return QStringLiteral("ISOSpeed");
        case 0x9000: return QStringLiteral("ExifVersion");
        case 0x9003: return QStringLiteral("DateTimeOriginal");
        case 0x9004: return QStringLiteral("DateTimeDigitized");
        case 0x9209: return QStringLiteral("Flash");
        case 0x920A: return QStringLiteral("FocalLength");
        case 0xA001: return QStringLiteral("ColorSpace");
        case 0xA002: return QStringLiteral("PixelXDimension");
        case 0xA003: return QStringLiteral("PixelYDimension");
        case 0xA405: return QStringLiteral("FocalLengthIn35mm");
        case 0xA420: return QStringLiteral("ImageUniqueID");
        default:     return QString("Tag_0x%1").arg(tag, 4, 16, QChar('0'));
    }
}

/// @brief Parse EXIF IFD entries from TIFF data and collect key-value pairs
void parseExifIFD(const QByteArray& tiffData, uint32_t ifdOffset,
                  bool littleEndian, QMap<QString, QString>& metadata,
                  int depth = 0)
{
    // Guard against infinite recursion from malformed data
    if (depth > 4) return;

    const int dataSize = tiffData.size();
    if (static_cast<int>(ifdOffset + 2) > dataSize) return;

    const char* base = tiffData.constData();
    const uint16_t entryCount = readU16(base + ifdOffset, littleEndian);

    for (uint16_t i = 0; i < entryCount; ++i) {
        const uint32_t entryOffset = ifdOffset + 2 + (i * 12);
        if (static_cast<int>(entryOffset + 12) > dataSize) break;

        const char* entry = base + entryOffset;
        const uint16_t tag  = readU16(entry, littleEndian);
        const uint16_t type = readU16(entry + 2, littleEndian);
        const uint32_t count = readU32(entry + 4, littleEndian);

        const QString tagName = exifTagName(tag);

        // Calculate data size based on type
        int unitSize = 0;
        switch (type) {
            case 1: case 7: unitSize = 1; break;  // BYTE, UNDEFINED
            case 2:         unitSize = 1; break;  // ASCII
            case 3:         unitSize = 2; break;  // SHORT
            case 4:         unitSize = 4; break;  // LONG
            case 5:         unitSize = 8; break;  // RATIONAL
            case 9:         unitSize = 4; break;  // SLONG
            case 10:        unitSize = 8; break;  // SRATIONAL
            default:        continue;
        }

        const uint32_t totalBytes = count * static_cast<uint32_t>(unitSize);
        const char* valuePtr = nullptr;

        if (totalBytes <= 4) {
            valuePtr = entry + 8;
        } else {
            const uint32_t offset = readU32(entry + 8, littleEndian);
            if (static_cast<int>(offset + totalBytes) > dataSize) continue;
            valuePtr = base + offset;
        }

        // Sub-IFD pointers (ExifOffset, GPSInfo)
        if ((tag == 0x8769 || tag == 0x8825) && type == 4 && count == 1) {
            const uint32_t subIfdOffset = readU32(entry + 8, littleEndian);
            parseExifIFD(tiffData, subIfdOffset, littleEndian, metadata, depth + 1);
            continue;
        }

        // Extract value as string
        QString value;
        switch (type) {
            case 2: // ASCII
                value = QString::fromLatin1(valuePtr, static_cast<int>(count - 1)).trimmed();
                break;
            case 3: // SHORT
                if (count == 1) {
                    value = QString::number(readU16(valuePtr, littleEndian));
                }
                break;
            case 4: // LONG
                if (count == 1) {
                    value = QString::number(readU32(valuePtr, littleEndian));
                }
                break;
            case 5: { // RATIONAL
                if (count == 1) {
                    const uint32_t num = readU32(valuePtr, littleEndian);
                    const uint32_t den = readU32(valuePtr + 4, littleEndian);
                    if (den != 0) {
                        value = QString("%1/%2").arg(num).arg(den);
                    }
                }
                break;
            }
            case 7: // UNDEFINED — show as version string for small counts
                if (count <= 8) {
                    value = QString::fromLatin1(valuePtr, static_cast<int>(count)).trimmed();
                }
                break;
            default:
                break;
        }

        if (!value.isEmpty()) {
            metadata.insert(tagName, value);
        }
    }
}

/// @brief Extract EXIF metadata from a JPEG file
[[nodiscard]] QMap<QString, QString> extractJpegExif(const QByteArray& fileData)
{
    QMap<QString, QString> metadata;

    if (fileData.size() < 4) return metadata;

    // Check JPEG SOI marker (0xFF 0xD8)
    if (static_cast<uint8_t>(fileData[0]) != 0xFF ||
        static_cast<uint8_t>(fileData[1]) != 0xD8) {
        return metadata;
    }

    // Scan for APP1 marker (0xFF 0xE1)
    int offset = 2;
    while (offset + 4 < fileData.size()) {
        if (static_cast<uint8_t>(fileData[offset]) != 0xFF) break;

        const uint8_t marker = static_cast<uint8_t>(fileData[offset + 1]);

        // End markers
        if (marker == 0xDA || marker == 0xD9) break;

        const uint16_t segLen = readBE16(fileData.constData() + offset + 2);

        // APP1 = EXIF data
        if (marker == 0xE1 && segLen > 8) {
            const int exifStart = offset + 4;
            // Check "Exif\0\0" header
            if (fileData.mid(exifStart, 6) == QByteArray("Exif\0\0", 6)) {
                // TIFF data starts after the Exif header
                const int tiffStart = exifStart + 6;
                const QByteArray tiffData = fileData.mid(tiffStart, segLen - 8);

                if (tiffData.size() >= 8) {
                    // Determine byte order
                    bool littleEndian = (tiffData[0] == 'I' && tiffData[1] == 'I');

                    // Read IFD0 offset
                    const uint32_t ifd0Offset = readU32(tiffData.constData() + 4, littleEndian);
                    parseExifIFD(tiffData, ifd0Offset, littleEndian, metadata);
                }
            }
            break; // Only process first APP1
        }

        offset += 2 + segLen;
    }

    return metadata;
}

/// @brief Extract PNG text metadata chunks (tEXt, iTXt, zTXt)
[[nodiscard]] QMap<QString, QString> extractPngMetadata(const QByteArray& fileData)
{
    QMap<QString, QString> metadata;

    // PNG signature: 0x89 P N G \r \n 0x1A \n
    if (fileData.size() < 8) return metadata;
    static const char kPngSig[] = "\x89PNG\r\n\x1A\n";
    if (std::memcmp(fileData.constData(), kPngSig, 8) != 0) return metadata;

    int offset = 8;
    while (offset + 12 <= fileData.size()) {
        // Each chunk: 4-byte length (BE) + 4-byte type + data + 4-byte CRC
        const uint32_t chunkLen = readBE16(fileData.constData() + offset) * 65536u +
                                  readBE16(fileData.constData() + offset + 2);
        const QByteArray chunkType = fileData.mid(offset + 4, 4);

        if (chunkType == "tEXt" && static_cast<int>(chunkLen) > 0) {
            const int chunkLenInt = static_cast<int>(chunkLen);
            const QByteArray chunkData = fileData.mid(offset + 8, chunkLenInt);
            const int nullPos = chunkData.indexOf('\0');
            if (nullPos > 0) {
                const QString key = QString::fromLatin1(chunkData.left(nullPos));
                const QString val = QString::fromLatin1(chunkData.mid(nullPos + 1));
                metadata.insert(key, val);
            }
        } else if (chunkType == "iTXt" && static_cast<int>(chunkLen) > 0) {
            const int chunkLenInt = static_cast<int>(chunkLen);
            const QByteArray chunkData = fileData.mid(offset + 8, chunkLenInt);
            const int nullPos = chunkData.indexOf('\0');
            if (nullPos > 0) {
                const QString key = QString::fromLatin1(chunkData.left(nullPos));
                // iTXt has compression flag + method + lang + translated keyword + text
                // For simplicity, extract text after the 3rd null byte
                int textStart = nullPos + 1;
                for (int nullCount = 0;
                     nullCount < 3 && textStart < chunkData.size();
                     ++textStart) {
                    if (chunkData[textStart] == '\0') ++nullCount;
                }
                if (textStart < chunkData.size()) {
                    const QString val = QString::fromUtf8(chunkData.mid(textStart));
                    metadata.insert(key, val);
                }
            }
        } else if (chunkType == "IEND") {
            break;
        }

        offset += 8 + static_cast<int>(chunkLen) + 4; // length + type + data + CRC
    }

    return metadata;
}

/// @brief Create a SearchMatch from a metadata key-value pair
/// @param matchInKey true when the regex matched within the key name,
///                   false (default) when it matched within the value
[[nodiscard]] SearchMatch makeMetadataMatch(const QString& filePath,
                                            const QString& key,
                                            const QString& value,
                                            const QRegularExpressionMatch& regexMatch,
                                            int fieldIndex,
                                            bool matchInKey = false)
{
    SearchMatch match;
    match.file_path = filePath;
    match.line_number = fieldIndex;
    match.line_content = QString("[Metadata] %1: %2").arg(key, value);

    if (matchInKey) {
        // Match is within the key — offset from "[Metadata] " prefix (11 chars)
        constexpr int kMetadataPrefix = 11; // "[Metadata] ".length()
        match.match_start = kMetadataPrefix + static_cast<int>(regexMatch.capturedStart());
        match.match_end = kMetadataPrefix + static_cast<int>(regexMatch.capturedEnd());
    } else {
        // Match is within the value — offset past "[Metadata] key: " prefix
        const int prefix = QString("[Metadata] %1: ").arg(key).length();
        match.match_start = prefix + static_cast<int>(regexMatch.capturedStart());
        match.match_end = prefix + static_cast<int>(regexMatch.capturedEnd());
    }

    return match;
}

} // anonymous namespace

QVector<SearchMatch> AdvancedSearchWorker::searchImageMetadata(
    const QString& filePath,
    const QRegularExpression& regex)
{
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    // Read enough for EXIF parsing (first 256 KB is sufficient for metadata)
    constexpr qint64 kMaxMetadataRead = 256 * 1024;
    const QByteArray fileData = file.read(std::min(file.size(), kMaxMetadataRead));
    file.close();

    if (checkStop()) return matches;

    const QString ext = QFileInfo(filePath).suffix().toLower();
    QMap<QString, QString> metadata;

    // Parse format-specific metadata
    if (ext == "jpg" || ext == "jpeg") {
        metadata = extractJpegExif(fileData);
    } else if (ext == "png") {
        metadata = extractPngMetadata(fileData);
    }

    // Supplement with QImageReader metadata (works for many formats)
    {
        QImageReader reader(filePath);
        reader.setAutoDetectImageFormat(true);
        const QStringList keys = reader.textKeys();
        for (const auto& key : keys) {
            if (!metadata.contains(key)) {
                metadata.insert(key, reader.text(key));
            }
        }

        // Add image dimensions as metadata
        const QSize imgSize = reader.size();
        if (imgSize.isValid()) {
            metadata.insert("Width", QString::number(imgSize.width()));
            metadata.insert("Height", QString::number(imgSize.height()));
            metadata.insert("Dimensions",
                QString("%1x%2").arg(imgSize.width()).arg(imgSize.height()));
        }
    }

    // Add basic file info as metadata
    {
        const QFileInfo info(filePath);
        metadata.insert("FileName", info.fileName());
        metadata.insert("FileSize",
            QString("%1 bytes").arg(info.size()));
        metadata.insert("LastModified",
            info.lastModified().toString(Qt::ISODate));
    }

    if (checkStop()) return matches;

    // Search all metadata fields against the regex
    int fieldIndex = 1;
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
        if (checkStop()) return matches;

        const QString& value = it.value();
        auto matchIter = regex.globalMatch(value);

        while (matchIter.hasNext()) {
            auto regexMatch = matchIter.next();
            matches.append(
                makeMetadataMatch(filePath, it.key(), value, regexMatch, fieldIndex));

            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }

        // Also search the key name
        auto keyMatchIter = regex.globalMatch(it.key());
        while (keyMatchIter.hasNext()) {
            auto regexMatch = keyMatchIter.next();
            matches.append(
                makeMetadataMatch(filePath, it.key(), value, regexMatch, fieldIndex, true));

            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }

        ++fieldIndex;
    }

    return matches;
}

// ── File Metadata Search ────────────────────────────────────────────────────

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
[[nodiscard]] QMap<QString, QString> extractZipXmlMetadata(const QByteArray& fileData)
{
    QMap<QString, QString> metadata;

    // Target files we want to extract
    static const QStringList kMetadataFiles = {
        "docprops/core.xml",
        "docprops/app.xml",
        "meta.xml",
        "content.opf",
        "oebps/content.opf"
    };

    // Scan for Local File Headers (signature 0x04034b50)
    int offset = 0;
    const int dataSize = fileData.size();

    while (offset + 30 < dataSize) {
        // Check Local File Header signature (little-endian: 50 4B 03 04)
        if (static_cast<uint8_t>(fileData[offset])     != 0x50 ||
            static_cast<uint8_t>(fileData[offset + 1]) != 0x4B ||
            static_cast<uint8_t>(fileData[offset + 2]) != 0x03 ||
            static_cast<uint8_t>(fileData[offset + 3]) != 0x04) {
            break; // No more local file headers
        }

        const uint16_t nameLen = readU16(fileData.constData() + offset + 26, true);
        const uint16_t extraLen = readU16(fileData.constData() + offset + 28, true);
        const uint32_t compSize = readU32(fileData.constData() + offset + 18, true);
        const uint16_t compMethod = readU16(fileData.constData() + offset + 8, true);

        if (offset + 30 + nameLen > dataSize) break;

        const QString entryName = QString::fromLatin1(
            fileData.constData() + offset + 30, nameLen);

        const int dataStart = offset + 30 + nameLen + extraLen;
        const int entrySize = static_cast<int>(compSize);

        // Check if this is a metadata file we want (case-insensitive)
        const QString lowerName = entryName.toLower();
        bool isTarget = false;
        for (const auto& target : kMetadataFiles) {
            if (lowerName.endsWith(target)) {
                isTarget = true;
                break;
            }
        }

        if (isTarget && (compMethod == 0 || compMethod == 8)
            && dataStart + entrySize <= dataSize) {
            // Extract entry data — decompress if deflated
            QByteArray xmlData;
            if (compMethod == 0) {
                xmlData = fileData.mid(dataStart, entrySize);
            } else {
                const uint32_t uncompSize = readU32(
                    fileData.constData() + offset + 22, true);
                xmlData = inflateZipEntry(
                    fileData.mid(dataStart, entrySize), uncompSize);
                if (xmlData.isEmpty()) {
                    offset = dataStart + entrySize;
                    continue;
                }
            }
            const QString xmlText = QString::fromUtf8(xmlData);

            // Simple XML tag extraction (avoid full XML parser dependency)
            // Look for <dc:title>, <dc:creator>, <dc:subject>, <dc:description>,
            // <cp:keywords>, <dc:language>, <dcterms:created>, <dcterms:modified>,
            // <Application>, <Company>, etc.
            static const QRegularExpression tagPattern(
                R"(<(?:dc:|cp:|dcterms:|meta:)?(\w+)[^>]*>([^<]+)</)",
                QRegularExpression::CaseInsensitiveOption);

            auto tagIter = tagPattern.globalMatch(xmlText);
            while (tagIter.hasNext()) {
                auto tagMatch = tagIter.next();
                const QString key = tagMatch.captured(1).trimmed();
                const QString value = tagMatch.captured(2).trimmed();
                if (!key.isEmpty() && !value.isEmpty() && value.length() < 1000) {
                    // Capitalize key for display
                    const QString displayKey = key[0].toUpper() + key.mid(1);
                    metadata.insert(displayKey, value);
                }
            }
        }

        // Advance to next entry
        offset = dataStart + entrySize;
    }

    return metadata;
}

/// @brief Extract basic metadata from a PDF file
///        Parses the PDF /Info dictionary for Title, Author, Subject, etc.
[[nodiscard]] QMap<QString, QString> extractPdfMetadata(const QByteArray& fileData)
{
    QMap<QString, QString> metadata;

    // PDF Info dictionary entries look like: /Title (Some Title)
    // or /Title <hex string>
    static const QRegularExpression infoPattern(
        R"(/(\w+)\s*\(([^)]{0,500})\))",
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
        static const QSet<QString> kPdfInfoKeys = {
            "Title", "Author", "Subject", "Keywords", "Creator",
            "Producer", "CreationDate", "ModDate"
        };

        if (kPdfInfoKeys.contains(key) && !value.isEmpty()) {
            metadata.insert(key, value);
        }
    }

    return metadata;
}

/// @brief Extract ID3v2 metadata from MP3 files
[[nodiscard]] QMap<QString, QString> extractMp3Metadata(const QByteArray& fileData)
{
    QMap<QString, QString> metadata;

    if (fileData.size() < 10) return metadata;

    // Check ID3v2 header: "ID3"
    if (fileData[0] != 'I' || fileData[1] != 'D' || fileData[2] != '3') {
        return metadata;
    }

    // ID3v2 size is stored as synchsafe integer (4 bytes, 7 bits each)
    const uint32_t tagSize =
        (static_cast<uint32_t>(static_cast<uint8_t>(fileData[6]) & 0x7F) << 21) |
        (static_cast<uint32_t>(static_cast<uint8_t>(fileData[7]) & 0x7F) << 14) |
        (static_cast<uint32_t>(static_cast<uint8_t>(fileData[8]) & 0x7F) <<  7) |
         static_cast<uint32_t>(static_cast<uint8_t>(fileData[9]) & 0x7F);

    const auto tagSizeBytes = static_cast<qsizetype>(tagSize + 10);
    const qsizetype maxOffsetBytes = std::min(tagSizeBytes, fileData.size());
    const int maxOffset = static_cast<int>(maxOffsetBytes);
    int offset = 10;

    // ID3v2 frame mapping
    static const QMap<QByteArray, QString> kFrameNames = {
        {"TIT2", "Title"},    {"TPE1", "Artist"},   {"TALB", "Album"},
        {"TYER", "Year"},     {"TDRC", "RecordingDate"},
        {"TRCK", "Track"},    {"TCON", "Genre"},    {"COMM", "Comment"},
        {"TPE2", "AlbumArtist"}, {"TCOM", "Composer"},
        {"TPUB", "Publisher"}, {"TCOP", "Copyright"}
    };

    while (offset + 10 < maxOffset) {
        const QByteArray frameId = fileData.mid(offset, 4);

        // Frame size (4 bytes, big-endian for ID3v2.3, syncsafe for v2.4)
        const uint32_t frameSize =
            (static_cast<uint32_t>(static_cast<uint8_t>(fileData[offset + 4])) << 24) |
            (static_cast<uint32_t>(static_cast<uint8_t>(fileData[offset + 5])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(fileData[offset + 6])) <<  8) |
             static_cast<uint32_t>(static_cast<uint8_t>(fileData[offset + 7]));

        if (frameSize == 0 || frameId[0] == '\0') break;
        if (offset + 10 + static_cast<int>(frameSize) > maxOffset) break;

        if (kFrameNames.contains(frameId) && frameSize > 1 && frameSize < 1000) {
            // First byte is encoding: 0=ISO-8859-1, 1=UTF-16, 2=UTF-16BE, 3=UTF-8
            const uint8_t encoding = static_cast<uint8_t>(fileData[offset + 10]);
            const QByteArray frameData = fileData.mid(offset + 11,
                static_cast<int>(frameSize - 1));

            QString value;
            switch (encoding) {
                case 0:
                    value = QString::fromLatin1(frameData).trimmed();
                    break;
                case 1: // UTF-16 with BOM
                case 2: // UTF-16BE
                    value = QString::fromUtf16(
                        reinterpret_cast<const char16_t*>(frameData.constData()),
                        frameData.size() / 2).trimmed();
                    break;
                case 3:
                    value = QString::fromUtf8(frameData).trimmed();
                    break;
                default:
                    value = QString::fromLatin1(frameData).trimmed();
                    break;
            }

            // Remove null terminators
            value = value.remove(QChar('\0'));

            if (!value.isEmpty()) {
                metadata.insert(kFrameNames[frameId], value);
            }
        }

        offset += 10 + static_cast<int>(frameSize);
    }

    return metadata;
}

} // anonymous namespace

QVector<SearchMatch> AdvancedSearchWorker::searchFileMetadata(
    const QString& filePath,
    const QRegularExpression& regex)
{
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    // Limit read size for metadata extraction
    constexpr qint64 kMaxMetadataRead = 512 * 1024; // 512 KB for metadata
    const QByteArray fileData = file.read(std::min(file.size(), kMaxMetadataRead));
    file.close();

    if (checkStop()) return matches;

    const QString ext = QFileInfo(filePath).suffix().toLower();
    QMap<QString, QString> metadata;

    // Format-specific metadata extraction
    if (ext == "pdf") {
        metadata = extractPdfMetadata(fileData);
    } else if (ext == "docx" || ext == "xlsx" || ext == "pptx"
            || ext == "odt" || ext == "ods" || ext == "odp"
            || ext == "epub") {
        // Re-read full file for ZIP-based formats (central directory at end)
        QFile fullFile(filePath);
        if (fullFile.open(QIODevice::ReadOnly)) {
            const qint64 maxZip = 10LL * 1024 * 1024; // 10 MB limit for archive parsing
            if (fullFile.size() <= maxZip) {
                metadata = extractZipXmlMetadata(fullFile.readAll());
            }
            fullFile.close();
        }
    } else if (ext == "mp3") {
        metadata = extractMp3Metadata(fileData);
    }

    // Always add basic filesystem metadata
    {
        const QFileInfo info(filePath);
        metadata.insert("FileName", info.fileName());
        metadata.insert("FileSize", QString("%1 bytes").arg(info.size()));
        metadata.insert("FileType", ext.toUpper());
        metadata.insert("LastModified", info.lastModified().toString(Qt::ISODate));
        metadata.insert("Created", info.birthTime().toString(Qt::ISODate));
    }

    if (checkStop()) return matches;

    // Search all metadata fields against the regex
    int fieldIndex = 1;
    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
        if (checkStop()) return matches;

        const QString& value = it.value();
        auto matchIter = regex.globalMatch(value);

        while (matchIter.hasNext()) {
            auto regexMatch = matchIter.next();
            matches.append(
                makeMetadataMatch(filePath, it.key(), value, regexMatch, fieldIndex));

            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }

        // Also search the key name itself
        auto keyMatchIter = regex.globalMatch(it.key());
        while (keyMatchIter.hasNext()) {
            auto regexMatch = keyMatchIter.next();
            matches.append(
                makeMetadataMatch(filePath, it.key(), value, regexMatch, fieldIndex, true));

            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return matches;
            }
        }

        ++fieldIndex;
    }

    return matches;
}

// ── Archive Content Search ───────────────────────────────────────────────────

QVector<SearchMatch> AdvancedSearchWorker::searchArchive(
    const QString& filePath,
    const QRegularExpression& regex)
{
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    // Size limit for archive parsing
    constexpr qint64 kMaxArchiveSize = 100LL * 1024 * 1024; // 100 MB
    if (file.size() > kMaxArchiveSize) {
        logWarning("AdvancedSearchWorker: archive '{}' too large ({} bytes), skipping",
                   filePath.toStdString(), file.size());
        return matches;
    }

    const QByteArray archiveData = file.readAll();
    file.close();

    if (checkStop()) return matches;

    // Parse ZIP Local File Headers to extract and search entry contents
    // We look for the PK\x03\x04 signature (Local File Header)
    int offset = 0;
    const int dataSize = archiveData.size();

    // Text extensions worth searching inside archives
    static const QSet<QString> kArchiveTextExts = {
        "txt", "md", "csv", "json", "xml", "html", "htm", "css", "js",
        "py", "cpp", "c", "h", "hpp", "java", "rs", "go", "ts", "tsx",
        "jsx", "rb", "pl", "sh", "bat", "ps1", "yaml", "yml", "toml",
        "ini", "cfg", "conf", "log", "sql", "xhtml", "svg", "opf", "ncx"
    };

    int entryIndex = 0;

    while (offset + 30 < dataSize) {
        if (checkStop()) return matches;

        // Check Local File Header signature: PK\x03\x04
        if (static_cast<uint8_t>(archiveData[offset])     != 0x50 ||
            static_cast<uint8_t>(archiveData[offset + 1]) != 0x4B ||
            static_cast<uint8_t>(archiveData[offset + 2]) != 0x03 ||
            static_cast<uint8_t>(archiveData[offset + 3]) != 0x04) {
            break;
        }

        const uint16_t compMethod = readU16(archiveData.constData() + offset + 8, true);
        const uint32_t compSize   = readU32(archiveData.constData() + offset + 18, true);
        const uint32_t uncompSize = readU32(archiveData.constData() + offset + 22, true);
        const uint16_t nameLen    = readU16(archiveData.constData() + offset + 26, true);
        const uint16_t extraLen   = readU16(archiveData.constData() + offset + 28, true);

        if (offset + 30 + nameLen > dataSize) break;

        const QString entryName = QString::fromUtf8(
            archiveData.constData() + offset + 30, nameLen);

        const int dataStart = offset + 30 + nameLen + extraLen;
        const int entrySize = static_cast<int>(compSize);

        ++entryIndex;

        // First, search the entry filename against the regex
        const QString archivePath = QString("%1!/%2").arg(filePath, entryName);
        {
            auto nameMatchIter = regex.globalMatch(entryName);
            while (nameMatchIter.hasNext()) {
                auto regexMatch = nameMatchIter.next();

                SearchMatch match;
                match.file_path = archivePath;
                match.line_number = 0;
                match.line_content = QString("[Archive Entry] %1").arg(entryName);
                match.match_start = QString("[Archive Entry] ").length()
                    + static_cast<int>(regexMatch.capturedStart());
                match.match_end = QString("[Archive Entry] ").length()
                    + static_cast<int>(regexMatch.capturedEnd());

                matches.append(match);

                if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                    return matches;
                }
            }
        }

        // For stored or deflate-compressed text files, search the content
        const QString entryExt = QFileInfo(entryName).suffix().toLower();
        if ((compMethod == 0 || compMethod == 8)
            && kArchiveTextExts.contains(entryExt)
            && dataStart + entrySize <= dataSize
            && uncompSize > 0 && uncompSize < 10 * 1024 * 1024) {

            QByteArray entryData;
            if (compMethod == 0) {
                entryData = archiveData.mid(dataStart, entrySize);
            } else {
                entryData = inflateZipEntry(
                    archiveData.mid(dataStart, entrySize), uncompSize);
                if (entryData.isEmpty()) {
                    offset = dataStart + entrySize;
                    continue;
                }
            }
            const QString textContent = QString::fromUtf8(entryData);

            // Search line-by-line
            const QStringList lines = textContent.split('\n');
            for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
                if (checkStop()) return matches;

                const QString& line = lines[lineIdx];
                auto matchIter = regex.globalMatch(line);

                while (matchIter.hasNext()) {
                    auto regexMatch = matchIter.next();

                    SearchMatch match;
                    match.file_path = archivePath;
                    match.line_number = lineIdx + 1;
                    match.line_content = line;
                    match.match_start = static_cast<int>(regexMatch.capturedStart());
                    match.match_end = static_cast<int>(regexMatch.capturedEnd());

                    // Context lines
                    for (int j = std::max(0, lineIdx - m_config.context_lines);
                         j < lineIdx; ++j) {
                        match.context_before.append(lines[j]);
                    }
                    for (int j = lineIdx + 1;
                         j <= std::min(static_cast<int>(lines.size()) - 1,
                                       lineIdx + m_config.context_lines);
                         ++j) {
                        match.context_after.append(lines[j]);
                    }

                    matches.append(match);

                    if (m_config.max_results > 0
                        && matches.size() >= m_config.max_results) {
                        return matches;
                    }
                }
            }
        }

        // Advance to next entry
        offset = dataStart + entrySize;
    }

    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchBinary(
    const QString& filePath,
    const QRegularExpression& regex)
{
    QVector<SearchMatch> matches;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return matches;
    }

    // Size guard — refuse to load extremely large files into memory
    const qint64 maxBinarySize = m_config.max_file_size > 0
        ? m_config.max_file_size
        : (100LL * 1024 * 1024); // 100 MB fallback
    if (file.size() > maxBinarySize) {
        logWarning("AdvancedSearchWorker: binary file '{}' exceeds size limit ({} bytes), skipping",
                   filePath.toStdString(), file.size());
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
        match.line_number = static_cast<int>(regexMatch.capturedStart()); // byte offset
        match.line_content = regexMatch.captured();
        match.match_start = 0;
        match.match_end = static_cast<int>(regexMatch.capturedLength());

        // Provide hex context (16 bytes before and after)
        const int start = std::max(0, static_cast<int>(regexMatch.capturedStart()) - 16);
        const int end = std::min(static_cast<int>(content.size()),
            static_cast<int>(regexMatch.capturedEnd()) + 16);
        const QByteArray context = content.mid(start, end - start);
        match.context_before.append(
            QString("Hex: %1").arg(QString(context.toHex(' '))));

        matches.append(match);

        if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
            break;
        }
    }

    return matches;
}

} // namespace sak
