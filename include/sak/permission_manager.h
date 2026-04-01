// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"
#include "sak/user_profile_types.h"

#include <QFileInfo>
#include <QString>

#include <expected>

#ifdef Q_OS_WIN
#include <windows.h>

#include <aclapi.h>
#endif

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
    bool stripPermissions(const QString& path);

    /**
     * @brief Take ownership of file/folder for destination user
     * @param path File or folder path
     * @param userSID SID of destination user
     * @return true if successful
     */
    bool takeOwnership(const QString& path, const QString& userSID);

    /**
     * @brief Set standard user permissions
     * @param path File or folder path
     * @param userSID SID of user to grant permissions
     * @return true if successful
     */
    bool setStandardUserPermissions(const QString& path, const QString& userSID);

    /**
     * @brief Check if we have permission to modify file/folder
     * @param path Path to check
     * @return true if we can modify permissions
     */
    static bool canModifyPermissions(const QString& path);

    /**
     * @brief Apply permission strategy to file/folder
     * @param path File or folder path
     * @param mode Permission mode to apply
     * @param destinationUserSID SID for destination user (if applicable)
     * @return true if successful
     */
    bool applyPermissionStrategy(const QString& path,
                                 PermissionMode mode,
                                 const QString& destinationUserSID = QString());

    /**
     * @brief Get current file owner
     * @param path File or folder path
     * @return Owner SID or empty string
     */
    QString getOwner(const QString& path);

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
    bool setSecurityDescriptorSddl(const QString& path, const QString& sddl);

    // ======================================================================
    // Elevation-Aware Overloads (std::expected)
    // ======================================================================

    /// @brief Strip permissions, returning error_code::elevation_required on access denied
    [[nodiscard]] auto tryStripPermissions(const QString& path)
        -> std::expected<void, sak::error_code>;

    /// @brief Take ownership, returning error_code::elevation_required on access denied
    [[nodiscard]] auto tryTakeOwnership(const QString& path, const QString& userSID)
        -> std::expected<void, sak::error_code>;

    /// @brief Set standard user permissions, returning error_code on failure
    [[nodiscard]] auto trySetStandardUserPermissions(const QString& path, const QString& userSID)
        -> std::expected<void, sak::error_code>;

    /**
     * @brief Check if running with administrator privileges
     * @return true if admin
     */
    static bool isRunningAsAdmin();

    /**
     * @brief Get last error message
     * @return Error description
     */
    QString getLastError() const { return m_lastError; }

private:
#ifdef Q_OS_WIN
    static bool enablePrivilege(const wchar_t* privilegeName);
#endif

    QString m_lastError;
};

}  // namespace sak
