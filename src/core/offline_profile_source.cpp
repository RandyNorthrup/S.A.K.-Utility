// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/offline_profile_source.h"

#include "sak/app_paths.h"
#include "sak/drive_scanner.h"
#include "sak/elevation_broker.h"
#include "sak/logger.h"
#include "sak/no_follow_fs.h"
#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QScopeGuard>

#include <algorithm>
#include <cstring>
#include <vector>

#include <windows.h>

namespace {

// Every buffer, cap and timeout this file needs is named here; the magic-number gate is not
// a suggestion, and a probe buffer is exactly the kind of number that silently truncates.
constexpr DWORD kVolumeNameBufferChars = 512;
constexpr DWORD kMountPathBufferChars = 512;
constexpr int kMountPathQueryAttempts = 3;
constexpr int kMountPathBufferGrowth = 2;
constexpr DWORD kRegistryStringBufferChars = 512;
constexpr DWORD kRegistryKeyBufferChars = 256;
constexpr DWORD kVolumeLabelBufferChars = 256;
constexpr DWORD kErrorTextBufferChars = 512;
constexpr size_t kSamSidOffset = 0x0C;
constexpr size_t kSamSidStructBytes = 24;  // 1 rev + 1 count + 6 authority + 4 sub-authorities
constexpr int kSidNtAuthority = 5;
constexpr int kSidIdentityAuthority = 21;
constexpr unsigned char kSidRevisionNt = 1;     // the revision byte every NT SID carries
constexpr int kSidMachineSubAuthorities = 4;    // 21 plus the three machine dwords
constexpr size_t kSidAuthorityFieldOffset = 2;  // IdentifierAuthority spans bytes 2..7
constexpr size_t kSidAuthorityFieldBytes = 6;
constexpr int kBitsPerByte = 8;
constexpr int kMachineRidFirst = 1;  // sub[0] is the 21 already checked; dwords follow it
constexpr int kRecoveryKeyDigits = 48;
constexpr int kRecoveryKeyGroupSize = 6;
constexpr int kGuidTailChars = 13;  // the dash-tail of a volume GUID is enough to label it
constexpr int kHelperOutputBytes = 262'144;
constexpr int kScriptTimeoutSeconds = 120;
constexpr int kHelperMessageMaxChars = 2000;
constexpr int kSizeWalkFileCap = 20'000;
constexpr int kSizeWalkDepthCap = 12;
constexpr size_t kUsersListingBufferChars = MAX_PATH + 8;
constexpr int kFileSizeHighShift = 32;  // nFileSizeHigh holds the upper 32 bits of the size

// RegLoadKey and RegUnloadKey need SeRestorePrivilege (SE_RESTORE_NAME); there is no
// separate "load registry" privilege in the LSA privilege list. permission_manager.cpp
// enables the same privilege for registry work.
constexpr const wchar_t* kLoadHivePrivilegeName = SE_RESTORE_NAME;

constexpr const char* kUnlockMarker = "SAK-OFFLINE-UNLOCK-OK";
constexpr const char* kMountMarker = "SAK-OFFLINE-MOUNT-OK";
constexpr const char* kUnmountMarker = "SAK-OFFLINE-UNMOUNT-OK";

/// @brief FormatMessage for the last Win32 error, in the shape a technician can read.
QString winErrorText(const QString& action) {
    wchar_t buffer[kErrorTextBufferChars] = {};
    const DWORD code = GetLastError();
    const DWORD written = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         code,
                                         0,
                                         buffer,
                                         kErrorTextBufferChars,
                                         nullptr);
    QString text = QString::fromWCharArray(buffer, static_cast<qsizetype>(written));
    while (text.endsWith(QLatin1Char('\r')) || text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    if (text.isEmpty()) {
        text = QStringLiteral("Win32 error %1").arg(code);
    }
    return QStringLiteral("%1 failed: %2").arg(action, text);
}

QString registryErrorText(const QString& action, LSTATUS status) {
    wchar_t buffer[kErrorTextBufferChars] = {};
    const DWORD written = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                         nullptr,
                                         static_cast<DWORD>(status),
                                         0,
                                         buffer,
                                         kErrorTextBufferChars,
                                         nullptr);
    QString text = QString::fromWCharArray(buffer, static_cast<qsizetype>(written));
    while (text.endsWith(QLatin1Char('\r')) || text.endsWith(QLatin1Char('\n'))) {
        text.chop(1);
    }
    if (text.isEmpty()) {
        text = QStringLiteral("registry status %1").arg(static_cast<int>(status));
    }
    return QStringLiteral("%1 failed: %2").arg(action, text);
}

/// @brief Enable SeRestorePrivilege, which RegLoadKey/RegUnloadKey require, for this process.
///        An elevated token HOLDS the privilege but starts with it disabled; RegLoadKey
///        without enabling it fails with a reason a technician cannot act on. Fail-closed:
///        no privilege, no load.
bool enableLoadRegistryPrivilege(QString& error) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == 0) {
        error = winErrorText(QStringLiteral("open this process token"));
        return false;
    }
    const auto close_token = qScopeGuard([token] { CloseHandle(token); });
    LUID privilege_luid = {};
    if (!LookupPrivilegeValueW(nullptr, kLoadHivePrivilegeName, &privilege_luid)) {
        error = winErrorText(QStringLiteral("look up the registry-restore privilege"));
        return false;
    }
    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = privilege_luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr)) {
        error = winErrorText(QStringLiteral("enable the registry-restore privilege"));
        return false;
    }
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED) {
        error = QStringLiteral(
            "Administrator access with the registry-restore privilege is required to read an "
            "offline registry hive.");
        return false;
    }
    return true;
}

enum class Presence {
    Present,
    Absent,
    Unknown
};

/// @brief GetFileAttributesW three-valued: present, provably absent, or unknown. The
///        "absent" test is DriveScanner::indicatorAbsentFor, so "no Windows here" and
///        "locked, cannot tell" stay the two different answers they are.
Presence probePath(const QString& native_path) {
    const DWORD attrs = GetFileAttributesW(native_path.toStdWString().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return Presence::Present;
    }
    return DriveScanner::indicatorAbsentFor(GetLastError()) ? Presence::Absent : Presence::Unknown;
}

/// @brief Mount path(s) of a volume name. An empty list is a genuine unmounted volume;
///        a failed query is an error, never an empty list (fail closed).
bool queryMountPaths(const QString& volume_name, QStringList& paths, QString& error) {
    const std::wstring volume = volume_name.toStdWString();
    DWORD needed = 0;
    std::vector<wchar_t> buffer(kMountPathBufferChars);
    for (int attempt = 0; attempt < kMountPathQueryAttempts; ++attempt) {
        needed = 0;
        if (GetVolumePathNamesForVolumeNameW(
                volume.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), &needed)) {
            // Double-NUL-terminated multi-string of mount path(s); none = genuinely unmounted.
            for (const wchar_t* p = buffer.data(); *p != L'\0'; p += wcslen(p) + 1) {
                paths.append(QString::fromWCharArray(p));
            }
            return true;
        }
        const DWORD code = GetLastError();
        if (code == ERROR_MORE_DATA && needed > buffer.size()) {
            buffer.resize(static_cast<size_t>(needed) * kMountPathBufferGrowth);
            continue;
        }
        error = winErrorText(QStringLiteral("read the mount points of %1").arg(volume_name));
        return false;
    }
    error = QStringLiteral("The mount points of %1 could not be read completely.").arg(volume_name);
    return false;
}

/// @brief Every volume in the OS mount table, GUID names without trailing backslash.
///        False with a reason when the enumeration itself failed -- never a short list
///        presented as the complete truth.
bool enumerateVolumeNames(QStringList& names, QString& error) {
    std::vector<wchar_t> first(kVolumeNameBufferChars);
    HANDLE find = FindFirstVolumeW(first.data(), kVolumeNameBufferChars);
    if (find == INVALID_HANDLE_VALUE) {
        error = winErrorText(QStringLiteral("enumerate the volumes of this machine"));
        return false;
    }
    names.append(QString::fromWCharArray(first.data()));
    bool ok = true;
    while (true) {
        std::vector<wchar_t> next(kVolumeNameBufferChars);
        if (FindNextVolumeW(find, next.data(), kVolumeNameBufferChars)) {
            names.append(QString::fromWCharArray(next.data()));
            continue;
        }
        if (GetLastError() != ERROR_NO_MORE_FILES) {
            error = winErrorText(QStringLiteral("enumerate the volumes of this machine"));
            ok = false;
        }
        break;
    }
    FindVolumeClose(find);
    return ok;
}

struct FolderListing {
    QString name;
    bool is_reparse{false};
};

/// @brief The "." and ".." entries every FindFirstFile walk returns, shared by the two walks
///        below so a skip stays one decision and the walks each read at their own bounds.
bool isDotEntry(const QString& name) {
    return name == QLatin1String(".") || name == QLatin1String("..");
}

/// @brief Direct subfolders of a Users directory, native APIs so a \\?\Volume{GUID} root
///        works as well as a drive letter. A reparse point is REPORTED, not followed:
///        elevated code follows no link (owner ruling), and junction refusal is the guard.
bool listUserFolders(const QString& users_root, QVector<FolderListing>& folders, QString& error) {
    const std::wstring pattern = (users_root + QStringLiteral("*")).toStdWString();
    WIN32_FIND_DATAW find_data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            return true;  // genuinely no Users directory: an empty, honest list
        }
        error = winErrorText(QStringLiteral("read %1").arg(users_root));
        return false;
    }
    bool ok = true;
    do {
        const QString name = QString::fromWCharArray(find_data.cFileName);
        if (isDotEntry(name)) {
            continue;
        }
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            continue;
        }
        folders.push_back({name, (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
    } while (FindNextFileW(find, &find_data));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        error = winErrorText(QStringLiteral("read %1").arg(users_root));
        ok = false;
    }
    FindClose(find);
    return ok;
}

/// @brief Recursive size of a profile folder, bounded by file count and depth. Returns -1
///        when the walk could not even start; otherwise a lower bound (documented, like
///        sumFolderFileSizes: a capped walk says "at least", and the caller shows -1 as
///        "size unknown" rather than a wrong number).
qint64 walkSize(const QString& path, int depth, int& files_remaining, bool& complete) {
    if (depth > kSizeWalkDepthCap || files_remaining <= 0) {
        complete = false;
        return 0;
    }
    qint64 total = 0;
    const std::wstring pattern = (path + QStringLiteral("*")).toStdWString();
    WIN32_FIND_DATAW find_data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) {
        complete = false;
        return 0;
    }
    do {
        const QString name = QString::fromWCharArray(find_data.cFileName);
        if (isDotEntry(name)) {
            continue;
        }
        const bool directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directory) {
            if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                total += walkSize(
                    path + name + QStringLiteral("\\"), depth + 1, files_remaining, complete);
            }
            continue;
        }
        if (files_remaining <= 0) {
            complete = false;
            break;
        }
        --files_remaining;
        total += (static_cast<qint64>(find_data.nFileSizeHigh) << kFileSizeHighShift) +
                 static_cast<qint64>(find_data.nFileSizeLow);
    } while (FindNextFileW(find, &find_data));
    if (GetLastError() != ERROR_NO_MORE_FILES) {
        complete = false;
    }
    FindClose(find);
    return total;
}

/// @brief The last path component of a hive ProfileImagePath value ("\Users\jdoe" -> "jdoe").
QString lastPathComponent(const QString& path) {
    QString trimmed = path;
    while (trimmed.endsWith(QLatin1Char('\\')) || trimmed.endsWith(QLatin1Char('/'))) {
        trimmed.chop(1);
    }
    const qsizetype slash = std::max(trimmed.lastIndexOf(QLatin1Char('\\')),
                                     trimmed.lastIndexOf(QLatin1Char('/')));
    return slash < 0 ? trimmed : trimmed.mid(slash + 1);
}

/// @brief Doubles apostrophes for embedding inside a single-quoted PowerShell literal
///        (the StagedScriptCredentials precedent): the staged path may sit under a name
///        like O'Brien, and an unescaped quote would close the literal and inject.
QString escapeSingleQuoted(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return escaped;
}

/// @brief True when the helper's stdout carries the script's own completion marker.
bool hasMarker(const QString& output, const char* marker) {
    return output.contains(QLatin1String(marker));
}

/// @brief The tail of helper output used in error messages; bounded so a chatty cmdlet
///        cannot paste an unbounded block into a dialog or a log.
QString outputTail(const QString& output) {
    return output.right(kHelperMessageMaxChars);
}

}  // namespace

namespace sak {

// ---------------------------------------------------------------------------
// OfflineRegistryHive
// ---------------------------------------------------------------------------

std::expected<OfflineRegistryHive, QString> OfflineRegistryHive::load(
    const QString& hive_file_path) {
    QString privilege_error;
    if (!enableLoadRegistryPrivilege(privilege_error)) {
        return std::unexpected(privilege_error);
    }
    // The key name is random and throwaway: two rescues (or two hives of one rescue) must
    // not collide under HKLM, and nothing of ours may persist there after the run.
    const std::wstring key_name = L"SAK-Offline-" + no_follow::randomToken().toStdWString();
    const std::wstring native = QDir::toNativeSeparators(hive_file_path).toStdWString();
    const LSTATUS status = RegLoadKeyW(HKEY_LOCAL_MACHINE, key_name.c_str(), native.c_str());
    if (status != ERROR_SUCCESS) {
        return std::unexpected(registryErrorText(
            QStringLiteral("load the offline hive '%1'").arg(hive_file_path), status));
    }
    OfflineRegistryHive hive(key_name);
    hive.m_loaded = true;
    return hive;
}

OfflineRegistryHive::OfflineRegistryHive(OfflineRegistryHive&& other) noexcept
    : m_key_name(std::move(other.m_key_name)), m_loaded(other.m_loaded) {
    other.m_loaded = false;
}

OfflineRegistryHive::~OfflineRegistryHive() {
    if (!m_loaded) {
        return;
    }
    if (auto closed = unload(); !closed) {
        // A hive still mounted in HKLM is visible damage on the technician's machine.
        // It cannot be un-done here any more than the caller could, so it is LOUD, not
        // silent: the run's log carries the key Windows still holds.
        logger::instance().log(log_level::warning,
                               "Offline registry hive could not be unloaded: %1",
                               closed.error().toStdString());
    }
}

std::expected<void, QString> OfflineRegistryHive::unload() {
    if (!m_loaded) {
        return {};
    }
    const LSTATUS status = RegUnLoadKeyW(HKEY_LOCAL_MACHINE, m_key_name.c_str());
    if (status != ERROR_SUCCESS) {
        return std::unexpected(
            registryErrorText(QStringLiteral("unload the offline hive"), status));
    }
    // Verify, per the plan: an unload that Windows did not honor must not be assumed.
    HKEY probe = nullptr;
    const LSTATUS reopen =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, m_key_name.c_str(), 0, KEY_READ, &probe);
    if (reopen == ERROR_SUCCESS) {
        RegCloseKey(probe);
        return std::unexpected(
            QStringLiteral("The offline hive key '%1' is still loaded in HKLM after unload.")
                .arg(QString::fromStdWString(m_key_name)));
    }
    m_loaded = false;
    return {};
}

QString OfflineRegistryHive::readString(const QString& sub_key, const QString& value_name) const {
    HKEY key = nullptr;
    const std::wstring full = m_key_name + L"\\" + sub_key.toStdWString();
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return {};
    }
    const auto close_key = qScopeGuard([key] { RegCloseKey(key); });
    std::vector<wchar_t> buffer(kRegistryStringBufferChars);
    DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key,
                                            value_name.toStdWString().c_str(),
                                            nullptr,
                                            &type,
                                            reinterpret_cast<BYTE*>(buffer.data()),
                                            &size);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return {};
    }
    std::wstring text(buffer.data(), size / sizeof(wchar_t));
    while (!text.empty() && text.back() == L'\0') {
        text.pop_back();
    }
    return QString::fromStdWString(text);
}

QByteArray OfflineRegistryHive::readBinary(const QString& sub_key,
                                           const QString& value_name) const {
    HKEY key = nullptr;
    const std::wstring full = m_key_name + L"\\" + sub_key.toStdWString();
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return {};
    }
    const auto close_key = qScopeGuard([key] { RegCloseKey(key); });
    DWORD needed = 0;
    DWORD type = 0;
    // The type probe must pass &type: a nullptr lpType leaves type untouched (0),
    // and the REG_BINARY check below would then reject every value on every hive.
    LSTATUS status =
        RegQueryValueExW(key, value_name.toStdWString().c_str(), nullptr, &type, nullptr, &needed);
    if (status != ERROR_SUCCESS || type != REG_BINARY || needed == 0) {
        return {};
    }
    std::vector<BYTE> buffer(needed);
    DWORD size = needed;
    status = RegQueryValueExW(
        key, value_name.toStdWString().c_str(), nullptr, nullptr, buffer.data(), &size);
    if (status != ERROR_SUCCESS || size != needed) {
        return {};
    }
    return QByteArray(reinterpret_cast<const char*>(buffer.data()), static_cast<qsizetype>(size));
}

QStringList OfflineRegistryHive::subKeyNames(const QString& sub_key) const {
    HKEY key = nullptr;
    const std::wstring full = m_key_name + L"\\" + sub_key.toStdWString();
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, full.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return {};
    }
    const auto close_key = qScopeGuard([key] { RegCloseKey(key); });
    QStringList names;
    std::vector<wchar_t> buffer(kRegistryKeyBufferChars);
    for (DWORD index = 0;; ++index) {
        DWORD size = static_cast<DWORD>(buffer.size());
        const LSTATUS status =
            RegEnumKeyExW(key, index, buffer.data(), &size, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_FILES || status == ERROR_MORE_DATA) {
            break;  // the names collected so far stand; an over-long name is not a profile SID
        }
        if (status != ERROR_SUCCESS) {
            break;
        }
        names.append(QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(size)));
    }
    return names;
}

// ---------------------------------------------------------------------------
// OfflineProfileSource: pure decision seams
// ---------------------------------------------------------------------------

QString OfflineProfileSource::normalizeVolumeRoot(const QString& root) {
    QString native = QDir::toNativeSeparators(root);
    while (native.endsWith(QLatin1Char('\\'))) {
        native.chop(1);
    }
    return native + QLatin1Char('\\');
}

bool OfflineProfileSource::isLiveSystemRoot(const QString& candidate, const QString& live_root) {
    const QString a = normalizeVolumeRoot(candidate).toUpper();
    const QString b = normalizeVolumeRoot(live_root).toUpper();
    return !a.isEmpty() && !b.isEmpty() && a == b;
}

bool OfflineProfileSource::isVolumeGuidPath(const QString& volume_name) {
    // \\?\Volume{8-4-4-4-12 hex} with no trailing backslash, checked by construction rather
    // than by a regex: the escapes a \\?\ pattern needs in a regex string hide more than
    // they reveal.
    if (!volume_name.startsWith(QStringLiteral("\\\\?\\Volume{")) ||
        !volume_name.endsWith(QLatin1Char('}'))) {
        return false;
    }
    constexpr QLatin1String kGuidPrefix("\\\\?\\Volume{");
    constexpr int kGuidInnerChars = 36;
    constexpr int kGuidDashPositions[4] = {8, 13, 18, 23};
    const QString guid = volume_name.mid(kGuidPrefix.size(),
                                         volume_name.size() - kGuidPrefix.size() - 1);
    if (guid.size() != kGuidInnerChars) {
        return false;
    }
    for (int position = 0; position < guid.size(); ++position) {
        const QChar ch = guid.at(position);
        const bool dash_expected = std::find(std::begin(kGuidDashPositions),
                                             std::end(kGuidDashPositions),
                                             position) != std::end(kGuidDashPositions);
        if (dash_expected) {
            if (ch != QLatin1Char('-')) {
                return false;
            }
        } else if (!ch.isLetterOrNumber() ||
                   !QStringLiteral("0123456789abcdefABCDEF").contains(ch)) {
            return false;
        }
    }
    return true;
}

bool OfflineProfileSource::isRunMountDirectory(const QString& directory) {
    const QString session = app_paths::sessionTempDirectory();
    if (session.isEmpty()) {
        return false;
    }
    const QString prefix = QDir::toNativeSeparators(session) + QLatin1Char('\\');
    const QString native = QDir::toNativeSeparators(directory);
    if (!native.startsWith(prefix, Qt::CaseInsensitive)) {
        return false;
    }
    const QString tail = native.mid(prefix.size());
    constexpr QLatin1String kMountPrefix("offline-");
    return tail.startsWith(kMountPrefix) && !tail.contains(QLatin1Char('\\')) &&
           tail.size() > kMountPrefix.size();
}

bool OfflineProfileSource::isReservedProfileFolderName(const QString& folder_name) {
    static const QStringList reserved = {QStringLiteral("Public"),
                                         QStringLiteral("Default"),
                                         QStringLiteral("Default User"),
                                         QStringLiteral("All Users")};
    return reserved.contains(folder_name, Qt::CaseInsensitive);
}

QString OfflineProfileSource::machineSidFromSamAccountValue(const QByteArray& account_f_value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(account_f_value.constData());
    const size_t size = static_cast<size_t>(account_f_value.size());
    if (size < kSamSidOffset + kSamSidStructBytes) {
        return {};
    }
    const unsigned char* sid = bytes + kSamSidOffset;
    // A real SID structure, not a guess: revision 1, authority 5 (NT), four sub-authorities
    // (21 + three machine dwords). Anything else parses to empty and the scanner refuses to
    // classify rather than trust an offset it could not prove.
    if (sid[0] != kSidRevisionNt || sid[1] != kSidMachineSubAuthorities) {
        return {};
    }
    quint64 authority = 0;
    for (size_t i = kSidAuthorityFieldOffset;
         i < kSidAuthorityFieldOffset + kSidAuthorityFieldBytes;
         ++i) {
        authority = (authority << kBitsPerByte) | sid[i];
    }
    if (authority != static_cast<quint64>(kSidNtAuthority)) {
        return {};
    }
    quint32 sub[kSidMachineSubAuthorities] = {};
    std::memcpy(sub,
                sid + kSidAuthorityFieldOffset + kSidAuthorityFieldBytes,
                sizeof(sub));  // little-endian on the wire and on the host
    if (sub[0] != static_cast<quint32>(kSidIdentityAuthority)) {
        return {};
    }
    // sub[0] is the 21 just proven; the three machine dwords are the rest of the string.
    QString machine_sid = QStringLiteral("S-1-5-21");
    for (int rid = kMachineRidFirst; rid < kSidMachineSubAuthorities; ++rid) {
        machine_sid += QLatin1Char('-') + QString::number(sub[rid]);
    }
    return machine_sid;
}

bool OfflineProfileSource::sidBelongsToMachine(const QString& profile_sid,
                                               const QString& machine_sid) {
    if (profile_sid.isEmpty() || machine_sid.isEmpty()) {
        return false;
    }
    return profile_sid.startsWith(machine_sid + QLatin1Char('-'), Qt::CaseInsensitive);
}

QString OfflineProfileSource::profileSkipReason(const QString& folder_name,
                                                const QString& sid,
                                                const QString& machine_sid,
                                                bool hive_read) {
    if (isReservedProfileFolderName(folder_name)) {
        return QStringLiteral("Built-in Windows folder, not a user account.");
    }
    if (!hive_read) {
        return {};  // honest fallback: folder names only, warning carried beside the list
    }
    if (sid.isEmpty()) {
        return QStringLiteral(
            "No single account record in that Windows matches this folder "
            "(a removed account, or two records of the same name).");
    }
    if (machine_sid.isEmpty()) {
        return QStringLiteral(
            "That Windows' account authority could not be read (its SAM hive), so this "
            "account cannot be proven local; offline rescue only takes local accounts.");
    }
    if (!sidBelongsToMachine(sid, machine_sid)) {
        return QStringLiteral(
            "Domain or network-account profile; offline rescue covers local accounts only.");
    }
    return {};
}

std::expected<QString, QString> OfflineProfileSource::normalizeRecoveryKey(const QString& typed) {
    QString digits;
    digits.reserve(kRecoveryKeyDigits);
    for (const QChar& ch : typed) {
        if (ch.isLetter()) {
            return std::unexpected(
                QStringLiteral("A BitLocker recovery key is 48 digits; the typed value contains "
                               "letters."));
        }
        if (ch.isDigit()) {
            digits.append(ch);
        }
    }
    if (digits.size() != kRecoveryKeyDigits) {
        return std::unexpected(
            QStringLiteral(
                "A BitLocker recovery key is 48 digits shown in 8 groups of 6; the typed value "
                "has %1 digits.")
                .arg(digits.size()));
    }
    QString canonical;
    for (int group = 0; group < kRecoveryKeyDigits / kRecoveryKeyGroupSize; ++group) {
        if (group > 0) {
            canonical.append(QLatin1Char('-'));
        }
        canonical.append(digits.mid(group * kRecoveryKeyGroupSize, kRecoveryKeyGroupSize));
    }
    return canonical;
}

std::expected<QString, QString> OfflineProfileSource::bitLockerUnlockScript(
    const QString& volume_name, const QString& staged_key_path) {
    if (!isVolumeGuidPath(volume_name)) {
        return std::unexpected(QStringLiteral("%1 is not a volume GUID path.").arg(volume_name));
    }
    if (staged_key_path.isEmpty() || !QFileInfo(staged_key_path).isAbsolute()) {
        return std::unexpected(
            QStringLiteral("The staged recovery-key file must be an absolute path."));
    }
    // The KEY never appears here: only the path of the held file the helper reads it from.
    const QString mount = escapeSingleQuoted(volume_name + QLatin1Char('\\'));
    const QString key_file = escapeSingleQuoted(QDir::toNativeSeparators(staged_key_path));
    return QStringLiteral(
               "$ErrorActionPreference = 'Stop'\n"
               "$ProgressPreference = 'SilentlyContinue'\n"
               "if (-not (Get-Command Unlock-BitLocker -ErrorAction SilentlyContinue)) { "
               "Write-Error 'This Windows has no BitLocker cmdlets (Unlock-BitLocker).'; exit 1 }\n"
               "$key = [IO.File]::ReadAllText('%1').Trim()\n"
               "Unlock-BitLocker -MountPoint '%2' -RecoveryPassword $key | Out-String\n"
               "Write-Output '%3'\n")
        .arg(key_file, mount, QLatin1String(kUnlockMarker));
}

// ---------------------------------------------------------------------------
// OfflineProfileSource: hive facts
// ---------------------------------------------------------------------------

namespace {

/// @brief The offline machine's computer name from its own SYSTEM hive.
///
///        "CurrentControlSet" is deliberately NOT read: inside a hive it is a link
///        node whose target is an absolute registry path, so following it resolves
///        into the LIVE machine's SYSTEM hive, not the loaded copy -- an offline PC
///        would be labeled with the scanning PC's own name. The offline system's
///        last-used control set is named by Select\\Default ("001" -> ControlSet001).
///        No guessed default: if no control set answers, the name stays empty and
///        the caller falls back honestly.
QString computerNameFromSystemHive(const OfflineRegistryHive& system) {
    const QString default_set =
        system.readString(QStringLiteral("Select"), QStringLiteral("Default")).trimmed();
    QStringList control_sets;
    if (!default_set.isEmpty()) {
        control_sets << QStringLiteral("ControlSet%1").arg(default_set);
    }
    control_sets << QStringLiteral("ControlSet001") << QStringLiteral("ControlSet002");
    for (const QString& control_set : control_sets) {
        const QString name =
            system.readString(control_set + QStringLiteral("\\Control\\ComputerName\\ComputerName"),
                              QStringLiteral("ComputerName"));
        if (!name.isEmpty()) {
            return name;
        }
    }
    return {};
}

}  // namespace

OfflineHiveFacts OfflineProfileSource::readOfflineHives(const QString& windows_root) {
    OfflineHiveFacts facts;
    const QString config = windows_root + QStringLiteral("System32\\config\\");

    auto software = OfflineRegistryHive::load(config + QStringLiteral("SOFTWARE"));
    if (software) {
        const QString profile_list =
            QStringLiteral("Microsoft\\Windows NT\\CurrentVersion\\ProfileList");
        for (const QString& sid : software->subKeyNames(profile_list)) {
            facts.profile_list.push_back(
                {sid,
                 software->readString(profile_list + QLatin1Char('\\') + sid,
                                      QStringLiteral("ProfileImagePath"))});
        }
        facts.profile_list_ok = true;  // the list itself completed; an empty one is genuine
        if (auto closed = software->unload(); !closed) {
            facts.errors.append(closed.error());
        }
    } else {
        facts.errors.append(software.error());
    }

    auto system = OfflineRegistryHive::load(config + QStringLiteral("SYSTEM"));
    if (system) {
        facts.computer_name = computerNameFromSystemHive(*system);
        if (auto closed = system->unload(); !closed) {
            facts.errors.append(closed.error());
        }
    } else {
        facts.errors.append(system.error());
    }

    auto sam = OfflineRegistryHive::load(config + QStringLiteral("SAM"));
    if (sam) {
        facts.machine_sid = machineSidFromSamAccountValue(
            sam->readBinary(QStringLiteral("Domains\\Account"), QStringLiteral("F")));
        facts.machine_sid_ok = !facts.machine_sid.isEmpty();
        if (auto closed = sam->unload(); !closed) {
            facts.errors.append(closed.error());
        }
    } else {
        facts.errors.append(sam.error());
    }
    return facts;
}

// ---------------------------------------------------------------------------
// OfflineProfileSource: discovery
// ---------------------------------------------------------------------------

QString OfflineProfileSource::elevationRequiredMessage() {
    return QStringLiteral(
        "Reading another Windows installation needs administrator access. Run S.A.K. Utility "
        "as administrator and try again.");
}

namespace {

/// @brief Resolve any mount spelling (drive letter, mount point, or volume GUID path)
///        to the volume GUID path WITHOUT its trailing backslash.
///
/// The live-system guard compares volume NAMES: GetVolumePathNameW hands back "C:\\",
/// which never equals the "\\?\\Volume{GUID}" the enumeration returns -- leaving the
/// running system listed as an offline source. Everything that guards, matches or
/// refuses a volume goes through this resolution first, so a drive letter and the GUID
/// are the same volume to the code. Empty when Windows does not know the mount.
QString resolveVolumeGuid(const QString& mount) {
    const QString trimmed = mount.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (OfflineProfileSource::isVolumeGuidPath(trimmed)) {
        return trimmed;
    }
    QString point = QDir::toNativeSeparators(trimmed);
    if (!point.endsWith(QLatin1Char('\\'))) {
        point += QLatin1Char('\\');
    }
    wchar_t guid[kVolumeNameBufferChars] = {};
    if (!GetVolumeNameForVolumeMountPointW(
            reinterpret_cast<LPCWSTR>(point.utf16()), guid, kVolumeNameBufferChars)) {
        return {};
    }
    QString name = QString::fromWCharArray(guid);
    while (name.endsWith(QLatin1Char('\\'))) {
        name.chop(1);
    }
    return OfflineProfileSource::isVolumeGuidPath(name) ? name : QString();
}

}  // namespace

QString OfflineProfileSource::liveSystemVolumeName() {
    wchar_t system_dir[MAX_PATH] = {};
    if (GetSystemWindowsDirectoryW(system_dir, MAX_PATH) == 0) {
        return {};
    }
    std::vector<wchar_t> volume(kVolumeNameBufferChars);
    if (!GetVolumePathNameW(system_dir, volume.data(), kVolumeNameBufferChars)) {
        return {};
    }
    // A drive letter is not comparable with the enumerated GUID names, so it is
    // resolved all the way to the GUID (fail closed when that resolution fails).
    return resolveVolumeGuid(QString::fromWCharArray(volume.data()));
}

namespace {

/// @brief What one volume root says about hosting a Windows installation, three-valued.
struct RootProbe {
    bool readable{true};
    bool windows_found{false};
    QString unread_reason;
};

RootProbe probeWindowsPresence(const QString& root) {
    RootProbe probe;
    const Presence windows = probePath(root + QStringLiteral("Windows"));
    if (windows == Presence::Unknown) {
        probe.readable = false;
        probe.unread_reason = QStringLiteral(
            "This volume could not be read. It is most likely BitLocker-locked (or was left "
            "in a state Windows cannot open). If it is encrypted, unlock it below with its "
            "recovery key and scan again; nothing was assumed about its contents.");
        return probe;
    }
    if (windows == Presence::Absent) {
        return probe;  // readable, provably no Windows tree: not a rescue source
    }
    const Presence kernel = probePath(root + QStringLiteral("Windows\\System32\\ntoskrnl.exe"));
    if (kernel == Presence::Unknown) {
        probe.readable = false;
        probe.unread_reason = QStringLiteral(
            "The Windows folder on this volume could not be read completely (access denied or "
            "the volume is locked). Nothing was assumed about its contents.");
        return probe;
    }
    probe.windows_found = kernel == Presence::Present;
    return probe;
}

QString volumeLabelOrTail(const QString& root, const QString& volume_name) {
    wchar_t label[kVolumeLabelBufferChars] = {};
    if (GetVolumeInformationW(root.toStdWString().c_str(),
                              label,
                              kVolumeLabelBufferChars,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              0)) {
        const QString text = QString::fromWCharArray(label);
        if (!text.isEmpty()) {
            return text;
        }
    }
    return volume_name.right(kGuidTailChars);
}

/// @brief The SID of the hive row whose ProfileImagePath names this folder. Zero or two
///        matches are NO sid -- a guessed match between duplicates is exactly the wrong.
QString matchSidForFolder(const QVector<OfflineHiveProfileRow>& rows, const QString& folder_name) {
    QString found;
    int matches = 0;
    for (const auto& row : rows) {
        if (lastPathComponent(row.profile_image_path).compare(folder_name, Qt::CaseInsensitive) ==
            0) {
            found = row.sid;
            ++matches;
        }
    }
    return matches == 1 ? found : QString();
}

OfflineVolumeSource buildVolumeSource(const QString& volume_name) {
    OfflineVolumeSource source;
    source.volume_name = volume_name;

    QStringList mounts;
    QString mount_error;
    if (!queryMountPaths(volume_name, mounts, mount_error)) {
        source.readable = false;
        source.unread_reason = mount_error;
        source.display_name = volume_name.right(kGuidTailChars);
        return source;
    }
    const QString root =
        mounts.isEmpty()
            ? OfflineProfileSource::normalizeVolumeRoot(volume_name + QLatin1Char('\\'))
            : OfflineProfileSource::normalizeVolumeRoot(mounts.first());
    source.root_path = mounts.isEmpty() ? QString() : root;

    const RootProbe probe = probeWindowsPresence(root);
    source.windows_found = probe.windows_found;
    source.readable = probe.readable;
    source.unread_reason = probe.unread_reason;
    source.display_name = volumeLabelOrTail(root, volume_name);
    if (!source.readable || !source.windows_found) {
        return source;
    }

    const OfflineHiveFacts facts =
        OfflineProfileSource::readOfflineHives(root + QStringLiteral("Windows\\"));
    if (!facts.errors.isEmpty()) {
        source.hive_error = facts.errors.join(QStringLiteral("; "));
    }
    source.machine_sid = facts.machine_sid;
    source.computer_name = facts.computer_name;
    if (source.computer_name.isEmpty()) {
        source.computer_name = source.display_name;  // honest label fallback (plan: volume label)
    }

    QString list_error;
    source.profiles = OfflineProfileSource::scanUsersDirectory(root + QStringLiteral("Users\\"),
                                                               facts,
                                                               list_error);
    if (!list_error.isEmpty()) {
        source.readable = false;
        source.unread_reason = list_error;
        source.profiles.clear();
    }
    return source;
}

}  // namespace

OfflineSourceDiscovery OfflineProfileSource::discover(bool is_elevated) {
    OfflineSourceDiscovery result;
    if (!is_elevated) {
        result.error = elevationRequiredMessage();
        return result;
    }
    const QString live = liveSystemVolumeName();
    if (live.isEmpty()) {
        result.error = QStringLiteral(
            "This Windows' own volume could not be determined, so the scan refused: without "
            "it, the running system could be read as an offline one.");
        return result;
    }
    QStringList volume_names;
    QString enum_error;
    if (!enumerateVolumeNames(volume_names, enum_error)) {
        result.error = enum_error;
        return result;
    }
    result.enumeration_ok = true;
    for (const QString& volume_name : volume_names) {
        if (isLiveSystemRoot(volume_name, live)) {
            continue;  // the running system is the live feature's job, never this one's
        }
        OfflineVolumeSource source = buildVolumeSource(volume_name);
        if (source.readable && !source.windows_found) {
            continue;  // a genuinely non-Windows volume: nothing to offer, nothing to claim
        }
        result.volumes.push_back(source);
    }
    return result;
}

OfflineVolumeSource OfflineProfileSource::discoverVolume(const QString& volume_name,
                                                         bool is_elevated) {
    OfflineVolumeSource source;
    source.volume_name = volume_name;
    if (!is_elevated) {
        source.readable = false;
        source.unread_reason = elevationRequiredMessage();
        return source;
    }
    // Named however the technician or the wizard spells it, it must become the volume
    // GUID before it can be compared with the live system or enumerated: refusing a
    // drive letter with "not a GUID" would hide the honest reason ("this is the
    // running Windows") behind a syntax complaint.
    const QString guid = resolveVolumeGuid(volume_name);
    if (guid.isEmpty()) {
        source.readable = false;
        source.unread_reason =
            QStringLiteral("'%1' is not a volume on this PC, or Windows could not name it.")
                .arg(volume_name);
        return source;
    }
    source.volume_name = guid;
    const QString live = liveSystemVolumeName();
    if (live.isEmpty()) {
        source.readable = false;
        source.unread_reason = QStringLiteral("This Windows' own volume could not be determined.");
        return source;
    }
    if (isLiveSystemRoot(guid, live)) {
        source.readable = false;
        source.unread_reason = QStringLiteral(
            "This is the running Windows installation; use the live profile backup for it.");
        return source;
    }
    return buildVolumeSource(guid);
}

QVector<OfflineProfileEntry> OfflineProfileSource::scanUsersDirectory(const QString& users_root,
                                                                      const OfflineHiveFacts& facts,
                                                                      QString& error) {
    QVector<FolderListing> folders;
    if (!listUserFolders(users_root, folders, error)) {
        return {};
    }
    const bool hive_read = facts.profile_list_ok && facts.machine_sid_ok;
    QVector<OfflineProfileEntry> entries;
    for (const FolderListing& folder : folders) {
        OfflineProfileEntry entry;
        entry.folder_name = folder.name;
        entry.profile_path = users_root + folder.name;
        if (folder.is_reparse) {
            entry.included = false;
            entry.skip_reason = QStringLiteral(
                "This folder is a link, not a real profile folder; nothing followed it.");
        } else {
            entry.sid = matchSidForFolder(facts.profile_list, folder.name);
            entry.sid_from_hive = !entry.sid.isEmpty();
            entry.skip_reason =
                profileSkipReason(folder.name, entry.sid, facts.machine_sid, hive_read);
            entry.included = entry.skip_reason.isEmpty();
        }
        bool complete = true;
        int files_remaining = kSizeWalkFileCap;
        const qint64 size =
            walkSize(entry.profile_path + QLatin1Char('\\'), 0, files_remaining, complete);
        entry.estimated_size_bytes = complete ? size : -1;
        entries.push_back(entry);
    }
    return entries;
}

// ---------------------------------------------------------------------------
// OfflineProfileSource: elevated helper scripts (unlock, mount, unmount)
// ---------------------------------------------------------------------------

OfflineProfileSource::ElevatedScriptRunner OfflineProfileSource::sessionScriptRunner() {
    return [](const QString& script, const QString& summary) -> std::expected<QString, QString> {
        const auto broker = ElevationBroker::sessionBroker();
        QJsonObject payload;
        payload[QStringLiteral("command")] = script;
        payload[QStringLiteral("timeout_seconds")] = kScriptTimeoutSeconds;
        payload[QStringLiteral("max_output_bytes")] = kHelperOutputBytes;
        const auto result = broker->executeTask(QStringLiteral("RunPowerShell"), summary, payload);
        if (!result) {
            return std::unexpected(
                QStringLiteral("The elevation helper did not run: %1").arg(summary));
        }
        const QString output = result->data.value(QStringLiteral("stdout")).toString();
        if (!result->success) {
            const QString reason = result->error_message.isEmpty() ? outputTail(output)
                                                                   : result->error_message;
            return std::unexpected(reason);
        }
        return output;
    };
}

std::expected<QString, QString> OfflineProfileSource::unlockEncryptedVolume(
    const QString& volume_name,
    const QString& typed_recovery_key,
    const ElevatedScriptRunner& run_elevated) {
    auto key = normalizeRecoveryKey(typed_recovery_key);
    if (!key) {
        return std::unexpected(key.error());
    }
    if (!isVolumeGuidPath(volume_name)) {
        return std::unexpected(QStringLiteral("%1 is not a volume GUID path.").arg(volume_name));
    }
    auto staged = app_paths::writeHeldSessionTempFile(
        QStringLiteral("sak-bitlocker-recovery-%1.key").arg(no_follow::randomToken()),
        key->toUtf8());
    if (!staged) {
        return std::unexpected(
            QStringLiteral("Could not stage the recovery key: %1").arg(staged.error()));
    }
    // Same lifecycle as StagedScriptCredentials: the hold drops first, then the file is
    // zeroed and removed. The key must not survive the run it unlocked.
    const auto shred = qScopeGuard([&staged, &key] {
        staged->first.reset();
        QFile overwrite(staged->second);
        if (overwrite.open(QIODevice::ReadWrite | QIODevice::ExistingOnly)) {
            overwrite.write(QByteArray(key->size(), '\0'));
            overwrite.flush();
            overwrite.close();
        }
        QFile::remove(staged->second);
    });
    auto script = bitLockerUnlockScript(volume_name, staged->second);
    if (!script) {
        return std::unexpected(script.error());
    }
    const auto output = run_elevated(*script, QStringLiteral("Unlock offline BitLocker volume"));
    if (!output) {
        return std::unexpected(output.error());
    }
    if (!hasMarker(*output, kUnlockMarker)) {
        return std::unexpected(
            QStringLiteral("The unlock did not report success: %1").arg(outputTail(*output)));
    }
    return *output;
}

std::expected<QString, QString> OfflineProfileSource::mountVolumeToRunDirectory(
    const QString& volume_name, const ElevatedScriptRunner& run_elevated) {
    if (!isVolumeGuidPath(volume_name)) {
        return std::unexpected(QStringLiteral("%1 is not a volume GUID path.").arg(volume_name));
    }
    const QString session = app_paths::sessionTempDirectory();
    if (session.isEmpty()) {
        return std::unexpected(QStringLiteral("This run's temporary folder is not established."));
    }
    const QString mount_dir =
        QDir(session).filePath(QStringLiteral("offline-%1").arg(no_follow::randomToken()));
    if (!QDir().mkpath(mount_dir)) {
        return std::unexpected(
            QStringLiteral("Could not create the mount directory %1.").arg(mount_dir));
    }
    const QString script = QStringLiteral(
                               "$ErrorActionPreference = 'Stop'\n"
                               "$mountvol = Join-Path $env:WINDIR 'System32\\mountvol.exe'\n"
                               "if (-not (Test-Path -LiteralPath $mountvol)) { "
                               "Write-Error 'This Windows has no mountvol.exe; the offline volume "
                               "cannot be mounted.'; exit 1 }\n"
                               "& $mountvol '%1' '%2'\n"
                               "if ($LASTEXITCODE -ne 0) { Write-Error ('mountvol failed with code "
                               "' + $LASTEXITCODE); exit 1 }\n"
                               "Write-Output '%3'\n")
                               .arg(escapeSingleQuoted(QDir::toNativeSeparators(mount_dir)),
                                    escapeSingleQuoted(volume_name),
                                    QLatin1String(kMountMarker));
    const auto output = run_elevated(script, QStringLiteral("Mount offline volume"));
    if (!output) {
        QDir().rmdir(mount_dir);
        return std::unexpected(output.error());
    }
    if (!hasMarker(*output, kMountMarker)) {
        QDir().rmdir(mount_dir);
        return std::unexpected(
            QStringLiteral("The mount did not report success: %1").arg(outputTail(*output)));
    }
    return QDir::toNativeSeparators(mount_dir);
}

std::expected<void, QString> OfflineProfileSource::unmountRunDirectory(
    const QString& mount_directory, const ElevatedScriptRunner& run_elevated) {
    if (!isRunMountDirectory(mount_directory)) {
        return std::unexpected(
            QStringLiteral("%1 is not a mount directory this run created; refusing to unmount "
                           "anything else.")
                .arg(mount_directory));
    }
    const QString script = QStringLiteral(
                               "$ErrorActionPreference = 'Stop'\n"
                               "$mountvol = Join-Path $env:WINDIR 'System32\\mountvol.exe'\n"
                               "if (-not (Test-Path -LiteralPath $mountvol)) { "
                               "Write-Error 'This Windows has no mountvol.exe; the mount point "
                               "cannot be removed.'; exit 1 }\n"
                               "& $mountvol '%1' /p\n"
                               "if ($LASTEXITCODE -ne 0) { Write-Error ('mountvol /p failed with "
                               "code ' + $LASTEXITCODE); exit 1 }\n"
                               "Write-Output '%2'\n")
                               .arg(escapeSingleQuoted(QDir::toNativeSeparators(mount_directory)),
                                    QLatin1String(kUnmountMarker));
    const auto output = run_elevated(script, QStringLiteral("Unmount offline volume"));
    if (!output) {
        return std::unexpected(output.error());
    }
    if (!hasMarker(*output, kUnmountMarker)) {
        return std::unexpected(
            QStringLiteral("The unmount did not report success: %1").arg(outputTail(*output)));
    }
    QDir().rmdir(QDir::fromNativeSeparators(mount_directory));  // our own now-empty directory
    return {};
}

// ---------------------------------------------------------------------------
// OfflineVolumeMount: the RAII mount the assistant action and the wizard share
// ---------------------------------------------------------------------------

std::expected<OfflineVolumeMount, QString> OfflineVolumeMount::acquire(
    const QString& volume_name, const OfflineElevatedScriptRunner& run_elevated) {
    const auto directory = OfflineProfileSource::mountVolumeToRunDirectory(volume_name,
                                                                           run_elevated);
    if (!directory) {
        return std::unexpected(directory.error());
    }
    return OfflineVolumeMount(*directory, run_elevated);
}

OfflineVolumeMount::OfflineVolumeMount(OfflineVolumeMount&& other) noexcept
    : m_directory(std::move(other.m_directory)), m_runner(std::move(other.m_runner)) {}

OfflineVolumeMount::~OfflineVolumeMount() {
    if (m_directory.isEmpty()) {
        return;
    }
    // A mount point left on the technician's PC is visible damage, so a release the destructor
    // could not prove is reported, never swallowed. release() keeps the directory on failure,
    // which is what makes this second attempt the last one rather than a loop.
    const auto released = release();
    if (!released) {
        sak::logWarning("The offline mount '{}' could not be removed: {}",
                        m_directory.toStdString(),
                        released.error().toStdString());
    }
}

std::expected<void, QString> OfflineVolumeMount::release() {
    if (m_directory.isEmpty()) {
        return {};  // idempotent: already released
    }
    const auto unmounted = OfflineProfileSource::unmountRunDirectory(m_directory, m_runner);
    if (!unmounted) {
        return std::unexpected(unmounted.error());
    }
    m_directory.clear();
    return {};
}

// ---------------------------------------------------------------------------
// OfflineProfileSource: choosing, preparing and converting one offline volume
// ---------------------------------------------------------------------------

namespace {

/// @brief "here is what the scan did find", for the refusals that list alternatives.
QString volumeOptionList(const QVector<OfflineVolumeSource>& volumes) {
    QStringList options;
    for (const OfflineVolumeSource& source : volumes) {
        options.append(QStringLiteral("%1 (%2)").arg(source.display_name, source.volume_name));
    }
    return options.isEmpty() ? QStringLiteral("none") : options.join(QStringLiteral("; "));
}

/// @brief Same list for the volumes that MATCHED a request, so an ambiguous name is answered
///        with the candidates rather than the whole scan.
QString sourceNameList(const QVector<const OfflineVolumeSource*> matches) {
    QStringList options;
    for (const OfflineVolumeSource* source : matches) {
        options.append(QStringLiteral("%1 (%2)").arg(source->display_name, source->volume_name));
    }
    return options.join(QStringLiteral("; "));
}

/// @brief The refusal for a source that was found but could not be read, or mounted and
///        still could not be read. The unmount failing on top of the read failing is said in
///        one message: the technician has to clean up both.
QString unreadSourceRefusal(const OfflineVolumeSource& source,
                            const QString& mount_directory,
                            const QString& release_error) {
    const QString read_problem = source.unread_reason.isEmpty()
                                     ? QStringLiteral("The mounted source could not be read.")
                                     : source.unread_reason;
    if (release_error.isEmpty()) {
        return mount_directory.isEmpty()
                   ? QStringLiteral("The source volume '%1' could not be read: %2")
                         .arg(source.display_name, read_problem)
                   : read_problem;
    }
    return QStringLiteral(
               "%1 The mount directory '%2' could not be removed either; remove it "
               "from an administrator prompt: %3")
        .arg(read_problem, mount_directory, release_error);
}

}  // namespace

std::expected<OfflineVolumeSource, QString> OfflineProfileSource::matchVolumeRequest(
    const OfflineSourceDiscovery& discovery,
    const QString& requested_volume,
    const QString& caller_label) {
    const QString wanted = requested_volume.trimmed();
    if (wanted.isEmpty()) {
        return std::unexpected(QStringLiteral("%1 needs a source volume: pick one that holds "
                                              "another Windows installation. Found: %2")
                                   .arg(caller_label, volumeOptionList(discovery.volumes)));
    }
    QVector<const OfflineVolumeSource*> found;
    for (const OfflineVolumeSource& source : discovery.volumes) {
        if (source.volume_name.compare(wanted, Qt::CaseInsensitive) == 0 ||
            source.root_path.compare(wanted, Qt::CaseInsensitive) == 0 ||
            source.display_name.compare(wanted, Qt::CaseInsensitive) == 0 ||
            source.volume_name.contains(wanted, Qt::CaseInsensitive)) {
            found.append(&source);
        }
    }
    if (found.size() == 1) {
        return *found.first();
    }
    if (found.isEmpty()) {
        return std::unexpected(
            QStringLiteral(
                "No offline Windows installation named '%1'. This scan covers every volume except "
                "this PC's own running system.")
                .arg(wanted));
    }
    return std::unexpected(
        QStringLiteral("'%1' matches more than one volume: %2").arg(wanted, sourceNameList(found)));
}

namespace {

/// @brief Takes the mount of an abandoned attempt back down. Returns empty when the release
///        was proven; the reason when it was not (fail closed: the technician must know the
///        mount point is still on their PC).
QString abandonMount(std::optional<OfflineVolumeMount>& mount) {
    if (!mount.has_value()) {
        return {};
    }
    const auto released = mount->release();
    const QString error = released ? QString() : released.error();
    mount.reset();
    return error;
}

/// @brief Mounts an unmounted volume under this run's temp folder, then re-discovers it so the
///        hive facts and the profile paths exist as a walkable directory. On refusal the mount
///        is already taken back down; success moves it into prep.mount.
std::optional<QString> mountOfflineVolume(OfflineVolumeSource& source,
                                          OfflineVolumePrep& prep,
                                          const bool is_elevated,
                                          const OfflineElevatedScriptRunner& run_elevated) {
    auto mounted = OfflineVolumeMount::acquire(source.volume_name, run_elevated);
    if (!mounted) {
        return mounted.error();
    }
    source = OfflineProfileSource::discoverVolume(source.volume_name, is_elevated);
    if (!source.readable || source.root_path.isEmpty()) {
        const QString mount_directory = mounted->directory();
        const auto released = mounted->release();
        return unreadSourceRefusal(source,
                                   mount_directory,
                                   released ? QString() : released.error());
    }
    prep.mount.emplace(std::move(*mounted));
    return std::nullopt;
}

}  // namespace

std::expected<OfflineVolumePrep, QString> OfflineProfileSource::prepareVolume(
    const QString& requested_volume,
    const QString& caller_label,
    const bool is_elevated,
    const ElevatedScriptRunner& run_elevated) {
    if (!is_elevated) {
        return std::unexpected(elevationRequiredMessage());
    }
    const OfflineSourceDiscovery discovery = discover(is_elevated);
    if (!discovery.enumeration_ok) {
        return std::unexpected(discovery.error);
    }
    const auto found = matchVolumeRequest(discovery, requested_volume, caller_label);
    if (!found) {
        return std::unexpected(found.error());
    }

    OfflineVolumePrep prep;
    OfflineVolumeSource source = *found;
    if (!source.readable) {
        return std::unexpected(unreadSourceRefusal(source, QString(), QString()));
    }
    if (source.root_path.isEmpty()) {
        // Unmounted: mount it under this run's temp folder, then re-discover so the hive
        // facts and the profile paths exist as a walkable directory.
        if (const auto refusal = mountOfflineVolume(source, prep, is_elevated, run_elevated)) {
            return std::unexpected(*refusal);
        }
    }
    if (!source.windows_found) {
        const QString release_error = abandonMount(prep.mount);
        if (!release_error.isEmpty()) {
            return std::unexpected(release_error);
        }
        return std::unexpected(
            QStringLiteral(
                "'%1' holds no Windows installation, so there is nothing to rescue from it.")
                .arg(source.display_name));
    }
    prep.source = source;
    return prep;
}

UserProfile OfflineProfileSource::toUserProfile(const OfflineProfileEntry& entry) {
    UserProfile user;
    user.username = entry.folder_name;  // honest label: v1 has no account name, only the folder
    user.sid = entry.sid_from_hive ? entry.sid : QString();
    user.profile_path = entry.profile_path;
    user.total_size_estimated = WindowsUserScanner::estimateProfileSize(entry.profile_path);
    user.folder_selections = WindowsUserScanner::getDefaultFolderSelections(entry.profile_path);
    user.is_selected = true;
    return user;
}

}  // namespace sak
