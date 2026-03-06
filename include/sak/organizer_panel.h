// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QTextEdit>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QListWidget>
#include <QMap>
#include <memory>

class QVBoxLayout;
class OrganizerWorker;
class DuplicateFinderWorker;

namespace sak {
class DetachableLogWindow;
class LogToggleSwitch;

/**
 * @brief Directory organizer and duplicate finder feature panel
 * 
 * Organizes files by extension into categorized subdirectories and
 * scans directories for duplicate files using content-based hashing.
 */
class OrganizerPanel : public QWidget {
    Q_OBJECT

public:
    explicit OrganizerPanel(QWidget* parent = nullptr);
    ~OrganizerPanel() override;

    OrganizerPanel(const OrganizerPanel&) = delete;
    OrganizerPanel& operator=(const OrganizerPanel&) = delete;
    OrganizerPanel(OrganizerPanel&&) = delete;
    OrganizerPanel& operator=(OrganizerPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onBrowseClicked();
    void onPreviewClicked();
    void onExecuteClicked();
    void onCancelClicked();
    void onAddCategoryClicked();
    void onRemoveCategoryClicked();
    void onResetCategoriesClicked();
    void onSettingsClicked();
    
    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int errorCode, const QString& errorMessage);
    void onWorkerCancelled();
    void onFileProgress(int current, int total, const QString& filePath);
    void onPreviewResults(const QString& summary, int operationCount);

    // Duplicate detection slots
    void onDedupAddDirectoryClicked();
    void onDedupRemoveDirectoryClicked();
    void onDedupScanClicked();
    void onDedupCancelClicked();
    void onDedupSettingsClicked();
    void onDedupWorkerStarted();
    void onDedupWorkerFinished();
    void onDedupWorkerFailed(int errorCode, const QString& errorMessage);
    void onDedupWorkerCancelled();
    void onDedupScanProgress(int current, int total, const QString& path);
    void onDedupResultsReady(const QString& summary, int duplicateCount, qint64 wastedSpace);

private:
    void setupUi();
    void setupUi_directoryAndCategories(QVBoxLayout* mainLayout);
    void setupUi_controlsAndConnections(QVBoxLayout* mainLayout);
    void setupUi_duplicateDetection(QVBoxLayout* mainLayout);
    void setupDefaultCategories();
    QMap<QString, QStringList> getCategoryMapping() const;
    bool validateCategoryMapping() const;
    void setOperationRunning(bool running);
    void setDedupRunning(bool running);
    void showScrollableResultsDialog(const QString& title, const QString& text);
    void logMessage(const QString& message);

    // Organizer widgets
    QLineEdit* m_target_path{nullptr};
    QPushButton* m_browse_button{nullptr};
    
    QTableWidget* m_category_table{nullptr};
    QPushButton* m_add_category_button{nullptr};
    QPushButton* m_remove_category_button{nullptr};
    QPushButton* m_reset_categories_button{nullptr};
    
    QComboBox* m_collision_strategy{nullptr};
    QCheckBox* m_preview_mode_checkbox{nullptr};
    
    QPushButton* m_preview_button{nullptr};
    QPushButton* m_execute_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    
    LogToggleSwitch* m_logToggle{nullptr};
    
    std::unique_ptr<OrganizerWorker> m_worker;
    bool m_operation_running{false};

    // Duplicate detection widgets
    QListWidget* m_dedup_directory_list{nullptr};
    QPushButton* m_dedup_add_button{nullptr};
    QPushButton* m_dedup_remove_button{nullptr};
    QPushButton* m_dedup_scan_button{nullptr};
    QPushButton* m_dedup_cancel_button{nullptr};
    QSpinBox* m_dedup_min_size{nullptr};
    QCheckBox* m_dedup_recursive{nullptr};
    QCheckBox* m_dedup_parallel_hashing{nullptr};
    QSpinBox* m_dedup_thread_count{nullptr};

    std::unique_ptr<DuplicateFinderWorker> m_dedup_worker;
    bool m_dedup_running{false};
};

} // namespace sak
