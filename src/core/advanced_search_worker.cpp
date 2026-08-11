// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_worker.cpp
/// @brief Implements directory-recursive file content search on a worker thread

#include "sak/advanced_search_worker.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <thread>

#include <zlib.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

namespace {
constexpr qsizetype kMaxTextSearchLines = 500'000;

/// Longest line handed to QRegularExpression::globalMatch. Qt exposes no match
/// or time limit for PCRE2 and the stop flag is only reachable BETWEEN lines, so
/// a hostile pattern (the pattern is user- or AI-supplied) whose cost grows with
/// the subject length would otherwise spin the worker thread uninterruptibly on
/// a single very long line. Lines past this length are not scanned at all and
/// the file is recorded as unreadable, so the omission is surfaced rather than
/// silently swallowed.
constexpr qsizetype kMaxScanLineChars = 256 * 1024;

/// GetDriveTypeW() value for a mapped network drive. Named here so the
/// classification is available on every platform; the value is checked against
/// the Win32 macro below.
constexpr unsigned int kWin32DriveRemote = 4;
#ifdef Q_OS_WIN
static_assert(kWin32DriveRemote == DRIVE_REMOTE, "DRIVE_REMOTE value drifted");
#endif

/// Bytes per bounded overlapped read window. Small enough that a stalled server
/// is detected inside one timeout budget, large enough that a 100 MiB archive is
/// not thousands of SMB round trips.
constexpr qint64 kNetworkReadWindowBytes = 1LL * sak::kBytesPerMB;

/// Ceiling on the configured network timeout so the seconds-to-milliseconds
/// conversion cannot overflow int.
constexpr int kMaxNetworkTimeoutSec = 300;

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

/// @brief Whether searchTargetFile would only read a prefix of @p size_bytes.
///
/// Truncation happens solely when the caller left max_file_size unlimited (<=0):
/// searchTargetFile then caps the read at kDefaultBinarySearchBytes. With a
/// positive max_file_size, oversized files are dropped up front by
/// shouldSkipTargetFile and never read. A truncated read searches only the head,
/// so a real match past the cap would look like a clean "not found" -- the scan
/// must be marked incomplete rather than reported as authoritative.
[[nodiscard]] bool targetReadWouldTruncate(qint64 max_file_size, uint64_t size_bytes) {
    return max_file_size <= 0 && size_bytes > static_cast<uint64_t>(kDefaultBinarySearchBytes);
}

/// @brief Validate a local (non-target, non-network) directory root before it is
///        iterated: it must exist, be a directory, and be readable. Returns the
///        fail-closed error code, or nullopt when the root is usable.
[[nodiscard]] std::optional<sak::error_code> validateLocalDirectoryRoot(const QString& root) {
    const QFileInfo info(root);
    if (!info.exists() || !info.isDir()) {
        return sak::error_code::file_not_found;
    }
    if (!info.isReadable()) {
        return sak::error_code::permission_denied;
    }
    return std::nullopt;
}

/// @brief Read every line of a LOCAL text file into @p lines.
///
/// Kept on the plain QFile/QTextStream path deliberately: local reads must stay
/// fast, and only a network path needs the bounded overlapped reader.
/// @return nullopt on success, else the fail-closed reason for the caller to
///         record -- the partially filled @p lines must then not be searched.
[[nodiscard]] std::optional<QString> readLocalTextLines(const QString& file_path,
                                                        QStringList& lines) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("open failed");
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    // Read all lines for context window support.
    while (!stream.atEnd()) {
        lines.append(stream.readLine());

        // Safety limit: refuse a file that somehow passed the byte-size check.
        if (lines.size() > kMaxTextSearchLines) {
            return QStringLiteral("exceeds %1 lines").arg(kMaxTextSearchLines);
        }
    }
    return std::nullopt;
}

/// @brief Win32 drive type of @p path's "X:\" root, or 0 (DRIVE_UNKNOWN) when the
///        path has no drive-letter root or the platform has no such concept.
///        GetDriveTypeW answers from the local mount table, so it does not itself
///        touch the (possibly unresponsive) share.
[[nodiscard]] unsigned int rootDriveType(const QString& path) {
#ifdef Q_OS_WIN
    constexpr int kDriveRootLength = 3;
    const QString root =
        QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).left(kDriveRootLength);
    if (root.size() < kDriveRootLength || root[1] != QLatin1Char(':')) {
        return 0;
    }
    return GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
#else
    Q_UNUSED(path)
    return 0;
#endif
}

#ifdef Q_OS_WIN

/// @brief Owns a Win32 HANDLE and closes it exactly once, on every exit path.
///        CreateFileW and CreateEventW report failure differently
///        (INVALID_HANDLE_VALUE vs nullptr), so both count as "nothing owned".
class ScopedWin32Handle {
public:
    explicit ScopedWin32Handle(HANDLE handle) : m_handle(handle) {}
    ~ScopedWin32Handle() {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    ScopedWin32Handle(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle& operator=(const ScopedWin32Handle&) = delete;
    ScopedWin32Handle(ScopedWin32Handle&&) = delete;
    ScopedWin32Handle& operator=(ScopedWin32Handle&&) = delete;

    [[nodiscard]] bool valid() const {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle;
};

/// @brief One bounded overlapped read window, grouped so the reader below stays
///        inside the five-parameter gate limit.
struct BoundedReadWindow {
    HANDLE file{nullptr};
    HANDLE event{nullptr};
    qint64 offset{0};
    DWORD bytes{0};
    int timeout_ms{0};
};

/// @brief Issue one overlapped ReadFile and wait at most @p w.timeout_ms for it.
///
/// On timeout the outstanding I/O is cancelled AND drained: the kernel may still
/// write into @p out and into the OVERLAPPED, both of which live in this frame,
/// so returning before the request has truly finished would corrupt the stack.
/// CancelIoEx makes the drain complete promptly instead of inheriting the hang.
[[nodiscard]] AdvancedSearchWorker::NetworkReadStep readBoundedWindow(const BoundedReadWindow& w,
                                                                      char* out,
                                                                      DWORD& read_bytes) {
    using Step = AdvancedSearchWorker::NetworkReadStep;
    constexpr quint64 kLow32BitMask = 0xFF'FF'FF'FFULL;
    constexpr int kDwordBitCount = 32;
    read_bytes = 0;

    OVERLAPPED overlapped{};
    overlapped.hEvent = w.event;
    overlapped.Offset = static_cast<DWORD>(static_cast<quint64>(w.offset) & kLow32BitMask);
    overlapped.OffsetHigh = static_cast<DWORD>(static_cast<quint64>(w.offset) >> kDwordBitCount);
    ResetEvent(w.event);

    if (ReadFile(w.file, out, w.bytes, nullptr, &overlapped) == FALSE &&
        GetLastError() != ERROR_IO_PENDING) {
        return GetLastError() == ERROR_HANDLE_EOF ? Step::EndOfFile : Step::Failed;
    }

    if (WaitForSingleObject(w.event, static_cast<DWORD>(w.timeout_ms)) != WAIT_OBJECT_0) {
        CancelIoEx(w.file, &overlapped);
        DWORD drained = 0;
        GetOverlappedResult(w.file, &overlapped, &drained, TRUE);
        return Step::TimedOut;
    }

    DWORD transferred = 0;
    const bool io_ok = GetOverlappedResult(w.file, &overlapped, &transferred, FALSE) != FALSE;
    const bool at_eof = !io_ok && GetLastError() == ERROR_HANDLE_EOF;
    read_bytes = transferred;
    return AdvancedSearchWorker::classifyNetworkReadStep(
        true, io_ok || at_eof, static_cast<qint64>(transferred), static_cast<qint64>(w.bytes));
}

/// @brief Read at most @p max_bytes of @p path with every window bounded by
///        @p timeout_ms.
///
/// A timeout discards everything read so far: a truncated buffer handed back as
/// "the file" would turn a hung share into a silent false-negative search result.
[[nodiscard]] std::expected<QByteArray, QString> readNetworkFileBounded(const QString& path,
                                                                        qint64 max_bytes,
                                                                        int timeout_ms) {
    const std::wstring wide_path = path.toStdWString();
    const ScopedWin32Handle file(CreateFileW(wide_path.c_str(),
                                             GENERIC_READ,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                             nullptr,
                                             OPEN_EXISTING,
                                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                                             nullptr));
    if (!file.valid()) {
        return std::unexpected(QStringLiteral("open failed (Win32 error %1)").arg(GetLastError()));
    }
    const ScopedWin32Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return std::unexpected(
            QStringLiteral("read event creation failed (Win32 error %1)").arg(GetLastError()));
    }

    QByteArray data;
    qint64 filled = 0;
    while (filled < max_bytes) {
        const qint64 window = std::min(kNetworkReadWindowBytes, max_bytes - filled);
        data.resize(filled + window);
        const BoundedReadWindow request{
            file.get(), event.get(), filled, static_cast<DWORD>(window), timeout_ms};
        DWORD landed = 0;
        const auto step = readBoundedWindow(request, data.data() + filled, landed);
        if (step == AdvancedSearchWorker::NetworkReadStep::TimedOut) {
            return std::unexpected(
                QStringLiteral("network read timed out after %1 ms").arg(timeout_ms));
        }
        if (step == AdvancedSearchWorker::NetworkReadStep::Failed) {
            return std::unexpected(
                QStringLiteral("network read failed (Win32 error %1)").arg(GetLastError()));
        }
        filled += landed;
        if (step == AdvancedSearchWorker::NetworkReadStep::EndOfFile) {
            break;
        }
    }
    data.resize(filled);
    return data;
}

#endif  // Q_OS_WIN
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
    // Every path reaching here names an enumerated file: the walk feeds it QDirIterator::next()
    // or a listing entry, and the single-file path is gated by QFileInfo(root_path).isFile().
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

bool AdvancedSearchWorker::requiresNetworkProbe(const QString& path, unsigned int root_drive_type) {
    if (path.startsWith("\\\\") || path.startsWith("//")) {
        return true;
    }
    // A drive letter mapped to an SMB share looks local. Without this rule the
    // bounded accessibility probe is skipped entirely for it, so an unresponsive
    // share blocks the walk with nothing to time it out.
    return root_drive_type == kWin32DriveRemote;
}

bool AdvancedSearchWorker::isNetworkPath(const QString& path) {
    return requiresNetworkProbe(path, rootDriveType(path));
}

bool AdvancedSearchWorker::readsOverNetwork(const QString& file_path) const {
    // The root's classification (computed once in prepareSearchConfig) covers
    // every file under it, and a path that is itself UNC is caught by a prefix
    // test. Neither costs a syscall, so the ordinary local walk is unaffected.
    if (m_root_on_network || file_path.startsWith("\\\\") || file_path.startsWith("//")) {
        return true;
    }
    // A reparse point inside an otherwise local tree can still resolve onto a
    // share (a planted symlink is an attacker-reachable way to do exactly that),
    // and opening it reads over SMB. The TARGET decides, not the link's own
    // local-looking path. Only entries that are actually links pay this lookup.
    const QFileInfo info(file_path);
    if (!info.isSymLink()) {
        return false;
    }
    return isNetworkPath(info.symLinkTarget());
}

AdvancedSearchWorker::NetworkReadStep AdvancedSearchWorker::classifyNetworkReadStep(
    bool wait_signalled, bool io_succeeded, qint64 bytes_read, qint64 bytes_requested) {
    if (bytes_requested <= 0) {
        // A zero-length window is a caller bug, never a clean stop.
        return NetworkReadStep::Failed;
    }
    if (!wait_signalled) {
        // Fail closed: the caller cancels the I/O and discards the partial buffer.
        // A timeout is a FAILURE, never a short read.
        return NetworkReadStep::TimedOut;
    }
    if (!io_succeeded || bytes_read < 0) {
        return NetworkReadStep::Failed;
    }
    if (bytes_read < bytes_requested) {
        // A file read comes up short only at end of file; keep what landed.
        return NetworkReadStep::EndOfFile;
    }
    return NetworkReadStep::Complete;
}

int AdvancedSearchWorker::networkReadTimeoutMs(int network_timeout_sec) {
    const int configured = network_timeout_sec > 0 ? network_timeout_sec
                                                   : kAdvancedSearchNetworkTimeoutSec;
    return std::clamp(configured, 1, kMaxNetworkTimeoutSec) * kMillisecondsPerSecond;
}

std::expected<QByteArray, QString> AdvancedSearchWorker::readSearchFileBytes(
    const QString& file_path, qint64 max_bytes) const {
    if (max_bytes <= 0) {
        return std::unexpected(QStringLiteral("read budget is not positive"));
    }
#ifdef Q_OS_WIN
    if (readsOverNetwork(file_path)) {
        // QFile has no read timeout, so a hostile or dead SMB server would block
        // this worker thread forever on a plain read. Network paths go through the
        // bounded overlapped read; local paths keep the plain (fast) QFile read.
        return readNetworkFileBounded(file_path,
                                      max_bytes,
                                      networkReadTimeoutMs(m_config.network_timeout_sec));
    }
#endif
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("open failed"));
    }
    return file.read(std::min(file.size(), max_bytes));
}

bool AdvancedSearchWorker::checkNetworkPathAccessible(const QString& path) const {
    // A stat on an unresponsive SMB share can block indefinitely, hanging the
    // whole search. Bound it by network_timeout_sec: run the probe on a detached
    // thread and, if it does not answer in time, fail closed (treat the path as
    // inaccessible) rather than block. The shared packaged_task keeps the future
    // valid without blocking this thread on the future's destructor.
    const int timeout_sec = m_config.network_timeout_sec > 0 ? m_config.network_timeout_sec
                                                             : kAdvancedSearchNetworkTimeoutSec;
    auto probe = std::make_shared<std::packaged_task<bool()>>([path]() {
        const QFileInfo info(path);
        return info.exists() && info.isReadable();
    });
    std::future<bool> result = probe->get_future();
    std::thread([probe]() { (*probe)(); }).detach();
    if (result.wait_for(std::chrono::seconds(timeout_sec)) != std::future_status::ready) {
        return false;
    }
    return result.get();
}

// -- Main Search Execution ---------------------------------------------------

std::optional<QString> AdvancedSearchWorker::firstInvalidExcludePattern(
    const QStringList& patterns) {
    for (const auto& pattern : patterns) {
        if (!QRegularExpression(pattern).isValid()) {
            return pattern;
        }
    }
    return std::nullopt;
}

auto AdvancedSearchWorker::prepareSearchConfig()
    -> std::expected<QRegularExpression, sak::error_code> {
    m_files_unreadable = 0;  // fresh count for this run
    m_scan_incomplete = false;
    m_incomplete_notes.clear();

    // Clamp context_lines before any match is built: a negative value silently
    // inverts the context window, and INT_MIN makes line_index - ctx overflow
    // (signed-overflow UB) in buildContextMatch/appendArchiveLineMatches.
    m_config.context_lines = std::clamp(m_config.context_lines, 0, kAdvancedSearchMaxContextLines);

    auto regexResult = compileRegex();
    if (!regexResult) {
        logError(
            "AdvancedSearchWorker: regex compilation "
            "failed: {}",
            regexResult.error().toStdString());
        return std::unexpected(sak::error_code::invalid_argument);
    }
    const auto& regex = regexResult.value();

    // An invalid exclude pattern used to be silently dropped, so the search would
    // then descend into paths the user explicitly asked to exclude. Fail closed:
    // refuse the whole search rather than over-search.
    if (const std::optional<QString> bad = firstInvalidExcludePattern(m_config.exclude_patterns)) {
        logError("AdvancedSearchWorker: invalid exclude pattern '{}'; refusing search",
                 bad->toStdString());
        return std::unexpected(sak::error_code::invalid_argument);
    }

    // Compile exclusion patterns once (all now known valid).
    m_compiled_excludes.clear();
    for (const auto& pattern : m_config.exclude_patterns) {
        m_compiled_excludes.append(
            QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
    }

    // Check network accessibility. The classification is kept for the run: every
    // scanned file lives under this root, so the per-file read path can decide
    // between the plain QFile read and the bounded overlapped read without
    // repeating the drive-type lookup.
    m_root_on_network = !m_config.use_file_system_target && isNetworkPath(m_config.root_path);
    if (m_root_on_network) {
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

    // Hard-cap the running total: searchFile caps a single file at max_results,
    // not at the remaining allowance, so without truncating here the aggregate
    // could overshoot the requested limit by up to a full file's worth.
    if (m_config.max_results > 0) {
        const int remaining = m_config.max_results - total_matches;
        if (remaining <= 0) {
            return false;
        }
        if (matches.size() > remaining) {
            matches.resize(remaining);
        }
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
    // R2: when skip_symlinks is set, add QDir::NoSymLinks so a reparse-point entry is excluded via
    // Qt's non-following isSymLink() check (evaluated in matchesFilters BEFORE isFile(), which
    // would resolve the target). A planted symlink to a UNC share is thus never yielded, never
    // opened, and never leaks the credential hash. Symlinked DIRECTORIES are already not descended
    // (the iterator has no FollowSymlinks flag); this additionally drops symlinked FILE entries.
    QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot;
    if (m_config.skip_symlinks) {
        filters |= QDir::NoSymLinks;
    }
    QDirIterator it(m_config.root_path, filters, QDirIterator::Subdirectories);

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

    reportScanOutcome(total_matches, total_files, file_count);
}

void AdvancedSearchWorker::reportScanOutcome(int total_matches,
                                             int total_files,
                                             int files_scanned) {
    // A hit result cap means matches (and the files after it) were dropped, so
    // the reported total is a floor rather than the answer.
    if (m_config.max_results > 0 && total_matches >= m_config.max_results) {
        markScanIncomplete(QStringLiteral("result cap of %1 reached; further matches were dropped")
                               .arg(m_config.max_results));
    }
    if (m_scan_incomplete) {
        // Something was skipped, so a "0 matches" or low count here is not
        // authoritative. Report the run as incomplete and log the reasons rather
        // than letting the user conclude the string is absent.
        logWarning("AdvancedSearchWorker: scan incomplete -- {}",
                   m_incomplete_notes.join(QStringLiteral("; ")).toStdString());
        reportProgress(files_scanned,
                       files_scanned,
                       QString("Search INCOMPLETE: %1 matches in %2 files (%3 scanned); some files "
                               "could not be searched, results may be missing matches")
                           .arg(total_matches)
                           .arg(total_files)
                           .arg(files_scanned));
        return;
    }
    reportProgress(files_scanned,
                   files_scanned,
                   QString("Search complete: %1 matches in %2 files (%3 files scanned)")
                       .arg(total_matches)
                       .arg(total_files)
                       .arg(files_scanned));
}

bool AdvancedSearchWorker::processTargetEntry(const FileManagementEntry& entry,
                                              const QRegularExpression& regex,
                                              int& total_matches,
                                              int& total_files,
                                              TargetBatchState& batch) {
    ++batch.file_count;
    if (!shouldSkipTargetFile(entry)) {
        if (targetReadWouldTruncate(m_config.max_file_size, entry.size_bytes)) {
            markScanIncomplete(QStringLiteral("file read truncated at '%1'").arg(entry.path));
        }
        if (!processTargetFile(entry, regex, batch.batch_matches, total_matches, total_files)) {
            return false;
        }
    }
    if (batch.file_count % kBatchSize == 0 && !batch.batch_matches.isEmpty()) {
        Q_EMIT resultsReady(batch.batch_matches);
        batch.batch_matches.clear();
    }
    constexpr int kProgressInterval = 100;
    if (batch.file_count % kProgressInterval == 0) {
        reportProgress(batch.file_count,
                       0,
                       QString("Searching target... %1 files scanned, %2 matches found")
                           .arg(batch.file_count)
                           .arg(total_matches));
    }
    return true;
}

void AdvancedSearchWorker::markScanIncomplete(const QString& note) {
    m_scan_incomplete = true;
    // Cap the retained notes so a deeply-truncated tree cannot grow this list
    // without bound; the incomplete flag alone conveys the essential state.
    constexpr int kMaxIncompleteNotes = 32;
    if (m_incomplete_notes.size() < kMaxIncompleteNotes) {
        m_incomplete_notes.append(note);
    }
}

void AdvancedSearchWorker::recordUnreadableFile(const QString& file_path, const QString& reason) {
    ++m_files_unreadable;
    markScanIncomplete(QStringLiteral("'%1': %2").arg(file_path, reason));
}

bool AdvancedSearchWorker::searchTargetDirectory(const QString& directory_path,
                                                 const QRegularExpression& regex,
                                                 int& total_matches,
                                                 int& total_files,
                                                 TargetBatchState& batch) {
    if (checkStop()) {
        return false;
    }

    const auto listing = FileManagementFileSystemBridge::listDirectory(
        m_config.file_system_target, directory_path, kTargetSearchBrowseMaxEntries);
    if (!listing.ok) {
        logWarning("AdvancedSearchWorker: target listing failed at '{}': {}",
                   directory_path.toStdString(),
                   listing.blockers.join(QStringLiteral("; ")).toStdString());
        markScanIncomplete(QStringLiteral("listing failed at '%1'").arg(directory_path));
        return true;
    }
    if (!listing.warnings.isEmpty()) {
        // A truncated listing (ok == true but entries capped) silently omits
        // files past the cap, which would otherwise turn a real match into a
        // false "not found". Record it so the run is reported as incomplete.
        markScanIncomplete(QStringLiteral("listing truncated at '%1': %2")
                               .arg(directory_path, listing.warnings.join(QStringLiteral("; "))));
    }

    for (const auto& entry : listing.entries) {
        if (checkStop()) {
            return false;
        }
        if (entry.regular_file) {
            if (!processTargetEntry(entry, regex, total_matches, total_files, batch)) {
                return false;
            }
        } else if (entry.directory) {
            if (!searchTargetDirectory(entry.path, regex, total_matches, total_files, batch)) {
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

    // Cap the running total: searchTargetFile caps a single file at max_results,
    // not at the remaining allowance, so without truncating here the aggregate
    // could overshoot the requested limit by up to a full file's worth (the
    // local processSearchFile path already does this).
    if (m_config.max_results > 0) {
        const int remaining = m_config.max_results - total_matches;
        if (remaining <= 0) {
            return false;
        }
        if (matches.size() > remaining) {
            matches.resize(remaining);
        }
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
    TargetBatchState batch;
    searchTargetDirectory(m_config.root_path.trimmed().isEmpty() ? QStringLiteral("/")
                                                                 : m_config.root_path,
                          regex,
                          total_matches,
                          total_files,
                          batch);

    if (!batch.batch_matches.isEmpty()) {
        Q_EMIT resultsReady(batch.batch_matches);
    }
    reportScanOutcome(total_matches, total_files, batch.file_count);
}

void AdvancedSearchWorker::searchSingleFile(const QRegularExpression& regex,
                                            int& total_matches,
                                            int& total_files) {
    // The explicitly-named single file goes through the SAME filters as the
    // directory walk. It previously skipped shouldSkipFile entirely, so
    // max_file_size and the extension filter were bypassed on this path and an
    // arbitrarily large file was read whole. A filtered-out file was never
    // opened, so reporting "0 matches" for it would be a false negative: mark
    // the run incomplete instead.
    if (shouldSkipFile(m_config.root_path)) {
        markScanIncomplete(QStringLiteral("'%1' was filtered out (exclusion, extension filter or "
                                          "max_file_size) and never searched")
                               .arg(m_config.root_path));
        reportScanOutcome(total_matches, total_files, 1);
        return;
    }

    auto matches = searchFile(m_config.root_path, regex);
    if (!matches.isEmpty()) {
        total_matches += matches.size();
        total_files++;
        Q_EMIT fileSearched(m_config.root_path, matches.size());
        Q_EMIT resultsReady(matches);
    }
    reportScanOutcome(total_matches, total_files, 1);
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
        searchSingleFile(regex, totalMatches, totalFiles);
        return {};
    }

    // A local directory root must be an accessible directory before we iterate
    // it; otherwise a mistyped/unmounted/permission-denied root falls through to
    // QDirIterator, yields zero files, and reports a clean "0 matches" (a false
    // negative). Fail closed and surface the real reason.
    if (const auto root_err = validateLocalDirectoryRoot(m_config.root_path)) {
        logError("AdvancedSearchWorker: search root not an accessible directory: {}",
                 m_config.root_path.toStdString());
        return std::unexpected(*root_err);
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
        metadata.insert(QStringLiteral("FileSize"),
                        QStringLiteral("%1 bytes").arg(file.size_bytes));
        metadata.insert(QStringLiteral("FileType"), QFileInfo(file.path).suffix().toUpper());
        matches.append(collectMetadataMatches(file.path, regex, metadata));
    }

    const uint64_t readLimit = m_config.max_file_size > 0
                                   ? static_cast<uint64_t>(m_config.max_file_size)
                                   : static_cast<uint64_t>(kDefaultBinarySearchBytes);
    const auto read =
        FileManagementFileSystemBridge::readFile(m_config.file_system_target, file.path, readLimit);
    if (!read.ok) {
        // A failed read yields no content matches, indistinguishable from a file
        // that genuinely holds none. Record it so the run is reported incomplete
        // instead of swallowing a real read failure as a clean result.
        recordUnreadableFile(file.path, read.blockers.join(QStringLiteral("; ")));
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

bool AdvancedSearchWorker::lineWithinScanLimit(const QString& file_path,
                                               const QString& line,
                                               int line_index,
                                               bool& already_recorded) {
    if (line.size() <= kMaxScanLineChars) {
        return true;
    }
    if (!already_recorded) {
        already_recorded = true;
        recordUnreadableFile(file_path,
                             QStringLiteral("line %1 exceeds the %2-character regex scan limit")
                                 .arg(line_index + 1)
                                 .arg(kMaxScanLineChars));
    }
    return false;
}

void AdvancedSearchWorker::scanLinesForMatches(const QString& file_path,
                                               const QStringList& lines,
                                               const QRegularExpression& regex,
                                               QVector<SearchMatch>& matches) {
    bool over_long_recorded = false;
    for (int i = 0; i < lines.size(); ++i) {
        if (checkStop()) {
            return;
        }

        const QString& line = lines[i];
        if (!lineWithinScanLimit(file_path, line, i, over_long_recorded)) {
            continue;
        }

        auto match_iter = regex.globalMatch(line);
        while (match_iter.hasNext()) {
            const auto regex_match = match_iter.next();
            matches.append(buildContextMatch(file_path, lines, i, regex_match));

            if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
                return;
            }
        }
    }
}

QVector<SearchMatch> AdvancedSearchWorker::searchTextContent(const QString& filePath,
                                                             const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    // Bound the bytes pulled into memory before reading anything. Oversized files
    // are dropped up front by shouldSkipFile only while max_file_size is positive;
    // with it unlimited an arbitrarily large file (in the worst case a single
    // unbounded line) would otherwise be slurped whole into one QString.
    const qint64 read_budget = m_config.max_file_size > 0 ? m_config.max_file_size
                                                          : kDefaultBinarySearchBytes;
    if (QFileInfo(filePath).size() > read_budget) {
        logWarning("AdvancedSearchWorker: file '{}' exceeds the text read budget, skipping",
                   filePath.toStdString());
        recordUnreadableFile(
            filePath, QStringLiteral("exceeds the %1-byte text read budget").arg(read_budget));
        return matches;
    }

    // A read over SMB has no timeout in QFile, so a network file goes through the
    // bounded reader and is then scanned from bytes -- the same byte path the
    // raw/virtual target search already uses. Local files keep QTextStream.
    if (readsOverNetwork(filePath)) {
        const auto read = readSearchFileBytes(filePath, read_budget);
        if (!read) {
            recordUnreadableFile(filePath, read.error());
            return matches;
        }
        return searchTextBytes(filePath, *read, regex);
    }

    QStringList lines;
    if (const std::optional<QString> error = readLocalTextLines(filePath, lines)) {
        // Unreadable (or over-limit) file: count it so an empty result is not
        // mistaken for a genuinely clean file -- a false negative to surface.
        recordUnreadableFile(filePath, *error);
        return matches;
    }

    scanLinesForMatches(filePath, lines, regex, matches);
    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchTextBytes(const QString& file_path,
                                                           const QByteArray& data,
                                                           const QRegularExpression& regex) {
    QVector<SearchMatch> matches;
    const QStringList lines = QString::fromUtf8(data).split(QLatin1Char('\n'));
    if (lines.size() > kMaxTextSearchLines) {
        logWarning("AdvancedSearchWorker: virtual file '{}' exceeds 500k lines, skipping",
                   file_path.toStdString());
        recordUnreadableFile(file_path,
                             QStringLiteral("exceeds %1 lines").arg(kMaxTextSearchLines));
        return matches;
    }

    scanLinesForMatches(file_path, lines, regex, matches);
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
        parts.append(QString::number(reader(valuePtr + (idx * type_size))));
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
        const QString rat = extractExifRational(valuePtr + (idx * kExifRationalBytes),
                                                littleEndian);
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
            // EXIF ASCII values are NUL-terminated, but a malformed tag may not
            // be. Drop the final byte only when it is actually the NUL, otherwise
            // a real trailing character would be silently lost.
            int len = static_cast<int>(count);
            if (valuePtr[len - 1] == '\0') {
                --len;
            }
            return QString::fromLatin1(valuePtr, len).trimmed();
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
    // Resolve the out-of-line value offset with 64-bit math so a crafted
    // offset near UINT32_MAX cannot wrap (or, when cast to int, go negative)
    // and slip past the bound into a wild pointer.
    const uint32_t offset = readU32(entry + 8, little_endian);
    if (static_cast<uint64_t>(offset) + total_bytes > static_cast<uint64_t>(data_size)) {
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
    // All range checks use 64-bit math: ifdOffset, entry offsets, and value
    // sizes are attacker-controlled 32-bit fields, so 32-bit/int arithmetic
    // would wrap or go negative and defeat the bound.
    if (static_cast<uint64_t>(ifdOffset) + kExifEntryCountBytes >
        static_cast<uint64_t>(data_size)) {
        return;
    }

    const char* base = tiffData.constData();
    const uint16_t entry_count = readU16(base + ifdOffset, littleEndian);
    for (uint16_t i = 0; i < entry_count; ++i) {
        const uint64_t entry_offset = static_cast<uint64_t>(ifdOffset) + kExifEntryCountBytes +
                                      (static_cast<uint64_t>(i) * kExifIfdEntrySize);
        if (entry_offset + kExifIfdEntrySize > static_cast<uint64_t>(data_size)) {
            break;
        }

        const char* entry = base + static_cast<qsizetype>(entry_offset);
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

        // count and unit_size are attacker-controlled; a 32-bit product can
        // overflow, so size the value span in 64-bit and reject anything that
        // cannot fit the buffer before it reaches the pointer math.
        const uint64_t total_bytes = static_cast<uint64_t>(count) *
                                     static_cast<uint64_t>(unit_size);
        if (total_bytes > static_cast<uint64_t>(data_size)) {
            continue;
        }
        const char* value_ptr = resolveExifValuePtr(
            entry, base, static_cast<uint32_t>(total_bytes), data_size, littleEndian);
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
    // Require a valid TIFF byte-order mark. Treating any non-"II" header as
    // big-endian would misread offsets/counts from a corrupt payload.
    const bool little_endian = (tiff_data[0] == 'I' && tiff_data[1] == 'I');
    const bool big_endian = (tiff_data[0] == 'M' && tiff_data[1] == 'M');
    if (!little_endian && !big_endian) {
        return;
    }
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

    qsizetype offset = kPngSignatureBytes;
    while (offset + kPngChunkOverheadBytes <= fileData.size()) {
        // chunk_len is a full 32-bit on-disk field. Keep it and the offset
        // advance in unsigned/qsizetype math: narrowing to int made a length
        // >= 0x80000000 negative, which drove offset backwards and led to an
        // out-of-bounds read before the buffer on the next iteration.
        const uint32_t chunk_len =
            (readBE16(fileData.constData() + offset) * kPngChunkLengthHighWordFactor) +
            readBE16(fileData.constData() + offset + kExifEntryCountBytes);
        const QByteArray chunk_type = fileData.mid(offset + kExifLongBytes, kExifLongBytes);

        if (chunk_type == "IEND") {
            break;
        }
        // A chunk that claims more bytes than remain is malformed: stop rather
        // than advance past the end.
        if (static_cast<uint64_t>(chunk_len) >
            static_cast<uint64_t>(fileData.size() - offset - kPngChunkOverheadBytes)) {
            break;
        }

        if (chunk_len > 0) {
            const QByteArray chunk_data = fileData.mid(offset + kExifValueOffset,
                                                       static_cast<qsizetype>(chunk_len));
            if (chunk_type == "tEXt") {
                parseTEXtChunk(chunk_data, metadata);
            } else if (chunk_type == "iTXt") {
                parseITXtChunk(chunk_data, metadata);
            }
        }

        offset += kPngChunkOverheadBytes + static_cast<qsizetype>(chunk_len);
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

/// @brief Add QImageReader-derived fields (text keys, dimensions) to @p metadata.
/// @return False when the reader could not be pointed at the image at all.
///
/// A LOCAL file is handed to QImageReader by path so it can seek the whole file.
/// A file @p over_network is handed the already-bounded prefix through a QBuffer
/// instead: QImageReader would otherwise open the share itself with a plain
/// synchronous read, which is exactly the unbounded hang the bounded reader
/// exists to prevent.
[[nodiscard]] bool supplementWithImageReader(const QString& file_path,
                                             const QByteArray& bounded_data,
                                             bool over_network,
                                             QMap<QString, QString>& metadata) {
    QBuffer buffer;
    QImageReader reader;
    if (over_network) {
        buffer.setData(bounded_data);
        if (!buffer.open(QIODevice::ReadOnly)) {
            return false;
        }
        reader.setDevice(&buffer);
    } else {
        reader.setFileName(file_path);
    }
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
    return true;
}

QVector<SearchMatch> AdvancedSearchWorker::searchImageMetadata(const QString& filePath,
                                                               const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    constexpr qint64 kMaxImageMetadataRead = 256 * 1024;
    const auto read = readSearchFileBytes(filePath, kMaxImageMetadataRead);
    if (!read) {
        // Unreadable image: count it so an empty result is not mistaken for a
        // genuinely metadata-free file (a false negative the caller must surface).
        recordUnreadableFile(filePath, read.error());
        return matches;
    }

    if (checkStop()) {
        return matches;
    }

    const QString ext = QFileInfo(filePath).suffix().toLower();
    QMap<QString, QString> metadata;
    gatherFormatMetadata(*read, ext, metadata);
    if (!supplementWithImageReader(filePath, *read, readsOverNetwork(filePath), metadata)) {
        recordUnreadableFile(filePath, QStringLiteral("image reader could not be opened"));
        return matches;
    }

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

/// Trailing two signature bytes of the records that legitimately terminate a
/// ZIP's local-file-header chain.
constexpr std::uint16_t kZipCentralDirTail = 0x0102;
constexpr std::uint16_t kZipEndOfCentralDirTail = 0x0506;
constexpr std::uint16_t kZipZip64EndOfCentralDirTail = 0x0606;

/// @brief Whether @p offset points at the central directory (or an end-of-central
///        directory record, for an archive with no further entries), i.e. the
///        local-file-header chain ended exactly where a well-formed ZIP says it
///        should. Anything else means the walk stopped early and entries past
///        that point were never examined.
[[nodiscard]] bool isZipDirectorySignature(const QByteArray& data, qsizetype offset) {
    constexpr qsizetype kSignatureBytes = 4;
    if (offset < 0 || offset + kSignatureBytes > data.size()) {
        return false;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(data.constData() + offset);
    if (raw[0] != kZipSignaturePk0 || raw[1] != kZipSignaturePk1) {
        return false;
    }
    const auto tail =
        static_cast<std::uint16_t>((raw[kByteIndex2] << kByteShift8) | raw[kByteIndex3]);
    return tail == kZipCentralDirTail || tail == kZipEndOfCentralDirTail ||
           tail == kZipZip64EndOfCentralDirTail;
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
constexpr int kZipFlagsOffset = 6;
/// General-purpose bit 3: the entry's sizes are not in the local file header but
/// in a data descriptor written AFTER the compressed bytes (a streaming ZIP).
constexpr std::uint16_t kZipFlagDataDescriptor = 0x0008;
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

// Parse one ZIP local file header. Returns the next offset to visit, or -1 to
// stop. All arithmetic is 64-bit: comp_size/name_len/extra_len are
// attacker-controlled on-disk fields, so int/uint32 math could narrow negative
// or wrap and drive the caller's offset backwards into an out-of-bounds read.
[[nodiscard]] qsizetype processZipMetadataEntry(const QByteArray& fileData,
                                                qsizetype offset,
                                                QMap<QString, QString>& metadata) {
    const qsizetype data_size = fileData.size();
    if (!isZipLocalFileHeader(fileData, static_cast<int>(offset))) {
        return -1;
    }
    const uint16_t name_len = readU16(fileData.constData() + offset + 26, true);
    const uint16_t extra_len = readU16(fileData.constData() + offset + 28, true);
    const uint32_t comp_size = readU32(fileData.constData() + offset + 18, true);
    const uint16_t comp_method = readU16(fileData.constData() + offset + 8, true);
    if (offset + kZipLocalHeaderSize + name_len > data_size) {
        return -1;
    }

    const qsizetype data_start = offset + 30 + static_cast<qsizetype>(name_len) +
                                 static_cast<qsizetype>(extra_len);
    const qsizetype entry_size = static_cast<qsizetype>(comp_size);
    if (data_start > data_size || entry_size > data_size - data_start) {
        return -1;
    }

    const QString entry_name = QString::fromLatin1(fileData.constData() + offset + 30, name_len);
    const bool wanted = isMetadataTarget(entry_name) &&
                        (comp_method == kZipMethodStored || comp_method == kZipMethodDeflated);
    if (wanted) {
        QByteArray xml_data = decompressZipEntry(fileData,
                                                 static_cast<int>(offset),
                                                 static_cast<int>(data_start),
                                                 static_cast<int>(entry_size),
                                                 comp_method);
        if (!xml_data.isEmpty()) {
            extractXmlTags(QString::fromUtf8(xml_data), metadata);
        }
    }

    // Always move forward: a zero-length stored entry would otherwise spin.
    const qsizetype next = data_start + entry_size;
    return next > offset ? next : -1;
}

[[nodiscard]] QMap<QString, QString> extractZipXmlMetadata(const QByteArray& fileData) {
    QMap<QString, QString> metadata;
    qsizetype offset = 0;
    const qsizetype data_size = fileData.size();
    while (offset + kZipLocalHeaderSize < data_size) {
        offset = processZipMetadataEntry(fileData, offset, metadata);
        if (offset < 0) {
            break;
        }
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

    // Scan only the leading region of the (already length-capped) buffer for
    // /Key (value) Info entries. This is a deliberately shallow parse: an Info
    // dictionary that lives near the tail of a large PDF (past this window) is
    // not extracted -- filesystem metadata still applies via the caller.
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
    // Subtraction-based bound in 64-bit: narrowing frame_size to int made a
    // value >= 0x80000000 negative, so the additive check passed and the
    // caller's offset advance then went deeply negative -> OOB read.
    if (offset < 0 || max_offset < offset + kId3FrameHeaderSize) {
        return false;
    }
    if (static_cast<std::uint64_t>(frame_size) >
        static_cast<std::uint64_t>(max_offset - offset - kId3FrameHeaderSize)) {
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
    case kId3EncodingUtf16: {
        // Encoding 1 is UTF-16 with a leading BOM; honor it rather than
        // assuming the host byte order (QString::fromUtf16 is host-endian).
        QStringDecoder decoder(QStringConverter::Utf16);
        return QString(decoder.decode(frame_data)).trimmed();
    }
    case kId3EncodingUtf16Be: {
        // Encoding 2 is UTF-16 big-endian with no BOM.
        QStringDecoder decoder(QStringConverter::Utf16BE);
        return QString(decoder.decode(frame_data)).trimmed();
    }
    case kId3EncodingUtf8:
        return QString::fromUtf8(frame_data).trimmed();
    default:
        // ID3v2 defines encodings 0-3 only. An out-of-range byte marks a
        // malformed frame: decode nothing rather than guess a codec (which would
        // surface garbage as if it were real text).
        return {};
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

std::expected<QMap<QString, QString>, QString> AdvancedSearchWorker::metadataForExtension(
    const QString& file_path, const QString& ext, const QByteArray& leading) const {
    if (ext == QStringLiteral("pdf")) {
        return extractPdfMetadata(leading);
    }
    if (ext == QStringLiteral("mp3")) {
        return extractMp3Metadata(leading);
    }
    if (!isZipMetadataExtension(ext)) {
        return QMap<QString, QString>{};
    }
    // A ZIP-based document (docx/odt/epub) keeps its metadata parts anywhere in
    // the container, so this one format needs a second, larger read -- through the
    // same bounded reader, never a raw re-open of the share. An oversized
    // container yields no format metadata, as before.
    if (QFileInfo(file_path).size() > kMaxZipMetadataRead) {
        return QMap<QString, QString>{};
    }
    const auto container = readSearchFileBytes(file_path, kMaxZipMetadataRead);
    if (!container) {
        return std::unexpected(container.error());
    }
    return extractZipXmlMetadata(*container);
}

QVector<SearchMatch> AdvancedSearchWorker::searchFileMetadata(const QString& filePath,
                                                              const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    const auto leading = readSearchFileBytes(filePath, kMaxMetadataRead);
    if (!leading) {
        // Unreadable file: count it so an empty result is not mistaken for a
        // genuinely metadata-free file (a false negative the caller must surface).
        recordUnreadableFile(filePath, leading.error());
        return matches;
    }

    if (checkStop()) {
        return matches;
    }

    const QString ext = QFileInfo(filePath).suffix().toLower();
    const auto extracted = metadataForExtension(filePath, ext, *leading);
    if (!extracted) {
        recordUnreadableFile(filePath, extracted.error());
        return matches;
    }
    QMap<QString, QString> metadata = *extracted;
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

    // A streaming ZIP (general-purpose bit 3) stores compressed_size 0 in every
    // local header and the real sizes in a trailing data descriptor. Trusting the
    // header would put next_offset INSIDE the compressed bytes, so the chain
    // silently dies at the first entry and the remaining entries are never
    // searched. Refuse the entry: the caller records the archive as incomplete
    // instead of returning a partial result that reads like a full scan.
    const uint16_t flags = readU16(archive_data.constData() + offset + kZipFlagsOffset, true);
    if ((flags & kZipFlagDataDescriptor) != 0) {
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

    // A STORED (method 0) entry is uncompressed, so its compressed and uncompressed sizes MUST be
    // equal. A crafted stored entry that declares a tiny uncompressed_size but a huge
    // compressed_size defeats the uncompressed-size text cap downstream, which would then copy
    // compressed_size bytes and split them into an enormous QStringList (billion-laughs DoS).
    // Reject the inconsistency.
    if (compMethod == kZipMethodStored && compSize != uncompSize) {
        return std::nullopt;
    }

    if (offset + kZipLocalHeaderSize + nameLen > data_size) {
        return std::nullopt;
    }

    // compSize is an attacker-controlled 32-bit field. Size the entry and its
    // successor offset in 64-bit and reject anything that would not fit the
    // buffer, so a value >= 0x80000000 cannot narrow negative and drive the
    // caller's offset backwards into an out-of-bounds read.
    const qsizetype data_start = static_cast<qsizetype>(offset) + kZipLocalHeaderSize +
                                 static_cast<qsizetype>(nameLen) + static_cast<qsizetype>(extraLen);
    const qsizetype entry_size = static_cast<qsizetype>(compSize);
    if (data_start > data_size || entry_size > data_size - data_start) {
        return std::nullopt;
    }
    const qsizetype next_offset = data_start + entry_size;
    if (next_offset <= offset) {  // must always advance
        return std::nullopt;
    }

    ArchiveEntry entry;
    entry.name = QString::fromUtf8(archive_data.constData() + offset + kZipLocalHeaderSize,
                                   nameLen);
    entry.path = QString("%1!/%2").arg(file_path, entry.name);
    entry.data_start = static_cast<int>(data_start);
    entry.entry_size = static_cast<int>(entry_size);
    entry.next_offset = static_cast<int>(next_offset);
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
                                                    QVector<SearchMatch>& matches) {
    bool over_long_recorded = false;
    for (int lineIdx = 0; lineIdx < lines.size(); ++lineIdx) {
        if (checkStop()) {
            return true;
        }

        const QString& line = lines[lineIdx];
        if (!lineWithinScanLimit(archive_path, line, lineIdx, over_long_recorded)) {
            continue;
        }
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
                                                       QVector<SearchMatch>& matches) {
    const QString entryExt = QFileInfo(entry.name).suffix().toLower();
    if (!isArchiveTextCompression(entry.compression_method) || !isArchiveTextExtension(entryExt)) {
        return false;
    }
    if (entry.data_start + entry.entry_size > archive_data.size() || entry.uncompressed_size == 0 ||
        entry.uncompressed_size >= kMaxArchiveTextEntrySize ||
        entry.entry_size >= static_cast<int>(kMaxArchiveTextEntrySize)) {
        // Cap the COPIED bytes (entry_size == the on-disk/compressed size) too, not just the
        // declared uncompressed size: a stored entry copies entry_size bytes verbatim, so an
        // over-cap compressed size must be refused before mid()+split materialize it.
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

qsizetype AdvancedSearchWorker::scanArchiveEntries(const QByteArray& archive_data,
                                                   const QString& file_path,
                                                   const QRegularExpression& regex,
                                                   QVector<SearchMatch>& matches) {
    int offset = 0;
    const int data_size = archive_data.size();
    while (offset + kZipLocalHeaderSize < data_size) {
        if (checkStop()) {
            return -1;
        }

        const std::optional<ArchiveEntry> entry = readArchiveEntry(archive_data, file_path, offset);
        if (!entry.has_value()) {
            break;
        }

        if (appendArchiveNameMatches(*entry, regex, matches)) {
            return -1;
        }

        if (appendArchiveContentMatches(archive_data, *entry, regex, matches)) {
            return -1;
        }

        offset = entry->next_offset;
    }
    return offset;
}

QVector<SearchMatch> AdvancedSearchWorker::searchArchive(const QString& filePath,
                                                         const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    const qint64 archive_size = QFileInfo(filePath).size();
    if (archive_size > kMaxArchiveSize) {
        logWarning("AdvancedSearchWorker: archive '{}' too large ({} bytes), skipping",
                   filePath.toStdString(),
                   archive_size);
        // Over the cap -> not searched at all; surface it as incomplete rather
        // than reporting a clean "no matches".
        recordUnreadableFile(
            filePath, QStringLiteral("exceeds the %1-byte archive limit").arg(kMaxArchiveSize));
        return matches;
    }

    const auto read = readSearchFileBytes(filePath, kMaxArchiveSize);
    if (!read) {
        // Unreadable archive: count it so an empty result is not mistaken for an
        // archive that genuinely contains no match (a false negative to surface).
        recordUnreadableFile(filePath, read.error());
        return matches;
    }
    const QByteArray& archiveData = *read;

    if (checkStop()) {
        return matches;
    }

    const qsizetype stop_offset = scanArchiveEntries(archiveData, filePath, regex, matches);

    // The local-file-header chain must land exactly on the central directory. A
    // premature stop means every entry past that point went unexamined -- a
    // truncated/corrupt archive, or a streaming (data-descriptor) ZIP whose local
    // headers carry no sizes. Record it so the partial result is surfaced as
    // incomplete instead of being read as "the archive holds no match".
    if (stop_offset >= 0 && !isZipDirectorySignature(archiveData, stop_offset)) {
        recordUnreadableFile(filePath,
                             QStringLiteral(
                                 "entry chain stopped at offset %1 before the central directory")
                                 .arg(stop_offset));
    }

    return matches;
}

QVector<SearchMatch> AdvancedSearchWorker::searchBinary(const QString& filePath,
                                                        const QRegularExpression& regex) {
    QVector<SearchMatch> matches;

    // Size guard -- refuse to load extremely large files into memory
    const qint64 maxBinarySize = m_config.max_file_size > 0 ? m_config.max_file_size
                                                            : kDefaultBinarySearchBytes;
    const qint64 binary_size = QFileInfo(filePath).size();
    if (binary_size > maxBinarySize) {
        logWarning("AdvancedSearchWorker: binary file '{}' exceeds size limit ({} bytes), skipping",
                   filePath.toStdString(),
                   binary_size);
        recordUnreadableFile(
            filePath, QStringLiteral("exceeds the %1-byte binary read limit").arg(maxBinarySize));
        return matches;
    }

    // Read file as raw bytes (bounded reader: a network path gets a read timeout).
    const auto read = readSearchFileBytes(filePath, maxBinarySize);
    if (!read) {
        // Not scanned at all: an empty result here must not read as "no match".
        recordUnreadableFile(filePath, read.error());
        return matches;
    }
    const QByteArray& content = *read;

    // Decode as Latin-1, not UTF-8: each byte maps 1:1 to a distinct code point
    // (U+0000..U+00FF) and the mapping is fully reversible. This makes ANY byte
    // value matchable -- including arbitrary "hex" bytes via regex \xNN escapes --
    // and makes a code-unit index equal the raw byte offset exactly. UTF-8
    // decoding instead collapsed every invalid byte to U+FFFD, so non-text bytes
    // could never match and toUtf8() re-encoding skewed the reported offsets.
    const QString textContent = QString::fromLatin1(content);
    auto matchIter = regex.globalMatch(textContent);

    while (matchIter.hasNext()) {
        auto regexMatch = matchIter.next();

        // With a Latin-1 decode the code-unit position IS the raw byte offset.
        const int matchStartByte = static_cast<int>(regexMatch.capturedStart());
        const int matchEndByte = static_cast<int>(regexMatch.capturedEnd());

        SearchMatch match;
        match.file_path = filePath;
        match.line_number = matchStartByte;  // byte offset into the raw file
        match.line_content = regexMatch.captured();
        match.match_start = 0;
        match.match_end = static_cast<int>(regexMatch.capturedLength());

        // Provide hex context (16 bytes before and after)
        const int start = std::max(0, matchStartByte - 16);
        const int end = std::min(static_cast<int>(content.size()), matchEndByte + 16);
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
    const QString& file_path, const QByteArray& data, const QRegularExpression& regex) const {
    QVector<SearchMatch> matches;
    // Latin-1 (not UTF-8): byte-exact, reversible, and code-unit index == byte
    // offset. See searchBinary for the full rationale.
    const QString textContent = QString::fromLatin1(data);
    auto matchIter = regex.globalMatch(textContent);
    while (matchIter.hasNext()) {
        const auto regexMatch = matchIter.next();

        const int matchStartByte = static_cast<int>(regexMatch.capturedStart());
        const int matchEndByte = static_cast<int>(regexMatch.capturedEnd());

        SearchMatch match;
        match.file_path = file_path;
        match.line_number = matchStartByte;  // byte offset into the raw file
        match.line_content = regexMatch.captured();
        match.match_start = 0;
        match.match_end = static_cast<int>(regexMatch.capturedLength());

        constexpr int kBinaryContextBytes = 16;
        const int start = std::max(0, matchStartByte - kBinaryContextBytes);
        const int end = std::min(static_cast<int>(data.size()), matchEndByte + kBinaryContextBytes);
        match.context_before.append(
            QString("Hex: %1").arg(QString(data.mid(start, end - start).toHex(' '))));
        matches.append(match);

        if (m_config.max_results > 0 && matches.size() >= m_config.max_results) {
            break;
        }
    }
    return matches;
}

}  // namespace sak
