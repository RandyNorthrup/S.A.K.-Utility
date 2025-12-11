#pragma once

#include "sak/user_profile_types.h"
#include "sak/windows_user_scanner.h"
#include "sak/smart_file_filter.h"
#include <QWizard>
#include <QWizardPage>
#include <QLabel>
#include <QTableWidget>
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
 * @brief Wizard for backing up Windows user profiles
 * 
 * 6-Page Wizard:
 * 1. Welcome & Instructions
 * 2. Scan & Select Users
 * 3. Customize Per-User Data
 * 4. Smart Filter Configuration
 * 5. Backup Settings
 * 6. Execution & Progress
 */
class UserProfileBackupWizard : public QWizard {
    Q_OBJECT

public:
    enum PageId {
        Page_Welcome = 0,
        Page_SelectUsers,
        Page_CustomizeData,
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

private:
    BackupManifest m_manifest;
    QVector<UserProfile> m_scannedUsers;
    SmartFilter m_smartFilter;
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

} // namespace sak
