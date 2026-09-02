// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_store_file_loader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include <windows.h>

namespace sak::ai::detail {

namespace {

constexpr DWORD kFinalPathInitialChars = 1024;
constexpr qsizetype kExtendedUncPrefixChars = 8;
constexpr qsizetype kExtendedPathPrefixChars = 4;

void appendError(QStringList* errors, const QString& message) {
    if (errors != nullptr) {
        errors->append(message);
    }
}

QString lowerStoreName(const StoreFileLoadPolicy& policy) {
    return policy.store_name.toLower();
}

QString extensionPattern(const StoreFileLoadPolicy& policy) {
    return QStringLiteral("*") + policy.file_extension;
}

bool validPolicy(const StoreFileLoadPolicy& policy, QStringList* errors) {
    constexpr qint64 kMaxReadFileBytes = static_cast<qint64>(std::numeric_limits<DWORD>::max()) - 1;
    if (!policy.store_name.isEmpty() && policy.file_extension.startsWith(QLatin1Char('.')) &&
        !policy.file_type_name.isEmpty() && policy.max_file_bytes > 0 &&
        policy.max_file_bytes <= kMaxReadFileBytes && policy.max_files > 0) {
        return true;
    }
    appendError(errors, QStringLiteral("Invalid AI store file-load policy"));
    return false;
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~ScopedHandle() {
        if (valid()) {
            CloseHandle(m_handle);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&&) = delete;
    ScopedHandle& operator=(ScopedHandle&&) = delete;
    [[nodiscard]] HANDLE get() const { return m_handle; }
    [[nodiscard]] bool valid() const { return m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle;
};

class ScopedFindHandle {
public:
    explicit ScopedFindHandle(HANDLE handle) : m_handle(handle) {}
    ~ScopedFindHandle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            FindClose(m_handle);
        }
    }
    ScopedFindHandle(const ScopedFindHandle&) = delete;
    ScopedFindHandle& operator=(const ScopedFindHandle&) = delete;

private:
    HANDLE m_handle;
};

QString normalizedFinalPath(HANDLE handle) {
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

bool sameObject(HANDLE left, HANDLE right) {
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

bool handleIsReparsePoint(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    return GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) == 0 ||
           (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

QString win32Error(const QString& action, const QString& path, DWORD error) {
    return QStringLiteral("%1: %2 (Win32 error %3)").arg(action, path).arg(error);
}

bool loadResourceFile(const QString& path,
                      const StoreFileLoadPolicy& policy,
                      const StoreFileConsumer& consume,
                      QStringList* errors) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendError(errors, QStringLiteral("%1: %2").arg(path, file.errorString()));
        return false;
    }
    const qint64 size = file.size();
    if (size < 0 || size > policy.max_file_bytes) {
        appendError(errors,
                    QStringLiteral("%1 file too large or unreadable (%2 bytes, max %3): %4")
                        .arg(policy.store_name)
                        .arg(size)
                        .arg(policy.max_file_bytes)
                        .arg(path));
        return false;
    }
    const QByteArray bytes = file.read(policy.max_file_bytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != size) {
        appendError(errors,
                    QStringLiteral("Resource %1 read was incomplete: %2")
                        .arg(lowerStoreName(policy), path));
        return false;
    }
    return consume(path, bytes);
}

bool loadResourceDirectory(const QString& directory,
                           const StoreFileLoadPolicy& policy,
                           const StoreFileConsumer& consume,
                           QStringList* errors) {
    const QDir resource_directory(directory);
    if (!resource_directory.exists()) {
        appendError(errors,
                    QStringLiteral("%1 resource directory not found: %2")
                        .arg(policy.store_name, directory));
        return false;
    }
    const QStringList files =
        resource_directory.entryList({extensionPattern(policy)}, QDir::Files, QDir::Name);
    if (files.size() > policy.max_files) {
        appendError(errors,
                    QStringLiteral("%1 resource directory exceeds %2 files: %3")
                        .arg(policy.store_name)
                        .arg(policy.max_files)
                        .arg(directory));
        return false;
    }
    bool ok = true;
    for (const QString& file_name : files) {
        if (!loadResourceFile(resource_directory.filePath(file_name), policy, consume, errors)) {
            ok = false;
        }
    }
    return ok;
}

struct NativeStoreFiles {
    QStringList names;
    bool capped{false};
};

bool collectStoreFileName(const WIN32_FIND_DATAW& data,
                          const StoreFileLoadPolicy& policy,
                          NativeStoreFiles* result) {
    const QString name = QString::fromWCharArray(data.cFileName);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !name.endsWith(policy.file_extension, Qt::CaseInsensitive)) {
        return true;
    }
    if (result->names.size() >= policy.max_files) {
        result->capped = true;
        return false;
    }
    result->names.append(name);
    return true;
}

std::optional<NativeStoreFiles> enumerateNativeStoreFiles(HANDLE directory_handle,
                                                          const StoreFileLoadPolicy& policy,
                                                          QStringList* errors) {
    const QString directory_path = normalizedFinalPath(directory_handle);
    if (directory_path.isEmpty()) {
        appendError(
            errors,
            QStringLiteral("Cannot resolve opened %1 directory").arg(lowerStoreName(policy)));
        return std::nullopt;
    }
    const QString pattern =
        QDir::toNativeSeparators(QDir(directory_path).filePath(extensionPattern(policy)));
    WIN32_FIND_DATAW data{};
    const HANDLE raw_find = FindFirstFileExW(reinterpret_cast<LPCWSTR>(pattern.utf16()),
                                             FindExInfoBasic,
                                             &data,
                                             FindExSearchNameMatch,
                                             nullptr,
                                             FIND_FIRST_EX_LARGE_FETCH);
    if (raw_find == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) {
            return NativeStoreFiles{};
        }
        appendError(
            errors,
            win32Error(QStringLiteral("Cannot enumerate %1 directory").arg(lowerStoreName(policy)),
                       directory_path,
                       error));
        return std::nullopt;
    }
    const ScopedFindHandle find_handle(raw_find);
    NativeStoreFiles result;
    while (true) {
        if (!collectStoreFileName(data, policy, &result)) {
            break;
        }
        if (FindNextFileW(raw_find, &data) == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                appendError(errors,
                            win32Error(QStringLiteral("%1 directory enumeration failed")
                                           .arg(policy.store_name),
                                       directory_path,
                                       error));
                return std::nullopt;
            }
            break;
        }
    }
    std::sort(result.names.begin(),
              result.names.end(),
              [](const QString& left, const QString& right) {
                  const int folded = QString::compare(left, right, Qt::CaseInsensitive);
                  return folded == 0 ? left < right : folded < 0;
              });
    return result;
}

std::optional<qint64> handleFileSize(HANDLE handle) {
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0 || size.QuadPart < 0) {
        return std::nullopt;
    }
    return size.QuadPart;
}

std::optional<QByteArray> readStableStoreBytes(HANDLE file_handle,
                                               const QString& display_path,
                                               const StoreFileLoadPolicy& policy,
                                               const StoreFileLoadHooks& hooks,
                                               QStringList* errors) {
    const auto size_before = handleFileSize(file_handle);
    if (!size_before || *size_before > policy.max_file_bytes) {
        appendError(errors,
                    QStringLiteral("%1 file too large or unreadable (max %2): %3")
                        .arg(policy.store_name)
                        .arg(policy.max_file_bytes)
                        .arg(display_path));
        return std::nullopt;
    }
    if (hooks.before_file_read) {
        hooks.before_file_read(display_path);
    }
    QByteArray bytes(static_cast<qsizetype>(policy.max_file_bytes + 1), Qt::Uninitialized);
    DWORD bytes_read = 0;
    if (ReadFile(
            file_handle, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr) ==
        0) {
        appendError(errors,
                    win32Error(QStringLiteral("Cannot read %1 file").arg(lowerStoreName(policy)),
                               display_path,
                               GetLastError()));
        return std::nullopt;
    }
    bytes.resize(static_cast<qsizetype>(bytes_read));
    const auto size_after = handleFileSize(file_handle);
    if (!size_after || *size_after > policy.max_file_bytes) {
        appendError(errors,
                    QStringLiteral("%1 file too large or changed during read: %2")
                        .arg(policy.store_name, display_path));
        return std::nullopt;
    }
    if (*size_before != *size_after || *size_after != bytes.size()) {
        appendError(errors,
                    QStringLiteral("%1 file changed during read (%2 -> %3, read %4): %5")
                        .arg(policy.store_name)
                        .arg(*size_before)
                        .arg(*size_after)
                        .arg(bytes.size())
                        .arg(display_path));
        return std::nullopt;
    }
    return bytes;
}

bool fileParentMatchesDirectory(HANDLE file_handle, HANDLE directory_handle) {
    const QString file_path = normalizedFinalPath(file_handle);
    if (file_path.isEmpty()) {
        return false;
    }
    const QString parent_path = QDir::toNativeSeparators(QFileInfo(file_path).absolutePath());
    const ScopedHandle parent(CreateFileW(reinterpret_cast<LPCWSTR>(parent_path.utf16()),
                                          FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr));
    return parent.valid() && !handleIsReparsePoint(parent.get()) &&
           sameObject(parent.get(), directory_handle);
}

struct StoreLoadContext {
    const StoreFileLoadPolicy& policy;
    const StoreFileLoadHooks& hooks;
    const StoreFileConsumer& consume;
    QStringList* errors;
};

bool loadNativeFile(HANDLE directory_handle,
                    const QString& display_directory,
                    const QString& file_name,
                    const StoreLoadContext& context) {
    const QString opened_directory = normalizedFinalPath(directory_handle);
    if (opened_directory.isEmpty()) {
        appendError(context.errors,
                    QStringLiteral("Cannot resolve opened %1 directory")
                        .arg(lowerStoreName(context.policy)));
        return false;
    }
    const QString opened_path =
        QDir::toNativeSeparators(QDir(opened_directory).filePath(file_name));
    const QString display_path = QDir(display_directory).filePath(file_name);
    if (context.hooks.before_file_open) {
        context.hooks.before_file_open(display_path);
    }
    const ScopedHandle file(CreateFileW(reinterpret_cast<LPCWSTR>(opened_path.utf16()),
                                        GENERIC_READ | FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                            FILE_FLAG_SEQUENTIAL_SCAN,
                                        nullptr));
    if (!file.valid()) {
        appendError(
            context.errors,
            win32Error(QStringLiteral("Cannot open %1 file").arg(lowerStoreName(context.policy)),
                       display_path,
                       GetLastError()));
        return false;
    }
    if (handleIsReparsePoint(file.get()) ||
        !fileParentMatchesDirectory(file.get(), directory_handle)) {
        appendError(context.errors,
                    QStringLiteral("Refusing redirected %1 file outside opened directory: %2")
                        .arg(lowerStoreName(context.policy), display_path));
        return false;
    }
    const auto bytes = readStableStoreBytes(
        file.get(), display_path, context.policy, context.hooks, context.errors);
    return bytes && context.consume(display_path, *bytes);
}

bool loadNativeDirectory(const QString& directory,
                         const StoreFileLoadPolicy& policy,
                         const StoreFileLoadHooks& hooks,
                         const StoreFileConsumer& consume,
                         QStringList* errors) {
    if (!QDir::isAbsolutePath(directory)) {
        appendError(errors,
                    QStringLiteral("%1 directory must be an absolute local path: %2")
                        .arg(policy.store_name, directory));
        return false;
    }
    const QString native_directory = QDir::toNativeSeparators(QDir::cleanPath(directory));
    const ScopedHandle directory_handle(
        CreateFileW(reinterpret_cast<LPCWSTR>(native_directory.utf16()),
                    FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr));
    if (!directory_handle.valid()) {
        appendError(
            errors,
            win32Error(QStringLiteral("Cannot open %1 directory").arg(lowerStoreName(policy)),
                       directory,
                       GetLastError()));
        return false;
    }
    if (handleIsReparsePoint(directory_handle.get())) {
        appendError(errors,
                    QStringLiteral("Refusing %1 directory reparse point: %2")
                        .arg(lowerStoreName(policy), directory));
        return false;
    }
    const auto enumeration = enumerateNativeStoreFiles(directory_handle.get(), policy, errors);
    if (!enumeration) {
        return false;
    }
    if (enumeration->capped) {
        appendError(errors,
                    QStringLiteral("%1 directory has more than %2 %3 files: %4")
                        .arg(policy.store_name)
                        .arg(policy.max_files)
                        .arg(policy.file_type_name)
                        .arg(directory));
        return false;
    }
    bool ok = true;
    const StoreLoadContext context{policy, hooks, consume, errors};
    for (const QString& file_name : enumeration->names) {
        if (!loadNativeFile(directory_handle.get(), directory, file_name, context)) {
            ok = false;
        }
    }
    return ok;
}

}  // namespace

bool loadStoreDirectoryFiles(const QString& directory,
                             const StoreFileLoadPolicy& policy,
                             const StoreFileLoadHooks& hooks,
                             const StoreFileConsumer& consume,
                             QStringList* errors) {
    if (!validPolicy(policy, errors) || !consume) {
        if (!consume) {
            appendError(errors, QStringLiteral("AI store file consumer is not configured"));
        }
        return false;
    }
    if (directory.startsWith(QLatin1Char(':'))) {
        return loadResourceDirectory(directory, policy, consume, errors);
    }
    return loadNativeDirectory(directory, policy, hooks, consume, errors);
}

}  // namespace sak::ai::detail
