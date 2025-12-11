#pragma once

#include <QWizard>
#include <QWizardPage>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QTableWidget>
#include <QCheckBox>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <memory>
#include <optional>

namespace sak {

class UserDataManager;

/**
 * @brief Wizard for guided restoration of application data
 * 
 * Multi-page wizard that guides users through:
 * - Page 1: Welcome and introduction
 * - Page 2: Select backup to restore
 * - Page 3: Configure restore options
 * - Page 4: Execute restore with progress
 * 
 * Uses UserDataManager for actual restore operations.
 */
class RestoreWizard : public QWizard {
    Q_OBJECT

public:
    explicit RestoreWizard(QWidget* parent = nullptr);
    ~RestoreWizard() override = default;

    RestoreWizard(const RestoreWizard&) = delete;
    RestoreWizard& operator=(const RestoreWizard&) = delete;
    RestoreWizard(RestoreWizard&&) = delete;
    RestoreWizard& operator=(RestoreWizard&&) = delete;

    enum PageId {
        PageWelcome = 0,
        PageSelectBackup,
        PageConfigure,
        PageProgress
    };

private:
    std::shared_ptr<UserDataManager> m_dataManager;
};

// ============================================================================
// Page 1: Welcome
// ============================================================================
class RestoreWelcomePage : public QWizardPage {
    Q_OBJECT

public:
    explicit RestoreWelcomePage(QWidget* parent = nullptr);
    ~RestoreWelcomePage() override = default;

    RestoreWelcomePage(const RestoreWelcomePage&) = delete;
    RestoreWelcomePage& operator=(const RestoreWelcomePage&) = delete;
    RestoreWelcomePage(RestoreWelcomePage&&) = delete;
    RestoreWelcomePage& operator=(RestoreWelcomePage&&) = delete;

private:
    void setupUI();
    
    QLabel* m_titleLabel{nullptr};
    QLabel* m_descriptionLabel{nullptr};
};

// ============================================================================
// Page 2: Select Backup
// ============================================================================
class RestoreSelectBackupPage : public QWizardPage {
    Q_OBJECT

public:
    explicit RestoreSelectBackupPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent = nullptr);
    ~RestoreSelectBackupPage() override = default;

    RestoreSelectBackupPage(const RestoreSelectBackupPage&) = delete;
    RestoreSelectBackupPage& operator=(const RestoreSelectBackupPage&) = delete;
    RestoreSelectBackupPage(RestoreSelectBackupPage&&) = delete;
    RestoreSelectBackupPage& operator=(RestoreSelectBackupPage&&) = delete;

    bool isComplete() const override;
    void initializePage() override;

    QStringList getSelectedBackups() const;

private Q_SLOTS:
    void onBrowseBackupDirectory();
    void onRefreshList();
    void onItemSelectionChanged();
    void onVerifyBackup();

private:
    void setupUI();
    void populateBackupList();
    
    std::shared_ptr<UserDataManager> m_dataManager;
    QLineEdit* m_backupDirEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QPushButton* m_refreshButton{nullptr};
    QPushButton* m_verifyButton{nullptr};
    QTableWidget* m_backupTableWidget{nullptr};
    QLabel* m_statusLabel{nullptr};
};

// ============================================================================
// Page 3: Configure Restore
// ============================================================================
class RestoreConfigurePage : public QWizardPage {
    Q_OBJECT

public:
    explicit RestoreConfigurePage(QWidget* parent = nullptr);
    ~RestoreConfigurePage() override = default;

    RestoreConfigurePage(const RestoreConfigurePage&) = delete;
    RestoreConfigurePage& operator=(const RestoreConfigurePage&) = delete;
    RestoreConfigurePage(RestoreConfigurePage&&) = delete;
    RestoreConfigurePage& operator=(RestoreConfigurePage&&) = delete;

    bool isComplete() const override;
    void initializePage() override;

    QString getRestoreLocation() const;
    bool getVerifyChecksum() const;
    bool getCreateBackup() const;
    bool getOverwriteExisting() const;
    bool getRestoreTimestamps() const;

private Q_SLOTS:
    void onBrowseDestination();
    void onUseOriginalLocation();

private:
    void setupUI();
    
    QLineEdit* m_destinationEdit{nullptr};
    QPushButton* m_browseButton{nullptr};
    QPushButton* m_originalButton{nullptr};
    QCheckBox* m_verifyCheckBox{nullptr};
    QCheckBox* m_createBackupCheckBox{nullptr};
    QCheckBox* m_overwriteCheckBox{nullptr};
    QCheckBox* m_timestampsCheckBox{nullptr};
    QLabel* m_warningLabel{nullptr};
};

// ============================================================================
// Page 4: Progress and Completion
// ============================================================================
class RestoreProgressPage : public QWizardPage {
    Q_OBJECT

public:
    explicit RestoreProgressPage(std::shared_ptr<UserDataManager> dataManager, QWidget* parent = nullptr);
    ~RestoreProgressPage() override = default;

    RestoreProgressPage(const RestoreProgressPage&) = delete;
    RestoreProgressPage& operator=(const RestoreProgressPage&) = delete;
    RestoreProgressPage(RestoreProgressPage&&) = delete;
    RestoreProgressPage& operator=(RestoreProgressPage&&) = delete;

    void initializePage() override;
    bool isComplete() const override;

private Q_SLOTS:
    void onOperationStarted(const QString& appName, const QString& operation);
    void onProgressUpdate(int current, int total, const QString& message);
    void onOperationCompleted(const QString& appName, bool success, const QString& message);
    void onOperationError(const QString& appName, const QString& error);

private:
    void setupUI();
    void startRestore();
    
    std::shared_ptr<UserDataManager> m_dataManager;
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_progressBar{nullptr};
    QTextEdit* m_logTextEdit{nullptr};
    bool m_restoreComplete{false};
    bool m_restoreSuccess{false};
    int m_completedRestores{0};
    int m_totalRestores{0};
};

} // namespace sak
