// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/widget_helpers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextEdit>
#include <QWidget>

#include <memory>

class QVBoxLayout;
class QGroupBox;
class OrganizerWorker;
class DuplicateFinderWorker;

namespace sak {
class DetachableLogWindow;
class LogToggleSwitch;

/**
 * @brief Unified file management panel with tabbed organizer and duplicate finder
 *
 * Tab 1 -- File Organizer: sorts files by extension into categorized subdirectories
 * Tab 2 -- Duplicate Finder: scans directories for duplicate files using content hashing
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

    /** @brief Access the internal tab widget to add external tabs */
    QTabWidget* tabWidget() const { return m_tabs; }

    /** @brief Access the dynamic header widgets for runtime updates */
    const PanelHeaderWidgets& headerWidgets() const { return m_headerWidgets; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Organizer slots
    void onBrowseClicked();
    void onPreviewClicked();
    void onExecuteClicked();
    void onCancelClicked();
    void onAddCategoryClicked();
    void onRemoveCategoryClicked();
    void onResetCategoriesClicked();
    void onSettingsClicked();
    void onTargetPathChanged(const QString& path);

    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int errorCode, const QString& errorMessage);
    void onWorkerCancelled();
    void onFileProgress(int current, int total, const QString& filePath);
    void onPreviewResults(const QString& summary, int operationCount);

    // Duplicate detection slots
    void onDedupAddDirectoryClicked();
    void onDedupRemoveDirectoryClicked();
    void onDedupClearAllClicked();
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
    QWidget* createOrganizerTab();
    QGroupBox* createTargetDirectoryGroup();
    QGroupBox* createCategoryMappingGroup();
    void createOrganizerControls(QVBoxLayout* layout, QPushButton*& settingsBtn);
    QWidget* createDuplicateFinderTab();
    QGroupBox* createScanDirectoriesGroup();
    void createDedupControls(QVBoxLayout* layout, QPushButton*& settingsBtn);
    void setupDefaultCategories();
    QMap<QString, QStringList> getCategoryMapping() const;
    bool validateCategoryMapping() const;
    void setOperationRunning(bool running);
    void setDedupRunning(bool running);
    void updateDirectorySummary();
    void updateDedupDirectorySummary();
    void showScrollableResultsDialog(const QString& title, const QString& text);
    void logMessage(const QString& message);

    // Tab widget
    QTabWidget* m_tabs{nullptr};

    // Dynamic header labels
    PanelHeaderWidgets m_headerWidgets{};

    // Organizer widgets
    QLineEdit* m_target_path{nullptr};
    QPushButton* m_browse_button{nullptr};
    QLabel* m_dir_summary_label{nullptr};

    QTableWidget* m_category_table{nullptr};
    QPushButton* m_add_category_button{nullptr};
    QPushButton* m_remove_category_button{nullptr};
    QPushButton* m_reset_categories_button{nullptr};

    QComboBox* m_collision_strategy{nullptr};
    QCheckBox* m_preview_mode_checkbox{nullptr};
    QCheckBox* m_create_subdirs_checkbox{nullptr};

    QPushButton* m_preview_button{nullptr};
    QPushButton* m_execute_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    QProgressBar* m_progress_bar{nullptr};

    // Shared
    LogToggleSwitch* m_logToggle{nullptr};

    std::unique_ptr<OrganizerWorker> m_worker;
    bool m_operation_running{false};

    // Duplicate detection widgets
    QListWidget* m_dedup_directory_list{nullptr};
    QPushButton* m_dedup_add_button{nullptr};
    QPushButton* m_dedup_remove_button{nullptr};
    QPushButton* m_dedup_clear_button{nullptr};
    QPushButton* m_dedup_scan_button{nullptr};
    QPushButton* m_dedup_cancel_button{nullptr};
    QLabel* m_dedup_summary_label{nullptr};
    QLabel* m_dedup_results_label{nullptr};
    QProgressBar* m_dedup_progress_bar{nullptr};
    QSpinBox* m_dedup_min_size{nullptr};
    QCheckBox* m_dedup_recursive{nullptr};
    QCheckBox* m_dedup_parallel_hashing{nullptr};
    QSpinBox* m_dedup_thread_count{nullptr};

    std::unique_ptr<DuplicateFinderWorker> m_dedup_worker;
    bool m_dedup_running{false};
};

}  // namespace sak
