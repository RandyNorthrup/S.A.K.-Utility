// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QDirIterator>
#include <QProcess>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include "sak/logger.h"

#include <windows.h>

#include <lm.h>
#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")
#endif

namespace sak {

WindowsUserScanner::WindowsUserScanner(QObject* parent) : QObject(parent) {}

QVector<UserProfile> WindowsUserScanner::scanUsers() {
    QVector<UserProfile> profiles;

#ifdef Q_OS_WIN
    enumerateWindowsUsers(profiles);
#endif

    return profiles;
}

#ifdef Q_OS_WIN
bool WindowsUserScanner::enumerateWindowsUsers(QVector<UserProfile>& profiles) {
    Q_ASSERT(!profiles.isEmpty());
    LPUSER_INFO_3 userInfo = nullptr;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;

    NET_API_STATUS status = NetUserEnum(nullptr,                // local computer
                                        3,                      // level 3 (detailed info)
                                        FILTER_NORMAL_ACCOUNT,  // normal user accounts only
                                        reinterpret_cast<LPBYTE*>(&userInfo),
                                        MAX_PREFERRED_LENGTH,
                                        &entriesRead,
                                        &totalEntries,
                                        &resumeHandle);

    if (status != NERR_Success && status != ERROR_MORE_DATA) {
        return false;
    }

    QString currentUser = getCurrentUsername();

    for (DWORD i = 0; i < entriesRead; ++i) {
        UserProfile profile;
        profile.username = QString::fromWCharArray(userInfo[i].usri3_name);

        // Get profile path
        profile.profile_path = getProfilePath(profile.username);

        // Skip if profile doesn't exist
        if (profile.profile_path.isEmpty() || !QDir(profile.profile_path).exists()) {
            continue;
        }

        // Get SID
        profile.sid = getUserSID(profile.username);

        // Check if current user - cache toLower() to avoid redundant calls
        const QString currentUserLower = currentUser.toLower();
        const QString usernameLower = profile.username.toLower();
        profile.is_current_user = (usernameLower == currentUserLower);

        // Quick size estimate
        profile.total_size_estimated = estimateProfileSize(profile.profile_path);

        // Populate default folder selections
        populateFolderSelections(profile);

        profiles.append(profile);

        Q_EMIT userFound(profile.username);
        Q_EMIT scanProgress(profiles.size(), totalEntries);
    }

    NetApiBufferFree(userInfo);
    return true;
}
#endif

QString WindowsUserScanner::getCurrentUsername() {
#ifdef Q_OS_WIN
    wchar_t username[256];
    DWORD size = 256;
    if (GetUserNameW(username, &size)) {
        return QString::fromWCharArray(username);
    }
#endif
    return QString();
}

QString WindowsUserScanner::getUserSID(const QString& username) {
    Q_ASSERT(!username.isEmpty());
#ifdef Q_OS_WIN
    // Try to lookup SID using LookupAccountName
    wchar_t* usernameW = (wchar_t*)username.utf16();
    PSID sid = nullptr;
    DWORD sidSize = 0;
    wchar_t domain[256];
    DWORD domainSize = 256;
    SID_NAME_USE sidType;

    // First call to get required buffer size
    LookupAccountNameW(nullptr, usernameW, nullptr, &sidSize, domain, &domainSize, &sidType);

    if (sidSize == 0) {
        return QString();
    }

    sid = (PSID)LocalAlloc(LPTR, sidSize);
    if (!sid) {
        return QString();
    }
    domainSize = 256;

    if (!LookupAccountNameW(nullptr, usernameW, sid, &sidSize, domain, &domainSize, &sidType)) {
        LocalFree(sid);
        return QString();
    }

    LPWSTR sidString = nullptr;
    if (!ConvertSidToStringSidW(sid, &sidString)) {
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
    Q_ASSERT(profileDir);
    if (valueType != REG_EXPAND_SZ) {
        return QString::fromWCharArray(profileDir);
    }
    wchar_t expandedPath[MAX_PATH] = {};
    DWORD expandedLen = ExpandEnvironmentStringsW(profileDir, expandedPath, MAX_PATH);
    if (expandedLen > 0 && expandedLen <= MAX_PATH) {
        return QString::fromWCharArray(expandedPath);
    }
    return QString::fromWCharArray(profileDir);
}

/// @brief Look up a user's profile path from the registry using their SID
QString lookupRegistryProfilePath(const QString& sid) {
    Q_ASSERT(!sid.isEmpty());
    QString regPath =
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

    QString registryPath = expandRegistryPath(profileDir, valueType);
    if (QDir(registryPath).exists()) {
        return registryPath;
    }
    return {};
}

}  // anonymous namespace

QString WindowsUserScanner::getProfilePath(const QString& username) {
    Q_ASSERT(!username.isEmpty());
    // First try standard location using SystemDrive environment variable
    QString systemDrive = QString::fromLocal8Bit(qgetenv("SystemDrive"));
    if (systemDrive.isEmpty()) {
        systemDrive = "C:";
    }
    QString standardPath = systemDrive + "\\Users\\" + username;
    if (QDir(standardPath).exists()) {
        return standardPath;
    }

#ifdef Q_OS_WIN
    // Query registry for actual profile path using the user's SID
    QString sid = getUserSID(username);
    if (sid.isEmpty()) {
        return {};
    }
    return lookupRegistryProfilePath(sid);
#else
    return {};
#endif
}

bool WindowsUserScanner::isUserLoggedIn(const QString& username) {
    Q_ASSERT(!username.isEmpty());
#ifdef Q_OS_WIN
    // Enumerate active sessions using WTS API
    PWTS_SESSION_INFOW pSessionInfo = nullptr;
    DWORD sessionCount = 0;

    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &sessionCount)) {
        // Failed to enumerate sessions
        return false;
    }

    bool isLoggedIn = false;

    for (DWORD i = 0; i < sessionCount; i++) {
        // Skip disconnected and idle sessions
        if (pSessionInfo[i].State != WTSActive) {
            continue;
        }

        // Get username for this session
        LPWSTR pUserName = nullptr;
        DWORD bytesReturned = 0;

        if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,
                                         pSessionInfo[i].SessionId,
                                         WTSUserName,
                                         &pUserName,
                                         &bytesReturned)) {
            continue;
        }

        QString sessionUser = QString::fromWCharArray(pUserName);
        WTSFreeMemory(pUserName);

        // Compare usernames (case-insensitive)
        if (sessionUser.compare(username, Qt::CaseInsensitive) == 0) {
            isLoggedIn = true;
            break;
        }
    }

    WTSFreeMemory(pSessionInfo);
    return isLoggedIn;
#else
    (void)username;
    return false;
#endif
}

qint64 WindowsUserScanner::estimateProfileSize(const QString& profilePath) {
    Q_ASSERT(!profilePath.isEmpty());
    qint64 totalSize = 0;

    // Quick estimate by scanning main folders (non-recursive)
    QStringList mainFolders = {"Documents", "Desktop", "Pictures", "Videos", "Music", "Downloads"};

    for (const QString& folder : mainFolders) {
        QString folderPath = profilePath + "/" + folder;
        if (!QDir(folderPath).exists()) {
            continue;
        }

        QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
        int fileCount = 0;
        while (it.hasNext() && fileCount < 1000) {  // Limit for speed
            it.next();
            totalSize += it.fileInfo().size();
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
        QString fullPath = profilePath + "/" + relativePath;
        if (QDir(fullPath).exists()) {
            sel.size_bytes = WindowsUserScanner::estimateProfileSize(fullPath);
            // Quick file count
            QDirIterator it(fullPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && sel.file_count < 10'000) {
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

qint64 WindowsUserScanner::quickSizeEstimate(const QString& path, int maxDepth) {
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(maxDepth >= 0);
    if (maxDepth <= 0) {
        return 0;
    }

    qint64 size_bytes = 0;
    QDir dir(path);

    if (!dir.exists()) {
        return 0;
    }

    // Get files in current directory
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& file : files) {
        size_bytes += file.size();
    }

    // Recurse into subdirectories (limited depth)
    if (maxDepth > 1) {
        QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& subdir : dirs) {
            size_bytes += quickSizeEstimate(subdir.absoluteFilePath(), maxDepth - 1);
        }
    }

    return size_bytes;
}

}  // namespace sak
