// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QStandardPaths>

#include <limits>

#ifdef Q_OS_WIN
#include "sak/logger.h"

#include <windows.h>

#include <lm.h>
#include <sddl.h>
#include <userenv.h>
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "userenv.lib")
#endif

namespace sak {

namespace {
#ifdef Q_OS_WIN
constexpr DWORD kNetUserInfoDetailedLevel = 3;
constexpr int kWindowsAccountBufferChars = 256;
constexpr int kProfileSizeEstimateFileLimit = 1000;
constexpr int kFolderSelectionFileLimit = 10'000;
#endif
}  // namespace

WindowsUserScanner::WindowsUserScanner(QObject* parent) : QObject(parent) {}

QVector<UserProfile> WindowsUserScanner::scanUsers() {
    // Convenience overload for callers that do not track completeness. It must NOT let a hard
    // enumeration failure pass as a genuine empty user set: scanUsers(bool&) already clears the
    // list and reports queryOk == false on a NetUserEnum failure, so surface that failure here
    // instead of silently returning an ambiguous empty vector.
    bool queryOk = false;
    QVector<UserProfile> profiles = scanUsers(queryOk);
#ifdef Q_OS_WIN
    if (!queryOk) {
        sak::logError(
            "WindowsUserScanner::scanUsers(): enumeration failed; the empty result "
            "must not be read as 'no users found'");
    }
#endif
    return profiles;
}

QVector<UserProfile> WindowsUserScanner::scanUsers(bool& queryOk) {
    QVector<UserProfile> profiles;

#ifdef Q_OS_WIN
    // enumerateWindowsUsers returns false only on a hard NetUserEnum failure (e.g. ERROR_ACCESS_
    // DENIED without elevation -- the detailed USER_INFO_3 level needs admin). A drained-but-empty
    // enumeration returns true: that is a genuine "no matching accounts", not a failed read.
    queryOk = enumerateWindowsUsers(profiles);
#else
    queryOk = true;  // no Windows accounts to enumerate on this platform: genuine empty
#endif

    return profiles;
}

#ifdef Q_OS_WIN
bool WindowsUserScanner::enumerateWindowsUsers(QVector<UserProfile>& profiles) {
    // scanUsers(bool&) is the only caller and declares profiles as a fresh local.
    Q_ASSERT(profiles.isEmpty());
    const QString currentUserLower = getCurrentUsername().toLower();
    DWORD resumeHandle = 0;
    NET_API_STATUS status = NERR_Success;

    // Drain every page: NetUserEnum returns ERROR_MORE_DATA (not failure) when the SAM has
    // more normal accounts than fit one MAX_PREFERRED_LENGTH buffer. The single-call version
    // accepted that status and scanned only the first page, silently dropping later users.
    do {
        LPUSER_INFO_3 userInfo = nullptr;
        DWORD entriesRead = 0;
        DWORD totalEntries = 0;
        status = NetUserEnum(nullptr,                // local computer
                             kNetUserInfoDetailedLevel,
                             FILTER_NORMAL_ACCOUNT,  // normal user accounts only
                             reinterpret_cast<LPBYTE*>(&userInfo),
                             MAX_PREFERRED_LENGTH,
                             &entriesRead,
                             &totalEntries,
                             &resumeHandle);
        if (status != NERR_Success && status != ERROR_MORE_DATA) {
            // Surface the exact failure instead of returning a silently-partial list: the
            // caller's queryOk already flips false, but the concrete status was being lost.
            sak::logError("NetUserEnum failed; user enumeration incomplete (status {})",
                          QString::number(status).toStdString());
            // Fail closed: drop any profiles gathered from earlier pages so a caller that
            // ignores queryOk cannot mistake a partial (failed) enumeration for a complete one.
            profiles.clear();
            return false;
        }
        for (DWORD i = 0; i < entriesRead; ++i) {
            UserProfile profile;
            profile.username = QString::fromWCharArray(userInfo[i].usri3_name);
            profile.profile_path = getProfilePath(profile.username);
            if (profile.profile_path.isEmpty() || !QDir(profile.profile_path).exists()) {
                continue;
            }
            profile.sid = getUserSID(profile.username);
            if (profile.sid.isEmpty()) {
                // Surface, do not silently swallow: the SID drives later ownership/permission
                // operations, so a profile recorded without one must be visibly flagged.
                sak::logWarning("Could not resolve SID for user {}; profile recorded without a SID",
                                profile.username.toStdString());
            }
            profile.is_current_user = (profile.username.toLower() == currentUserLower);
            profile.total_size_estimated = estimateProfileSize(profile.profile_path);
            populateFolderSelections(profile);
            profiles.append(profile);
            Q_EMIT userFound(profile.username);
            Q_EMIT scanProgress(static_cast<int>(profiles.size()), static_cast<int>(totalEntries));
        }
        NetApiBufferFree(userInfo);
    } while (status == ERROR_MORE_DATA);
    return true;
}
#endif

QString WindowsUserScanner::getCurrentUsername() {
#ifdef Q_OS_WIN
    wchar_t username[kWindowsAccountBufferChars];
    DWORD size = kWindowsAccountBufferChars;
    if (GetUserNameW(username, &size) != 0) {
        return QString::fromWCharArray(username);
    }
#endif
    return QString();
}

QString WindowsUserScanner::getUserSID(const QString& username) {
    if (username.isEmpty()) {
        // Fail closed: an empty name is no account. LookupAccountNameW does not reliably fail on
        // one -- it can resolve to the local domain -- and callers use the returned SID to take
        // ownership and set permissions, so a non-user SID must never leave here.
        return {};
    }
#ifdef Q_OS_WIN
    // Try to lookup SID using LookupAccountName
    const auto* usernameW = reinterpret_cast<const wchar_t*>(username.utf16());
    PSID sid = nullptr;
    DWORD sidSize = 0;
    wchar_t domain[kWindowsAccountBufferChars];
    DWORD domainSize = kWindowsAccountBufferChars;
    SID_NAME_USE sidType;

    // First call to get required buffer size
    LookupAccountNameW(nullptr, usernameW, nullptr, &sidSize, domain, &domainSize, &sidType);

    if (sidSize == 0) {
        return QString();
    }

    sid = static_cast<PSID>(LocalAlloc(LPTR, sidSize));
    if (sid == nullptr) {
        return QString();
    }
    domainSize = kWindowsAccountBufferChars;

    if (LookupAccountNameW(nullptr, usernameW, sid, &sidSize, domain, &domainSize, &sidType) == 0) {
        LocalFree(sid);
        return QString();
    }

    LPWSTR sidString = nullptr;
    if (ConvertSidToStringSidW(sid, &sidString) == 0) {
        LocalFree(sid);
        return QString();
    }

    QString result = QString::fromWCharArray(sidString);
    LocalFree(sidString);
    LocalFree(sid);
    return result;
#else
    (void)username;
    return QString();
#endif
}

namespace {

/// @brief Expand a registry path value, handling REG_EXPAND_SZ environment variables
QString expandRegistryPath(const wchar_t* profileDir, DWORD valueType) {
    // lookupRegistryProfilePath is the only caller and passes its own stack buffer.
    Q_ASSERT(profileDir);
    if (valueType != REG_EXPAND_SZ) {
        return QString::fromWCharArray(profileDir);
    }
    wchar_t expandedPath[MAX_PATH] = {};
    DWORD const expandedLen = ExpandEnvironmentStringsW(profileDir, expandedPath, MAX_PATH);
    if (expandedLen > 0 && expandedLen <= MAX_PATH) {
        return QString::fromWCharArray(expandedPath);
    }
    // Fail closed: a failed or overflowing expansion must NOT fall back to the raw,
    // unexpanded %VAR% text -- that would let malformed registry data become a profile path.
    return {};
}

/// @brief Look up a user's profile path from the registry using their SID
QString lookupRegistryProfilePath(const QString& sid) {
    // getProfilePath is the only caller and returns early on an empty SID.
    Q_ASSERT(!sid.isEmpty());
    const QString regPath =
        QString("SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\%1").arg(sid);
    HKEY hKey = nullptr;
    LONG result = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE, reinterpret_cast<LPCWSTR>(regPath.utf16()), 0, KEY_READ, &hKey);

    if (result != ERROR_SUCCESS) {
        return {};
    }

    wchar_t profileDir[MAX_PATH] = {};
    DWORD bufferSize = sizeof(profileDir);
    DWORD valueType = 0;

    result = RegQueryValueExW(hKey,
                              L"ProfileImagePath",
                              nullptr,
                              &valueType,
                              reinterpret_cast<LPBYTE>(profileDir),
                              &bufferSize);

    RegCloseKey(hKey);

    if (result != ERROR_SUCCESS) {
        return {};
    }
    if (valueType != REG_SZ && valueType != REG_EXPAND_SZ) {
        return {};
    }
    // RegQueryValueExW does NOT guarantee the returned string is NUL-terminated and can fill
    // the whole buffer; bound the read by the returned byte count and force a terminator so
    // expandRegistryPath / ExpandEnvironmentStringsW / fromWCharArray never read past
    // profileDir. Reject a malformed length or one that left no room to terminate.
    if (bufferSize == 0 || (bufferSize % sizeof(wchar_t)) != 0 || bufferSize > sizeof(profileDir)) {
        return {};
    }
    const DWORD charCount = bufferSize / sizeof(wchar_t);
    if (charCount >= MAX_PATH) {
        return {};  // no slot left to terminate safely -> treat as hostile/malformed
    }
    profileDir[charCount] = L'\0';

    QString registryPath = expandRegistryPath(profileDir, valueType);
    if (QDir(registryPath).exists()) {
        return registryPath;
    }
    return {};
}

}  // anonymous namespace

QString WindowsUserScanner::getProfilePath(const QString& username) {
    if (username.isEmpty()) {
        // Fail closed: with no name the standard-location path built below collapses to the
        // profiles root itself, which exists -- so an empty name would resolve to the parent
        // of every user's profile instead of reporting "not found".
        return {};
    }

#ifdef Q_OS_WIN
    // AUTHORITATIVE source first: the SID -> ProfileImagePath registry mapping is where a
    // profile actually lives. A relocated profile is correct ONLY there, so it must win over
    // the guessed <SystemDrive>\Users\<name> location (no guessed-default preferred over the
    // authoritative source). lookupRegistryProfilePath already fails closed when the path it
    // names does not exist, so a stale registry entry falls through to the standard location.
    const QString sid = getUserSID(username);
    if (!sid.isEmpty()) {
        const QString registryPath = lookupRegistryProfilePath(sid);
        if (!registryPath.isEmpty()) {
            return registryPath;
        }
    }
#endif

    // Standard location only as a SECONDARY, when the authoritative lookup yielded nothing
    // (or off Windows). Still gated by existence so a nonexistent guess is reported as
    // "not found" rather than returned.
    // No 'C:' fallback: an unset SystemDrive means the standard location cannot be resolved,
    // so report "not found" rather than guess a drive letter. The guess is not merely
    // redundant -- on a machine whose system drive is not C: but which happens to have a
    // C:\Users\Username, guessing C: would return a path that is NOT this user's profile. This
    // matches UserProfileRestoreWorker::resolveCreateNewUser, which refuses the same guess,
    // and the no-guessed-default rule already applied to the authoritative lookup above.
    const QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    if (systemDrive.isEmpty()) {
        return {};
    }
    const QString standardPath = systemDrive + "\\Users\\" + username;
    if (QDir(standardPath).exists()) {
        return standardPath;
    }
    return {};
}

qint64 WindowsUserScanner::sumFolderFileSizes(const QString& folderPath, int fileLimit) {
    qint64 total = 0;
    int count = 0;
    QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && count < fileLimit) {
        it.next();
        const qint64 sz = it.fileInfo().size();
        // Saturate rather than overflow: file sizes are attacker-influenceable metadata
        // (e.g. a crafted sparse file reporting a near-INT64_MAX logical size), and signed
        // qint64 overflow is undefined behavior.
        if (sz > 0) {
            if (total > std::numeric_limits<qint64>::max() - sz) {
                return std::numeric_limits<qint64>::max();
            }
            total += sz;
        }
        ++count;
    }
    return total;
}

qint64 WindowsUserScanner::estimateProfileSize(const QString& profilePath) {
    // Public static, so profilePath arrives from another translation unit. An empty one
    // would build "/Documents", "/Desktop" and so on, which QDir resolves against the
    // current drive root -- the function would happily size an unrelated tree and report
    // it as a user's profile. 0 is already this function's answer for a profile with
    // nothing in it.
    if (profilePath.isEmpty()) {
        return 0;
    }

    qint64 totalSize = 0;

    // Quick estimate by scanning main folders (non-recursive)
    const QStringList mainFolders = {
        "Documents", "Desktop", "Pictures", "Videos", "Music", "Downloads"};

    for (const QString& folder : mainFolders) {
        const QString folderPath = profilePath + "/" + folder;
        if (!QDir(folderPath).exists()) {
            continue;
        }

        QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
        int fileCount = 0;
        while (it.hasNext() && fileCount < kProfileSizeEstimateFileLimit) {
            it.next();
            const qint64 sz = it.fileInfo().size();
            // Saturate rather than overflow (see sumFolderFileSizes): signed qint64
            // overflow on attacker-influenceable file sizes is undefined behavior.
            if (sz > 0) {
                if (totalSize > std::numeric_limits<qint64>::max() - sz) {
                    return std::numeric_limits<qint64>::max();
                }
                totalSize += sz;
            }
            fileCount++;
        }
    }

    return totalSize;
}

void WindowsUserScanner::populateFolderSelections(UserProfile& profile) {
    profile.folder_selections = getDefaultFolderSelections(profile.profile_path);
}

QVector<FolderSelection> WindowsUserScanner::getDefaultFolderSelections(
    const QString& profilePath) {
    QVector<FolderSelection> selections;

    auto createSelection = [&](FolderType type,
                               const QString& displayName,
                               const QString& relativePath,
                               bool selected) {
        FolderSelection sel;
        sel.type = type;
        sel.display_name = displayName;
        sel.relative_path = relativePath;
        sel.selected = selected;
        sel.include_patterns = QStringList{"*"};
        sel.exclude_patterns = QStringList();

        // Calculate size if folder exists
        sel.size_bytes = 0;
        sel.file_count = 0;
        const QString fullPath = profilePath + "/" + relativePath;
        if (QDir(fullPath).exists()) {
            // Size THIS folder directly. estimateProfileSize expects a profile
            // ROOT and scans its standard subfolders, so calling it on a single
            // folder (e.g. .../Documents) looked for .../Documents/Documents and
            // reported 0 for almost every selection.
            sel.size_bytes = WindowsUserScanner::sumFolderFileSizes(fullPath,
                                                                    kFolderSelectionFileLimit);
            // Quick file count
            QDirIterator it(fullPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && sel.file_count < kFolderSelectionFileLimit) {
                it.next();
                sel.file_count++;
            }
        }

        selections.append(sel);
    };

    // Standard folders (selected by default)
    createSelection(FolderType::Documents, "Documents", "Documents", true);
    createSelection(FolderType::Desktop, "Desktop", "Desktop", true);
    createSelection(FolderType::Pictures, "Pictures", "Pictures", true);
    createSelection(FolderType::Downloads, "Downloads", "Downloads", true);

    // Optional folders (not selected by default)
    createSelection(FolderType::Videos, "Videos", "Videos", false);
    createSelection(FolderType::Music, "Music", "Music", false);
    createSelection(FolderType::Favorites, "Favorites", "Favorites", false);

    // AppData (selective, not selected by default)
    createSelection(FolderType::AppData_Roaming, "AppData (Roaming)", "AppData\\Roaming", false);
    createSelection(FolderType::AppData_Local, "AppData (Local)", "AppData\\Local", false);

    return selections;
}

}  // namespace sak
