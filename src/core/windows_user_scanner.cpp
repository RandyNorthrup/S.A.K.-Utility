#include "sak/windows_user_scanner.h"
#include <QDir>
#include <QDirIterator>
#include <QStandardPaths>
#include <QProcess>

#ifdef Q_OS_WIN
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

WindowsUserScanner::WindowsUserScanner(QObject* parent)
    : QObject(parent)
{
}

QVector<UserProfile> WindowsUserScanner::scanUsers() {
    QVector<UserProfile> profiles;
    
#ifdef Q_OS_WIN
    enumerateWindowsUsers(profiles);
#endif
    
    return profiles;
}

#ifdef Q_OS_WIN
bool WindowsUserScanner::enumerateWindowsUsers(QVector<UserProfile>& profiles) {
    LPUSER_INFO_3 userInfo = nullptr;
    DWORD entriesRead = 0;
    DWORD totalEntries = 0;
    DWORD resumeHandle = 0;
    
    NET_API_STATUS status = NetUserEnum(
        nullptr,                    // local computer
        3,                          // level 3 (detailed info)
        FILTER_NORMAL_ACCOUNT,      // normal user accounts only
        reinterpret_cast<LPBYTE*>(&userInfo),
        MAX_PREFERRED_LENGTH,
        &entriesRead,
        &totalEntries,
        &resumeHandle
    );
    
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
        
        // Check if current user
        profile.is_current_user = (profile.username.toLower() == currentUser.toLower());
        
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
    
    if (sidSize > 0) {
        sid = (PSID)LocalAlloc(LPTR, sidSize);
        domainSize = 256;
        
        if (LookupAccountNameW(nullptr, usernameW, sid, &sidSize, domain, &domainSize, &sidType)) {
            LPWSTR sidString = nullptr;
            if (ConvertSidToStringSidW(sid, &sidString)) {
                QString result = QString::fromWCharArray(sidString);
                LocalFree(sidString);
                LocalFree(sid);
                return result;
            }
        }
        
        LocalFree(sid);
    }
#endif
    return QString();
}

QString WindowsUserScanner::getProfilePath(const QString& username) {
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
    // Try registry lookup
    QString sid = getUserSID(username);
    if (!sid.isEmpty()) {
        // Query registry: HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList\{SID}
        // For now, fallback to standard path
    }
#endif
    
    return QString();
}

bool WindowsUserScanner::isUserLoggedIn(const QString& username) {
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
        
        if (WTSQuerySessionInformationW(
                WTS_CURRENT_SERVER_HANDLE,
                pSessionInfo[i].SessionId,
                WTSUserName,
                &pUserName,
                &bytesReturned)) {
            
            QString sessionUser = QString::fromWCharArray(pUserName);
            WTSFreeMemory(pUserName);
            
            // Compare usernames (case-insensitive)
            if (sessionUser.compare(username, Qt::CaseInsensitive) == 0) {
                isLoggedIn = true;
                break;
            }
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
    qint64 totalSize = 0;
    
    // Quick estimate by scanning main folders (non-recursive)
    QStringList mainFolders = {"Documents", "Desktop", "Pictures", "Videos", "Music", "Downloads"};
    
    for (const QString& folder : mainFolders) {
        QString folderPath = profilePath + "/" + folder;
        QDir dir(folderPath);
        if (dir.exists()) {
            QDirIterator it(folderPath, QDir::Files, QDirIterator::Subdirectories);
            int fileCount = 0;
            while (it.hasNext() && fileCount < 1000) { // Limit for speed
                it.next();
                totalSize += it.fileInfo().size();
                fileCount++;
            }
        }
    }
    
    return totalSize;
}

void WindowsUserScanner::populateFolderSelections(UserProfile& profile) {
    profile.folder_selections = getDefaultFolderSelections(profile.profile_path);
}

QVector<FolderSelection> WindowsUserScanner::getDefaultFolderSelections(const QString& profilePath) {
    QVector<FolderSelection> selections;
    
    auto createSelection = [&](FolderType type, const QString& displayName, 
                               const QString& relativePath, bool selected) {
        FolderSelection sel;
        sel.type = type;
        sel.display_name = displayName;
        sel.relative_path = relativePath;
        sel.selected = selected;
        sel.include_patterns = QStringList{"*"};
        sel.exclude_patterns = QStringList();
        
        // Calculate size if folder exists
        QString fullPath = profilePath + "/" + relativePath;
        QDir dir(fullPath);
        if (dir.exists()) {
            sel.size_bytes = WindowsUserScanner::estimateProfileSize(fullPath);
            // Quick file count
            sel.file_count = 0;
            QDirIterator it(fullPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext() && sel.file_count < 10000) {
                it.next();
                sel.file_count++;
            }
        } else {
            sel.size_bytes = 0;
            sel.file_count = 0;
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
    if (maxDepth <= 0) return 0;
    
    qint64 size = 0;
    QDir dir(path);
    
    if (!dir.exists()) return 0;
    
    // Get files in current directory
    QFileInfoList files = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& file : files) {
        size += file.size();
    }
    
    // Recurse into subdirectories (limited depth)
    if (maxDepth > 1) {
        QFileInfoList dirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& subdir : dirs) {
            size += quickSizeEstimate(subdir.absoluteFilePath(), maxDepth - 1);
        }
    }
    
    return size;
}

} // namespace sak
