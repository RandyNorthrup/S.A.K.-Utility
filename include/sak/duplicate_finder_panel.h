#pragma once

#include <QWidget>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QProgressBar>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <memory>

class DuplicateFinderWorker;

/**
 * @brief Duplicate file finder feature panel
 * 
 * Scans directories for duplicate files and reports results.
 */
class DuplicateFinderPanel : public QWidget {
    Q_OBJECT

public:
    explicit DuplicateFinderPanel(QWidget* parent = nullptr);
    ~DuplicateFinderPanel() override;

    DuplicateFinderPanel(const DuplicateFinderPanel&) = delete;
    DuplicateFinderPanel& operator=(const DuplicateFinderPanel&) = delete;
    DuplicateFinderPanel(DuplicateFinderPanel&&) = delete;
    DuplicateFinderPanel& operator=(DuplicateFinderPanel&&) = delete;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);

private Q_SLOTS:
    void onAddDirectoryClicked();
    void onRemoveDirectoryClicked();
    void onScanClicked();
    void onCancelClicked();
    
    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int error_code, const QString& error_message);
    void onWorkerCancelled();
    void onScanProgress(int current, int total, const QString& path);
    void onResultsReady(const QString& summary, int duplicate_count, qint64 wasted_space);

private:
    void setupUi();
    void setOperationRunning(bool running);
    void logMessage(const QString& message);

    QListWidget* m_directory_list{nullptr};
    QPushButton* m_add_directory_button{nullptr};
    QPushButton* m_remove_directory_button{nullptr};
    
    QSpinBox* m_min_size_spinbox{nullptr};
    QCheckBox* m_recursive_checkbox{nullptr};
    
    QProgressBar* m_progress_bar{nullptr};
    QLabel* m_status_label{nullptr};
    QLabel* m_results_label{nullptr};
    
    QPushButton* m_scan_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    
    QTextEdit* m_log_viewer{nullptr};
    
    std::unique_ptr<DuplicateFinderWorker> m_worker;
    bool m_operation_running{false};
};
