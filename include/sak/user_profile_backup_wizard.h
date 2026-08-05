// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/backup_file_codec.h"
#include "sak/smart_file_filter.h"
#include "sak/user_profile_types.h"
#include "sak/windows_user_scanner.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
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

#include <atomic>
#include <memory>

class QVBoxLayout;

namespace sak {

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
     * @brief Stable accessor for the scanned/selected users.
     *
     * Returns the wizard-owned vector (populated by the Select Users page) so
     * the execute page copies real data instead of an unset "scannedUsers"
     * property, which always yielded an empty, dangling list.
     */
    const QVector<UserProfile>& scannedUsers() const { return m_scannedUsers; }

    /**
     * @brief Get the smart filter configuration
     */
    SmartFilter getSmartFilter() const { return m_smartFilter; }

    /**
     * @brief Get the permission mode chosen on the settings page.
     * @return the selected PermissionMode, or StripAll (safest) if unset/invalid.
     */
    PermissionMode getPermissionMode() const;

    /**
     * @brief Per-file compression/encryption chosen on the settings page.
     *
     * Read from the page directly rather than through a registered wizard field: a
     * QWizard field is readable by name from every page, and the encryption password
     * should not be. Returns pass-through options if the page is not present.
     */
    [[nodiscard]] BackupCodecOptions getCodecOptions() const;

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
    void setupScanControls(QVBoxLayout* layout);
    void setupUserTable(QVBoxLayout* layout);
    void setupSelectionControls(QVBoxLayout* layout);
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
    ~UserProfileBackupAppDataPage() override;

    void initializePage() override;
    bool isComplete() const override;
    void cleanupPage() override;

private Q_SLOTS:
    void onScanAppData();
    /// @brief Apply worker-thread scan results to the tree (GUI thread)
    void onScanFinished();
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUi();
    void populateTree(const QVector<AppDataSourceInfo>& sources);
    void updateParentCheckState(QTreeWidgetItem* parent);
    void updateNextButtonText();
    /// @brief Collect checked app-data sources and commit them to the wizard
    void commitAppDataSelection();

    QVector<UserProfile>& m_users;
    QTreeWidget* m_appDataTree{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    bool m_scanned{false};

    /// @brief Watches the off-GUI-thread app-data size scan
    QFutureWatcher<QVector<AppDataSourceInfo>>* m_scanWatcher{nullptr};
    /// @brief Cooperative cancel flag so teardown can bound the scan wait
    std::shared_ptr<std::atomic_bool> m_scanCancel;
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
    /// @brief Collect checked rows (copying the scanned profile intact, so the
    /// XML payload is preserved) and commit them to the wizard + summary.
    void commitWifiSelection();

    QTreeWidget* m_networkTree{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    QVector<WifiProfileInfo> m_scannedProfiles;  // Full payloads (incl. XML), by row
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
    void onEthernetItemChanged(QTableWidgetItem* item);

private:
    void setupUi();
    void setupSelectionButtons(QVBoxLayout* layout);
    void populateTable(const QVector<EthernetConfigInfo>& configs);
    void updateNextButtonText();
    /// @brief Collect checked rows (copying the scanned config intact, so DNS and
    /// other fields are preserved) and commit them to the wizard + summary.
    void commitEthernetSelection();

    QTableWidget* m_ethernetTable{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_scanProgress{nullptr};
    QVector<EthernetConfigInfo> m_scannedConfigs;  // Full configs (incl. DNS), by row
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

    /// @brief Per-file transforms chosen on this page, read by the execute page.
    /// The password is only non-empty when encryption is actually enabled.
    [[nodiscard]] bool compressionEnabled() const;
    [[nodiscard]] int compressionLevel() const;
    [[nodiscard]] bool encryptionEnabled() const;
    [[nodiscard]] QString encryptionPassword() const;

private Q_SLOTS:
    void onBrowseDestination();
    void updateSummary();

private:
    void setupUi();
    void setupUi_destination(QVBoxLayout* layout);
    void setupUi_transforms(QVBoxLayout* layout);
    void setupUi_permissions(QVBoxLayout* layout);
    /// @brief Refuse a mismatched or too-short password; true when the run may proceed.
    bool validateEncryptionPassword();
    void setupUi_summaryAndRegistration(QVBoxLayout* layout);
    /// @brief Screen the typed destination and store the accepted form in m_destinationPath
    /// @return false (with a refusal dialog) for blank, relative, or source-overlapping paths
    bool validateDestination();
    /// @brief Ask before reusing an existing folder; true when the run may proceed
    bool confirmExistingDestination(const QString& destination);
    void installExecutePage();

    BackupManifest& m_manifest;
    QString m_destinationPath;

    QLineEdit* m_destinationEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QComboBox* m_permissionModeCombo{nullptr};
    QCheckBox* m_verifyCheck{nullptr};
    QLabel* m_summaryLabel{nullptr};

    QCheckBox* m_compressCheck{nullptr};
    QComboBox* m_compressionLevelCombo{nullptr};
    QCheckBox* m_encryptCheck{nullptr};
    QLineEdit* m_passwordEdit{nullptr};
    QLineEdit* m_passwordConfirmEdit{nullptr};
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

    /// @brief Re-screen the destination this page was constructed with
    /// @return an empty string when the destination may be used, else the refusal reason
    QString screenRunDestination() const;
    /// @brief Screen, then ensure the backup destination directory exists (create if needed)
    /// @param error Set to a user-facing reason when the destination is refused or cannot be made
    /// @return true if the directory exists or was created, false otherwise
    bool ensureDestinationDirectory(QString& error);
    /// @brief Report a pre-start failure, re-enable retry, leave page incomplete
    void failBackupPreflight(const QString& message);
    /// @brief Write every metadata sidecar; false if any write fails
    bool saveAllSidecars(UserProfileBackupWizard* wiz);

    /// @brief Mirror the SELECTED WiFi/Ethernet/AppData selections into the manifest
    ///        so they are covered by the manifest's SHA-256 digest. The standalone
    ///        *.json sidecars are written for inspection but are NOT integrity
    ///        protected, so restore trusts only these manifest-embedded copies.
    void recordNetworkSelectionsInManifest(UserProfileBackupWizard* wiz);

    /// @brief Save installed apps list to backup directory; false on write failure
    bool saveInstalledAppsToBackup(const QVector<InstalledAppInfo>& installedApps);

    /// @brief Save WiFi profiles to backup directory; false on write failure
    bool saveWifiProfilesToBackup(const QVector<WifiProfileInfo>& profiles);

    /// @brief Save Ethernet configs to backup directory; false on write failure
    bool saveEthernetConfigsToBackup(const QVector<EthernetConfigInfo>& configs);

    /// @brief Save app data sources to backup directory; false on write failure
    bool saveAppDataSourcesToBackup(const QVector<AppDataSourceInfo>& sources);

    /// @brief Create, connect, and start the backup worker
    void connectAndStartBackupWorker(SmartFilter smartFilter, PermissionMode permissionMode);

    BackupManifest& m_manifest;
    QVector<UserProfile> m_users;  // Owned copy: the source list may not outlive this page
    const QString& m_destinationPath;

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
    void setupScanControls(QVBoxLayout* layout);
    void setupAppsTree(QVBoxLayout* layout);
    void setupSelectionControls(QVBoxLayout* layout);
    void populateTree(const QVector<InstalledAppInfo>& apps);
    void updateParentCheckState(QTreeWidgetItem* parent);
    void updateNextButtonText();
    /// @brief Collect checked apps and commit them to the wizard + summary
    void commitAppSelection();

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
