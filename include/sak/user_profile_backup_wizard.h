// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"
#include "sak/windows_user_scanner.h"
#include "sak/smart_file_filter.h"
#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QTableWidget>
#include <QTreeWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include <QTextEdit>
#include <QProgressBar>

namespace sak {

// Forward declarations
class UserProfileBackupWorker;

/**
 * @brief Installed app entry for backup/restore
 */
struct InstalledAppInfo {
    QString name;
    QString version;
    QString publisher;
    QString choco_package;
    QString category;
    bool selected{true};
};

/**
 * @brief Wizard for backing up Windows user profiles
 * 
 * 7-Page Wizard:
 * 1. Welcome & Instructions
 * 2. Scan & Select Users
 * 3. Customize Per-User Data
 * 4. Installed Applications Selection
 * 5. Smart Filter Configuration
 * 6. Backup Settings
 * 7. Execution & Progress
 */
class UserProfileBackupWizard : public QWizard {
    Q_OBJECT

public:
    /// Unscoped enum required for implicit int conversion by QWizard::setPage()
    enum PageId {
        Page_Welcome = 0,
        Page_SelectUsers,
        Page_CustomizeData,
        Page_InstalledApps,
        Page_SmartFilters,
        Page_BackupSettings,
        Page_Execute
    };

    explicit UserProfileBackupWizard(QWidget* parent = nullptr);
    ~UserProfileBackupWizard() override;
    
    /**
     * @brief Get the backup manifest after completion
     */
    BackupManifest getBackupManifest() const { return m_manifest; }
    
    /**
     * @brief Get the smart filter configuration
     */
    SmartFilter getSmartFilter() const { return m_smartFilter; }
    
    /**
     * @brief Get compression level (0-9: 0=none, 3=fast, 6=balanced, 9=max)
     */
    int getCompressionLevel() const;
    
    /**
     * @brief Check if encryption is enabled
     */
    bool isEncryptionEnabled() const;
    
    /**
     * @brief Get encryption password
     */
    QString getEncryptionPassword() const;

    /** @brief Get/set installed apps selected for backup */
    QVector<InstalledAppInfo> installedApps() const { return m_installedApps; }
    void setInstalledApps(const QVector<InstalledAppInfo>& apps) { m_installedApps = apps; }

private:
    BackupManifest m_manifest;
    QVector<UserProfile> m_scannedUsers;
    SmartFilter m_smartFilter;
    QVector<InstalledAppInfo> m_installedApps;
};

/**
 * @brief Page 1: Welcome & Instructions
 */
class UserProfileBackupWelcomePage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupWelcomePage(QWidget* parent = nullptr);
    
private:
    void setupUi();
};

/**
 * @brief Page 2: Scan & Select Users
 */
class UserProfileBackupSelectUsersPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupSelectUsersPage(QVector<UserProfile>& users, QWidget* parent = nullptr);
    
    void initializePage() override;
    bool isComplete() const override;
    
private Q_SLOTS:
    void onScanUsers();
    void onSelectAll();
    void onSelectNone();
    void onUserScanned(const QString& username);
    void updateSummary();
    
private:
    void setupUi();
    void populateTable();
    
    QVector<UserProfile>& m_users;
    WindowsUserScanner* m_scanner{nullptr};
    
    QTableWidget* m_userTable{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    
    bool m_scanned{false};
};

/**
 * @brief Page 3: Customize Per-User Data
 */
class UserProfileBackupCustomizeDataPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupCustomizeDataPage(QVector<UserProfile>& users, QWidget* parent = nullptr);
    
    void initializePage() override;
    bool isComplete() const override;
    
private Q_SLOTS:
    void onCustomizeUser();
    void updateSummary();
    
private:
    void setupUi();
    void populateUserList();
    
    QVector<UserProfile>& m_users;
    
    QTableWidget* m_userTable{nullptr};
    QPushButton* m_customizeButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_instructionLabel{nullptr};
};

/**
 * @brief Page 4: Smart Filter Configuration
 */
class UserProfileBackupSmartFiltersPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupSmartFiltersPage(SmartFilter& filter, QWidget* parent = nullptr);
    
    void initializePage() override;
    
private Q_SLOTS:
    void onResetToDefaults();
    void onViewDangerousList();
    void updateSummary();
    
private:
    void setupUi();
    void loadFilterSettings();
    
    SmartFilter& m_filter;
    
    QCheckBox* m_enableFileSizeLimitCheck{nullptr};
    QSpinBox* m_maxFileSizeSpinBox{nullptr};
    QCheckBox* m_enableFolderSizeLimitCheck{nullptr};
    QSpinBox* m_maxFolderSizeSpinBox{nullptr};
    QCheckBox* m_excludeCacheCheck{nullptr};
    QCheckBox* m_excludeTempCheck{nullptr};
    QCheckBox* m_excludeLockCheck{nullptr};
    QPushButton* m_viewDangerousButton{nullptr};
    QPushButton* m_resetButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
};

/**
 * @brief Page 5: Backup Settings
 */
class UserProfileBackupSettingsPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupSettingsPage(BackupManifest& manifest, QWidget* parent = nullptr);
    
    void initializePage() override;
    bool validatePage() override;
    
private Q_SLOTS:
    void onBrowseDestination();
    void updateSummary();
    
private:
    void setupUi();
    
    BackupManifest& m_manifest;
    QString m_destinationPath;
    
    QLineEdit* m_destinationEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QComboBox* m_compressionCombo{nullptr};
    QCheckBox* m_encryptionCheck{nullptr};
    QLineEdit* m_passwordEdit{nullptr};
    QLineEdit* m_passwordConfirmEdit{nullptr};
    QComboBox* m_permissionModeCombo{nullptr};
    QCheckBox* m_verifyCheck{nullptr};
    QLabel* m_summaryLabel{nullptr};
};

/**
 * @brief Page 6: Execution & Progress
 */
class UserProfileBackupExecutePage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupExecutePage(BackupManifest& manifest, 
                               const QVector<UserProfile>& users,
                               const QString& destinationPath,
                               QWidget* parent = nullptr);
    
    void initializePage() override;
    bool isComplete() const override;
    
private Q_SLOTS:
    void onStartBackup();
    void onBackupProgress(int current, int total, qint64 bytes, qint64 totalBytes);
    void onBackupComplete(bool success, const QString& message);
    void onLogMessage(const QString& message);
    
private:
    void setupUi();
    void appendLog(const QString& message);
    
    BackupManifest& m_manifest;
    const QVector<UserProfile>& m_users;
    const QString& m_destinationPath;
    UserProfileBackupWorker* m_worker{nullptr};
    
    QProgressBar* m_overallProgress{nullptr};
    QProgressBar* m_currentProgress{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_currentUserLabel{nullptr};
    QTextEdit* m_logEdit{nullptr};
    QPushButton* m_startButton{nullptr};
    
    bool m_started{false};
    bool m_completed{false};
};

/**
 * @brief Page 4: Installed Applications Selection
 * 
 * Scans for installed applications and presents them in a hierarchical
 * tree with categories and checkboxes. Selected apps are saved to the
 * backup for potential restoration via Chocolatey.
 */
class UserProfileBackupInstalledAppsPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupInstalledAppsPage(QWidget* parent = nullptr);

    void initializePage() override;
    void cleanupPage() override;
    bool isComplete() const override;

private Q_SLOTS:
    void onScanApps();
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUi();
    void populateTree(const QVector<InstalledAppInfo>& apps);
    void updateParentCheckState(QTreeWidgetItem* parent);
    void updateNextButtonText();

    QTreeWidget* m_appTree{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};

    bool m_scanned{false};
};

} // namespace sak
