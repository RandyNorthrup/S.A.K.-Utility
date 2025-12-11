#pragma once

#include "sak/user_profile_types.h"
#include <QObject>

namespace sak {

/**
 * @brief Scans Windows system for user profiles
 * 
 * Uses Windows NetUserEnum API to enumerate local user accounts
 * and validates their profile directories exist.
 */
class WindowsUserScanner : public QObject {
    Q_OBJECT

public:
    explicit WindowsUserScanner(QObject* parent = nullptr);
    
    /**
     * @brief Scan system for all user profiles
     * @return List of detected user profiles
     */
    QVector<UserProfile> scanUsers();
    
    /**
     * @brief Get current logged-in username
     */
    static QString getCurrentUsername();
    
    /**
     * @brief Get user's SID from username
     * @param username Windows username
     * @return SID string (S-1-5-21-...) or empty if failed
     */
    static QString getUserSID(const QString& username);
    
    /**
     * @brief Get profile path for username
     * @param username Windows username
     * @return Profile path (C:\Users\Username) or empty if not found
     */
    static QString getProfilePath(const QString& username);
    
    /**
     * @brief Check if user is currently logged in
     * @param username Windows username
     * @return true if user has active session
     */
    static bool isUserLoggedIn(const QString& username);
    
    /**
     * @brief Estimate size of user profile
     * @param profilePath Path to user profile
     * @return Estimated size in bytes (quick estimate, not full scan)
     */
    static qint64 estimateProfileSize(const QString& profilePath);
    
    /**
     * @brief Get default folder selections for a user
     * @param profilePath User profile path
     * @return Standard folder selections with basic info
     */
    static QVector<FolderSelection> getDefaultFolderSelections(const QString& profilePath);

Q_SIGNALS:
    void scanProgress(int current, int total);
    void userFound(const QString& username);
    
private:
    bool enumerateWindowsUsers(QVector<UserProfile>& profiles);
    void populateFolderSelections(UserProfile& profile);
    qint64 quickSizeEstimate(const QString& path, int maxDepth = 2);
};

} // namespace sak
