#pragma once

#include "sak/user_profile_types.h"
#include <QString>
#include <QFileInfo>

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
    bool canModifyPermissions(const QString& path);
    
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
    bool getSecurityInfo(const QString& path, PSECURITY_DESCRIPTOR* pSD);
    bool setSecurityInfo(const QString& path, PSECURITY_DESCRIPTOR pSD);
    bool enablePrivilege(const wchar_t* privilegeName);
#endif
    
    QString m_lastError;
};

} // namespace sak
