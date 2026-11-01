// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"
#include "sak/user_profile_types.h"

#include <QFileInfo>
#include <QString>

#include <expected>
#include <optional>

#include <windows.h>

#include <aclapi.h>

namespace sak {

/**
 * @brief Manages file/folder permissions during backup and restore
 *
 * Primary strategy: STRIP permissions to prevent corruption
 * Advanced: Preserve/restore ACLs when needed (requires admin)
 */
class PermissionManager {
public:
    PermissionManager();
    ~PermissionManager();

    /**
     * @brief Strip all explicit permissions, inherit from parent (SAFEST)
     * @param path File or folder path
     * @return true if successful
     */
    [[nodiscard]] bool stripPermissions(const QString& path);

    /**
     * @brief Take ownership of file/folder for destination user
     * @param path File or folder path
     * @param userSID SID of destination user
     * @return true if successful
     */
    [[nodiscard]] bool takeOwnership(const QString& path, const QString& userSID);

    /**
     * @brief Set standard user permissions
     * @param path File or folder path
     * @param userSID SID of user to grant permissions
     * @return true if successful
     */
    [[nodiscard]] bool setStandardUserPermissions(const QString& path, const QString& userSID);

    /**
     * @brief Get current file owner
     * @param path File or folder path
     * @return Owner SID or empty string
     */
    static QString getOwner(const QString& path);

    /**
     * @brief Get full security descriptor in SDDL format
     * @param path File or folder path
     * @return SDDL string or empty if failed
     */
    QString getSecurityDescriptorSddl(const QString& path);

    /**
     * @brief Apply security descriptor from SDDL
     * @param path File or folder path
     * @param sddl SDDL string
     * @return true if applied
     */
    [[nodiscard]] bool setSecurityDescriptorSddl(const QString& path, const QString& sddl);

    /// @brief True when the real parsed-SDDL apply path would include the primary-group field.
    /// Pure seam: parses only; does not touch a filesystem object or require WRITE_OWNER.
    [[nodiscard]] static bool sddlAppliesPrimaryGroupForTesting(const QString& sddl);

    // ======================================================================
    // Elevation-Aware Overloads (std::expected)
    // ======================================================================

    /// @brief Strip permissions, returning error_code::elevation_required on access denied
    [[nodiscard]] auto tryStripPermissions(const QString& path)
        -> std::expected<void, sak::error_code>;

    /// @brief Take ownership, returning error_code::elevation_required on access denied
    [[nodiscard]] auto tryTakeOwnership(const QString& path, const QString& userSID)
        -> std::expected<void, sak::error_code>;

    /// @brief Pure preflight for the administrator-only ownership path.
    /// @return elevation_required when not elevated; no refusal when elevated.
    [[nodiscard]] static std::optional<sak::error_code> takeOwnershipElevationRefusal(
        bool is_admin) noexcept;

    /// @brief Set standard user permissions, returning error_code on failure
    [[nodiscard]] auto trySetStandardUserPermissions(const QString& path, const QString& userSID)
        -> std::expected<void, sak::error_code>;


    /**
     * @brief Get last error message
     * @return Error description
     */
    QString getLastError() const { return m_lastError; }

private:
    static bool enablePrivilege(const wchar_t* privilegeName);

    QString m_lastError;
};

/// @brief Restrict a file the current user owns to that user, SYSTEM and Administrators.
///
/// Applies the protected DACL trySetStandardUserPermissions applies -- no inherited entries, so
/// other users of the machine lose access -- through the same no-follow handle, which refuses a
/// reparse point and a file with more than one hard link. Unlike trySetStandardUserPermissions it
/// accepts a path whose ancestors resolve elsewhere (a mapped drive, a SUBST drive, a junctioned
/// folder): it is for a file the caller just created in its own folder, where that is another
/// spelling of the same file. SYSTEM and Administrators stay on it for the reason
/// buildStandardUserDacl gives: both can take ownership anyway, and a file they cannot open breaks
/// backup, antivirus and cleanup without protecting anything.
///
/// Unlike constructing a PermissionManager, this enables NO privilege: the owner of an object
/// may always write its DACL, so a caller protecting its own file needs nothing more.
/// @return The refusal or Win32 failure, in words, when the DACL was not applied.
[[nodiscard]] std::expected<void, QString> restrictFileToCurrentUser(const QString& path);

}  // namespace sak
