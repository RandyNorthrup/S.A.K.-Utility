// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

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
     * @brief Scan the system for local Windows user profiles (NetUserEnum normal SAM accounts).
     *        Cached domain/Entra-only accounts and orphaned profiles without a local account are
     *        NOT enumerated, so a genuine local result is not a claim of every profile on disk.
     * @return List of detected user profiles
     */
    QVector<UserProfile> scanUsers();

    /**
     * @brief Scan the system for local Windows user profiles (NetUserEnum normal SAM accounts),
     *        reporting whether the enumeration succeeded
     * @param queryOk Set false if the underlying NetUserEnum call failed (e.g. access denied
     *                without elevation); true for a genuine result (including a genuine empty
     *                one). Lets a caller tell "no users" from "could not read users".
     * @return List of detected user profiles
     */
    QVector<UserProfile> scanUsers(bool& queryOk);

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

    // No session-state accessor here: isUserLoggedIn() was removed with its WTS dependency
    // because nothing in the product ever asked whether a profile's owner was signed in.

    /**
     * @brief Estimate size of user profile
     * @param profilePath Path to user profile
     * @return Estimated size in bytes (quick estimate, not full scan)
     */
    static qint64 estimateProfileSize(const QString& profilePath);

    /// @brief Sum of file sizes directly under @p folderPath (recursive), capped
    ///        at @p fileLimit files. Unlike estimateProfileSize (which expects a
    ///        profile ROOT and scans its standard subfolders), this sizes a single
    ///        folder -- so a per-folder selection reports a real size, not 0. NOTE:
    ///        the walk stops after @p fileLimit files, so a folder with more files
    ///        than that reports a LOWER-BOUND size, not the exact total.
    ///        Unit-testable.
    [[nodiscard]] static qint64 sumFolderFileSizes(const QString& folderPath, int fileLimit);

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
    static void populateFolderSelections(UserProfile& profile);
};

}  // namespace sak
