#pragma once

#include <QWizard>
#include <QWizardPage>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <memory>

namespace sak {

class UserDataManager;

/**
 * @brief Wizard for guided backup of application data
 * 
 * Multi-page wizard that guides users through:
 * - Page 1: Welcome and introduction
 * - Page 2: Select applications and data paths
 * - Page 3: Configure backup options
 * - Page 4: Execute backup with progress
 * 
 * Uses UserDataManager for actual backup operations.
 */
class BackupWizard : public QWizard {
    Q_OBJECT

public:
    explicit BackupWizard(QWidget* parent = nullptr);
    ~BackupWizard() override = default;

    BackupWizard(const BackupWizard&) = delete;
    BackupWizard& operator=(const BackupWizard&) = delete;
    BackupWizard(BackupWizard&&) = delete;
    BackupWizard& operator=(BackupWizard&&) = delete;

    enum PageId {
        PageWelcome = 0,
        PageSelectApps,
        PageConfigure,
        PageProgress
    };

private:
    std::shared_ptr<UserDataManager> m_dataManager;
};

// ============================================================================
// Page 1: Welcome
// ============================================================================
class BackupWelcomePage : public QWizardPage {
    Q_OBJECT

public:
    explicit BackupWelcomePage(QWidget* parent = nullptr);
    ~BackupWelcomePage() override = default;

    BackupWelcomePage(const BackupWelcomePage&) = delete;
    BackupWelcomePage& operator=(const BackupWelcomePage&) = delete;
    BackupWelcomePage(BackupWelcomePage&&) = delete;
    BackupWelcomePage& operator=(BackupWelcomePage&&) = delete;

private:
    void setupUI();
    
    QLabel* m_titleLabel{nullptr};
    QLabel* m_descriptionLabel{nullptr};
};

// ============================================================================
// Page 2: Select Applications
// ============================================================================
class BackupSelectAppsPage : public QWizardPage {
    Q_OBJECT

public:
    explicit BackupSelectAppsPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent = nullptr);
    ~BackupSelectAppsPage() override = default;

    BackupSelectAppsPage(const BackupSelectAppsPage&) = delete;
    BackupSelectAppsPage& operator=(const BackupSelectAppsPage&) = delete;
    BackupSelectAppsPage(BackupSelectAppsPage&&) = delete;
    BackupSelectAppsPage& operator=(BackupSelectAppsPage&&) = delete;

    bool isComplete() const override;
    void initializePage() override;

    QStringList getSelectedApps() const;
    QStringList getSelectedPaths() const;

private Q_SLOTS:
    void onScanCommonApps();
    void onBrowseCustomPath();
    void onItemSelectionChanged();

private:
    void setupUI();
    void populateCommonApps();
    
    std::shared_ptr<UserDataManager> m_dataManager;
    QListWidget* m_appListWidget{nullptr};
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_browseButton{nullptr};
    QLabel* m_statusLabel{nullptr};
};

// ============================================================================
// Page 3: Configure Backup
// ============================================================================
class BackupConfigurePage : public QWizardPage {
    Q_OBJECT

public:
    explicit BackupConfigurePage(QWidget* parent = nullptr);
    ~BackupConfigurePage() override = default;

    BackupConfigurePage(const BackupConfigurePage&) = delete;
    BackupConfigurePage& operator=(const BackupConfigurePage&) = delete;
    BackupConfigurePage(BackupConfigurePage&&) = delete;
    BackupConfigurePage& operator=(BackupConfigurePage&&) = delete;

    bool isComplete() const override;
    void initializePage() override;

    QString getBackupLocation() const;
    bool getCompressEnabled() const;
    bool getVerifyChecksum() const;
    QStringList getExclusionPatterns() const;

private Q_SLOTS:
    void onBrowseDestination();

private:
    void setupUI();
    
    QLineEdit* m_destinationEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QCheckBox* m_compressCheckBox{nullptr};
    QCheckBox* m_verifyCheckBox{nullptr};
    QTextEdit* m_exclusionEdit{nullptr};
    QLabel* m_sizeEstimateLabel{nullptr};
};

// ============================================================================
// Page 4: Progress and Completion
// ============================================================================
class BackupProgressPage : public QWizardPage {
    Q_OBJECT

public:
    explicit BackupProgressPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent = nullptr);
    ~BackupProgressPage() override = default;

    BackupProgressPage(const BackupProgressPage&) = delete;
    BackupProgressPage& operator=(const BackupProgressPage&) = delete;
    BackupProgressPage(BackupProgressPage&&) = delete;
    BackupProgressPage& operator=(BackupProgressPage&&) = delete;

    void initializePage() override;
    bool isComplete() const override;

private Q_SLOTS:
    void onOperationStarted(const QString& appName, const QString& operation);
    void onProgressUpdate(int current, int total, const QString& message);
    void onOperationCompleted(const QString& appName, bool success, const QString& message);
    void onOperationError(const QString& appName, const QString& error);

private:
    void setupUI();
    void startBackup();
    
    std::shared_ptr<UserDataManager> m_dataManager;
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_progressBar{nullptr};
    QTextEdit* m_logTextEdit{nullptr};
    bool m_backupComplete{false};
    bool m_backupSuccess{false};
    int m_completedBackups{0};
    int m_totalBackups{0};
};

} // namespace sak
