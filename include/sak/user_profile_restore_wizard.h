// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"
#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTreeWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QFileDialog>
#include <QHeaderView>

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
    
    QLabel* m_infoLabel;
    QLineEdit* m_backupPathEdit;
    QPushButton* m_browseButton;
    QLabel* m_manifestInfoLabel;
    
private Q_SLOTS:
    void onBrowseBackup();
    void onBackupPathChanged();
};

/**
 * @brief Page 2: User mapping (source users → destination users)
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
 * @brief Page 5: App Restore (install saved applications)
 */
class UserProfileRestoreAppRestorePage : public QWizardPage {
    Q_OBJECT

public:
    explicit UserProfileRestoreAppRestorePage(QWidget* parent = nullptr);
    void initializePage() override;
    bool isComplete() const override;

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
    /// @brief Install apps sequentially; returns (installed, failed) counts
    QPair<int,int> installAppsSequentially(const QVector<RestoreAppInfo>& apps);

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
 * @brief Page 6: Permission and conflict settings
 */
class UserProfileRestorePermissionSettingsPage : public QWizardPage {
    Q_OBJECT
    
public:
    explicit UserProfileRestorePermissionSettingsPage(QWidget* parent = nullptr);
    
    bool validatePage() override;
    
private:
    void setupUi();
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
    
private:
    QString m_backupPath;
    BackupManifest m_manifest;
    QVector<UserMapping> m_userMappings;
    QVector<RestoreAppInfo> m_restoreApps;
    ConflictResolution m_conflictResolution;
    PermissionMode m_permissionMode;
    bool m_verifyFiles;
    bool m_createBackup;
};

} // namespace sak
