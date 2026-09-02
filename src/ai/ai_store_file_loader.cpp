// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_store_file_loader.h"

#include "sak/windows_path_policy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>

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

void setError(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
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
    ScopedHandle(ScopedHandle&& other) noexcept
        : m_handle(std::exchange(other.m_handle, INVALID_HANDLE_VALUE)) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                CloseHandle(m_handle);
            }
            m_handle = std::exchange(other.m_handle, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
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

bool validChildPolicy(const StoreChildFileLoadPolicy& policy, QStringList* errors) {
    constexpr qint64 kMaxReadFileBytes = static_cast<qint64>(std::numeric_limits<DWORD>::max()) - 1;
    const QString file_leaf = QFileInfo(policy.file_name).fileName();
    if (!policy.store_name.isEmpty() && !policy.child_type_name.isEmpty() &&
        !policy.file_name.isEmpty() && file_leaf == policy.file_name && policy.max_file_bytes > 0 &&
        policy.max_file_bytes <= kMaxReadFileBytes && policy.max_child_directories > 0) {
        return true;
    }
    appendError(errors, QStringLiteral("Invalid AI child-store file-load policy"));
    return false;
}

bool validDirectChildName(const QString& name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") &&
           QFileInfo(name).fileName() == name && !name.contains(QLatin1Char('/')) &&
           !name.contains(QLatin1Char('\\'));
}

std::optional<ScopedHandle> openPinnedRootDirectory(const QString& directory,
                                                    const QString& store_name,
                                                    bool prevent_rename,
                                                    QStringList* errors) {
    if (!QDir::isAbsolutePath(directory)) {
        appendError(errors,
                    QStringLiteral("%1 root must be an absolute local path: %2")
                        .arg(store_name, directory));
        return std::nullopt;
    }
    const QString clean_directory = QDir::cleanPath(QFileInfo(directory).absoluteFilePath());
    const QString native_directory = QDir::toNativeSeparators(clean_directory);
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
                        (prevent_rename ? 0U : FILE_SHARE_DELETE);
    ScopedHandle handle(CreateFileW(reinterpret_cast<LPCWSTR>(native_directory.utf16()),
                                    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                                    share,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr));
    if (!handle.valid()) {
        appendError(errors,
                    win32Error(QStringLiteral("Cannot open %1 root").arg(store_name),
                               directory,
                               GetLastError()));
        return std::nullopt;
    }
    const QString opened_path = normalizedFinalPath(handle.get());
    if (handleIsReparsePoint(handle.get()) || opened_path.isEmpty() ||
        opened_path.compare(clean_directory, Qt::CaseInsensitive) != 0) {
        appendError(errors,
                    QStringLiteral("Refusing redirected %1 root: %2").arg(store_name, directory));
        return std::nullopt;
    }
    return handle;
}

struct OpenedChildDirectory {
    StoreChildFileLoadResult result{StoreChildFileLoadResult::Refused};
    std::optional<ScopedHandle> handle;
    QString path;
};

struct ChildDirectoryOpenRequest {
    HANDLE parent_handle;
    QString child_name;
    QString store_name;
    bool prevent_rename;
    const StoreFileLoadHooks& hooks;
    QStringList* errors;
};

OpenedChildDirectory childDirectoryOpenFailure(const ChildDirectoryOpenRequest& request,
                                               const QString& path,
                                               DWORD error) {
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return {StoreChildFileLoadResult::Missing, std::nullopt, path};
    }
    appendError(request.errors,
                win32Error(QStringLiteral("Cannot open %1 child directory").arg(request.store_name),
                           path,
                           error));
    return {};
}

OpenedChildDirectory openPinnedChildDirectory(const ChildDirectoryOpenRequest& request) {
    if (!validDirectChildName(request.child_name)) {
        appendError(request.errors,
                    QStringLiteral("Invalid %1 child directory name: %2")
                        .arg(request.store_name, request.child_name));
        return {};
    }
    const QString parent_path = normalizedFinalPath(request.parent_handle);
    if (parent_path.isEmpty()) {
        appendError(
            request.errors,
            QStringLiteral("Cannot resolve opened %1 parent directory").arg(request.store_name));
        return {};
    }
    const QString path = QDir::cleanPath(QDir(parent_path).filePath(request.child_name));
    if (request.hooks.before_child_open) {
        request.hooks.before_child_open(path);
    }
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE |
                        (request.prevent_rename ? 0U : FILE_SHARE_DELETE);
    ScopedHandle child(
        CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES,
                    share,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr));
    if (!child.valid()) {
        return childDirectoryOpenFailure(request, path, GetLastError());
    }
    if (handleIsReparsePoint(child.get()) ||
        !fileParentMatchesDirectory(child.get(), request.parent_handle)) {
        appendError(request.errors,
                    QStringLiteral("Refusing redirected %1 child directory: %2")
                        .arg(request.store_name, path));
        return {};
    }
    const QString opened_path = normalizedFinalPath(child.get());
    if (opened_path.isEmpty()) {
        appendError(request.errors,
                    QStringLiteral("Cannot resolve opened %1 child directory: %2")
                        .arg(request.store_name, path));
        return {};
    }
    return {StoreChildFileLoadResult::Loaded, std::move(child), opened_path};
}

struct NativeChildDirectories {
    QStringList names;
    bool capped{false};
};

bool collectChildDirectoryName(const WIN32_FIND_DATAW& data,
                               const StoreChildFileLoadPolicy& policy,
                               NativeChildDirectories* result) {
    const QString name = QString::fromWCharArray(data.cFileName);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || name == QLatin1String(".") ||
        name == QLatin1String("..")) {
        return true;
    }
    if (result->names.size() >= policy.max_child_directories) {
        result->capped = true;
        return false;
    }
    result->names.append(name);
    return true;
}

struct ChildEnumerationContext {
    QString root_path;
    const StoreChildFileLoadPolicy& policy;
    QStringList* errors;
};

bool childEnumerationAdvanced(HANDLE find_handle,
                              WIN32_FIND_DATAW* data,
                              const ChildEnumerationContext& context,
                              bool* finished) {
    if (FindNextFileW(find_handle, data) != 0) {
        return true;
    }
    const DWORD error = GetLastError();
    *finished = error == ERROR_NO_MORE_FILES;
    if (*finished) {
        return true;
    }
    appendError(
        context.errors,
        win32Error(QStringLiteral("%1 root enumeration failed").arg(context.policy.store_name),
                   context.root_path,
                   error));
    return false;
}

std::optional<NativeChildDirectories> enumerateNativeChildDirectories(
    HANDLE root_handle, const StoreChildFileLoadPolicy& policy, QStringList* errors) {
    const QString root_path = normalizedFinalPath(root_handle);
    if (root_path.isEmpty()) {
        appendError(errors, QStringLiteral("Cannot resolve opened %1 root").arg(policy.store_name));
        return std::nullopt;
    }
    const QString pattern = QDir::toNativeSeparators(QDir(root_path).filePath(QStringLiteral("*")));
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
            return NativeChildDirectories{};
        }
        appendError(errors,
                    win32Error(QStringLiteral("Cannot enumerate %1 root").arg(policy.store_name),
                               root_path,
                               error));
        return std::nullopt;
    }
    const ScopedFindHandle find_handle(raw_find);
    NativeChildDirectories result;
    bool finished = false;
    const ChildEnumerationContext context{root_path, policy, errors};
    while (!finished) {
        if (!collectChildDirectoryName(data, policy, &result)) {
            break;
        }
        if (!childEnumerationAdvanced(raw_find, &data, context, &finished)) {
            return std::nullopt;
        }
    }
    std::sort(result.names.begin(), result.names.end());
    return result;
}

StoreFileLoadPolicy childFileReadPolicy(const StoreChildFileLoadPolicy& policy) {
    StoreFileLoadPolicy read_policy;
    read_policy.store_name = policy.store_name;
    read_policy.file_extension = QStringLiteral(".json");
    read_policy.file_type_name = policy.file_name;
    read_policy.max_file_bytes = policy.max_file_bytes;
    read_policy.max_files = 1;
    return read_policy;
}

struct PinnedChildFileRequest {
    HANDLE child_handle;
    QString display_child_path;
    QString child_name;
    const StoreChildFileLoadOptions& options;
    QStringList* errors;
};

StoreChildFileLoadResult loadPinnedChildFile(const PinnedChildFileRequest& request) {
    const auto& policy = request.options.policy;
    const QString opened_child = normalizedFinalPath(request.child_handle);
    const QString opened_file =
        QDir::toNativeSeparators(QDir(opened_child).filePath(policy.file_name));
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(opened_file.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return StoreChildFileLoadResult::Missing;
        }
        appendError(request.errors,
                    win32Error(QStringLiteral("Cannot inspect %1 file").arg(policy.store_name),
                               opened_file,
                               error));
        return StoreChildFileLoadResult::Refused;
    }
    const StoreFileLoadPolicy read_policy = childFileReadPolicy(policy);
    const StoreFileConsumer wrapped_consumer = [&](const QString&, const QByteArray& bytes) {
        return request.options.consume(request.child_name, request.display_child_path, bytes);
    };
    const StoreLoadContext context{
        read_policy, request.options.hooks, wrapped_consumer, request.errors};
    return loadNativeFile(
               request.child_handle, request.display_child_path, policy.file_name, context)
               ? StoreChildFileLoadResult::Loaded
               : StoreChildFileLoadResult::Refused;
}

bool validChildOptions(const StoreChildFileLoadOptions& options, QStringList* errors) {
    if (validChildPolicy(options.policy, errors) && options.consume) {
        return true;
    }
    if (!options.consume) {
        appendError(errors, QStringLiteral("AI child-store file consumer is not configured"));
    }
    return false;
}

struct OpenedChildStoreRoot {
    StoreChildFileLoadResult result{StoreChildFileLoadResult::Refused};
    std::optional<ScopedHandle> handle;
};

OpenedChildStoreRoot openChildStoreRoot(const QString& root_directory,
                                        const StoreChildFileLoadOptions& options,
                                        QStringList* errors) {
    const QString native_root = QDir::toNativeSeparators(QDir::cleanPath(root_directory));
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(native_root.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return {StoreChildFileLoadResult::Missing, std::nullopt};
        }
    }
    auto root = openPinnedRootDirectory(root_directory, options.policy.store_name, false, errors);
    if (!root) {
        return {};
    }
    return {StoreChildFileLoadResult::Loaded, std::move(root)};
}

struct ChildFileLoadContext {
    HANDLE root_handle;
    QString root_directory;
    const StoreChildFileLoadOptions& options;
    QStringList* errors;
};

StoreChildFileLoadResult loadNamedChildFile(const ChildFileLoadContext& context,
                                            const QString& child_name) {
    const auto& policy = context.options.policy;
    auto child = openPinnedChildDirectory({.parent_handle = context.root_handle,
                                           .child_name = child_name,
                                           .store_name = policy.store_name,
                                           .prevent_rename = false,
                                           .hooks = context.options.hooks,
                                           .errors = context.errors});
    if (child.result != StoreChildFileLoadResult::Loaded || !child.handle) {
        return child.result;
    }
    const QString display_child =
        QDir::cleanPath(QDir(context.root_directory).filePath(child_name));
    const int prior_error_count = context.errors == nullptr ? 0 : context.errors->size();
    const StoreChildFileLoadResult loaded =
        loadPinnedChildFile({.child_handle = child.handle->get(),
                             .display_child_path = display_child,
                             .child_name = child_name,
                             .options = context.options,
                             .errors = context.errors});
    if (loaded == StoreChildFileLoadResult::Refused && context.errors != nullptr &&
        context.errors->size() == prior_error_count) {
        appendError(
            context.errors,
            QStringLiteral("%1 consumer refused child: %2").arg(policy.store_name, child_name));
    }
    return loaded;
}

void appendChildDirectoryCapError(const QString& root_directory,
                                  const StoreChildFileLoadPolicy& policy,
                                  QStringList* errors) {
    appendError(errors,
                QStringLiteral("%1 root has more than %2 %3 directories: %4")
                    .arg(policy.store_name)
                    .arg(policy.max_child_directories)
                    .arg(policy.child_type_name)
                    .arg(root_directory));
}

struct NativeDirectoryPinState {
    std::vector<ScopedHandle> handles;
};

bool validPinRequest(const QStringList& components, int required_component_count) {
    return !components.isEmpty() && required_component_count >= 0 &&
           required_component_count <= components.size() &&
           std::ranges::none_of(components, [](const QString& component) {
               return !validDirectChildName(component);
           });
}

bool createOptionalPinnedChild(const QString& path, bool required, QStringList* errors) {
    if (required) {
        return true;
    }
    if (CreateDirectoryW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                         nullptr) != 0) {
        return true;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        return true;
    }
    appendError(
        errors,
        win32Error(QStringLiteral("Could not create pinned AI store directory"), path, error));
    return false;
}

struct PinWalkContext {
    const QStringList& components;
    int required_component_count;
    const StoreFileLoadHooks& hooks;
    QStringList* errors;
    NativeDirectoryPinState* state;
    QString* final_path;
};

bool pinDirectoryComponent(PinWalkContext& context, int index) {
    const QString child_path =
        QDir::cleanPath(QDir(*context.final_path).filePath(context.components.at(index)));
    if (!createOptionalPinnedChild(
            child_path, index < context.required_component_count, context.errors)) {
        return false;
    }
    auto child = openPinnedChildDirectory({.parent_handle = context.state->handles.back().get(),
                                           .child_name = context.components.at(index),
                                           .store_name = QStringLiteral("AI store directory"),
                                           .prevent_rename = true,
                                           .hooks = context.hooks,
                                           .errors = context.errors});
    if (child.result == StoreChildFileLoadResult::Missing) {
        appendError(context.errors,
                    QStringLiteral("Required AI store directory is missing: %1").arg(child_path));
        return false;
    }
    if (child.result != StoreChildFileLoadResult::Loaded || !child.handle) {
        return false;
    }
    *context.final_path = child.path;
    context.state->handles.emplace_back(std::move(*child.handle));
    return true;
}

struct AtomicStoreTempFile {
    ScopedHandle handle;
    QString path;
};

std::optional<AtomicStoreTempFile> createAtomicStoreTempFile(HANDLE directory_handle,
                                                             const QString& target_path,
                                                             const StoreFileLoadHooks& hooks,
                                                             QString* error_message) {
    const QString temp_leaf =
        QStringLiteral(".sak-%1.tmp").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString directory_path = normalizedFinalPath(directory_handle);
    const QString temp_path = QDir::toNativeSeparators(QDir(directory_path).filePath(temp_leaf));
    if (hooks.before_leaf_temp_open) {
        hooks.before_leaf_temp_open(temp_path);
    }
    ScopedHandle temp_file(CreateFileW(reinterpret_cast<LPCWSTR>(temp_path.utf16()),
                                       GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr,
                                       CREATE_NEW,
                                       FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH |
                                           FILE_FLAG_OPEN_REPARSE_POINT,
                                       nullptr));
    if (!temp_file.valid()) {
        setError(error_message,
                 win32Error(QStringLiteral("Could not create artifact temporary file"),
                            target_path,
                            GetLastError()));
        return std::nullopt;
    }
    if (handleIsReparsePoint(temp_file.get()) ||
        !fileParentMatchesDirectory(temp_file.get(), directory_handle)) {
        setError(
            error_message,
            QStringLiteral("Refusing redirected artifact temporary file: %1").arg(target_path));
        return std::nullopt;
    }
    return AtomicStoreTempFile{std::move(temp_file), temp_path};
}

bool writeAndFlushAtomicStoreFile(HANDLE file_handle,
                                  const QByteArray& bytes,
                                  const QString& target_path,
                                  QString* error_message) {
    constexpr qsizetype kMaxWriteBytes = static_cast<qsizetype>(std::numeric_limits<DWORD>::max());
    if (bytes.size() > kMaxWriteBytes) {
        setError(error_message,
                 QStringLiteral("Artifact payload exceeds the native write ceiling: %1")
                     .arg(target_path));
        return false;
    }
    DWORD written = 0;
    const BOOL write_ok = WriteFile(
        file_handle, bytes.constData(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    if (write_ok == 0 || written != static_cast<DWORD>(bytes.size()) ||
        FlushFileBuffers(file_handle) == 0) {
        setError(error_message,
                 win32Error(QStringLiteral("Could not write complete artifact"),
                            target_path,
                            GetLastError()));
        return false;
    }
    return true;
}

bool copyAndFlushAtomicStoreFile(HANDLE file_handle,
                                 const QString& source_path,
                                 const QString& target_path,
                                 QString* error_message) {
    constexpr qint64 kCopyChunkBytes = 1024 * 1024;
    QFile source(source_path);
    if (!source.open(QIODevice::ReadOnly)) {
        setError(error_message,
                 QStringLiteral("Could not read artifact source: %1").arg(source_path));
        return false;
    }
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(kCopyChunkBytes);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            setError(
                error_message,
                QStringLiteral("Could not read complete artifact source: %1").arg(source_path));
            return false;
        }
        DWORD written = 0;
        if (WriteFile(file_handle,
                      chunk.constData(),
                      static_cast<DWORD>(chunk.size()),
                      &written,
                      nullptr) == 0 ||
            written != static_cast<DWORD>(chunk.size())) {
            setError(error_message,
                     win32Error(QStringLiteral("Could not copy complete artifact"),
                                target_path,
                                GetLastError()));
            return false;
        }
    }
    if (FlushFileBuffers(file_handle) == 0) {
        setError(error_message,
                 win32Error(QStringLiteral("Could not flush copied artifact"),
                            target_path,
                            GetLastError()));
        return false;
    }
    return true;
}

struct AtomicPublishRequest {
    HANDLE file_handle;
    HANDLE directory_handle;
    QString filename;
    QString target_path;
    const StoreFileLoadHooks& hooks;
    QString* error_message;
};

bool publishAtomicStoreFile(const AtomicPublishRequest& request) {
    if (request.hooks.before_leaf_publish) {
        request.hooks.before_leaf_publish(request.target_path);
    }
    const std::wstring target_name =
        QDir::toNativeSeparators(
            QDir(normalizedFinalPath(request.directory_handle)).filePath(request.filename))
            .toStdWString();
    const DWORD target_bytes =
        static_cast<DWORD>(target_name.size() * sizeof(std::wstring::value_type));
    QByteArray storage(static_cast<qsizetype>(sizeof(FILE_RENAME_INFO) + target_bytes), 0);
    auto* rename_info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename_info->ReplaceIfExists = FALSE;
    rename_info->RootDirectory = nullptr;
    rename_info->FileNameLength = target_bytes;
    std::memcpy(rename_info->FileName, target_name.data(), target_bytes);
    if (SetFileInformationByHandle(
            request.file_handle, FileRenameInfo, rename_info, static_cast<DWORD>(storage.size())) ==
        0) {
        setError(request.error_message,
                 win32Error(QStringLiteral("Could not atomically publish artifact"),
                            request.target_path,
                            GetLastError()));
        return false;
    }
    if (handleIsReparsePoint(request.file_handle) ||
        !fileParentMatchesDirectory(request.file_handle, request.directory_handle)) {
        setError(request.error_message,
                 QStringLiteral("Published artifact identity changed unexpectedly: %1")
                     .arg(request.target_path));
        return false;
    }
    return true;
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

bool loadStoreChildDirectoryFiles(const QString& root_directory,
                                  const StoreChildFileLoadOptions& options,
                                  QStringList* errors) {
    if (!validChildOptions(options, errors)) {
        return false;
    }
    auto root = openChildStoreRoot(root_directory, options, errors);
    if (root.result == StoreChildFileLoadResult::Missing) {
        return true;
    }
    if (root.result != StoreChildFileLoadResult::Loaded || !root.handle) {
        return false;
    }
    const auto children =
        enumerateNativeChildDirectories(root.handle->get(), options.policy, errors);
    if (!children) {
        return false;
    }
    if (children->capped) {
        appendChildDirectoryCapError(root_directory, options.policy, errors);
        return false;
    }
    const ChildFileLoadContext context{root.handle->get(), root_directory, options, errors};
    for (const QString& child_name : children->names) {
        const StoreChildFileLoadResult loaded = loadNamedChildFile(context, child_name);
        if (loaded == StoreChildFileLoadResult::Missing) {
            continue;
        }
        if (loaded != StoreChildFileLoadResult::Loaded) {
            return false;
        }
    }
    return true;
}

StoreChildFileLoadResult loadStoreChildDirectoryFile(const QString& root_directory,
                                                     const QString& child_name,
                                                     const StoreChildFileLoadOptions& options,
                                                     QStringList* errors) {
    if (!validChildOptions(options, errors)) {
        return StoreChildFileLoadResult::Refused;
    }
    if (!validDirectChildName(child_name)) {
        appendError(errors,
                    QStringLiteral("Invalid %1 child directory name: %2")
                        .arg(options.policy.store_name, child_name));
        return StoreChildFileLoadResult::Refused;
    }
    auto root = openChildStoreRoot(root_directory, options, errors);
    if (root.result != StoreChildFileLoadResult::Loaded || !root.handle) {
        return root.result;
    }
    const ChildFileLoadContext context{root.handle->get(), root_directory, options, errors};
    return loadNamedChildFile(context, child_name);
}

StoreDirectoryPin::StoreDirectoryPin(QString path, std::shared_ptr<void> state)
    : m_path(std::move(path)), m_state(std::move(state)) {}

StoreDirectoryPin pinOrCreateStoreDirectory(const QString& root_directory,
                                            const QStringList& components,
                                            int required_component_count,
                                            const StoreFileLoadHooks& hooks,
                                            QString* error_message) {
    QStringList errors;
    if (!validPinRequest(components, required_component_count)) {
        setError(error_message, QStringLiteral("Invalid pinned store directory component list"));
        return {};
    }
    auto root = openPinnedRootDirectory(
        root_directory, QStringLiteral("AI store directory"), true, &errors);
    if (!root) {
        setError(error_message, errors.join(QStringLiteral("; ")));
        return {};
    }
    auto state = std::make_shared<NativeDirectoryPinState>();
    state->handles.emplace_back(std::move(*root));
    QString final_path = normalizedFinalPath(state->handles.back().get());
    PinWalkContext context{
        components, required_component_count, hooks, &errors, state.get(), &final_path};
    for (int index = 0; index < components.size(); ++index) {
        if (!pinDirectoryComponent(context, index)) {
            break;
        }
    }
    if (!errors.isEmpty() || state->handles.size() != static_cast<size_t>(components.size() + 1)) {
        setError(error_message, errors.join(QStringLiteral("; ")));
        return {};
    }
    setError(error_message, {});
    return StoreDirectoryPin(final_path, std::move(state));
}

QString writePinnedStoreFile(const StoreDirectoryPin& directory,
                             const QString& filename,
                             const QByteArray& bytes,
                             const StoreFileLoadHooks& hooks,
                             QString* error_message) {
    if (!directory.valid() || !sak::isSafeChildName(filename) ||
        QFileInfo(filename).fileName() != filename) {
        setError(error_message,
                 QStringLiteral("Artifact write filename is unsafe: %1").arg(filename));
        return {};
    }
    const auto state = std::static_pointer_cast<NativeDirectoryPinState>(directory.m_state);
    if (!state || state->handles.empty()) {
        setError(error_message, QStringLiteral("Artifact write directory pin is unavailable"));
        return {};
    }
    HANDLE directory_handle = state->handles.back().get();
    const QString target_path =
        QDir::cleanPath(QDir(normalizedFinalPath(directory_handle)).filePath(filename));
    auto temp = createAtomicStoreTempFile(directory_handle, target_path, hooks, error_message);
    if (!temp) {
        return {};
    }
    auto cleanup = qScopeGuard([&] { DeleteFileW(reinterpret_cast<LPCWSTR>(temp->path.utf16())); });
    if (!writeAndFlushAtomicStoreFile(temp->handle.get(), bytes, target_path, error_message) ||
        !publishAtomicStoreFile({.file_handle = temp->handle.get(),
                                 .directory_handle = directory_handle,
                                 .filename = filename,
                                 .target_path = target_path,
                                 .hooks = hooks,
                                 .error_message = error_message})) {
        return {};
    }
    cleanup.dismiss();
    setError(error_message, {});
    return target_path;
}

QString copyPinnedStoreFile(const StoreDirectoryPin& directory,
                            const QString& filename,
                            const QString& source_path,
                            const StoreFileLoadHooks& hooks,
                            QString* error_message) {
    if (!directory.valid() || !sak::isSafeChildName(filename) ||
        QFileInfo(filename).fileName() != filename) {
        setError(error_message,
                 QStringLiteral("Artifact copy filename is unsafe: %1").arg(filename));
        return {};
    }
    const auto state = std::static_pointer_cast<NativeDirectoryPinState>(directory.m_state);
    if (!state || state->handles.empty()) {
        setError(error_message, QStringLiteral("Artifact copy directory pin is unavailable"));
        return {};
    }
    HANDLE directory_handle = state->handles.back().get();
    const QString target_path =
        QDir::cleanPath(QDir(normalizedFinalPath(directory_handle)).filePath(filename));
    auto temp = createAtomicStoreTempFile(directory_handle, target_path, hooks, error_message);
    if (!temp) {
        return {};
    }
    auto cleanup = qScopeGuard([&] { DeleteFileW(reinterpret_cast<LPCWSTR>(temp->path.utf16())); });
    if (!copyAndFlushAtomicStoreFile(temp->handle.get(), source_path, target_path, error_message) ||
        !publishAtomicStoreFile({.file_handle = temp->handle.get(),
                                 .directory_handle = directory_handle,
                                 .filename = filename,
                                 .target_path = target_path,
                                 .hooks = hooks,
                                 .error_message = error_message})) {
        return {};
    }
    cleanup.dismiss();
    setError(error_message, {});
    return target_path;
}

}  // namespace sak::ai::detail
