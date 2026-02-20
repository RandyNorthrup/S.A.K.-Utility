// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QWidget>
#include <QTableView>
#include <QToolBar>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QComboBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QStandardItemModel>
#include <QFuture>
#include <memory>

// Forward declarations
namespace sak {
    class AppScanner;
    class ChocolateyManager;
    class PackageMatcher;
    class MigrationReport;
    class AppMigrationWorker;
    class BackupWizard;
    class RestoreWizard;

/**
 * @brief Application Migration Panel
 * 
 * Provides comprehensive UI for scanning installed applications,
 * matching to Chocolatey packages, backing up user data, installing
 * packages, and restoring user data on target system.
 * 
 * Features:
 * - Scan installed Windows applications
 * - Match apps to Chocolatey packages
 * - Generate/load migration reports
 * - Backup/restore user application data
 * - Install packages with retry logic
 * - Real-time progress tracking
 * - Version locking support
 * 
 * Thread-Safety: UI updates occur on main thread.
 * Worker operations use separate threads with signal/slot communication.
 */
class AppMigrationPanel : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Migration entry for table display
     */
    struct MigrationEntry {
        bool selected{true};
        QString app_name;
        QString version;
        QString publisher;
        QString install_location;
        QString choco_package;
        bool choco_available{false};
        QString match_confidence;  // High/Medium/Low/Manual
        double match_score{0.0};
        QString match_type;         // exact/fuzzy/search/manual/none
        QString available_version;
        bool version_locked{false};
        QString locked_version;
        QString status;  // Pending/Installing/Installed/Failed/Skipped
        int progress{0};  // 0-100
        QString error_message;
        bool has_user_data{false};
        qint64 data_size{0};
    };

    /**
     * @brief Construct app migration panel
     * @param parent Parent widget (typically MainWindow)
     */
    explicit AppMigrationPanel(QWidget* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~AppMigrationPanel() override;

    // Disable copy and move
    AppMigrationPanel(const AppMigrationPanel&) = delete;
    AppMigrationPanel& operator=(const AppMigrationPanel&) = delete;
    AppMigrationPanel(AppMigrationPanel&&) = delete;
    AppMigrationPanel& operator=(AppMigrationPanel&&) = delete;

Q_SIGNALS:
    /**
     * @brief Signal emitted when status message should be shown
     * @param message Status message
     * @param timeout_ms Timeout in milliseconds (0 = permanent)
     */
    void status_message(const QString& message, int timeout_ms);

    /**
     * @brief Signal emitted when progress updates
     * @param current Current progress value
     * @param maximum Maximum progress value
     */
    void progress_updated(int current, int maximum);

private Q_SLOTS:
    // Toolbar actions
    void onScanApps();
    void onMatchPackages();
    void onBackupData();
    void onInstallPackages();
    void onRestoreData();
    void onGenerateReport();
    void onLoadReport();
    void onRefresh();
    
    // Selection actions
    void onSelectAll();
    void onSelectNone();
    void onSelectMatched();
    void onInvertSelection();
    
    // Filter actions
    void onFilterChanged(const QString& text);
    void onConfidenceFilterChanged(int index);
    
    // Table interactions
    void onTableItemChanged(QStandardItem* item);
    
private:
    void setupUI();
    void setupToolbar();
    void setupTable();
    QWidget* setupStatusBar();
    void setupConnections();
    
    void updateTableFromEntries();
    void updateEntry(int row);
    void clearTable();
    
    void enableControls(bool enabled);
    void updateStatusSummary();
    
    QVector<MigrationEntry> getSelectedEntries() const;
    void setEntryStatus(int row, const QString& status, int progress = 0);
    
    // UI Components
    QToolBar* m_toolbar{nullptr};
    QTableView* m_tableView{nullptr};
    QStandardItemModel* m_tableModel{nullptr};
    
    // Toolbar buttons
    QPushButton* m_scanButton{nullptr};
    QPushButton* m_matchButton{nullptr};
    QPushButton* m_backupButton{nullptr};
    QPushButton* m_installButton{nullptr};
    QPushButton* m_restoreButton{nullptr};
    QPushButton* m_reportButton{nullptr};
    QPushButton* m_loadButton{nullptr};
    QPushButton* m_refreshButton{nullptr};
    
    // Selection controls
    QPushButton* m_selectAllButton{nullptr};
    QPushButton* m_selectNoneButton{nullptr};
    QPushButton* m_selectMatchedButton{nullptr};
    
    // Filter controls
    QLineEdit* m_filterEdit{nullptr};
    QComboBox* m_confidenceFilter{nullptr};
    
    // Status bar
    QLabel* m_statusLabel{nullptr};
    QProgressBar* m_progressBar{nullptr};
    QLabel* m_summaryLabel{nullptr};
    
    // Log
    QTextEdit* m_logTextEdit{nullptr};
    
    // Data
    QVector<MigrationEntry> m_entries;
    std::shared_ptr<MigrationReport> m_activeReport;
    
    // Backend components
    std::shared_ptr<sak::AppScanner> m_scanner;
    std::shared_ptr<sak::ChocolateyManager> m_chocoManager;
    std::shared_ptr<sak::PackageMatcher> m_matcher;
    std::shared_ptr<sak::AppMigrationWorker> m_worker;
    
    // Async operations
    QFuture<void> m_matchingFuture;
    std::atomic<bool> m_matchingInProgress{false};
    
    // State
    bool m_scanInProgress{false};
    bool m_installInProgress{false};
}; // class AppMigrationPanel

} // namespace sak
