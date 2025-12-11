#pragma once

#include <QWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QCheckBox>
#include <QTextEdit>
#include <QTableView>
#include <QStandardItemModel>
#include <QLineEdit>
#include <QComboBox>
#include <memory>
#include <vector>

namespace sak {
class UserDataManager;
}

/**
 * @brief Windows User Profile Backup Panel
 * 
 * Provides comprehensive user profile backup and restore functionality:
 * - User-friendly backup wizard with automatic profile scanning
 * - Intelligent folder selection (Documents, Desktop, Pictures, etc.)
 * - Permission handling and elevation when needed
 * - Restore wizard with user mapping and conflict resolution
 * - Detailed operation logging and progress tracking
 * 
 * Similar to Application Migration panel but for user data.
 */
class BackupPanel : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent widget
     */
    explicit BackupPanel(QWidget* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~BackupPanel() override;

    // Disable copy and move
    BackupPanel(const BackupPanel&) = delete;
    BackupPanel& operator=(const BackupPanel&) = delete;
    BackupPanel(BackupPanel&&) = delete;
    BackupPanel& operator=(BackupPanel&&) = delete;

Q_SIGNALS:
    /**
     * @brief Status message for main window status bar
     */
    void status_message(const QString& message, int timeout_ms);

private Q_SLOTS:
    // Main actions
    void onBackupSelected();
    void onRestoreBackup();

private:
    void setupUi();
    void setupConnections();
    void appendLog(const QString& message);

    // UI Components
    QPushButton* m_backupButton{nullptr};
    QPushButton* m_restoreButton{nullptr};
    QLabel* m_statusLabel{nullptr};
    QTextEdit* m_logTextEdit{nullptr};
    
    // Data
    std::shared_ptr<sak::UserDataManager> m_dataManager;
};
