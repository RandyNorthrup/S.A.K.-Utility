#pragma once

#include <QWidget>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QProgressBar>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>

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
    void status_message(const QString& message, int timeout_ms);

private Q_SLOTS:
    void on_add_directory_clicked();
    void on_remove_directory_clicked();
    void on_scan_clicked();
    void on_cancel_clicked();
    
    void on_worker_started();
    void on_worker_finished();
    void on_worker_failed(int error_code, const QString& error_message);
    void on_worker_cancelled();
    void on_scan_progress(int current, int total, const QString& path);
    void on_results_ready(const QString& summary, int duplicate_count, qint64 wasted_space);

private:
    void setup_ui();
    void set_operation_running(bool running);
    void log_message(const QString& message);

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
    
    DuplicateFinderWorker* m_worker{nullptr};
    bool m_operation_running{false};
};
