// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPair>
#include <QPointer>
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

class WindowsUserScanner;
class UserProfileRestoreWorker;
class ChocolateyManager;

struct RestoreAppInfo {
    QString name;
    QString version;
    QString publisher;
    QString choco_package;
    QString category;
    bool selected{true};
};

/**
 * @brief Page 1: Welcome and select backup file
 */
class UserProfileRestoreWelcomePage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreWelcomePage(QWidget* parent = nullptr);

private:
    void setupUi();
    /// @brief Render a successfully loaded manifest's summary and store it in the wizard.
    void showLoadedManifest(const BackupManifest& manifest);

    QLabel* m_infoLabel;
    QLineEdit* m_backupPathEdit;
    QPushButton* m_browseButton;
    QLabel* m_manifestInfoLabel;

private Q_SLOTS:
    void onBrowseBackup();
    void onBackupPathChanged();
};

/**
 * @brief Page 2: User mapping (source users -> destination users)
 */
class UserProfileRestoreUserMappingPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreUserMappingPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool validatePage() override;

private:
    void setupUi();
    void loadMappingTable();
    void updateSummary();
    /// @brief Build a single UserMapping from a table row
    UserMapping buildMappingForRow(int row, const BackupManifest& manifest) const;

    QTableWidget* m_mappingTable;
    QLabel* m_summaryLabel;
    QPushButton* m_autoMapButton;

    WindowsUserScanner* m_scanner;
    QVector<UserProfile> m_destinationUsers;

private Q_SLOTS:
    void onAutoMap();
    void onMappingChanged(int row, int column);
};

/**
 * @brief Page 3: Merge configuration for each mapping
 */
class UserProfileRestoreMergeConfigPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreMergeConfigPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool validatePage() override;

private:
    void setupUi();
    void loadMergeTable();
    void updateSummary();

    QTableWidget* m_mergeTable;
    QLabel* m_summaryLabel;

private Q_SLOTS:
    void onMergeSettingsChanged(int row, int column);
};

/**
 * @brief Page 4: Folder selection per user
 */
class UserProfileRestoreFolderSelectionPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreFolderSelectionPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool validatePage() override;

private:
    void setupUi();
    void loadFolderTable();
    void updateSummary();

    QTableWidget* m_folderTable;
    QLabel* m_summaryLabel;
    QPushButton* m_selectAllButton;
    QPushButton* m_selectNoneButton;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();
    void onFolderSelectionChanged(int row, int column);
};

/**
 * @brief Page 4a: Application Data Restore Selection
 */
class UserProfileRestoreAppDataPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreAppDataPage(QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;
    /// Persist the per-source checkbox state onto the wizard so the restore worker
    /// skips app-data trees the user unchecked. Reconstructed from the tree (name,
    /// relative_path, checkstate) -- no display-only selection is discarded.
    bool validatePage() override;
    /// Reset the one-time load state on a Back navigation so a later backup change
    /// on the welcome page cannot leave this page showing a previous backup's data.
    void cleanupPage() override;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUi();
    void loadAppDataSources();
    void populateTree(const QVector<AppDataSourceInfo>& sources);
    void updateParentCheckState(QTreeWidgetItem* parent);

    QTreeWidget* m_appDataTree{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    bool m_loaded{false};
};

/**
 * @brief Page 5: App Restore (install saved applications)
 */
class UserProfileRestoreAppRestorePage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreAppRestorePage(QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;
    /// Reset the one-time load state on a Back navigation so a later backup change
    /// cannot offer a previous backup's app list. No-op while an install is running.
    void cleanupPage() override;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onInstallApps();

private:
    void setupUi();
    void loadApps();
    void populateTree(const QVector<RestoreAppInfo>& apps);
    /// @brief Populate one category's app items; returns count of selected apps
    int populateCategoryApps(QTreeWidgetItem* categoryItem,
                             const QVector<const RestoreAppInfo*>& apps,
                             int& totalWithPackage);
    void updateParentCheckState(QTreeWidgetItem* parent);
    /// @brief Collect checked apps from the tree widget
    QVector<RestoreAppInfo> collectSelectedApps() const;
    /// @brief Install apps sequentially on a worker thread; returns (installed,
    /// failed) counts. Static and page-agnostic: all UI updates are posted only
    /// through the guarding QPointer, so the task can safely outlive the page.
    static QPair<int, int> installAppsSequentially(const QVector<RestoreAppInfo>& apps,
                                                   QPointer<UserProfileRestoreAppRestorePage> page);

    QTreeWidget* m_appTree{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QPushButton* m_installButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    QProgressBar* m_progressBar{nullptr};
    QVector<RestoreAppInfo> m_apps;
    bool m_loaded{false};
    bool m_installing{false};
};

/**
 * @brief Page 5a: Restore WiFi Networks
 */
class UserProfileRestoreNetworksPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreNetworksPage(QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;
    /// Persist the per-profile checkbox state (including each profile's XML) onto
    /// the wizard so the restore worker re-imports exactly the selected WiFi
    /// profiles via netsh.
    bool validatePage() override;
    /// Reset the one-time load state (and retained xml_data) on a Back navigation so
    /// a later backup change cannot re-import a previous backup's WiFi profiles.
    void cleanupPage() override;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();
    void onItemChanged(QTreeWidgetItem* item, int column);

private:
    void setupUi();
    void loadNetworkProfiles();
    void populateTree(const QVector<WifiProfileInfo>& profiles);

    QTreeWidget* m_networkTree{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    // Retained so validatePage() can carry each profile's xml_data (never shown in
    // the tree) back to the wizard; row order matches the tree.
    QVector<WifiProfileInfo> m_profiles;
    bool m_loaded{false};
};

/**
 * @brief Page 5b: Restore Ethernet Settings
 */
class UserProfileRestoreEthernetPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreEthernetPage(QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;
    /// Persist the per-adapter checkbox state (with full static-IP/DNS fields) onto
    /// the wizard so the restore worker applies exactly the selected configs via
    /// netsh.
    bool validatePage() override;
    /// Reset the one-time load state (and retained IP/DNS fields) on a Back
    /// navigation so a later backup change cannot re-apply a previous backup's
    /// adapter settings.
    void cleanupPage() override;

private Q_SLOTS:
    void onSelectAll();
    void onSelectNone();

private:
    void setupUi();
    void loadEthernetConfigs();
    void populateTable(const QVector<EthernetConfigInfo>& configs);

    QTableWidget* m_ethernetTable{nullptr};
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_summaryLabel{nullptr};
    // Retained so validatePage() can carry the full IP/DNS fields back to the
    // wizard; row order matches the table.
    QVector<EthernetConfigInfo> m_configs;
    bool m_loaded{false};
};

/**
 * @brief Page 6: Permission and conflict settings
 */
class UserProfileRestorePermissionSettingsPage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestorePermissionSettingsPage(QWidget* parent = nullptr);

    bool validatePage() override;

private:
    void setupUi();
    /// @brief Build verify/backup options, summary label, and signal connections
    void setupUi_optionsAndConnections(QVBoxLayout* layout);
    void updateSummary();

    QComboBox* m_permissionModeCombo;
    QComboBox* m_conflictResolutionCombo;
    QCheckBox* m_verifyCheckBox;
    QCheckBox* m_createBackupCheckBox;
    QLabel* m_summaryLabel;

private Q_SLOTS:
    void onSettingsChanged();
};

/**
 * @brief Page 6: Execution and progress
 */
class UserProfileRestoreExecutePage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreExecutePage(QWidget* parent = nullptr);

    void initializePage() override;
    bool isComplete() const override;

private:
    void setupUi();
    void startRestore();
    /// @brief Wire the restore worker's progress/log/completion signals to the UI
    void connectRestoreWorkerSignals(UserProfileRestoreWorker* worker);

    QLabel* m_statusLabel;
    QProgressBar* m_overallProgressBar;
    QProgressBar* m_currentProgressBar;
    QLabel* m_currentOperationLabel;
    QTextEdit* m_logText;
    QPushButton* m_cancelButton;
    QPushButton* m_viewLogButton;

    UserProfileRestoreWorker* m_worker;
    bool m_restoreComplete;
    bool m_restoreSuccess;

private Q_SLOTS:
    void onStartRestore();
    void onCancelRestore();
    void onOverallProgress(int current, int total, qint64 bytes, qint64 totalBytes);
    void onFileProgress(int current, int total);
    void onStatusUpdate(const QString& username, const QString& operation);
    void onLogMessage(const QString& message, bool isWarning);
    void onRestoreComplete(bool success, const QString& message);
    void onViewLog();
};

/**
 * @brief Main wizard for restoring user profiles
 */
class UserProfileRestoreWizard : public QWizard {
    Q_OBJECT

public:
    explicit UserProfileRestoreWizard(QWidget* parent = nullptr);

    // Shared data between pages
    QString backupPath() const { return m_backupPath; }
    void setBackupPath(const QString& path) { m_backupPath = path; }

    BackupManifest manifest() const { return m_manifest; }
    void setManifest(const BackupManifest& manifest) { m_manifest = manifest; }

    QVector<UserMapping> userMappings() const { return m_userMappings; }
    void setUserMappings(const QVector<UserMapping>& mappings) { m_userMappings = mappings; }

    ConflictResolution conflictResolution() const { return m_conflictResolution; }
    void setConflictResolution(ConflictResolution mode) { m_conflictResolution = mode; }

    PermissionMode permissionMode() const { return m_permissionMode; }
    void setPermissionMode(PermissionMode mode) { m_permissionMode = mode; }

    bool verifyFiles() const { return m_verifyFiles; }
    void setVerifyFiles(bool verify) { m_verifyFiles = verify; }

    bool createBackup() const { return m_createBackup; }
    void setCreateBackup(bool backup) { m_createBackup = backup; }

    QVector<RestoreAppInfo> restoreApps() const { return m_restoreApps; }
    void setRestoreApps(const QVector<RestoreAppInfo>& apps) { m_restoreApps = apps; }

    QVector<WifiProfileInfo> wifiProfiles() const { return m_wifiProfiles; }
    void setWifiProfiles(const QVector<WifiProfileInfo>& profiles) { m_wifiProfiles = profiles; }

    QVector<EthernetConfigInfo> ethernetConfigs() const { return m_ethernetConfigs; }
    void setEthernetConfigs(const QVector<EthernetConfigInfo>& configs) {
        m_ethernetConfigs = configs;
    }

    QVector<AppDataSourceInfo> appDataSources() const { return m_appDataSources; }
    void setAppDataSources(const QVector<AppDataSourceInfo>& sources) {
        m_appDataSources = sources;
    }

private:
    QString m_backupPath;
    BackupManifest m_manifest;
    QVector<UserMapping> m_userMappings;
    QVector<RestoreAppInfo> m_restoreApps;
    QVector<WifiProfileInfo> m_wifiProfiles;
    QVector<EthernetConfigInfo> m_ethernetConfigs;
    QVector<AppDataSourceInfo> m_appDataSources;
    ConflictResolution m_conflictResolution;
    PermissionMode m_permissionMode;
    bool m_verifyFiles;
    bool m_createBackup;
};

}  // namespace sak
