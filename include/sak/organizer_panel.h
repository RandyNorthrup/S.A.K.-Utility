#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QTextEdit>
#include <QProgressBar>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QMap>
#include <memory>

class OrganizerWorker;

/**
 * @brief Directory organizer feature panel
 * 
 * Organizes files by extension into categorized subdirectories.
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

Q_SIGNALS:
    void status_message(const QString& message, int timeout_ms);

private Q_SLOTS:
    void on_browse_clicked();
    void on_preview_clicked();
    void on_execute_clicked();
    void on_cancel_clicked();
    void on_add_category_clicked();
    void on_remove_category_clicked();
    
    void on_worker_started();
    void on_worker_finished();
    void on_worker_failed(int error_code, const QString& error_message);
    void on_worker_cancelled();
    void on_file_progress(int current, int total, const QString& file_path);
    void on_preview_results(const QString& summary, int operation_count);

private:
    void setup_ui();
    void setup_default_categories();
    QMap<QString, QStringList> get_category_mapping() const;
    void set_operation_running(bool running);
    void log_message(const QString& message);

    QLineEdit* m_target_path{nullptr};
    QPushButton* m_browse_button{nullptr};
    
    QTableWidget* m_category_table{nullptr};
    QPushButton* m_add_category_button{nullptr};
    QPushButton* m_remove_category_button{nullptr};
    
    QComboBox* m_collision_strategy{nullptr};
    QCheckBox* m_preview_mode_checkbox{nullptr};
    
    QProgressBar* m_progress_bar{nullptr};
    QLabel* m_status_label{nullptr};
    
    QPushButton* m_preview_button{nullptr};
    QPushButton* m_execute_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    
    QTextEdit* m_log_viewer{nullptr};
    
    std::unique_ptr<OrganizerWorker> m_worker;
    bool m_operation_running{false};
};
