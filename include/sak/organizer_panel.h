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
    void statusMessage(const QString& message, int timeout_ms);

private Q_SLOTS:
    void onBrowseClicked();
    void onPreviewClicked();
    void onExecuteClicked();
    void onCancelClicked();
    void onAddCategoryClicked();
    void onRemoveCategoryClicked();
    
    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int error_code, const QString& error_message);
    void onWorkerCancelled();
    void onFileProgress(int current, int total, const QString& file_path);
    void onPreviewResults(const QString& summary, int operation_count);

private:
    void setupUi();
    void setupDefaultCategories();
    QMap<QString, QStringList> getCategoryMapping() const;
    void setOperationRunning(bool running);
    void logMessage(const QString& message);

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
