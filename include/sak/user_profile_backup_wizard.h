// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/smart_file_filter.h"
#include "sak/user_profile_types.h"
#include "sak/windows_user_scanner.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTextEdit>
#include <QTreeWidget>
#include <QWizard>
#include <QWizardPage>

class QVBoxLayout;

namespace sak {

// Forward declarations
class UserProfileBackupWorker;

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
        Page_AppData,
        Page_SmartFilters,
        Page_KnownNetworks,
        Page_EthernetSettings,
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
    [[nodiscard]] bool isEncryptionEnabled() const;

    /**
     * @brief Get encryption password
     */
    QString getEncryptionPassword() const;

    /** @brief Get/set installed apps selected for backup */
    QVector<InstalledAppInfo> installedApps() const { return m_installedApps; }
    void setInstalledApps(const QVector<InstalledAppInfo>& apps) { m_installedApps = apps; }

    /** @brief Get/set WiFi profiles selected for backup */
    QVector<WifiProfileInfo> wifiProfiles() const { return m_wifiProfiles; }
    void setWifiProfiles(const QVector<WifiProfileInfo>& profiles) { m_wifiProfiles = profiles; }

    /** @brief Get/set ethernet configs selected for backup */
    QVector<EthernetConfigInfo> ethernetConfigs() const { return m_ethernetConfigs; }
    void setEthernetConfigs(const QVector<EthernetConfigInfo>& configs) {
        m_ethernetConfigs = configs;
    }

    /** @brief Get/set app data sources selected for backup */
    QVector<AppDataSourceInfo> appDataSources() const { return m_appDataSources; }
    void setAppDataSources(const QVector<AppDataSourceInfo>& sources) {
        m_appDataSources = sources;
    }

private:
    BackupManifest m_manifest;
    QVector<UserProfile> m_scannedUsers;
    SmartFilter m_smartFilter;
    QVector<InstalledAppInfo> m_installedApps;
    QVector<WifiProfileInfo> m_wifiProfiles;
    QVector<EthernetConfigInfo> m_ethernetConfigs;
    QVector<AppDataSourceInfo> m_appDataSources;
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
    explicit UserProfileBackupSelectUsersPage(QVector<UserProfile>& users,
                                              QWidget* parent = nullptr);

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
    explicit UserProfileBackupCustomizeDataPage(QVector<UserProfile>& users,
                                                QWidget* parent = nullptr);

    void initializePage() override;
    bool isComplete() const override;

private Q_SLOTS:
    void onCustomizeUser();
    void updateSummary();

private:
    void setupUi();
    void populateUserList();
    /// @brief Find the nth selected user in m_users
    UserProfile* findSelectedUserByRow(int selectedRow);

    QVector<UserProfile>& m_users;

    QTableWidget* m_userTable{nullptr};
    QPushButton* m_customizeButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_instructionLabel{nullptr};
};

/**
 * @brief Page 4a: Application Data Sources
 */
class UserProfileBackupAppDataPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupAppDataPage(QVector<UserProfile>& users, QWidget* parent = nullptr);

    void initializePage() override;
    bool isComplete() const override;
    void cleanupPage() override;

private Q_SLOTS:
    void onScanAppData();
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUi();
    void populateTree(const QVector<AppDataSourceInfo>& sources);
    void updateParentCheckState(QTreeWidgetItem* parent);
    void updateNextButtonText();

    QVector<UserProfile>& m_users;
    QTreeWidget* m_appDataTree{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    bool m_scanned{false};
};

/**
 * @brief Page 4b: Smart Filter Configuration
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
    /// @brief Create filter settings grid (file size, folder size limits)
    void setupUi_filterSettings(QVBoxLayout* layout);
    /// @brief Create exclusion checkboxes and control buttons
    void setupUi_exclusionsAndControls(QVBoxLayout* layout);
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
 * @brief Page 5a: Known WiFi Networks Backup (skippable)
 */
class UserProfileBackupKnownNetworksPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupKnownNetworksPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool isComplete() const override;
    void cleanupPage() override;

private Q_SLOTS:
    void onScanNetworks();
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUi();
    void populateTree(const QVector<WifiProfileInfo>& profiles);
    void updateNextButtonText();

    QTreeWidget* m_networkTree{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    bool m_scanned{false};
};

/**
 * @brief Page 5b: Ethernet Settings Backup (skippable)
 */
class UserProfileBackupEthernetSettingsPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileBackupEthernetSettingsPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool isComplete() const override;
    void cleanupPage() override;

private Q_SLOTS:
    void onScanEthernet();
    void onSelectAll();
    void onSelectNone();

private:
    void setupUi();
    void populateTable(const QVector<EthernetConfigInfo>& configs);
    void updateNextButtonText();

    QTableWidget* m_ethernetTable{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    bool m_scanned{false};
};

/**
 * @brief Page 6: Backup Settings
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
    void setupUi_destinationAndCompression(QVBoxLayout* layout);
    void setupUi_encryptionAndPermissions(QVBoxLayout* layout);
    void setupUi_summaryAndRegistration(QVBoxLayout* layout);

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

    /// @brief Save installed apps list from wizard to backup directory
    void saveInstalledAppsToBackup(const QVector<InstalledAppInfo>& installedApps);

    /// @brief Save WiFi profiles from wizard to backup directory
    void saveWifiProfilesToBackup(const QVector<WifiProfileInfo>& profiles);

    /// @brief Save Ethernet configs from wizard to backup directory
    void saveEthernetConfigsToBackup(const QVector<EthernetConfigInfo>& configs);

    /// @brief Save app data sources from wizard to backup directory
    void saveAppDataSourcesToBackup(const QVector<AppDataSourceInfo>& sources);

    /// @brief Create, connect, and start the backup worker
    void connectAndStartBackupWorker(SmartFilter smartFilter,
                                     PermissionMode permissionMode,
                                     int compressionLevel,
                                     bool encrypt,
                                     const QString& password);

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

    /// @brief Categorize an application by name/publisher into a UI group
    static QString categorizeApp(const QString& name, const QString& publisher);

    QTreeWidget* m_appTree{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};

    bool m_scanned{false};
};

}  // namespace sak
