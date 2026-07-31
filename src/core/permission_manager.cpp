// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file permission_manager.cpp
/// @brief Implements file system permission checking and ACL management

#include "sak/permission_manager.h"

#include "sak/elevation_manager.h"
#include "sak/error_codes.h"
#include "sak/logger.h"

#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace sak {

#ifdef Q_OS_WIN
namespace {
// Builds a DACL granting full control to the destination user PLUS SYSTEM and
// the local Administrators group. Without SYSTEM/Administrators a "standard
// user" ACL locks out the OS, services (backup, AV, indexing), and every
// administrator -- which can render the file unmanageable. On success the caller
// owns *outAcl and must LocalFree it.
DWORD buildStandardUserDacl(PSID userSid, PACL* outAcl) {
    BYTE systemSid[SECURITY_MAX_SID_SIZE];
    DWORD systemSize = sizeof(systemSid);
    BYTE adminSid[SECURITY_MAX_SID_SIZE];
    DWORD adminSize = sizeof(adminSid);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, systemSid, &systemSize) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, adminSid, &adminSize)) {
        return GetLastError();
    }

    const PSID sids[3] = {userSid,
                          reinterpret_cast<PSID>(systemSid),
                          reinterpret_cast<PSID>(adminSid)};
    EXPLICIT_ACCESSW ea[3];
    ZeroMemory(ea, sizeof(ea));
    for (int i = 0; i < 3; ++i) {
        ea[i].grfAccessPermissions = GENERIC_ALL;
        ea[i].grfAccessMode = SET_ACCESS;
        ea[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        ea[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea[i].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sids[i]);
    }
    return SetEntriesInAclW(3, ea, nullptr, outAcl);
}

// Applies ONLY the fields the parsed security descriptor actually specifies
// (owner if present, DACL if present, protected vs unprotected from the control
// flag). Returns the SetNamedSecurityInfoW result, or ERROR_INVALID_SECURITY_DESCR
// when the descriptor specifies neither an owner nor a DACL.
DWORD applyParsedSecurityDescriptor(const QString& path, PSECURITY_DESCRIPTOR pSD) {
    SECURITY_INFORMATION si = 0;

    PSID pOwner = nullptr;
    BOOL ownerDefaulted = FALSE;
    if (GetSecurityDescriptorOwner(pSD, &pOwner, &ownerDefaulted) && pOwner != nullptr) {
        si |= OWNER_SECURITY_INFORMATION;
    }

    PACL pDacl = nullptr;
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    if (!GetSecurityDescriptorDacl(pSD, &daclPresent, &pDacl, &daclDefaulted)) {
        return ERROR_INVALID_SECURITY_DESCR;
    }
    if (daclPresent) {
        si |= DACL_SECURITY_INFORMATION;
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (GetSecurityDescriptorControl(pSD, &control, &revision)) {
            si |= (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                                : UNPROTECTED_DACL_SECURITY_INFORMATION;
        }
    }

    if (si == 0) {
        return ERROR_INVALID_SECURITY_DESCR;
    }

    return SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                 SE_FILE_OBJECT,
                                 si,
                                 (si & OWNER_SECURITY_INFORMATION) ? pOwner : nullptr,
                                 nullptr,
                                 (si & DACL_SECURITY_INFORMATION) ? pDacl : nullptr,
                                 nullptr);
}
}  // namespace
#endif

PermissionManager::PermissionManager() {
#ifdef Q_OS_WIN
    // Enable privileges lazily — only succeeds when running elevated.
    // Non-elevated callers will get clear errors from methods that need them.
    if (ElevationManager::isElevated()) {
        enablePrivilege(SE_BACKUP_NAME);
        enablePrivilege(SE_RESTORE_NAME);
        enablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    }
#endif
}

PermissionManager::~PermissionManager() {}

bool PermissionManager::stripPermissions(const QString& path) {
    Q_ASSERT(!path.isEmpty());
#ifdef Q_OS_WIN
    QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        m_lastError = "File does not exist";
        return false;
    }

    // Remove all EXPLICIT ACEs and let the object re-inherit its parent's ACL.
    // CRITICAL: pass an initialized EMPTY (non-NULL) DACL, NOT nullptr. A nullptr
    // pDacl with DACL_SECURITY_INFORMATION sets a NULL DACL, which grants
    // *everyone* full access -- the opposite of stripping. An empty DACL grants
    // no explicit access; UNPROTECTED_DACL_SECURITY_INFORMATION then re-applies
    // the parent's inheritable ACEs.
    ACL emptyDacl;
    if (!InitializeAcl(&emptyDacl, sizeof(ACL), ACL_REVISION)) {
        m_lastError = QString("Failed to initialize empty DACL: %1").arg(GetLastError());
        return false;
    }

    DWORD result =
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                              SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr,
                              nullptr,
                              &emptyDacl,
                              nullptr);

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
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!userSID.isEmpty());
#ifdef Q_OS_WIN
    if (!isRunningAsAdmin()) {
        m_lastError = "Administrator privileges required to take ownership";
        return false;
    }

    // Convert SID string to PSID
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(const_cast<LPWSTR>(userSID.toStdWString().c_str()), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return false;
    }

    // Set owner
    DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                         SE_FILE_OBJECT,
                                         OWNER_SECURITY_INFORMATION,
                                         pSid,
                                         nullptr,
                                         nullptr,
                                         nullptr);

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
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!userSID.isEmpty());
#ifdef Q_OS_WIN
    // Convert SID
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(const_cast<LPWSTR>(userSID.toStdWString().c_str()), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return false;
    }

    // Grant the user full control, but KEEP SYSTEM + Administrators so the file
    // stays manageable by the OS and admins.
    PACL pNewAcl = nullptr;
    DWORD result = buildStandardUserDacl(pSid, &pNewAcl);

    if (result != ERROR_SUCCESS) {
        LocalFree(pSid);
        m_lastError = QString("Failed to create ACL: %1").arg(result);
        return false;
    }

    // Apply ACL
    result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr,
                                   nullptr,
                                   pNewAcl,
                                   nullptr);

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
        // cppcheck-suppress knownConditionTrueFalse ; Q_OS_WIN guards make this reachable
        return stripPermissions(path);

    case PermissionMode::AssignToDestination:
        if (destinationUserSID.isEmpty()) {
            m_lastError = "Destination user SID required";
            return false;
        }
        // cppcheck-suppress knownConditionTrueFalse ; Q_OS_WIN guards make this reachable
        return takeOwnership(path, destinationUserSID) &&
               setStandardUserPermissions(path, destinationUserSID);

    case PermissionMode::PreserveOriginal:
        // Don't modify permissions
        return true;

    case PermissionMode::Hybrid:
        // Strip, then assign to destination user
        // cppcheck-suppress knownConditionTrueFalse ; Q_OS_WIN guards make this reachable
        return stripPermissions(path) && setStandardUserPermissions(path, destinationUserSID);

    default:
        sak::logWarning("Unknown PermissionMode value: {}", static_cast<int>(mode));
        return false;
    }
}

QString PermissionManager::getOwner(const QString& path) {
    Q_ASSERT(!path.isEmpty());
#ifdef Q_OS_WIN
    PSID pOwnerSid = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;

    DWORD result = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                         SE_FILE_OBJECT,
                                         OWNER_SECURITY_INFORMATION,
                                         &pOwnerSid,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         &pSD);

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

QString PermissionManager::getSecurityDescriptorSddl(const QString& path) {
    Q_ASSERT(!path.isEmpty());
#ifdef Q_OS_WIN
    PSECURITY_DESCRIPTOR pSD = nullptr;
    DWORD result = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                         SE_FILE_OBJECT,
                                         OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
                                             DACL_SECURITY_INFORMATION,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         nullptr,
                                         &pSD);

    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to get security descriptor: %1").arg(result);
        return QString();
    }

    LPWSTR sddlString = nullptr;
    if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(
            pSD,
            SDDL_REVISION_1,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &sddlString,
            nullptr)) {
        m_lastError = QString("Failed to convert security descriptor: %1").arg(GetLastError());
        LocalFree(pSD);
        return QString();
    }

    QString sddl = QString::fromWCharArray(sddlString);
    LocalFree(sddlString);
    LocalFree(pSD);
    return sddl;
#else
    m_lastError = "Permission management only supported on Windows";
    return QString();
#endif
}

bool PermissionManager::setSecurityDescriptorSddl(const QString& path, const QString& sddl) {
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!sddl.isEmpty());
#ifdef Q_OS_WIN
    if (sddl.isEmpty()) {
        m_lastError = "Empty SDDL";
        return false;
    }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            const_cast<LPWSTR>(sddl.toStdWString().c_str()), SDDL_REVISION_1, &pSD, nullptr)) {
        m_lastError = QString("Failed to parse SDDL: %1").arg(GetLastError());
        return false;
    }

    // Apply ONLY the fields the SDDL actually specifies (see
    // applyParsedSecurityDescriptor). Forcing OWNER | DACL | PROTECTED would null
    // the owner / set a NULL DACL when the SDDL omitted them.
    const DWORD result = applyParsedSecurityDescriptor(path, pSD);
    LocalFree(pSD);

    if (result == ERROR_INVALID_SECURITY_DESCR) {
        m_lastError = "SDDL specifies neither an owner nor a valid DACL";
        return false;
    }
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to apply SDDL: %1").arg(result);
        return false;
    }

    return true;
#else
    m_lastError = "Permission management only supported on Windows";
    return false;
#endif
}

// ============================================================================
// Elevation-Aware Overloads
// ============================================================================

auto PermissionManager::tryStripPermissions(const QString& path)
    -> std::expected<void, sak::error_code> {
    Q_ASSERT(!path.isEmpty());
#ifdef Q_OS_WIN
    QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        m_lastError = "File does not exist";
        return std::unexpected(sak::error_code::file_not_found);
    }

    // See stripPermissions(): an EMPTY (non-NULL) DACL strips explicit access and
    // re-inherits from the parent; a nullptr pDacl would set a NULL DACL that
    // grants everyone full access.
    ACL emptyDacl;
    if (!InitializeAcl(&emptyDacl, sizeof(ACL), ACL_REVISION)) {
        m_lastError = QString("Failed to initialize empty DACL: %1").arg(GetLastError());
        return std::unexpected(sak::error_code::permission_update_failed);
    }

    DWORD result =
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                              SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr,
                              nullptr,
                              &emptyDacl,
                              nullptr);

    if (result == ERROR_ACCESS_DENIED) {
        m_lastError = "Access denied — administrator privileges required";
        return std::unexpected(sak::error_code::elevation_required);
    }
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to strip permissions: %1").arg(result);
        return std::unexpected(sak::error_code::permission_update_failed);
    }
    return {};
#else
    m_lastError = "Permission management only supported on Windows";
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

auto PermissionManager::tryTakeOwnership(const QString& path, const QString& userSID)
    -> std::expected<void, sak::error_code> {
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!userSID.isEmpty());
#ifdef Q_OS_WIN
    if (!isRunningAsAdmin()) {
        m_lastError = "Administrator privileges required to take ownership";
        return std::unexpected(sak::error_code::elevation_required);
    }

    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(const_cast<LPWSTR>(userSID.toStdWString().c_str()), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return std::unexpected(sak::error_code::invalid_argument);
    }

    DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                         SE_FILE_OBJECT,
                                         OWNER_SECURITY_INFORMATION,
                                         pSid,
                                         nullptr,
                                         nullptr,
                                         nullptr);

    LocalFree(pSid);

    if (result == ERROR_ACCESS_DENIED) {
        m_lastError = "Access denied — administrator privileges required";
        return std::unexpected(sak::error_code::elevation_required);
    }
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to take ownership: %1").arg(result);
        return std::unexpected(sak::error_code::permission_update_failed);
    }
    return {};
#else
    m_lastError = "Permission management only supported on Windows";
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

auto PermissionManager::trySetStandardUserPermissions(const QString& path, const QString& userSID)
    -> std::expected<void, sak::error_code> {
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!userSID.isEmpty());
#ifdef Q_OS_WIN
    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(const_cast<LPWSTR>(userSID.toStdWString().c_str()), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return std::unexpected(sak::error_code::invalid_argument);
    }

    // Keep SYSTEM + Administrators alongside the user (see buildStandardUserDacl).
    PACL pNewAcl = nullptr;
    DWORD result = buildStandardUserDacl(pSid, &pNewAcl);

    if (result != ERROR_SUCCESS) {
        LocalFree(pSid);
        m_lastError = QString("Failed to create ACL: %1").arg(result);
        return std::unexpected(sak::error_code::permission_update_failed);
    }

    result = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                                   SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   nullptr,
                                   nullptr,
                                   pNewAcl,
                                   nullptr);

    LocalFree(pNewAcl);
    LocalFree(pSid);

    if (result == ERROR_ACCESS_DENIED) {
        m_lastError = "Access denied — administrator privileges required";
        return std::unexpected(sak::error_code::elevation_required);
    }
    if (result != ERROR_SUCCESS) {
        m_lastError = QString("Failed to set permissions: %1").arg(result);
        return std::unexpected(sak::error_code::permission_update_failed);
    }
    return {};
#else
    m_lastError = "Permission management only supported on Windows";
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

bool PermissionManager::isRunningAsAdmin() {
    return ElevationManager::isElevated();
}

#ifdef Q_OS_WIN
bool PermissionManager::enablePrivilege(const wchar_t* privilegeName) {
    Q_ASSERT(privilegeName);
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

#endif

}  // namespace sak
