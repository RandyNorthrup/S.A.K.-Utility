// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_health_ledger.h"

#include "sak/app_action_guards.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/windows_path_policy.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QScopeGuard>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <windows.h>

namespace sak::ai {

namespace {
constexpr qsizetype kHealthErrorPreviewChars = 1000;
constexpr int kBackoffFactorMax = 1024;
constexpr int kBackoffFactorMultiplier = 2;
constexpr int kSecondsPerHour = kSecondsPerMinute * kSecondsPerMinute;
constexpr DWORD kFinalPathInitialChars = 1024;
constexpr qsizetype kExtendedUncPrefixChars = 8;
constexpr qsizetype kExtendedPathPrefixChars = 4;

std::function<void(const QString&)> g_before_file_open_hook;
std::function<void(const QString&)> g_before_file_read_hook;
std::function<void(const QString&)> g_before_temp_open_hook;

class LedgerHandle {
public:
    explicit LedgerHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~LedgerHandle() {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    LedgerHandle(const LedgerHandle&) = delete;
    LedgerHandle& operator=(const LedgerHandle&) = delete;
    LedgerHandle(LedgerHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }
    LedgerHandle& operator=(LedgerHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                CloseHandle(m_handle);
            }
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    [[nodiscard]] bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const { return m_handle; }

private:
    HANDLE m_handle;
};

QString ledgerFinalPath(HANDLE handle) {
    std::wstring buffer(static_cast<size_t>(kFinalPathInitialChars), L'\0');
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length == 0) {
        return {};
    }
    if (length >= buffer.size()) {
        buffer.resize(static_cast<size_t>(length) + 1U);
        length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        if (length == 0 || length >= buffer.size()) {
            return {};
        }
    }
    QString path = QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(length));
    if (path.startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive)) {
        path = QStringLiteral("\\\\") + path.mid(kExtendedUncPrefixChars);
    } else if (path.startsWith(QStringLiteral("\\\\?\\"), Qt::CaseInsensitive)) {
        path = path.mid(kExtendedPathPrefixChars);
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

bool ledgerHandleIsReparse(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) == 0 ||
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ledgerHandlesSameObject(HANDLE left, HANDLE right) {
    BY_HANDLE_FILE_INFORMATION left_info{};
    BY_HANDLE_FILE_INFORMATION right_info{};
    if (GetFileInformationByHandle(left, &left_info) == 0 ||
        GetFileInformationByHandle(right, &right_info) == 0) {
        return false;
    }
    return left_info.dwVolumeSerialNumber == right_info.dwVolumeSerialNumber &&
           left_info.nFileIndexHigh == right_info.nFileIndexHigh &&
           left_info.nFileIndexLow == right_info.nFileIndexLow;
}

QString ledgerWin32Error(const QString& action, const QString& path, DWORD error) {
    return QStringLiteral("%1: %2 (Win32 error %3)").arg(action, path).arg(error);
}

void setLedgerError(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

QString iso(const QDateTime& value) {
    return value.isValid() ? value.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime parseIso(const QJsonObject& object, const QString& key) {
    return QDateTime::fromString(object.value(key).toString(), Qt::ISODateWithMs);
}

QString normalizeKey(const QString& key) {
    return key.trimmed().toLower();
}

/// Tolerance for clock skew between the process that wrote a record and the one reading
/// it. Anything dated further ahead than this is not skew, it is a forged timestamp.
constexpr qint64 kClockSkewToleranceMs = 300'000;  // 5 minutes

/// Saturating increment. A persisted counter is bounded on load, but an in-process one
/// must still never wrap: signed overflow is undefined behavior, and a negative count
/// would read as a healthy record.
void incrementSaturating(int* counter) {
    if (*counter < kAiToolHealthMaxCount) {
        ++(*counter);
    }
}

/// Fail closed on a persistence path this process cannot bind safely. A relative path
/// resolves against the process CWD (a guessed location, not the configured one), and a
/// symlink/junction at the file OR any ancestor can redirect this elevated process's read
/// or write off the app's own data directory. pathReparseUnsafe also rejects blank and
/// UNC/device paths before performing any stat.
bool ledgerPathUnsafe(const QString& path, QString* error_message) {
    if (!QFileInfo(path).isAbsolute()) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("AI tool health ledger path must be absolute: %1").arg(path);
        }
        return true;
    }
    if (sak::pathReparseUnsafe(path)) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral(
                                 "Refusing AI tool health ledger path (it, or an ancestor, is a "
                                 "symlink/junction or a UNC/device path): %1")
                                 .arg(path);
        }
        return true;
    }
    return false;
}

std::optional<LedgerHandle> openLedgerDirectory(const QString& ledger_path,
                                                bool create,
                                                bool prevent_rename,
                                                QString* error_message) {
    const QString directory = QFileInfo(ledger_path).absolutePath();
    if (create && !QDir().mkpath(directory)) {
        setLedgerError(
            error_message,
            QStringLiteral("Could not create AI tool health ledger directory: %1").arg(directory));
        return std::nullopt;
    }
    const QString native_directory = QDir::toNativeSeparators(directory);
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
                        (prevent_rename ? 0U : FILE_SHARE_DELETE);
    LedgerHandle handle(CreateFileW(reinterpret_cast<LPCWSTR>(native_directory.utf16()),
                                    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                    share,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!handle.valid()) {
        setLedgerError(error_message,
                       ledgerWin32Error(QStringLiteral("Cannot open ledger directory"),
                                        directory,
                                        GetLastError()));
        return std::nullopt;
    }
    const QString final_path = ledgerFinalPath(handle.get());
    const QString expected_path = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    if (ledgerHandleIsReparse(handle.get()) || final_path.isEmpty() ||
        final_path.compare(expected_path, Qt::CaseInsensitive) != 0) {
        setLedgerError(error_message,
                       QStringLiteral("Refusing redirected AI tool health ledger directory: %1")
                           .arg(directory));
        return std::nullopt;
    }
    return handle;
}

bool ledgerFileParentMatches(HANDLE file_handle, HANDLE directory_handle) {
    const QString file_path = ledgerFinalPath(file_handle);
    if (file_path.isEmpty()) {
        return false;
    }
    const QString parent_path = QDir::toNativeSeparators(QFileInfo(file_path).absolutePath());
    const LedgerHandle parent(CreateFileW(reinterpret_cast<LPCWSTR>(parent_path.utf16()),
                                          FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr));
    return parent.valid() && !ledgerHandleIsReparse(parent.get()) &&
           ledgerHandlesSameObject(parent.get(), directory_handle);
}

std::optional<qint64> ledgerHandleSize(HANDLE handle) {
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0 || size.QuadPart < 0) {
        return std::nullopt;
    }
    return size.QuadPart;
}

enum class LedgerReadResult {
    Loaded,
    Missing,
    Error
};

LedgerReadResult inspectLedgerPath(const QString& path, QString* error_message) {
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return LedgerReadResult::Missing;
        }
        if (error_message != nullptr) {
            *error_message = ledgerWin32Error(
                QStringLiteral("Cannot inspect AI tool health ledger"), path, error);
        }
        return LedgerReadResult::Error;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("AI tool health ledger path is a directory: %1").arg(path);
        }
        return LedgerReadResult::Error;
    }
    return LedgerReadResult::Loaded;
}

struct OpenedLedgerFile {
    LedgerReadResult result = LedgerReadResult::Error;
    std::optional<LedgerHandle> handle;
};

OpenedLedgerFile openLedgerFileForRead(const QString& path,
                                       HANDLE directory_handle,
                                       QString* error_message) {
    const QString leaf = QFileInfo(path).fileName();
    const QString opened_path =
        QDir::toNativeSeparators(QDir(ledgerFinalPath(directory_handle)).filePath(leaf));
    if (g_before_file_open_hook) {
        g_before_file_open_hook(path);
    }
    LedgerHandle file(CreateFileW(reinterpret_cast<LPCWSTR>(opened_path.utf16()),
                                  GENERIC_READ | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                      FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr));
    if (!file.valid()) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return {LedgerReadResult::Missing, std::nullopt};
        }
        if (error_message != nullptr) {
            *error_message = ledgerWin32Error(
                QStringLiteral("Could not read AI tool health ledger"), path, error);
        }
        return {};
    }
    const bool file_parent_matches = ledgerFileParentMatches(file.get(), directory_handle);
    if (ledgerHandleIsReparse(file.get()) || !file_parent_matches) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Refusing redirected AI tool health ledger file: %1").arg(path);
        }
        return {};
    }
    OpenedLedgerFile result;
    result.result = LedgerReadResult::Loaded;
    result.handle.emplace(std::move(file));
    return result;
}

LedgerReadResult readStableLedgerFile(HANDLE file_handle,
                                      const QString& path,
                                      QByteArray* bytes,
                                      QString* error_message) {
    const auto size_before = ledgerHandleSize(file_handle);
    if (!size_before || *size_before > kAiToolHealthMaxLedgerBytes) {
        setLedgerError(
            error_message,
            QStringLiteral("AI tool health ledger is not a readable size: %1").arg(path));
        return LedgerReadResult::Error;
    }
    if (g_before_file_read_hook) {
        g_before_file_read_hook(path);
    }
    QByteArray buffer(static_cast<qsizetype>(kAiToolHealthMaxLedgerBytes + 1), Qt::Uninitialized);
    DWORD bytes_read = 0;
    if (ReadFile(
            file_handle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_read, nullptr) ==
        0) {
        setLedgerError(error_message,
                       ledgerWin32Error(QStringLiteral("Could not read AI tool health ledger"),
                                        path,
                                        GetLastError()));
        return LedgerReadResult::Error;
    }
    buffer.resize(static_cast<qsizetype>(bytes_read));
    const auto size_after = ledgerHandleSize(file_handle);
    if (!size_after || *size_after > kAiToolHealthMaxLedgerBytes || *size_before != *size_after ||
        *size_after != buffer.size()) {
        setLedgerError(error_message,
                       QStringLiteral("AI tool health ledger changed during read: %1").arg(path));
        return LedgerReadResult::Error;
    }
    *bytes = std::move(buffer);
    return LedgerReadResult::Loaded;
}

LedgerReadResult readLedgerBytes(const QString& path, QByteArray* bytes, QString* error_message) {
    const LedgerReadResult inspection = inspectLedgerPath(path, error_message);
    if (inspection != LedgerReadResult::Loaded) {
        return inspection;
    }
    auto directory = openLedgerDirectory(path, false, false, error_message);
    if (!directory) {
        return LedgerReadResult::Error;
    }
    OpenedLedgerFile file = openLedgerFileForRead(path, directory->get(), error_message);
    if (file.result != LedgerReadResult::Loaded) {
        return file.result;
    }
    return readStableLedgerFile(file.handle->get(), path, bytes, error_message);
}

/// True when @p key is absent (the record default applies) or actually holds a string.
/// A present but wrong-typed field is malformed: coercing it to "" would turn a corrupt
/// or tampered entry into a well-formed-looking one.
bool stringFieldOk(const QJsonObject& object, const QString& key) {
    return !object.contains(key) || object.value(key).isString();
}

/// Parse a whole, non-negative, in-range counter. False for a present-but-wrong-typed,
/// fractional, negative or out-of-range value.
bool parseCount(const QJsonObject& object, const QString& key, int* out) {
    *out = 0;
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return false;
    }
    const double raw = value.toDouble();
    if (raw < 0.0 || raw > static_cast<double>(kAiToolHealthMaxCount) || raw != std::floor(raw)) {
        return false;
    }
    *out = static_cast<int>(raw);
    return true;
}

/// Same, for the millisecond latency. The range is checked BEFORE the cast: converting a
/// double outside the integer's range is undefined behavior, not a clamp.
bool parseMillis(const QJsonObject& object, const QString& key, qint64* out) {
    *out = 0;
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return false;
    }
    const double raw = value.toDouble();
    if (raw < 0.0 || raw > static_cast<double>(kAiToolHealthMaxLatencyMs) ||
        raw != std::floor(raw)) {
        return false;
    }
    *out = static_cast<qint64>(raw);
    return true;
}

/// Parse an ISO-8601 timestamp. Absent, or the empty string toJson() writes for an unset
/// QDateTime, means "never" (an invalid QDateTime, which every reader already treats as
/// unset); a non-empty string that does not parse is malformed.
bool parseIsoStrict(const QJsonObject& object, const QString& key, QDateTime* out) {
    *out = QDateTime();
    if (!object.contains(key)) {
        return true;
    }
    const QJsonValue value = object.value(key);
    if (!value.isString()) {
        return false;
    }
    if (value.toString().isEmpty()) {
        return true;
    }
    const QDateTime parsed = parseIso(object, key);
    if (!parsed.isValid()) {
        return false;
    }
    *out = parsed.toUTC();
    return true;
}

/// True when every free-text field of a persisted record is absent or actually a string.
bool recordTextFieldsOk(const QJsonObject& object) {
    return stringFieldOk(object, QStringLiteral("key")) &&
           stringFieldOk(object, QStringLiteral("last_failure_class")) &&
           stringFieldOk(object, QStringLiteral("last_error_message"));
}

/// Parse every timestamp of a persisted record. All-or-nothing: the first malformed one
/// ends the parse, because a record accepted with some of its timestamps silently unset
/// would read as a tool that has never failed.
bool parseRecordTimestamps(const QJsonObject& object, AiToolHealthRecord* out) {
    return parseIsoStrict(object, QStringLiteral("last_success_utc"), &out->last_success_utc) &&
           parseIsoStrict(object, QStringLiteral("last_failure_utc"), &out->last_failure_utc) &&
           parseIsoStrict(object, QStringLiteral("disabled_until_utc"), &out->disabled_until_utc);
}

/// Parse the latency and the three counters of a persisted record, on the same terms: one
/// out-of-range or wrong-typed number condemns the whole record rather than being coerced.
bool parseRecordCounters(const QJsonObject& object, AiToolHealthRecord* out) {
    return parseMillis(object, QStringLiteral("last_latency_ms"), &out->last_latency_ms) &&
           parseCount(object, QStringLiteral("success_count"), &out->success_count) &&
           parseCount(object, QStringLiteral("failure_count"), &out->failure_count) &&
           parseCount(object, QStringLiteral("consecutive_failures"), &out->consecutive_failures);
}

/// The failure class carried by the result's EXPLICIT fields, or empty when it carries no
/// explicit denial/failure marker at all.
QString explicitFailureClass(const QJsonObject& result) {
    const QString declared = result.value(QStringLiteral("failure_class")).toString().trimmed();
    if (!declared.isEmpty()) {
        return declared;
    }
    if (result.value(QStringLiteral("policy_denied")).toBool(false)) {
        return QStringLiteral("policy_denied");
    }
    if (result.value(QStringLiteral("handler_missing")).toBool(false)) {
        return QStringLiteral("handler_missing");
    }
    if (result.value(QStringLiteral("lease_denied")).toBool(false)) {
        return QStringLiteral("lease_denied");
    }
    return {};
}

LedgerReadResult parseHealthLedgerDocument(const QString& path,
                                           QJsonDocument* document,
                                           QString* error_message) {
    QByteArray bytes;
    const LedgerReadResult read_result = readLedgerBytes(path, &bytes, error_message);
    if (read_result != LedgerReadResult::Loaded) {
        return read_result;
    }
    QJsonParseError parse_error;
    *document = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Invalid AI tool health ledger JSON: %1")
                                 .arg(parse_error.errorString());
        }
        return LedgerReadResult::Error;
    }
    if (!document->isObject()) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Invalid AI tool health ledger JSON root: expected object");
        }
        return LedgerReadResult::Error;
    }
    return LedgerReadResult::Loaded;
}

/// The reason a parsed ledger document's envelope cannot be trusted, or empty when every
/// claim it makes about itself holds. @p records receives the entry array only once it is
/// known to be one.
QString ledgerEnvelopeRefusal(const QJsonObject& root, QJsonArray* records) {
    if (root.value(QStringLiteral("schema_version")).toInt(-1) != kAiToolHealthSchemaVersion) {
        return QStringLiteral("unsupported or missing schema_version");
    }
    const QJsonValue records_value = root.value(QStringLiteral("records"));
    if (!records_value.isArray()) {
        // A missing or wrong-typed array used to collapse into an empty one and REPLACE
        // every in-memory record, so a truncated or tampered file silently cleared the
        // whole circuit breaker. Refuse it and keep what is already in memory.
        return QStringLiteral("'records' is missing or is not an array");
    }
    *records = records_value.toArray();
    if (records->size() > kAiToolHealthMaxRecords) {
        return QStringLiteral("more records than the ceiling of %1").arg(kAiToolHealthMaxRecords);
    }
    if (root.value(QStringLiteral("record_count")).toInt(-1) != records->size()) {
        return QStringLiteral("record_count does not match the records array");
    }
    return {};
}

/// Report a failed ledger write through the optional out-parameter. Every write step fails
/// the same way, so the null check lives here once instead of guarding each of them.
bool ledgerWriteFailed(QString* error_message, const QString& reason) {
    if (error_message != nullptr) {
        *error_message = reason;
    }
    return false;
}

std::optional<LedgerHandle> createLedgerTempFile(HANDLE directory_handle,
                                                 const QString& path,
                                                 const QString& target_leaf,
                                                 QString* temp_path,
                                                 QString* error_message) {
    const QString temp_leaf = QStringLiteral(".%1.%2.tmp")
                                  .arg(target_leaf,
                                       QUuid::createUuid().toString(QUuid::WithoutBraces));
    *temp_path =
        QDir::toNativeSeparators(QDir(ledgerFinalPath(directory_handle)).filePath(temp_leaf));
    if (g_before_temp_open_hook) {
        g_before_temp_open_hook(path);
    }
    LedgerHandle file(CreateFileW(reinterpret_cast<LPCWSTR>(temp_path->utf16()),
                                  GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH |
                                      FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
    if (!file.valid()) {
        ledgerWriteFailed(error_message,
                          ledgerWin32Error(QStringLiteral("Could not create ledger temporary file"),
                                           *temp_path,
                                           GetLastError()));
        return std::nullopt;
    }
    const bool temp_parent_matches = ledgerFileParentMatches(file.get(), directory_handle);
    if (ledgerHandleIsReparse(file.get()) || !temp_parent_matches) {
        ledgerWriteFailed(
            error_message,
            QStringLiteral("Refusing redirected ledger temporary file: %1").arg(*temp_path));
        return std::nullopt;
    }
    return file;
}

bool writeAndFlushLedgerFile(HANDLE file_handle,
                             const QString& path,
                             const QByteArray& payload,
                             QString* error_message) {
    DWORD written = 0;
    if (WriteFile(file_handle,
                  payload.constData(),
                  static_cast<DWORD>(payload.size()),
                  &written,
                  nullptr) == 0) {
        return ledgerWriteFailed(
            error_message,
            ledgerWin32Error(
                QStringLiteral("Could not write AI tool health ledger"), path, GetLastError()));
    }
    if (written != static_cast<DWORD>(payload.size())) {
        return ledgerWriteFailed(error_message,
                                 QStringLiteral(
                                     "Short write to AI tool health ledger (%1 of %2 bytes): %3")
                                     .arg(written)
                                     .arg(payload.size())
                                     .arg(path));
    }
    if (FlushFileBuffers(file_handle) == 0) {
        return ledgerWriteFailed(
            error_message,
            ledgerWin32Error(
                QStringLiteral("Could not flush AI tool health ledger"), path, GetLastError()));
    }
    return true;
}

bool commitLedgerFile(HANDLE file_handle,
                      HANDLE directory_handle,
                      const QString& path,
                      const QString& target_leaf,
                      QString* error_message) {
    const std::wstring target_name =
        QDir::toNativeSeparators(QDir(ledgerFinalPath(directory_handle)).filePath(target_leaf))
            .toStdWString();
    const DWORD target_name_bytes =
        static_cast<DWORD>(target_name.size() * sizeof(std::wstring::value_type));
    QByteArray rename_storage(static_cast<qsizetype>(sizeof(FILE_RENAME_INFO) + target_name_bytes),
                              0);
    auto* rename_info = reinterpret_cast<FILE_RENAME_INFO*>(rename_storage.data());
    rename_info->ReplaceIfExists = TRUE;
    rename_info->RootDirectory = nullptr;
    rename_info->FileNameLength = target_name_bytes;
    std::memcpy(rename_info->FileName, target_name.data(), target_name_bytes);
    if (SetFileInformationByHandle(
            file_handle, FileRenameInfo, rename_info, static_cast<DWORD>(rename_storage.size())) ==
        0) {
        return ledgerWriteFailed(
            error_message,
            ledgerWin32Error(
                QStringLiteral("Could not commit AI tool health ledger"), path, GetLastError()));
    }
    return true;
}

bool writeLedgerPayload(const QString& path, const QByteArray& payload, QString* error_message) {
    if (payload.size() > kAiToolHealthMaxLedgerBytes) {
        return ledgerWriteFailed(error_message,
                                 QStringLiteral(
                                     "AI tool health ledger payload exceeds %1 bytes (%2)")
                                     .arg(kAiToolHealthMaxLedgerBytes)
                                     .arg(payload.size()));
    }
    auto directory = openLedgerDirectory(path, true, true, error_message);
    if (!directory) {
        return false;
    }
    const QString target_leaf = QFileInfo(path).fileName();
    if (!sak::isSafeChildName(target_leaf)) {
        return ledgerWriteFailed(
            error_message,
            QStringLiteral("AI tool health ledger filename is unsafe: %1").arg(target_leaf));
    }
    QString temp_path;
    auto file =
        createLedgerTempFile(directory->get(), path, target_leaf, &temp_path, error_message);
    if (!file) {
        return false;
    }
    auto cleanup = qScopeGuard([&] { DeleteFileW(reinterpret_cast<LPCWSTR>(temp_path.utf16())); });
    if (!writeAndFlushLedgerFile(file->get(), path, payload, error_message) ||
        !commitLedgerFile(file->get(), directory->get(), path, target_leaf, error_message)) {
        return false;
    }
    cleanup.dismiss();
    return true;
}

bool messageContainsAny(const QString& message, const QStringList& markers) {
    return std::ranges::any_of(markers,
                               [&message](const auto& marker) { return message.contains(marker); });
}

}  // namespace

QJsonObject AiToolHealthRecord::toJson() const {
    QJsonObject object;
    object[QStringLiteral("key")] = key;
    object[QStringLiteral("last_success_utc")] = iso(last_success_utc);
    object[QStringLiteral("last_failure_utc")] = iso(last_failure_utc);
    object[QStringLiteral("disabled_until_utc")] = iso(disabled_until_utc);
    object[QStringLiteral("last_failure_class")] = last_failure_class;
    object[QStringLiteral("last_error_message")] = last_error_message;
    object[QStringLiteral("last_latency_ms")] = static_cast<double>(last_latency_ms);
    object[QStringLiteral("success_count")] = success_count;
    object[QStringLiteral("failure_count")] = failure_count;
    object[QStringLiteral("consecutive_failures")] = consecutive_failures;
    return object;
}

AiToolHealthRecord AiToolHealthRecord::fromJson(const QJsonObject& object, bool* ok) {
    if (ok != nullptr) {
        *ok = false;
    }
    if (!recordTextFieldsOk(object)) {
        return {};
    }
    AiToolHealthRecord record;
    record.key = normalizeKey(object.value(QStringLiteral("key")).toString());
    if (record.key.isEmpty() || record.key.size() > kAiToolHealthMaxKeyChars) {
        return {};
    }
    if (!parseRecordTimestamps(object, &record)) {
        return {};
    }
    record.last_failure_class = object.value(QStringLiteral("last_failure_class"))
                                    .toString()
                                    .left(kHealthErrorPreviewChars);
    record.last_error_message = object.value(QStringLiteral("last_error_message"))
                                    .toString()
                                    .left(kHealthErrorPreviewChars);
    if (!parseRecordCounters(object, &record)) {
        return {};
    }
    if (ok != nullptr) {
        *ok = true;
    }
    return record;
}

QJsonObject AiToolAvailability::toJson() const {
    QJsonObject object;
    object[QStringLiteral("available")] = available;
    object[QStringLiteral("key")] = key;
    object[QStringLiteral("failure_class")] = failure_class;
    object[QStringLiteral("reason")] = reason;
    object[QStringLiteral("disabled_until_utc")] = iso(disabled_until_utc);
    return object;
}

void AiToolHealthLedger::setBeforeFileOpenHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_open_hook = std::move(hook);
}

void AiToolHealthLedger::setBeforeFileReadHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_read_hook = std::move(hook);
}

void AiToolHealthLedger::setBeforeTempOpenHookForTesting(std::function<void(const QString&)> hook) {
    g_before_temp_open_hook = std::move(hook);
}

AiToolHealthLedger::AiToolHealthLedger(int suppress_after_failures,
                                       int base_backoff_ms,
                                       int max_backoff_ms)
    : m_suppress_after_failures(std::max(1, suppress_after_failures))
    , m_base_backoff_ms(std::max(1, base_backoff_ms))
    , m_max_backoff_ms(std::max(m_base_backoff_ms, max_backoff_ms)) {}

AiToolAvailability AiToolHealthLedger::check(const QString& key, QDateTime now_utc) const {
    const QMutexLocker locker(&m_mutex);
    const QString normalized = normalizeKey(key);
    AiToolAvailability result;
    result.key = normalized;
    // AiToolAvailability defaults to unavailable, so every "no suppression applies"
    // conclusion is stated explicitly here rather than inherited from a default.
    result.available = true;
    if (normalized.isEmpty()) {
        // A blank key identifies nothing, and recordSuccess/recordFailure refuse it too,
        // so no record can exist for it: there is no suppression to bypass. Denying here
        // instead would block every dispatch whose health key is legitimately unset.
        return result;
    }

    const auto it = m_records.constFind(normalized);
    if (it == m_records.constEnd()) {
        return result;
    }

    const QDateTime now = normalizedNow(now_utc);
    if (it->disabled_until_utc.isValid() && it->disabled_until_utc > now) {
        result.available = false;
        result.failure_class = QStringLiteral("health_backoff");
        result.disabled_until_utc = it->disabled_until_utc;
        result.reason = QStringLiteral(
                            "Tool/provider '%1' is temporarily disabled until %2 after %3 "
                            "consecutive failure(s): %4")
                            .arg(normalized,
                                 iso(it->disabled_until_utc),
                                 QString::number(it->consecutive_failures),
                                 it->last_error_message);
    }
    return result;
}

AiToolHealthRecord* AiToolHealthLedger::recordForUpdateUnlocked(const QString& normalized_key) {
    if (normalized_key.isEmpty() || normalized_key.size() > kAiToolHealthMaxKeyChars) {
        return nullptr;
    }
    const auto existing = m_records.find(normalized_key);
    if (existing != m_records.end()) {
        return &existing.value();
    }
    if (m_records.size() >= kAiToolHealthMaxRecords) {
        // Refuse to grow past the ceiling rather than let model- or response-supplied
        // keys expand m_records, every snapshot and the persisted file without bound.
        sak::logWarning(
            "AI tool health ledger is at its record ceiling; a new tool key is not tracked");
        return nullptr;
    }
    auto& new_record = m_records[normalized_key];
    new_record.key = normalized_key;
    return &new_record;
}

void AiToolHealthLedger::recordSuccess(const QString& key, qint64 latency_ms, QDateTime now_utc) {
    const QMutexLocker locker(&m_mutex);
    AiToolHealthRecord* rec = recordForUpdateUnlocked(normalizeKey(key));
    if (rec == nullptr) {
        return;
    }
    rec->last_success_utc = normalizedNow(now_utc);
    rec->last_latency_ms = std::clamp<qint64>(latency_ms, 0, kAiToolHealthMaxLatencyMs);
    incrementSaturating(&rec->success_count);
    rec->consecutive_failures = 0;
    rec->disabled_until_utc = {};
    persistIfConfigured();
}

void AiToolHealthLedger::recordFailure(const QString& key,
                                       const QString& failure_class,
                                       const QString& error_message,
                                       qint64 latency_ms,
                                       QDateTime now_utc) {
    const QMutexLocker locker(&m_mutex);
    AiToolHealthRecord* rec = recordForUpdateUnlocked(normalizeKey(key));
    if (rec == nullptr) {
        return;
    }
    const QDateTime now = normalizedNow(now_utc);
    rec->last_failure_utc = now;
    rec->last_latency_ms = std::clamp<qint64>(latency_ms, 0, kAiToolHealthMaxLatencyMs);
    rec->last_failure_class = failure_class.trimmed().isEmpty()
                                  ? QStringLiteral("tool_failed")
                                  : failure_class.trimmed().left(kHealthErrorPreviewChars);
    rec->last_error_message = error_message.trimmed().left(kHealthErrorPreviewChars);
    incrementSaturating(&rec->failure_count);
    incrementSaturating(&rec->consecutive_failures);
    if (rec->consecutive_failures >= m_suppress_after_failures) {
        rec->disabled_until_utc = now.addMSecs(backoffMs(rec->consecutive_failures));
    }
    persistIfConfigured();
}

void AiToolHealthLedger::setPersistencePath(const QString& path, int ttl_hours) {
    const QMutexLocker locker(&m_mutex);
    m_persistence_path = path.trimmed();
    // Bound the TTL. The freshness cutoff multiplies it by the seconds in an hour, which
    // overflows a plain int past ~596k hours (undefined behavior, and a corrupt cutoff),
    // and no caller has business asking to retain health state for longer than a year.
    m_ttl_hours = std::clamp(ttl_hours, 1, kAiToolHealthMaxTtlHours);
}

QString AiToolHealthLedger::persistencePath() const {
    const QMutexLocker locker(&m_mutex);
    return m_persistence_path;
}

bool AiToolHealthLedger::load(QString* error_message) {
    const QMutexLocker locker(&m_mutex);
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (m_persistence_path.isEmpty()) {
        return true;  // Persistence not configured: nothing to load.
    }
    if (ledgerPathUnsafe(m_persistence_path, error_message)) {
        return false;
    }
    QJsonDocument doc;
    const LedgerReadResult read_result =
        parseHealthLedgerDocument(m_persistence_path, &doc, error_message);
    if (read_result == LedgerReadResult::Missing) {
        return true;  // First run: no ledger has been written yet.
    }
    if (read_result == LedgerReadResult::Error) {
        return false;
    }
    return loadRecordsUnlocked(doc.object(), error_message);
}

bool AiToolHealthLedger::loadRecordsUnlocked(const QJsonObject& root, QString* error_message) {
    const auto refuse = [error_message](const QString& reason) {
        if (error_message) {
            *error_message = QStringLiteral("Refusing AI tool health ledger: %1").arg(reason);
        }
        return false;
    };
    QJsonArray records;
    const QString envelope_refusal = ledgerEnvelopeRefusal(root, &records);
    if (!envelope_refusal.isEmpty()) {
        return refuse(envelope_refusal);
    }

    const QDateTime now = normalizedNow({});
    QHash<QString, AiToolHealthRecord> loaded;
    for (const auto& value : records) {
        bool record_ok = false;
        const AiToolHealthRecord parsed_record = AiToolHealthRecord::fromJson(value.toObject(),
                                                                              &record_ok);
        if (!value.isObject() || !record_ok || !persistedRecordIsSane(parsed_record, now)) {
            return refuse(QStringLiteral("a record is malformed or inconsistent"));
        }
        if (loaded.contains(parsed_record.key)) {
            return refuse(QStringLiteral("duplicate record key '%1'").arg(parsed_record.key));
        }
        if (recordIsFresh(parsed_record, now)) {
            loaded.insert(parsed_record.key, parsed_record);
        }
    }
    m_records = std::move(loaded);
    return true;
}

bool AiToolHealthLedger::save(QString* error_message) const {
    const QMutexLocker locker(&m_mutex);
    return saveUnlocked(error_message);
}

bool AiToolHealthLedger::saveUnlocked(QString* error_message) const {
    if (error_message != nullptr) {
        error_message->clear();
    }
    if (m_persistence_path.isEmpty()) {
        return true;  // Persistence not configured: there is nothing to write.
    }
    if (ledgerPathUnsafe(m_persistence_path, error_message)) {
        return false;
    }

    const QByteArray payload = QJsonDocument(snapshotUnlocked()).toJson(QJsonDocument::Indented);
    return writeLedgerPayload(m_persistence_path, payload, error_message);
}

void AiToolHealthLedger::pruneExpired(QDateTime now_utc) {
    const QMutexLocker locker(&m_mutex);
    const QDateTime now = normalizedNow(now_utc);
    for (auto it = m_records.begin(); it != m_records.end();) {
        if (recordIsFresh(it.value(), now)) {
            ++it;
        } else {
            it = m_records.erase(it);
        }
    }
    persistIfConfigured();
}

AiToolHealthRecord AiToolHealthLedger::record(const QString& key) const {
    const QMutexLocker locker(&m_mutex);
    return m_records.value(normalizeKey(key));
}

QJsonObject AiToolHealthLedger::snapshot() const {
    const QMutexLocker locker(&m_mutex);
    return snapshotUnlocked();
}

QJsonObject AiToolHealthLedger::snapshotUnlocked() const {
    QJsonArray records;
    QStringList keys = m_records.keys();
    keys.sort(Qt::CaseSensitive);
    for (const QString& key : keys) {
        records.append(m_records.value(key).toJson());
    }
    QJsonObject object;
    object[QStringLiteral("schema_version")] = 1;
    object[QStringLiteral("saved_utc")] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    object[QStringLiteral("ttl_hours")] = m_ttl_hours;
    object[QStringLiteral("records")] = records;
    object[QStringLiteral("record_count")] = records.size();
    return object;
}

int AiToolHealthLedger::size() const {
    const QMutexLocker locker(&m_mutex);
    return static_cast<int>(m_records.size());
}

QString AiToolHealthLedger::classifyResult(const QJsonObject& result) {
    const QString explicit_class = explicitFailureClass(result);
    if (result.value(QStringLiteral("success")).toBool(false)) {
        // A result cannot be both a success and a denial/failure. Believing the success
        // flag over the field that contradicts it would walk a malformed (or hostile
        // tool-server) result straight past the health circuit breaker, so a contradictory
        // result is classified as the failure it also claims to be. With no such field
        // this is the empty "succeeded" classification, exactly as before.
        return explicit_class;
    }
    if (!explicit_class.isEmpty()) {
        return explicit_class;
    }
    const QString message = result.value(QStringLiteral("error_message")).toString().toLower();
    if (messageContainsAny(message, {QStringLiteral("checksum")})) {
        return QStringLiteral("checksum_mismatch");
    }
    if (messageContainsAny(message, {QStringLiteral("timed out"), QStringLiteral("timeout")})) {
        return QStringLiteral("timeout");
    }
    if (messageContainsAny(message, {QStringLiteral("cancelled"), QStringLiteral("canceled")})) {
        return QStringLiteral("cancelled");
    }
    if (messageContainsAny(message, {QStringLiteral("unavailable"), QStringLiteral("missing")})) {
        return QStringLiteral("unavailable");
    }
    return QStringLiteral("tool_failed");
}

QDateTime AiToolHealthLedger::normalizedNow(QDateTime now_utc) {
    return now_utc.isValid() ? now_utc.toUTC() : QDateTime::currentDateTimeUtc();
}

int AiToolHealthLedger::backoffMs(int consecutive_failures) const {
    int factor = 1;
    const int exponent = std::max(0, consecutive_failures - m_suppress_after_failures);
    for (int i = 0; i < exponent && factor < kBackoffFactorMax; ++i) {
        factor *= kBackoffFactorMultiplier;
    }
    // Compute in qint64 so a large configured base times the capped factor cannot overflow
    // int (UB) before the max_backoff clamp is applied.
    const qint64 scaled = static_cast<qint64>(m_base_backoff_ms) * factor;
    return static_cast<int>(std::min<qint64>(m_max_backoff_ms, scaled));
}

bool AiToolHealthLedger::persistedRecordIsSane(const AiToolHealthRecord& record,
                                               QDateTime now_utc) const {
    const QDateTime now = normalizedNow(now_utc);
    const QDateTime skew_limit = now.addMSecs(kClockSkewToleranceMs);
    if (record.last_success_utc.isValid() && record.last_success_utc > skew_limit) {
        return false;
    }
    if (record.last_failure_utc.isValid() && record.last_failure_utc > skew_limit) {
        return false;
    }
    // A disable must be caused by a threshold-reaching failure and stay inside the exact window
    // this ledger can write. A standalone future disabled_until stamp is a forged suppression.
    if (record.disabled_until_utc.isValid()) {
        if (!record.last_failure_utc.isValid() ||
            record.consecutive_failures < m_suppress_after_failures ||
            record.disabled_until_utc < record.last_failure_utc ||
            record.disabled_until_utc > record.last_failure_utc.addMSecs(m_max_backoff_ms)) {
            return false;
        }
    }
    return record.consecutive_failures <= record.failure_count;
}

bool AiToolHealthLedger::recordIsFresh(const AiToolHealthRecord& record, QDateTime now_utc) const {
    const QDateTime now = normalizedNow(now_utc);
    // Widen before multiplying: m_ttl_hours is bounded, but the product is computed in
    // qint64 so the seconds arithmetic can never overflow int (undefined behavior).
    const QDateTime cutoff = now.addSecs(-static_cast<qint64>(m_ttl_hours) * kSecondsPerHour);
    if (record.disabled_until_utc.isValid() && record.disabled_until_utc > now) {
        return true;
    }
    if (record.last_success_utc.isValid() && record.last_success_utc >= cutoff) {
        return true;
    }
    return record.last_failure_utc.isValid() && record.last_failure_utc >= cutoff;
}

void AiToolHealthLedger::persistIfConfigured() const {
    // Callers already hold m_mutex; use the unlocked save to avoid re-locking.
    if (m_persistence_path.isEmpty()) {
        return;
    }
    QString error;
    if (!saveUnlocked(&error)) {
        // The in-memory ledger is still authoritative for this process, but a failed
        // write means the suppression state will NOT survive a restart. The mutators are
        // void by contract, so surface the real error here instead of discarding it.
        sak::logWarning(
            QStringLiteral("Could not persist AI tool health ledger: %1").arg(error).toStdString());
    }
}

}  // namespace sak::ai
