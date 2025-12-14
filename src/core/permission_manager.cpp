#include "sak/permission_manager.h"
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace sak {

PermissionManager::PermissionManager() {
#ifdef Q_OS_WIN
    // Try to enable SE_BACKUP_NAME and SE_RESTORE_NAME privileges
    enablePrivilege(SE_BACKUP_NAME);
    enablePrivilege(SE_RESTORE_NAME);
    enablePrivilege(SE_TAKE_OWNERSHIP_NAME);
#endif
}

PermissionManager::~PermissionManager() {
}

bool PermissionManager::stripPermissions(const QString& path) {
#ifdef Q_OS_WIN
    QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        m_lastError = "File does not exist";
        return false;
    }
    
    // Get parent directory security to inherit from
    QString parentPath = fileInfo.absolutePath();
    
    // Remove all explicit ACEs, keep only inherited
    // Note: We'll use SetNamedSecurityInfo with NULL DACL to enable inheritance
    
    // Set the DACL
    DWORD result = SetNamedSecurityInfoW(
        (LPWSTR)path.toStdWString().c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,  // Empty DACL = inherit
        nullptr
    );
    
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to strip permissions: %1").arg(result);
        return false;
    }
    
    return true;
#else
    m_lastError = "Permission management only supported on Windows";
    return false;
#endif
}

bool PermissionManager::takeOwnership(const QString& path, const QString& userSID) {
#ifdef Q_OS_WIN
    if (!isRunningAsAdmin()) {
        m_lastError = "Administrator privileges required to take ownership";
        return false;
    }
    
    // Convert SID string to PSID
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW((LPWSTR)userSID.toStdWString().c_str(), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return false;
    }
    
    // Set owner
    DWORD result = SetNamedSecurityInfoW(
        (LPWSTR)path.toStdWString().c_str(),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        pSid,
        nullptr,
        nullptr,
        nullptr
    );
    
    LocalFree(pSid);
    
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to take ownership: %1").arg(result);
        return false;
    }
    
    return true;
#else
    m_lastError = "Permission management only supported on Windows";
    return false;
#endif
}

bool PermissionManager::setStandardUserPermissions(const QString& path, const QString& userSID) {
#ifdef Q_OS_WIN
    // Convert SID
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW((LPWSTR)userSID.toStdWString().c_str(), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return false;
    }
    
    // Create ACL with full control for user
    EXPLICIT_ACCESSW ea;
    ZeroMemory(&ea, sizeof(EXPLICIT_ACCESSW));
    
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = (LPWSTR)pSid;
    
    PACL pNewAcl = nullptr;
    DWORD result = SetEntriesInAclW(1, &ea, nullptr, &pNewAcl);
    
    if (result != ERROR_SUCCESS) {
        LocalFree(pSid);
        m_lastError = QString("Failed to create ACL: %1").arg(result);
        return false;
    }
    
    // Apply ACL
    result = SetNamedSecurityInfoW(
        (LPWSTR)path.toStdWString().c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        pNewAcl,
        nullptr
    );
    
    LocalFree(pNewAcl);
    LocalFree(pSid);
    
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to set permissions: %1").arg(result);
        return false;
    }
    
    return true;
#else
    m_lastError = "Permission management only supported on Windows";
    return false;
#endif
}

bool PermissionManager::canModifyPermissions(const QString& path) {
    QFileInfo fileInfo(path);
    return fileInfo.exists() && fileInfo.isWritable();
}

bool PermissionManager::applyPermissionStrategy(const QString& path, 
                                                PermissionMode mode,
                                                const QString& destinationUserSID) {
    switch (mode) {
        case PermissionMode::StripAll:
            return stripPermissions(path);
            
        case PermissionMode::AssignToDestination:
            if (destinationUserSID.isEmpty()) {
                m_lastError = "Destination user SID required";
                return false;
            }
            return takeOwnership(path, destinationUserSID) &&
                   setStandardUserPermissions(path, destinationUserSID);
            
        case PermissionMode::PreserveOriginal:
            // Don't modify permissions
            return true;
            
        case PermissionMode::Hybrid:
            // Strip, then assign to destination user
            return stripPermissions(path) &&
                   setStandardUserPermissions(path, destinationUserSID);
    }
    
    return false;
}

QString PermissionManager::getOwner(const QString& path) {
#ifdef Q_OS_WIN
    PSID pOwnerSid = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;
    
    DWORD result = GetNamedSecurityInfoW(
        (LPWSTR)path.toStdWString().c_str(),
        SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION,
        &pOwnerSid,
        nullptr,
        nullptr,
        nullptr,
        &pSD
    );
    
    if (result != ERROR_SUCCESS) {
        return QString();
    }
    
    LPWSTR sidString = nullptr;
    QString ownerSid;
    if (ConvertSidToStringSidW(pOwnerSid, &sidString)) {
        ownerSid = QString::fromWCharArray(sidString);
        LocalFree(sidString);
    }
    
    LocalFree(pSD);
    return ownerSid;
#else
    return QString();
#endif
}

bool PermissionManager::isRunningAsAdmin() {
#ifdef Q_OS_WIN
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    
    if (AllocateAndInitializeSid(&ntAuthority, 2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    
    return isAdmin == TRUE;
#else
    return false;
#endif
}

#ifdef Q_OS_WIN
bool PermissionManager::enablePrivilege(const wchar_t* privilegeName) {
    HANDLE hToken = nullptr;
    
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        CloseHandle(hToken);
        return false;
    }
    
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    
    bool result = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr) != 0;
    
    CloseHandle(hToken);
    return result;
}

bool PermissionManager::getSecurityInfo(const QString& path, PSECURITY_DESCRIPTOR* pSD) {
    DWORD result = GetNamedSecurityInfoW(
        (LPWSTR)path.toStdWString().c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        pSD
    );
    
    return result == ERROR_SUCCESS;
}
#endif

} // namespace sak
