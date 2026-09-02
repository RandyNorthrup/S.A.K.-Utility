// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_skill_store.h"

#include "sak/ai/ai_paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <windows.h>

namespace sak::ai {

namespace {

// A skill body is prose instructions; 1 MiB is far beyond any real skill and bounds
// the per-file allocation from an attacker-writable directory. The file count is
// capped so a directory stuffed with files cannot drive unbounded work/memory.
constexpr qint64 kMaxSkillFileBytes = 1LL * 1024 * 1024;
constexpr int kMaxSkillFiles = 512;
constexpr DWORD kFinalPathInitialChars = 1024;
constexpr qsizetype kExtendedUncPrefixChars = 8;
constexpr qsizetype kExtendedPathPrefixChars = 4;
std::function<void(const QString&)> g_before_file_open_hook;
std::function<void(const QString&)> g_before_file_read_hook;

void appendError(QStringList* errors, const QString& message) {
    if (errors != nullptr) {
        errors->append(message);
    }
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
    ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = INVALID_HANDLE_VALUE;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                CloseHandle(m_handle);
            }
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
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

bool loadResourceSkillFile(const QString& path, Skill* skill, QStringList* errors) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errors != nullptr) {
            errors->append(QStringLiteral("%1: %2").arg(path, file.errorString()));
        }
        return false;
    }
    // Bound the allocation before reading the whole file: fail closed on an
    // unreadable size or one exceeding the cap rather than readAll() an attacker
    // sized file into memory.
    const qint64 size = file.size();
    if (size < 0 || size > kMaxSkillFileBytes) {
        if (errors != nullptr) {
            errors->append(
                QStringLiteral("Skill file too large or unreadable (%1 bytes, max %2): %3")
                    .arg(size)
                    .arg(kMaxSkillFileBytes)
                    .arg(path));
        }
        return false;
    }
    const QByteArray bytes = file.read(kMaxSkillFileBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() != size) {
        appendError(errors, QStringLiteral("Resource skill read was incomplete: %1").arg(path));
        return false;
    }
    *skill = Skill::fromMarkdown(bytes, path);
    return true;
}

struct NativeSkillFiles {
    QStringList names;
    bool capped = false;
};

bool collectSkillFileName(const WIN32_FIND_DATAW& data, NativeSkillFiles* result) {
    const QString name = QString::fromWCharArray(data.cFileName);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        !name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive)) {
        return true;
    }
    if (result->names.size() >= kMaxSkillFiles) {
        result->capped = true;
        return false;
    }
    result->names.append(name);
    return true;
}

std::optional<NativeSkillFiles> enumerateNativeSkillFiles(HANDLE directory_handle,
                                                          QStringList* errors) {
    const QString directory_path = normalizedFinalPath(directory_handle);
    if (directory_path.isEmpty()) {
        appendError(errors, QStringLiteral("Cannot resolve opened skill directory"));
        return std::nullopt;
    }
    const QString pattern =
        QDir::toNativeSeparators(QDir(directory_path).filePath(QStringLiteral("*.md")));
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
            return NativeSkillFiles{};
        }
        appendError(
            errors,
            win32Error(QStringLiteral("Cannot enumerate skill directory"), directory_path, error));
        return std::nullopt;
    }
    const ScopedFindHandle find_handle(raw_find);
    NativeSkillFiles result;
    while (true) {
        if (!collectSkillFileName(data, &result)) {
            break;
        }
        if (FindNextFileW(raw_find, &data) == 0) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_MORE_FILES) {
                appendError(errors,
                            win32Error(QStringLiteral("Skill directory enumeration failed"),
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

std::optional<QByteArray> readStableSkillBytes(HANDLE file_handle,
                                               const QString& display_path,
                                               QStringList* errors) {
    const auto size_before = handleFileSize(file_handle);
    if (!size_before || *size_before > kMaxSkillFileBytes) {
        appendError(errors,
                    QStringLiteral("Skill file too large or unreadable (max %1): %2")
                        .arg(kMaxSkillFileBytes)
                        .arg(display_path));
        return std::nullopt;
    }
    if (g_before_file_read_hook) {
        g_before_file_read_hook(display_path);
    }
    QByteArray bytes(static_cast<qsizetype>(kMaxSkillFileBytes + 1), Qt::Uninitialized);
    DWORD bytes_read = 0;
    if (ReadFile(
            file_handle, bytes.data(), static_cast<DWORD>(bytes.size()), &bytes_read, nullptr) ==
        0) {
        appendError(
            errors,
            win32Error(QStringLiteral("Cannot read skill file"), display_path, GetLastError()));
        return std::nullopt;
    }
    bytes.resize(static_cast<qsizetype>(bytes_read));
    const auto size_after = handleFileSize(file_handle);
    if (!size_after || *size_after > kMaxSkillFileBytes) {
        appendError(
            errors,
            QStringLiteral("Skill file too large or changed during read: %1").arg(display_path));
        return std::nullopt;
    }
    if (*size_before != *size_after || *size_after != bytes.size()) {
        appendError(errors,
                    QStringLiteral("Skill file changed during read (%1 -> %2, read %3): %4")
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

bool loadNativeSkillFile(HANDLE directory_handle,
                         const QString& display_directory,
                         const QString& file_name,
                         Skill* skill,
                         QStringList* errors) {
    const QString opened_directory = normalizedFinalPath(directory_handle);
    if (opened_directory.isEmpty()) {
        appendError(errors, QStringLiteral("Cannot resolve opened skill directory"));
        return false;
    }
    const QString opened_path =
        QDir::toNativeSeparators(QDir(opened_directory).filePath(file_name));
    const QString display_path = QDir(display_directory).filePath(file_name);
    if (g_before_file_open_hook) {
        g_before_file_open_hook(display_path);
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
            errors,
            win32Error(QStringLiteral("Cannot open skill file"), display_path, GetLastError()));
        return false;
    }
    if (handleIsReparsePoint(file.get()) ||
        !fileParentMatchesDirectory(file.get(), directory_handle)) {
        appendError(errors,
                    QStringLiteral("Refusing redirected skill file outside opened directory: %1")
                        .arg(display_path));
        return false;
    }
    const auto bytes = readStableSkillBytes(file.get(), display_path, errors);
    if (!bytes) {
        return false;
    }
    *skill = Skill::fromMarkdown(*bytes, display_path);
    return true;
}

bool loadResourceDirectory(SkillStore* store, const QString& directory, QStringList* errors) {
    const QDir resource_directory(directory);
    if (!resource_directory.exists()) {
        appendError(errors,
                    QStringLiteral("Skill resource directory not found: %1").arg(directory));
        return false;
    }
    QStringList files =
        resource_directory.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    bool ok = files.size() <= kMaxSkillFiles;
    if (!ok) {
        appendError(errors,
                    QStringLiteral("Skill resource directory exceeds %1 files: %2")
                        .arg(kMaxSkillFiles)
                        .arg(directory));
        return false;
    }
    for (const QString& file_name : files) {
        Skill skill;
        if (!loadResourceSkillFile(resource_directory.filePath(file_name), &skill, errors) ||
            !store->addSkill(skill, errors)) {
            ok = false;
        }
    }
    return ok;
}

bool loadNativeDirectory(SkillStore* store, const QString& directory, QStringList* errors) {
    if (!QDir::isAbsolutePath(directory)) {
        appendError(
            errors,
            QStringLiteral("Skill directory must be an absolute local path: %1").arg(directory));
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
            win32Error(QStringLiteral("Cannot open skill directory"), directory, GetLastError()));
        return false;
    }
    if (handleIsReparsePoint(directory_handle.get())) {
        appendError(errors,
                    QStringLiteral("Refusing skill directory reparse point: %1").arg(directory));
        return false;
    }
    const auto enumeration = enumerateNativeSkillFiles(directory_handle.get(), errors);
    if (!enumeration) {
        return false;
    }
    if (enumeration->capped) {
        appendError(errors,
                    QStringLiteral("Skill directory has more than %1 Markdown files: %2")
                        .arg(kMaxSkillFiles)
                        .arg(directory));
        return false;
    }
    bool ok = true;
    for (const QString& file_name : enumeration->names) {
        Skill skill;
        if (!loadNativeSkillFile(directory_handle.get(), directory, file_name, &skill, errors) ||
            !store->addSkill(skill, errors)) {
            ok = false;
        }
    }
    return ok;
}

}  // namespace

void SkillStore::setBeforeFileOpenHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_open_hook = std::move(hook);
}

void SkillStore::setBeforeFileReadHookForTesting(std::function<void(const QString&)> hook) {
    g_before_file_read_hook = std::move(hook);
}

bool SkillStore::loadDefaults(QStringList* errors) {
    bool ok = loadBuiltIn(errors);
    const QString user_dir = defaultUserSkillDirectory();
    if (QDir(user_dir).exists()) {
        ok = loadDirectory(user_dir, errors) && ok;
    }
    return ok;
}

bool SkillStore::loadBuiltIn(QStringList* errors) {
    return loadDirectory(builtInResourceRoot(), errors);
}

bool SkillStore::loadDirectory(const QString& directory, QStringList* errors) {
    if (directory.startsWith(QStringLiteral(":"))) {
        return loadResourceDirectory(this, directory, errors);
    }
    return loadNativeDirectory(this, directory, errors);
}

bool SkillStore::addSkill(const Skill& skill, QStringList* errors) {
    if (!skill.isValid(errors)) {
        return false;
    }
    const auto existing = m_index_by_id.constFind(skill.id);
    if (existing != m_index_by_id.constEnd()) {
        m_skills[*existing] = skill;
    } else {
        m_index_by_id.insert(skill.id, static_cast<int>(m_skills.size()));
        m_skills.append(skill);
    }
    return true;
}

const Skill* SkillStore::skillById(const QString& id) const {
    const auto existing = m_index_by_id.constFind(id.trimmed());
    if (existing == m_index_by_id.constEnd()) {
        return nullptr;
    }
    const int index = *existing;
    if (index < 0 || index >= m_skills.size()) {
        return nullptr;
    }
    return &m_skills[index];
}

QString SkillStore::builtInResourceRoot() {
    return QStringLiteral(":/ai/skills");
}

QString SkillStore::defaultUserSkillDirectory() {
    return skillLibraryDirectory();
}

}  // namespace sak::ai
