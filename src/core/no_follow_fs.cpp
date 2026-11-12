// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file no_follow_fs.cpp
/// @brief File-system operations that never follow a junction or symbolic link.

#include "sak/no_follow_fs.h"

#include <QFileInfo>
#include <QRandomGenerator>

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>

namespace sak::no_follow {

namespace {

constexpr int kRandomWords = 4;  // 4 x 32 bits = 128 random bits
constexpr int kHexDigitsPerWord = 8;
constexpr int kHexBase = 16;
constexpr int kUniqueNameAttempts = 8;         // a 128-bit collision is not a real event
constexpr qsizetype kStagingNameKeepChars = 64;
constexpr int kMaxTreeDepth = 256;             // deeper than any real folder; refuses a crafted one
constexpr qsizetype kDriveRootLength = 3;      // "C:\"
constexpr qsizetype kDriveSeparatorIndex = 2;  // where the separator sits in "C:\"

const QString kExtendedPrefix = QStringLiteral("\\\\?\\");
const QString kExtendedUncPrefix = QStringLiteral("\\\\?\\UNC\\");
const QString kUncPrefix = QStringLiteral("\\\\");

constexpr DWORD kDirectoryAccess = FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES |
                                   READ_CONTROL | SYNCHRONIZE;
// No FILE_SHARE_DELETE: while a handle is held, nobody can rename, replace or remove the
// directory, or any directory above it.
constexpr DWORD kHoldShare = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr DWORD kAnyShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

QString lastErrorText(DWORD error) {
    return QStringLiteral("Windows error %1").arg(error);
}

QString nativeSeparators(QString path) {
    path.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return path;
}

QString withoutTrailingSeparator(QString path) {
    while (path.size() > kDriveRootLength && path.endsWith(QLatin1Char('\\'))) {
        path.chop(1);
    }
    return path;
}

/// The extended-length spelling of an absolute path, so a deep tree is not cut at MAX_PATH.
std::wstring win32Path(const QString& path) {
    if (path.startsWith(kExtendedPrefix)) {
        return path.toStdWString();
    }
    if (path.startsWith(kUncPrefix)) {
        return (kExtendedUncPrefix + path.mid(kUncPrefix.size())).toStdWString();
    }
    return (kExtendedPrefix + path).toStdWString();
}

/// A single entry name: not empty, not "." or "..", and no separator, colon or NUL.
bool isSingleName(const QString& name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..") &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
           !name.contains(QLatin1Char(':')) && !name.contains(QChar(u'\0'));
}

bool attributesAreLink(DWORD attributes, DWORD reparse_tag) {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           IsReparseTagNameSurrogate(reparse_tag);
}

/// Attributes and reparse tag of an open handle, on NTFS, ReFS, FAT32 and exFAT alike
/// (attributesFromAnswers says how).
std::expected<FILE_ATTRIBUTE_TAG_INFO, QString> attributeTag(HANDLE handle) {
    AttributeAnswers answers;
    FILE_ATTRIBUTE_TAG_INFO tag{};
    DWORD tag_error = ERROR_SUCCESS;
    if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) != 0) {
        answers.tag_attributes = tag.FileAttributes;
        answers.reparse_tag = tag.ReparseTag;
    } else {
        tag_error = GetLastError();
        FILE_BASIC_INFO basic{};
        if (GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) != 0) {
            answers.basic_attributes = basic.FileAttributes;
        }
    }
    const auto attributes = attributesFromAnswers(answers);
    if (!attributes) {
        return std::unexpected(lastErrorText(tag_error));
    }
    FILE_ATTRIBUTE_TAG_INFO info{};
    info.FileAttributes = attributes->first;
    info.ReparseTag = attributes->second;
    return info;
}

HANDLE openNoFollow(const QString& path, DWORD access, DWORD share) {
    return CreateFileW(win32Path(path).c_str(),
                       access,
                       share,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                       nullptr);
}

/// The root a path starts from: "C:\" or "\\server\share\". Empty when it has neither.
QString pathRoot(const QString& full) {
    if (full.size() >= kDriveRootLength && full.at(1) == QLatin1Char(':') &&
        full.at(kDriveSeparatorIndex) == QLatin1Char('\\')) {
        return full.left(kDriveRootLength);
    }
    if (!full.startsWith(kUncPrefix)) {
        return {};
    }
    const qsizetype server_end = full.indexOf(QLatin1Char('\\'), kUncPrefix.size());
    if (server_end < 0) {
        return {};
    }
    const qsizetype share_end = full.indexOf(QLatin1Char('\\'), server_end + 1);
    return share_end < 0 ? full + QLatin1Char('\\') : full.left(share_end + 1);
}

/// Where @p full should end up when nothing below its drive or share root is a link: the root as
/// Windows resolves it (a mapped drive's share, a SUBST drive's folder) plus the rest unchanged.
QString anchoredExpectedPath(const QString& full) {
    const QString root = pathRoot(full);
    if (root.isEmpty()) {
        return {};
    }
    // The plain spelling: a root is short, and the extended "\\?\C:\" reads as the volume itself
    // to tools that screen device paths (the test hardware guard refuses it).
    const UniqueHandle root_handle(CreateFileW(root.toStdWString().c_str(),
                                               FILE_READ_ATTRIBUTES,
                                               kAnyShare,
                                               nullptr,
                                               OPEN_EXISTING,
                                               FILE_FLAG_BACKUP_SEMANTICS,
                                               nullptr));
    if (!root_handle.valid()) {
        return {};
    }
    const QString resolved_root = finalPathOfHandle(root_handle.get());
    if (resolved_root.isEmpty()) {
        return {};
    }
    const QString rest = full.mid(root.size());
    if (rest.isEmpty()) {
        return resolved_root;
    }
    // "C:\" keeps its separator; a share or SUBST folder root ("\\server\share") has none.
    return resolved_root.endsWith(QLatin1Char('\\')) ? resolved_root + rest
                                                     : resolved_root + QLatin1Char('\\') + rest;
}

/// Check that an open directory handle is an ordinary directory, not a link.
std::expected<void, QString> requireOrdinaryDirectory(HANDLE handle, const QString& shown) {
    const auto tag = attributeTag(handle);
    if (!tag) {
        return std::unexpected(QStringLiteral("Cannot read %1: %2").arg(shown, tag.error()));
    }
    if (attributesAreLink(tag->FileAttributes, tag->ReparseTag)) {
        return std::unexpected(
            QStringLiteral("%1 is a link (a junction or symbolic link), so it was not used")
                .arg(shown));
    }
    if ((tag->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return std::unexpected(QStringLiteral("%1 is not a folder").arg(shown));
    }
    return {};
}

std::expected<void, QString> requireResolvesTo(HANDLE handle,
                                               const QString& expected,
                                               const QString& shown) {
    const QString resolved = finalPathOfHandle(handle);
    if (resolved.isEmpty() || expected.isEmpty()) {
        return std::unexpected(QStringLiteral("Cannot tell where %1 really is").arg(shown));
    }
    if (!samePath(resolved, expected)) {
        return std::unexpected(
            QStringLiteral("A link on the way to %1 leads to %2 instead, so it was not used")
                .arg(shown, resolved));
    }
    return {};
}

/// The bytes of a FILE_RENAME_INFO naming @p name relative to @p directory.
std::vector<std::byte> renameInfo(HANDLE directory, const QString& name, bool replace) {
    const std::wstring wide = name.toStdWString();
    const std::size_t name_bytes = wide.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = replace ? TRUE : FALSE;
    info->RootDirectory = directory;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::copy_n(wide.data(), wide.size(), static_cast<wchar_t*>(info->FileName));
    return buffer;
}

// kernel32's SetFileInformationByHandle refuses a FILE_RENAME_INFO whose RootDirectory is set
// (ERROR_INVALID_PARAMETER, measured on Windows 11 26200), so the rename relative to the held
// directory goes to NtSetInformationFile, which is what it and MoveFileExW call underneath.
struct NtIoStatusBlock {
    void* status_or_pointer{nullptr};
    ULONG_PTR information{0};
};
using NtSetInformationFileFn = LONG(NTAPI*)(HANDLE, NtIoStatusBlock*, void*, ULONG, int);
constexpr int kFileRenameInformationClass = 10;  // FILE_INFORMATION_CLASS::FileRenameInformation
constexpr LONG kStatusObjectNameCollision = static_cast<LONG>(0xC0'00'00'35L);

NtSetInformationFileFn ntSetInformationFile() {
    static const auto kFunction = reinterpret_cast<NtSetInformationFileFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    return kFunction;
}

std::expected<void, QString> renameByHandle(HANDLE file,
                                            HANDLE directory,
                                            const QString& name,
                                            Publish mode) {
    const NtSetInformationFileFn set_information = ntSetInformationFile();
    if (set_information == nullptr) {
        return std::unexpected(QStringLiteral("Cannot rename into %1").arg(name));
    }
    std::vector<std::byte> info = renameInfo(directory, name, mode == Publish::ReplaceName);
    NtIoStatusBlock status_block{};
    const LONG status = set_information(file,
                                        &status_block,
                                        info.data(),
                                        static_cast<ULONG>(info.size()),
                                        kFileRenameInformationClass);
    if (status == kStatusObjectNameCollision) {
        return std::unexpected(QStringLiteral("%1 already exists").arg(name));
    }
    if (status < 0) {
        return std::unexpected(QStringLiteral("Cannot rename into %1: NT status 0x%2")
                                   .arg(name)
                                   .arg(static_cast<ULONG>(status), 0, kHexBase));
    }
    return {};
}

void deleteByHandle(HANDLE handle) noexcept {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    (void)SetFileInformationByHandle(
        handle, FileDispositionInfo, &disposition, sizeof(disposition));
}

UniqueHandle duplicateHandle(HANDLE handle) {
    HANDLE copy = nullptr;
    if (DuplicateHandle(GetCurrentProcess(),
                        handle,
                        GetCurrentProcess(),
                        &copy,
                        0,
                        FALSE,
                        DUPLICATE_SAME_ACCESS) == 0) {
        return {};
    }
    return UniqueHandle(copy);
}

/// Delete one name that is not a folder to recurse into: a link is removed itself.
bool removeLeafEntry(const QString& path, DWORD attributes) {
    const std::wstring wide = win32Path(path);
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return RemoveDirectoryW(wide.c_str()) != 0;  // an empty folder or a directory link
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        (void)SetFileAttributesW(wide.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    }
    return DeleteFileW(wide.c_str()) != 0;  // never follows a file symbolic link
}

std::expected<void, QString> removeDirectoryContents(const QString& directory, int depth);

/// One entry found inside a held directory.
std::expected<void, QString> removeFoundEntry(const QString& path,
                                              const WIN32_FIND_DATAW& found,
                                              int depth) {
    const bool is_reparse = (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const bool is_directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (is_directory && !is_reparse) {
        if (auto inner = removeDirectoryContents(path, depth + 1); !inner) {
            return inner;
        }
    }
    // Any reparse point is removed as the name itself and never entered: a junction or symbolic
    // link here must not lead the delete out of this tree.
    if (!removeLeafEntry(path, found.dwFileAttributes)) {
        return std::unexpected(
            QStringLiteral("Cannot remove %1: %2").arg(path, lastErrorText(GetLastError())));
    }
    return {};
}

/// Empty @p directory, which must be an ordinary folder; it is held while it is emptied.
std::expected<void, QString> removeDirectoryContents(const QString& directory, int depth) {
    if (depth > kMaxTreeDepth) {
        return std::unexpected(QStringLiteral("%1 is nested too deeply to remove").arg(directory));
    }
    const UniqueHandle held(openNoFollow(directory, kDirectoryAccess, kHoldShare));
    if (!held.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot open %1: %2").arg(directory, lastErrorText(GetLastError())));
    }
    if (auto ordinary = requireOrdinaryDirectory(held.get(), directory); !ordinary) {
        return ordinary;  // swapped for a link after it was listed: leave what it points at alone
    }
    WIN32_FIND_DATAW found{};
    HANDLE search = FindFirstFileExW(win32Path(directory + QStringLiteral("\\*")).c_str(),
                                     FindExInfoBasic,
                                     &found,
                                     FindExSearchNameMatch,
                                     nullptr,
                                     0);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND
                   ? std::expected<void, QString>{}
                   : std::unexpected(
                         QStringLiteral("Cannot list %1: %2").arg(directory, lastErrorText(error)));
    }
    std::expected<void, QString> outcome;
    do {
        const QString name = QString::fromWCharArray(static_cast<const wchar_t*>(found.cFileName));
        if (name == QLatin1String(".") || name == QLatin1String("..")) {
            continue;
        }
        outcome = removeFoundEntry(directory + QLatin1Char('\\') + name, found, depth);
    } while (outcome && FindNextFileW(search, &found) != 0);
    FindClose(search);
    return outcome;
}

}  // namespace

// ======================================================================
// UniqueHandle
// ======================================================================

UniqueHandle::UniqueHandle(void* handle) noexcept
    : m_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}

UniqueHandle::~UniqueHandle() {
    reset();
}

UniqueHandle::UniqueHandle(UniqueHandle&& other) noexcept : m_handle(other.release()) {}

UniqueHandle& UniqueHandle::operator=(UniqueHandle&& other) noexcept {
    if (this != &other) {
        reset();
        m_handle = other.release();
    }
    return *this;
}

void* UniqueHandle::release() noexcept {
    return std::exchange(m_handle, nullptr);
}

void UniqueHandle::reset() noexcept {
    if (m_handle != nullptr) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
}

// ======================================================================
// Paths
// ======================================================================

QString finalPathOfHandle(void* handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD need = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (need == 0) {
        return {};
    }
    std::wstring buffer(need, L'\0');
    const DWORD got = GetFinalPathNameByHandleW(handle, buffer.data(), need, flags);
    if (got == 0 || got >= need) {
        return {};
    }
    buffer.resize(got);
    const QString path = QString::fromStdWString(buffer);
    if (path.startsWith(kExtendedUncPrefix, Qt::CaseInsensitive)) {
        return kUncPrefix + path.mid(kExtendedUncPrefix.size());
    }
    if (path.startsWith(kExtendedPrefix)) {
        return path.mid(kExtendedPrefix.size());
    }
    return path;
}

QString comparablePath(const QString& path) {
    if (path.trimmed().isEmpty() || path.contains(QChar(u'\0'))) {
        return {};
    }
    const std::wstring wide = nativeSeparators(path).toStdWString();
    const DWORD need = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (need == 0) {
        return {};
    }
    std::wstring full(need, L'\0');
    const DWORD got = GetFullPathNameW(wide.c_str(), need, full.data(), nullptr);
    if (got == 0 || got >= need) {
        return {};
    }
    full.resize(got);
    const DWORD long_need = GetLongPathNameW(full.c_str(), nullptr, 0);
    if (long_need != 0) {
        std::wstring long_name(long_need, L'\0');
        const DWORD long_got = GetLongPathNameW(full.c_str(), long_name.data(), long_need);
        if (long_got != 0 && long_got < long_need) {
            long_name.resize(long_got);
            full = long_name;
        }
    }
    return withoutTrailingSeparator(QString::fromStdWString(full));
}

bool samePath(const QString& a, const QString& b) {
    return withoutTrailingSeparator(nativeSeparators(a))
               .compare(withoutTrailingSeparator(nativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

EntryKind entryKind(const QString& path) {
    const QString full = comparablePath(path);
    if (full.isEmpty()) {
        return EntryKind::Other;
    }
    const UniqueHandle handle(openNoFollow(full, FILE_READ_ATTRIBUTES, kAnyShare));
    if (!handle.valid()) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? EntryKind::Missing
                                                                              : EntryKind::Other;
    }
    const auto tag = attributeTag(handle.get());
    if (!tag) {
        return EntryKind::Other;
    }
    if (attributesAreLink(tag->FileAttributes, tag->ReparseTag)) {
        return EntryKind::Link;
    }
    return (tag->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryKind::Directory
                                                                 : EntryKind::File;
}

std::optional<std::pair<unsigned long, unsigned long>> attributesFromAnswers(
    const AttributeAnswers& answers) {
    if (answers.tag_attributes) {
        return std::pair{*answers.tag_attributes, answers.reparse_tag};
    }
    if (!answers.basic_attributes ||
        (*answers.basic_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::nullopt;
    }
    return std::pair{*answers.basic_attributes, 0UL};
}

std::expected<EntryKind, QString> entryKindOfHandle(void* handle) {
    const auto tag = attributeTag(handle);
    if (!tag) {
        return std::unexpected(tag.error());
    }
    if (attributesAreLink(tag->FileAttributes, tag->ReparseTag)) {
        return EntryKind::Link;
    }
    return (tag->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? EntryKind::Directory
                                                                 : EntryKind::File;
}

bool volumeKeepsAcls(void* handle) {
    DWORD flags = 0;
    if (GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) ==
        0) {
        return false;
    }
    return (flags & FILE_PERSISTENT_ACLS) != 0;
}

QString randomToken() {
    QString token;
    for (int i = 0; i < kRandomWords; ++i) {
        token += QStringLiteral("%1").arg(
            QRandomGenerator::system()->generate(), kHexDigitsPerWord, kHexBase, QLatin1Char('0'));
    }
    return token;
}

// ======================================================================
// HeldDirectory
// ======================================================================

HeldDirectory::HeldDirectory(UniqueHandle handle, QString path)
    : m_handle(std::move(handle)), m_path(std::move(path)) {}

std::expected<HeldDirectory, QString> HeldDirectory::open(const QString& path) {
    const QString full = comparablePath(path);
    if (full.isEmpty()) {
        return std::unexpected(QStringLiteral("Not a usable folder path: %1").arg(path));
    }
    UniqueHandle handle(openNoFollow(full, kDirectoryAccess, kHoldShare));
    if (!handle.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot open %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    if (auto ordinary = requireOrdinaryDirectory(handle.get(), full); !ordinary) {
        return std::unexpected(ordinary.error());
    }
    if (auto where = requireResolvesTo(handle.get(), anchoredExpectedPath(full), full); !where) {
        return std::unexpected(where.error());
    }
    QString real = finalPathOfHandle(handle.get());
    return HeldDirectory(std::move(handle), withoutTrailingSeparator(real));
}

std::expected<HeldDirectory, QString> HeldDirectory::openAnchor(const QString& path) {
    const QString full = comparablePath(path);
    if (full.isEmpty()) {
        return std::unexpected(QStringLiteral("Not a usable folder path: %1").arg(path));
    }
    UniqueHandle handle(CreateFileW(win32Path(full).c_str(),
                                    kDirectoryAccess,
                                    kHoldShare,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr));
    if (!handle.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot open %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    QString real = finalPathOfHandle(handle.get());
    if (real.isEmpty()) {
        return std::unexpected(QStringLiteral("Cannot tell where %1 really is").arg(full));
    }
    return HeldDirectory(std::move(handle), withoutTrailingSeparator(real));
}

QString HeldDirectory::filePath(const QString& name) const {
    return m_path + QLatin1Char('\\') + nativeSeparators(name);
}

std::expected<HeldDirectory, QString> HeldDirectory::child(const QString& name, bool create) const {
    if (!isSingleName(name)) {
        return std::unexpected(QStringLiteral("Not a single folder name: %1").arg(name));
    }
    const QString full = filePath(name);
    if (create && CreateDirectoryW(win32Path(full).c_str(), nullptr) == 0 &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return std::unexpected(
            QStringLiteral("Cannot create %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    UniqueHandle handle(openNoFollow(full, kDirectoryAccess, kHoldShare));
    if (!handle.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot open %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    if (auto ordinary = requireOrdinaryDirectory(handle.get(), full); !ordinary) {
        return std::unexpected(ordinary.error());
    }
    // This directory is held, so its own path cannot move: the child must sit right under it.
    if (auto where = requireResolvesTo(handle.get(), full, full); !where) {
        return std::unexpected(where.error());
    }
    QString real = finalPathOfHandle(handle.get());
    return HeldDirectory(std::move(handle), withoutTrailingSeparator(real));
}

std::expected<HeldDirectory, QString> HeldDirectory::descend(const QString& relative,
                                                             bool create) const {
    const QStringList parts = nativeSeparators(relative).split(QLatin1Char('\\'),
                                                               Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return std::unexpected(QStringLiteral("No folder named below %1").arg(m_path));
    }
    auto current = child(parts.first(), create);
    for (qsizetype i = 1; current && i < parts.size(); ++i) {
        current = current->child(parts.at(i), create);
    }
    return current;
}

namespace {

/// The security attributes for @p sddl, or none when @p sddl is empty or the volume keeps no ACLs.
class SddlAttributes {
public:
    SddlAttributes(HANDLE volume_handle, const QString& sddl) {
        if (!sddl.isEmpty() && volumeKeepsAcls(volume_handle) &&
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.toStdWString().c_str(), SDDL_REVISION_1, &m_descriptor, nullptr) != 0) {
            m_attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
            m_attributes.lpSecurityDescriptor = m_descriptor;
            m_attributes.bInheritHandle = FALSE;
        }
        m_wanted = !sddl.isEmpty() && volumeKeepsAcls(volume_handle);
    }
    ~SddlAttributes() { LocalFree(m_descriptor); }
    SddlAttributes(const SddlAttributes&) = delete;
    SddlAttributes& operator=(const SddlAttributes&) = delete;
    SddlAttributes(SddlAttributes&&) = delete;
    SddlAttributes& operator=(SddlAttributes&&) = delete;
    /// False when a DACL was asked for and could not be built.
    [[nodiscard]] bool usable() const { return !m_wanted || m_descriptor != nullptr; }
    [[nodiscard]] SECURITY_ATTRIBUTES* get() {
        return m_descriptor != nullptr ? &m_attributes : nullptr;
    }

private:
    PSECURITY_DESCRIPTOR m_descriptor{nullptr};
    SECURITY_ATTRIBUTES m_attributes{};
    bool m_wanted{false};
};

}  // namespace

std::expected<HeldDirectory, QString> HeldDirectory::createChild(const QString& name,
                                                                 const QString& sddl) const {
    if (!isSingleName(name)) {
        return std::unexpected(QStringLiteral("Not a single folder name: %1").arg(name));
    }
    SddlAttributes attributes(m_handle.get(), sddl);
    if (!attributes.usable()) {
        return std::unexpected(QStringLiteral("Invalid folder permissions: %1").arg(sddl));
    }
    if (CreateDirectoryW(win32Path(filePath(name)).c_str(), attributes.get()) == 0) {
        return std::unexpected(QStringLiteral("Cannot create %1: %2")
                                   .arg(filePath(name), lastErrorText(GetLastError())));
    }
    return child(name, false);
}

namespace {

bool isAdministratorsOrSystem(PSID sid) {
    return sid != nullptr && (IsWellKnownSid(sid, WinBuiltinAdministratorsSid) != 0 ||
                              IsWellKnownSid(sid, WinLocalSystemSid) != 0);
}

/// Whether every entry of @p dacl allows only Administrators or SYSTEM (a deny entry is fine).
bool onlyAdministratorsOrSystemAllowed(PACL dacl) {
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (GetAce(dacl, i, &ace) == 0) {
            return false;
        }
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            if (!isAdministratorsOrSystem(reinterpret_cast<PSID>(&allowed->SidStart))) {
                return false;
            }
        } else if (header->AceType != ACCESS_DENIED_ACE_TYPE) {
            return false;  // callback, object and other allow forms: not what this module writes
        }
    }
    return true;
}

}  // namespace

bool isProtectedForAdministrators(void* handle) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetSecurityInfo(handle,
                        SE_FILE_OBJECT,
                        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                        &owner,
                        nullptr,
                        &dacl,
                        nullptr,
                        &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool ours = isAdministratorsOrSystem(owner) && dacl != nullptr &&
                      GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
                      (control & SE_DACL_PROTECTED) != 0 && onlyAdministratorsOrSystemAllowed(dacl);
    LocalFree(descriptor);
    return ours;
}

std::expected<HeldDirectory, QString> HeldDirectory::createUniqueChild(const QString& prefix,
                                                                       const QString& sddl) const {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const bool protect = !sddl.isEmpty() && volumeKeepsAcls(m_handle.get());
    if (protect && ConvertStringSecurityDescriptorToSecurityDescriptorW(
                       sddl.toStdWString().c_str(), SDDL_REVISION_1, &descriptor, nullptr) == 0) {
        return std::unexpected(QStringLiteral("Invalid folder permissions: %1").arg(sddl));
    }
    SECURITY_ATTRIBUTES attributes{.nLength = sizeof(SECURITY_ATTRIBUTES),
                                   .lpSecurityDescriptor = descriptor,
                                   .bInheritHandle = FALSE};
    QString created;
    DWORD error = ERROR_ALREADY_EXISTS;
    for (int attempt = 0; attempt < kUniqueNameAttempts && created.isEmpty(); ++attempt) {
        const QString name = prefix + randomToken();
        if (CreateDirectoryW(win32Path(filePath(name)).c_str(), protect ? &attributes : nullptr) !=
            0) {
            created = name;
        } else {
            error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) {
                break;
            }
        }
    }
    LocalFree(descriptor);
    if (created.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Cannot create a folder in %1: %2").arg(m_path, lastErrorText(error)));
    }
    return child(created, false);
}

// ======================================================================
// StagedFile
// ======================================================================

std::expected<StagedFile, QString> StagedFile::create(const HeldDirectory& directory,
                                                      const QString& name) {
    if (!isSingleName(name) || !directory.isOpen()) {
        return std::unexpected(QStringLiteral("Not a single file name: %1").arg(name));
    }
    StagedFile staged;
    staged.m_directory = duplicateHandle(directory.handle());
    if (!staged.m_directory.valid()) {
        return std::unexpected(QStringLiteral("Cannot hold %1").arg(directory.path()));
    }
    staged.m_directory_path = directory.path();
    staged.m_name = name;
    const QString staging_name = QLatin1Char('.') + name.left(kStagingNameKeepChars) +
                                 QLatin1Char('.') + randomToken() + QStringLiteral(".sakpart");
    staged.m_staging_path = directory.filePath(staging_name);
    UniqueHandle file(CreateFileW(win32Path(staged.m_staging_path).c_str(),
                                  GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
    if (!file.valid()) {
        return std::unexpected(QStringLiteral("Cannot create %1: %2")
                                   .arg(staged.m_staging_path, lastErrorText(GetLastError())));
    }
    if (auto where = requireResolvesTo(file.get(), staged.m_staging_path, staged.m_staging_path);
        !where) {
        deleteByHandle(file.get());
        return std::unexpected(where.error());
    }
    const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(file.get()),
                                           _O_WRONLY | _O_BINARY);
    if (descriptor < 0) {
        deleteByHandle(file.get());
        return std::unexpected(QStringLiteral("Cannot write %1").arg(staged.m_staging_path));
    }
    (void)file.release();  // the descriptor owns it now, and QFile owns the descriptor
    staged.m_file = std::make_unique<QFile>();
    if (!staged.m_file->open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        _close(descriptor);
        return std::unexpected(QStringLiteral("Cannot write %1").arg(staged.m_staging_path));
    }
    return staged;
}

StagedFile::StagedFile(StagedFile&& other) noexcept
    : m_file(std::move(other.m_file))
    , m_directory(std::move(other.m_directory))
    , m_directory_path(std::move(other.m_directory_path))
    , m_name(std::move(other.m_name))
    , m_staging_path(std::move(other.m_staging_path))
    , m_done(std::exchange(other.m_done, true)) {}

StagedFile& StagedFile::operator=(StagedFile&& other) noexcept {
    if (this != &other) {
        discard();
        m_file = std::move(other.m_file);
        m_directory = std::move(other.m_directory);
        m_directory_path = std::move(other.m_directory_path);
        m_name = std::move(other.m_name);
        m_staging_path = std::move(other.m_staging_path);
        m_done = std::exchange(other.m_done, true);
    }
    return *this;
}

StagedFile::~StagedFile() {
    discard();
}

QString StagedFile::finalPath() const {
    return m_directory_path + QLatin1Char('\\') + m_name;
}

std::expected<void, QString> StagedFile::publish(Publish mode) {
    if (m_done || !m_file || !m_file->isOpen()) {
        return std::unexpected(QStringLiteral("%1 is no longer open").arg(m_staging_path));
    }
    if (!m_file->flush()) {
        return std::unexpected(QStringLiteral("Cannot write %1").arg(m_staging_path));
    }
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(m_file->handle()));
    if (auto renamed = renameByHandle(handle, m_directory.get(), m_name, mode); !renamed) {
        return renamed;
    }
    m_done = true;
    m_file->close();
    return {};
}

void StagedFile::discard() noexcept {
    if (m_done) {
        return;
    }
    m_done = true;
    if (m_file && m_file->isOpen()) {
        deleteByHandle(reinterpret_cast<HANDLE>(_get_osfhandle(m_file->handle())));
        m_file->close();
    }
}

// ======================================================================
// Copy, move, remove, create
// ======================================================================

std::expected<void, QString> copyFileInto(const QString& source,
                                          const HeldDirectory& directory,
                                          const QString& name,
                                          Publish mode) {
    if (!isSingleName(name) || !directory.isOpen()) {
        return std::unexpected(QStringLiteral("Not a single file name: %1").arg(name));
    }
    const QString staging = directory.filePath(QLatin1Char('.') + name.left(kStagingNameKeepChars) +
                                               QLatin1Char('.') + randomToken() +
                                               QStringLiteral(".sakpart"));
    const std::wstring source_wide = win32Path(comparablePath(source));
    if (CopyFileExW(source_wide.c_str(),
                    win32Path(staging).c_str(),
                    nullptr,
                    nullptr,
                    nullptr,
                    COPY_FILE_FAIL_IF_EXISTS) == 0) {
        return std::unexpected(
            QStringLiteral("Cannot copy %1: %2").arg(source, lastErrorText(GetLastError())));
    }
    const UniqueHandle copied(
        openNoFollow(staging, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, kAnyShare));
    if (!copied.valid()) {
        return std::unexpected(QStringLiteral("Cannot open the copy %1").arg(staging));
    }
    std::expected<void, QString> outcome = requireResolvesTo(copied.get(), staging, staging);
    if (outcome) {
        outcome = renameByHandle(copied.get(), directory.handle(), name, mode);
    }
    if (!outcome) {
        deleteByHandle(copied.get());
    }
    return outcome;
}

std::expected<void, QString> moveInto(const QString& source,
                                      const HeldDirectory& directory,
                                      const QString& name,
                                      Publish mode) {
    if (!isSingleName(name) || !directory.isOpen()) {
        return std::unexpected(QStringLiteral("Not a single file name: %1").arg(name));
    }
    const QString full = comparablePath(source);
    const UniqueHandle moving(
        openNoFollow(full, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE, kAnyShare));
    if (!moving.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot open %1: %2").arg(source, lastErrorText(GetLastError())));
    }
    // A folder above the source swapped for a link would move a file from somewhere else.
    if (auto where = requireResolvesTo(moving.get(), anchoredExpectedPath(full), full); !where) {
        return where;
    }
    return renameByHandle(moving.get(), directory.handle(), name, mode);
}

std::expected<void, QString> copyFileTo(const QString& source,
                                        const QString& target,
                                        Publish mode) {
    const QFileInfo info(target);
    const auto folder = HeldDirectory::open(info.absolutePath());
    if (!folder) {
        return std::unexpected(folder.error());
    }
    return copyFileInto(source, *folder, info.fileName(), mode);
}

std::expected<void, QString> moveTo(const QString& source, const QString& target, Publish mode) {
    const QFileInfo info(target);
    const auto folder = HeldDirectory::open(info.absolutePath());
    if (!folder) {
        return std::unexpected(folder.error());
    }
    return moveInto(source, *folder, info.fileName(), mode);
}

std::expected<void, QString> removeTree(const QString& path) {
    const QString full = comparablePath(path);
    if (full.isEmpty()) {
        return std::unexpected(QStringLiteral("Not a usable path: %1").arg(path));
    }
    const DWORD attributes = GetFileAttributesW(win32Path(full).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? std::expected<void, QString>{}
                   : std::unexpected(
                         QStringLiteral("Cannot read %1: %2").arg(full, lastErrorText(error)));
    }
    const bool is_reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && !is_reparse) {
        if (auto emptied = removeDirectoryContents(full, 0); !emptied) {
            return emptied;
        }
    }
    if (!removeLeafEntry(full, attributes)) {
        return std::unexpected(
            QStringLiteral("Cannot remove %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    return {};
}

namespace {

bool writeAllTo(HANDLE file, const QByteArray& bytes) {
    qsizetype done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(bytes.size() - done);
        if (WriteFile(file, bytes.constData() + done, chunk, &written, nullptr) == 0 ||
            written == 0) {
            return false;
        }
        done += written;
    }
    return true;
}

bool readsBackAs(HANDLE file, const QByteArray& expected) {
    QByteArray actual(expected.size() + 1, Qt::Uninitialized);  // one more: a longer file fails
    DWORD read = 0;
    qsizetype total = 0;
    while (total < actual.size()) {
        if (ReadFile(file,
                     actual.data() + total,
                     static_cast<DWORD>(actual.size() - total),
                     &read,
                     nullptr) == 0) {
            return false;
        }
        if (read == 0) {
            break;
        }
        total += read;
    }
    return total == expected.size() && QByteArrayView(actual.constData(), total) == expected;
}

}  // namespace

std::expected<UniqueHandle, QString> writeHeldReadOnlyFile(const HeldDirectory& directory,
                                                           const QString& name,
                                                           const QByteArray& bytes) {
    {
        auto writer = createNewFile(directory, name, true);
        if (!writer) {
            return std::unexpected(writer.error());
        }
        if (!writeAllTo(writer->get(), bytes)) {
            return std::unexpected(QStringLiteral("Cannot write %1").arg(directory.filePath(name)));
        }
    }  // the writer closes here: a reader cannot share with a writer that excludes writing
    const QString full = directory.filePath(name);
    UniqueHandle held(openNoFollow(full, GENERIC_READ, FILE_SHARE_READ));
    if (!held.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot hold %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    // In the moment between, someone may have opened it to write; a write handle still open would
    // have refused the hold above, and one already closed is caught here.
    if (auto where = requireResolvesTo(held.get(), full, full); !where) {
        return std::unexpected(where.error());
    }
    if (!readsBackAs(held.get(), bytes)) {
        return std::unexpected(QStringLiteral("%1 changed after it was written").arg(full));
    }
    return held;
}

std::expected<UniqueHandle, QString> createNewFile(const HeldDirectory& directory,
                                                   const QString& name,
                                                   bool deny_write) {
    if (!isSingleName(name) || !directory.isOpen()) {
        return std::unexpected(QStringLiteral("Not a single file name: %1").arg(name));
    }
    const QString full = directory.filePath(name);
    UniqueHandle file(CreateFileW(win32Path(full).c_str(),
                                  // No DELETE access: a reader (the log viewer) opens without
                                  // FILE_SHARE_DELETE and would be refused.
                                  GENERIC_WRITE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                  deny_write ? FILE_SHARE_READ : FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr));
    if (!file.valid()) {
        return std::unexpected(
            QStringLiteral("Cannot create %1: %2").arg(full, lastErrorText(GetLastError())));
    }
    // The directory is held, so this cannot resolve elsewhere; checked anyway, fail closed.
    if (auto where = requireResolvesTo(file.get(), full, full); !where) {
        return std::unexpected(where.error());
    }
    return file;
}

}  // namespace sak::no_follow
