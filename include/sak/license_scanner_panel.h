#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QCheckBox>
#include <QProgressBar>
#include <QLabel>
#include <QLineEdit>
#include <QVector>

// Forward declarations
namespace sak {
    class license_scanner_worker;
}

/**
 * @brief License key scanner feature panel
 * 
 * Scans Windows Registry and filesystem for software license keys.
 */
class LicenseScannerPanel : public QWidget {
    Q_OBJECT

public:
    explicit LicenseScannerPanel(QWidget* parent = nullptr);
    ~LicenseScannerPanel() override;

    LicenseScannerPanel(const LicenseScannerPanel&) = delete;
    LicenseScannerPanel& operator=(const LicenseScannerPanel&) = delete;
    LicenseScannerPanel(LicenseScannerPanel&&) = delete;
    LicenseScannerPanel& operator=(LicenseScannerPanel&&) = delete;

private Q_SLOTS:
    void on_scan_clicked();
    void on_stop_clicked();
    void on_export_clicked();
    void on_clear_clicked();
    
    // Worker signals
    void on_worker_started();
    void on_worker_finished();
    void on_worker_failed(int error_code, const QString& message);
    void on_worker_cancelled();
    void on_scan_progress(int current, int total, const QString& location);
    void on_license_found(const QString& product_name, const QString& license_key);
    void on_scan_complete(int total_count);

private:
    void setup_ui();
    void update_ui_state(bool scanning);
    
    // UI Controls - Scan Options
    QCheckBox* m_registry_checkbox;
    QCheckBox* m_filesystem_checkbox;
    QCheckBox* m_system_licenses_checkbox;
    QLineEdit* m_additional_paths_edit;
    
    // UI Controls - Actions
    QPushButton* m_scan_button;
    QPushButton* m_stop_button;
    QPushButton* m_export_button;
    QPushButton* m_clear_button;
    
    // UI Controls - Progress
    QProgressBar* m_progress_bar;
    QLabel* m_status_label;
    QLabel* m_results_label;
    
    // UI Controls - Results
    QTableWidget* m_results_table;
    
    // Worker
    sak::license_scanner_worker* m_worker;
    int m_license_count;
};

