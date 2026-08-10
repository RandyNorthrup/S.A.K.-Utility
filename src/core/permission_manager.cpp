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

// Local sentinel (NOT a Win32 error; carries the customer bit 0x20000000) meaning
// the target was refused because it is a reparse point.
constexpr DWORD kReparseRefused = 0x20'00'00'01u;
// Local sentinel meaning the opened handle's fully-resolved path did NOT match the
// caller's intended path -- i.e. a junction/symlink in an ANCESTOR directory
// redirected the open onto another tree (the no-follow flag only guards the final
// component). Refused fail-closed so an elevated change cannot be misdirected.
constexpr DWORD kPathRedirected = 0x20'00'00'02u;
// Local sentinel: the target has more than one hard link, so an ACL/owner change on
// this name would silently mutate the security of the SHARED underlying file that
// another link also names. Refused fail-closed for the same reason as a reparse point
// -- an elevated change must not be redirected onto an unintended object.
constexpr DWORD kHardLinkRefused = 0x20'00'00'03u;
// Local sentinel: the parsed security descriptor carries a SACL (audit / mandatory
// integrity label). This code applies only owner/group/DACL, so a SACL is refused
// rather than silently dropped while the rest of the descriptor is applied.
constexpr DWORD kSaclUnsupported = 0x20'00'00'04u;

// A QString carrying an embedded NUL truncates to its prefix the moment it is handed
// to a NUL-terminated Win32 string, so a valid prefix would be honored while the rest
// is silently discarded. Refuse fail-closed anywhere untrusted path/SID/SDDL text is
// consumed.
bool hasEmbeddedNul(const QString& s) {
    return s.contains(QChar(u'\0'));
}

// The set of security fields to apply. Bundled into one struct so
// applySecurityNoFollow stays within the parameter budget while still carrying
// owner, group, AND dacl (group was previously dropped on SDDL restore).
struct SecurityChange {
    SECURITY_INFORMATION si;
    PSID owner;
    PSID group;
    PACL dacl;
};

// Fully-resolved DOS path of an open handle (all reparse points collapsed, 8.3
// short names and casing canonicalized), with the "\\?\" prefix stripped. Empty
// on any failure so the caller fails closed.
QString resolvedHandlePath(HANDLE h) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const DWORD need = GetFinalPathNameByHandleW(h, nullptr, 0, flags);
    if (need == 0) {
        return QString();
    }
    std::wstring buf(need, L'\0');
    const DWORD got = GetFinalPathNameByHandleW(h, buf.data(), need, flags);
    if (got == 0 || got >= need) {
        return QString();
    }
    buf.resize(got);
    QString path = QString::fromStdWString(buf);
    if (path.startsWith(QLatin1String("\\\\?\\"))) {
        path = path.mid(4);
    }
    return path;
}

// The caller's intended path made absolute and long-name/casing canonicalized
// (GetLongPathNameW does NOT collapse junctions, so this differs from
// resolvedHandlePath exactly when an ancestor was redirected). Empty on failure.
QString canonicalRequestedPath(const QString& requested) {
    const std::wstring w = requested.toStdWString();
    const DWORD need = GetFullPathNameW(w.c_str(), 0, nullptr, nullptr);
    if (need == 0) {
        return QString();
    }
    std::wstring full(need, L'\0');
    const DWORD got = GetFullPathNameW(w.c_str(), need, full.data(), nullptr);
    if (got == 0 || got >= need) {
        return QString();
    }
    full.resize(got);
    const DWORD longNeed = GetLongPathNameW(full.c_str(), nullptr, 0);
    if (longNeed == 0) {
        return QString();
    }
    std::wstring longBuf(longNeed, L'\0');
    const DWORD longGot = GetLongPathNameW(full.c_str(), longBuf.data(), longNeed);
    if (longGot == 0 || longGot >= longNeed) {
        return QString();
    }
    longBuf.resize(longGot);
    return QString::fromStdWString(longBuf);
}

// True when the no-follow handle really resolves to the path the caller named.
// A mismatch means an ancestor junction/symlink redirected the open elsewhere.
bool handleMatchesRequestedPath(HANDLE h, const QString& path) {
    const QString resolved = resolvedHandlePath(h);
    const QString wanted = canonicalRequestedPath(path);
    return !resolved.isEmpty() && !wanted.isEmpty() &&
           resolved.compare(wanted, Qt::CaseInsensitive) == 0;
}

// Applies the requested owner/group/dacl to PATH through a no-follow handle and
// refuses reparse points. Returns ERROR_SUCCESS, a Win32 error, kReparseRefused,
// or kPathRedirected. Opening with FILE_FLAG_OPEN_REPARSE_POINT and operating on
// THIS handle closes the final-component junction/symlink swap TOCTOU that the
// by-name SetNamedSecurityInfoW would expose; verifying the handle's resolved
// path additionally closes an ANCESTOR-directory junction swap. FAIL CLOSED.
DWORD applySecurityNoFollow(const QString& path, DWORD access, const SecurityChange& change) {
    if (hasEmbeddedNul(path)) {
        return ERROR_INVALID_NAME;
    }
    if (!QFileInfo(path).isAbsolute()) {
        // A relative or drive-relative path resolves through the mutable process current
        // directory; an elevated change must never target a CWD-dependent path.
        return ERROR_INVALID_NAME;
    }
    HANDLE h = CreateFileW(const_cast<LPWSTR>(path.toStdWString().c_str()),
                           access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info)) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        return e;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        CloseHandle(h);
        return kReparseRefused;
    }
    if (info.nNumberOfLinks > 1) {
        // A hard link makes this name and another one the SAME underlying file; an ACL/
        // owner change here would also re-permission whatever else the file is linked as.
        CloseHandle(h);
        return kHardLinkRefused;
    }
    if (!handleMatchesRequestedPath(h, path)) {
        CloseHandle(h);
        return kPathRedirected;
    }
    // SE_FILE_OBJECT (not SE_KERNEL_OBJECT): a file/directory handle needs the file
    // object type so UNPROTECTED_DACL_SECURITY_INFORMATION re-inherits from the parent
    // directory (SE_KERNEL_OBJECT has no parent-inheritance concept and would leave the
    // stripped DACL protected).
    const DWORD result = SetSecurityInfo(
        h, SE_FILE_OBJECT, change.si, change.owner, change.group, change.dacl, nullptr);
    CloseHandle(h);
    return result;
}

// Maps an applySecurityNoFollow result to the elevation-aware error contract,
// setting lastError. FAIL CLOSED: a refused reparse point and access-denied both
// surface as errors; only ERROR_SUCCESS returns a value.
std::expected<void, sak::error_code> interpretSecurityResult(DWORD result,
                                                             const char* opLabel,
                                                             QString& lastError) {
    if (result == ERROR_SUCCESS) {
        return {};
    }
    if (result == kReparseRefused) {
        lastError = "Refused: target is a reparse point (junction/symlink)";
        return std::unexpected(sak::error_code::permission_update_failed);
    }
    if (result == kPathRedirected) {
        lastError = "Refused: path resolves through a redirected junction/symlink ancestor";
        return std::unexpected(sak::error_code::permission_update_failed);
    }
    if (result == kHardLinkRefused) {
        lastError = "Refused: target has multiple hard links (shared underlying file)";
        return std::unexpected(sak::error_code::permission_update_failed);
    }
    if (result == ERROR_ACCESS_DENIED) {
        lastError = "Access denied - administrator privileges required";
        return std::unexpected(sak::error_code::elevation_required);
    }
    lastError = QString("%1: %2").arg(opLabel).arg(result);
    return std::unexpected(sak::error_code::permission_update_failed);
}

// Access-rights mask a SetSecurityInfo call needs for the given SECURITY_INFORMATION.
DWORD accessForSecurityInfo(SECURITY_INFORMATION si) {
    DWORD access = READ_CONTROL;
    if (si & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION)) {
        access |= WRITE_OWNER;  // primary group is set through WRITE_OWNER too
    }
    if (si & DACL_SECURITY_INFORMATION) {
        access |= WRITE_DAC;
    }
    return access;
}

// Adds the DACL fields (present flag + protected/unprotected) the descriptor
// specifies to @p change and @p si. Split out to keep applyParsedSecurityDescriptor
// within complexity limits. FAIL CLOSED: returns false on a genuine accessor error,
// or on an explicit present-but-NULL DACL (which would grant EVERYONE full control) --
// either must abort the apply rather than proceed with a guessed/absent DACL.
bool collectParsedDacl(PSECURITY_DESCRIPTOR pSD, SECURITY_INFORMATION& si, SecurityChange& change) {
    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    if (!GetSecurityDescriptorDacl(pSD, &daclPresent, &change.dacl, &daclDefaulted)) {
        return false;
    }
    if (!daclPresent) {
        // No DACL specified: leave the object's DACL untouched (do not set the bit).
        change.dacl = nullptr;
        return true;
    }
    if (change.dacl == nullptr) {
        // A present NULL DACL grants everyone full control; never apply one.
        return false;
    }
    si |= DACL_SECURITY_INFORMATION;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(pSD, &control, &revision)) {
        return false;
    }
    si |= (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION
                                        : UNPROTECTED_DACL_SECURITY_INFORMATION;
    return true;
}

// Applies ONLY the fields the parsed security descriptor actually specifies
// (owner, GROUP, and/or DACL, protected vs unprotected from the control flag).
// The group was previously dropped, silently losing the primary group on any
// save-then-restore round-trip. Returns the applySecurityNoFollow result,
// kReparseRefused/kPathRedirected for a redirected target, or
// ERROR_INVALID_SECURITY_DESCR when none of owner/group/DACL is specified.
DWORD applyParsedSecurityDescriptor(const QString& path, PSECURITY_DESCRIPTOR pSD) {
    SECURITY_INFORMATION si = 0;
    SecurityChange change{};

    // Refuse a descriptor that carries a SACL (audit / mandatory-integrity label):
    // this path applies only owner/group/DACL, so a SACL must be rejected rather than
    // silently dropped while the rest of the descriptor is applied. A FALSE control
    // query on a freshly-parsed descriptor is an accessor error -> fail closed.
    SECURITY_DESCRIPTOR_CONTROL sdControl = 0;
    DWORD sdRevision = 0;
    if (!GetSecurityDescriptorControl(pSD, &sdControl, &sdRevision)) {
        return ERROR_INVALID_SECURITY_DESCR;
    }
    if (sdControl & SE_SACL_PRESENT) {
        return kSaclUnsupported;
    }

    BOOL ownerDefaulted = FALSE;
    if (!GetSecurityDescriptorOwner(pSD, &change.owner, &ownerDefaulted)) {
        return ERROR_INVALID_SECURITY_DESCR;  // accessor error -> fail closed
    }
    if (change.owner != nullptr) {
        si |= OWNER_SECURITY_INFORMATION;
    }

    BOOL groupDefaulted = FALSE;
    if (!GetSecurityDescriptorGroup(pSD, &change.group, &groupDefaulted)) {
        return ERROR_INVALID_SECURITY_DESCR;  // accessor error -> fail closed
    }
    if (change.group != nullptr) {
        si |= GROUP_SECURITY_INFORMATION;
    }

    if (!collectParsedDacl(pSD, si, change)) {
        return ERROR_INVALID_SECURITY_DESCR;  // accessor error or present NULL DACL
    }

    if (si == 0) {
        return ERROR_INVALID_SECURITY_DESCR;
    }
    change.si = si;
    return applySecurityNoFollow(path, accessForSecurityInfo(si), change);
}

// Maps an applyParsedSecurityDescriptor result to the m_lastError text that
// setSecurityDescriptorSddl surfaces. An empty return means the apply succeeded;
// any non-empty string is a refusal/failure the caller must report fail-closed.
QString sddlApplyFailureMessage(DWORD result) {
    if (result == ERROR_INVALID_SECURITY_DESCR) {
        return "SDDL specifies neither an owner nor a valid DACL";
    }
    if (result == kReparseRefused) {
        return "Refused: target is a reparse point (junction/symlink)";
    }
    if (result == kPathRedirected) {
        return "Refused: path resolves through a redirected junction/symlink ancestor";
    }
    if (result == kHardLinkRefused) {
        return "Refused: target has multiple hard links (shared underlying file)";
    }
    if (result == kSaclUnsupported) {
        return "Refused: SDDL carries a SACL (audit/integrity label), which is not applied";
    }
    if (result != ERROR_SUCCESS) {
        return QString("Failed to apply SDDL: %1").arg(result);
    }
    return QString();
}
}  // namespace
#endif

PermissionManager::PermissionManager() {
#ifdef Q_OS_WIN
    // Enable privileges lazily -- only succeeds when running elevated.
    // Non-elevated callers will get clear errors from methods that need them.
    if (ElevationManager::isElevated()) {
        // Attempt all three independently (no short-circuit) and surface a failure to
        // enable any of them rather than silently ignoring it. A missing privilege is
        // not fatal here -- the operation that needs it still fails closed with a real
        // Win32 error -- but a swallowed enablement failure hides why that happens.
        const bool okBackup = enablePrivilege(SE_BACKUP_NAME);
        const bool okRestore = enablePrivilege(SE_RESTORE_NAME);
        const bool okTakeOwnership = enablePrivilege(SE_TAKE_OWNERSHIP_NAME);
        if (!okBackup || !okRestore || !okTakeOwnership) {
            sak::logWarning(
                "PermissionManager: could not enable all backup/restore/take-ownership "
                "privileges; ACL operations that need them will fail closed with access denied");
        }
    }
#endif
}

PermissionManager::~PermissionManager() {}

// The bool overloads are thin wrappers over the std::expected try* variants so
// each security-sensitive operation has a SINGLE implementation (no duplicated
// ACL/ownership logic). The try* variants own the reparse/no-follow hardening.
bool PermissionManager::stripPermissions(const QString& path) {
    return tryStripPermissions(path).has_value();
}

bool PermissionManager::takeOwnership(const QString& path, const QString& userSID) {
    return tryTakeOwnership(path, userSID).has_value();
}

bool PermissionManager::setStandardUserPermissions(const QString& path, const QString& userSID) {
    return trySetStandardUserPermissions(path, userSID).has_value();
}

// canModifyPermissions was removed here, together with its two tests. It was a public static
// with no caller anywhere in the tree, and it did not actually answer the question its name
// asked: it returned QFileInfo::exists() && isWritable(), i.e. write access to the file's DATA,
// which says nothing about WRITE_DAC/WRITE_OWNER or the elevation the ACL operations need. Every
// live entry point instead attempts the operation and fails closed with a real Win32 error.

// applyPermissionStrategy was removed here. It had zero callers anywhere in the tree - the
// backup and restore workers each dispatch on PermissionMode themselves - and it carried a
// fourth, contradictory definition of the Hybrid mode ("strip, then assign to the
// destination user") that also forgot the empty-SID guard its AssignToDestination branch
// had, so a Hybrid call with no SID stripped the file and then reported failure.

QString PermissionManager::getOwner(const QString& path) {
#ifdef Q_OS_WIN
    if (hasEmbeddedNul(path)) {
        return QString();
    }
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
#ifdef Q_OS_WIN
    if (hasEmbeddedNul(path)) {
        m_lastError = "Refused: path contains an embedded NUL";
        return QString();
    }
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
#ifdef Q_OS_WIN
    if (sddl.isEmpty()) {
        m_lastError = "Empty SDDL";
        return false;
    }

    if (hasEmbeddedNul(path) || hasEmbeddedNul(sddl)) {
        m_lastError = "Refused: path or SDDL contains an embedded NUL";
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

    const QString failure = sddlApplyFailureMessage(result);
    if (!failure.isEmpty()) {
        m_lastError = failure;
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
#ifdef Q_OS_WIN
    QFileInfo fileInfo(path);
    if (!fileInfo.exists()) {
        m_lastError = "File does not exist";
        return std::unexpected(sak::error_code::file_not_found);
    }

    // An EMPTY (non-NULL) DACL strips explicit access and re-inherits from the
    // parent; a nullptr pDacl would set a NULL DACL that grants everyone full
    // access. The change is applied to a verified no-follow handle so a reparse
    // swap cannot redirect it (see applySecurityNoFollow).
    ACL emptyDacl;
    if (!InitializeAcl(&emptyDacl, sizeof(ACL), ACL_REVISION)) {
        m_lastError = QString("Failed to initialize empty DACL: %1").arg(GetLastError());
        return std::unexpected(sak::error_code::permission_update_failed);
    }

    const SecurityChange change{DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                nullptr,
                                nullptr,
                                &emptyDacl};
    const DWORD result = applySecurityNoFollow(path, WRITE_DAC | READ_CONTROL, change);
    return interpretSecurityResult(result, "Failed to strip permissions", m_lastError);
#else
    m_lastError = "Permission management only supported on Windows";
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

auto PermissionManager::tryTakeOwnership(const QString& path, const QString& userSID)
    -> std::expected<void, sak::error_code> {
#ifdef Q_OS_WIN
    if (!isRunningAsAdmin()) {
        m_lastError = "Administrator privileges required to take ownership";
        return std::unexpected(sak::error_code::elevation_required);
    }

    if (hasEmbeddedNul(userSID)) {
        m_lastError = "Invalid SID: embedded NUL";
        return std::unexpected(sak::error_code::invalid_argument);
    }

    PSID pSid = nullptr;
    if (!ConvertStringSidToSidW(const_cast<LPWSTR>(userSID.toStdWString().c_str()), &pSid)) {
        m_lastError = QString("Invalid SID: %1").arg(GetLastError());
        return std::unexpected(sak::error_code::invalid_argument);
    }

    const SecurityChange change{OWNER_SECURITY_INFORMATION, pSid, nullptr, nullptr};
    const DWORD result = applySecurityNoFollow(path, WRITE_OWNER, change);
    LocalFree(pSid);
    return interpretSecurityResult(result, "Failed to take ownership", m_lastError);
#else
    m_lastError = "Permission management only supported on Windows";
    return std::unexpected(sak::error_code::platform_not_supported);
#endif
}

auto PermissionManager::trySetStandardUserPermissions(const QString& path, const QString& userSID)
    -> std::expected<void, sak::error_code> {
#ifdef Q_OS_WIN
    if (hasEmbeddedNul(userSID)) {
        m_lastError = "Invalid SID: embedded NUL";
        return std::unexpected(sak::error_code::invalid_argument);
    }

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

    const SecurityChange change{
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, pNewAcl};
    result = applySecurityNoFollow(path, WRITE_DAC | READ_CONTROL, change);
    LocalFree(pNewAcl);
    LocalFree(pSid);
    return interpretSecurityResult(result, "Failed to set permissions", m_lastError);
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
    // Private static; the constructor is the only caller and passes the
    // SE_BACKUP_NAME / SE_RESTORE_NAME / SE_TAKE_OWNERSHIP_NAME literals.
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

    const bool adjusted = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr) !=
                          0;
    // AdjustTokenPrivileges returns nonzero even when it could NOT enable the
    // privilege (the token does not hold it); GetLastError()==ERROR_NOT_ALL_ASSIGNED
    // is the only reliable signal. Capture it BEFORE CloseHandle, which overwrites
    // the thread's last-error. Report failure so a caller never treats an un-enabled
    // privilege as enabled.
    const DWORD adjustError = GetLastError();
    CloseHandle(hToken);
    return adjusted && adjustError != ERROR_NOT_ALL_ASSIGNED;
}

#endif

}  // namespace sak
