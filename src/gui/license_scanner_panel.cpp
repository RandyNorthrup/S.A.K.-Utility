#include "sak/license_scanner_panel.h"
#include "sak/license_scanner_worker.h"
#include "sak/logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileDialog>
#include <QTextStream>

LicenseScannerPanel::LicenseScannerPanel(QWidget* parent)
    : QWidget(parent)
    , m_worker(nullptr)
    , m_license_count(0)
{
    setup_ui();
}

LicenseScannerPanel::~LicenseScannerPanel()
{
    if (m_worker) {
        if (m_worker->isRunning()) {
            m_worker->request_stop();
            m_worker->wait(5000);
        }
        m_worker->deleteLater();
    }
}

void LicenseScannerPanel::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(10);
    
    // Scan Options Group
    auto* options_group = new QGroupBox("Scan Options", this);
    auto* options_layout = new QVBoxLayout(options_group);
    
    m_registry_checkbox = new QCheckBox("Scan Windows Registry", this);
    m_registry_checkbox->setChecked(true);
    options_layout->addWidget(m_registry_checkbox);
    
    m_filesystem_checkbox = new QCheckBox("Scan Common Locations", this);
    m_filesystem_checkbox->setChecked(true);
    options_layout->addWidget(m_filesystem_checkbox);
    
    m_system_licenses_checkbox = new QCheckBox("Include System Directories", this);
    m_system_licenses_checkbox->setChecked(false);
    options_layout->addWidget(m_system_licenses_checkbox);
    
    auto* paths_layout = new QHBoxLayout();
    auto* paths_label = new QLabel("Additional Paths:", this);
    m_additional_paths_edit = new QLineEdit(this);
    m_additional_paths_edit->setPlaceholderText("Semicolon-separated paths (optional)");
    paths_layout->addWidget(paths_label);
    paths_layout->addWidget(m_additional_paths_edit);
    options_layout->addLayout(paths_layout);
    
    main_layout->addWidget(options_group);
    
    // Action Buttons
    auto* button_layout = new QHBoxLayout();
    
    m_scan_button = new QPushButton("Start Scan", this);
    m_stop_button = new QPushButton("Stop", this);
    m_export_button = new QPushButton("Export Results", this);
    m_clear_button = new QPushButton("Clear", this);
    
    m_stop_button->setEnabled(false);
    m_export_button->setEnabled(false);
    
    button_layout->addWidget(m_scan_button);
    button_layout->addWidget(m_stop_button);
    button_layout->addWidget(m_export_button);
    button_layout->addWidget(m_clear_button);
    button_layout->addStretch();
    
    main_layout->addLayout(button_layout);
    
    // Progress Group
    auto* progress_group = new QGroupBox("Scan Progress", this);
    auto* progress_layout = new QVBoxLayout(progress_group);
    
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setRange(0, 100);
    m_progress_bar->setValue(0);
    progress_layout->addWidget(m_progress_bar);
    
    m_status_label = new QLabel("Ready to scan", this);
    progress_layout->addWidget(m_status_label);
    
    m_results_label = new QLabel("No licenses found yet", this);
    m_results_label->setStyleSheet("font-weight: bold;");
    progress_layout->addWidget(m_results_label);
    
    main_layout->addWidget(progress_group);
    
    // Results Table
    auto* results_group = new QGroupBox("Found Licenses", this);
    auto* results_layout = new QVBoxLayout(results_group);
    
    m_results_table = new QTableWidget(this);
    m_results_table->setColumnCount(5);
    m_results_table->setHorizontalHeaderLabels({"Product Name", "License Key", "Version", "Installation Path", "Registry Path"});
    m_results_table->horizontalHeader()->setStretchLastSection(true);
    m_results_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results_table->setAlternatingRowColors(true);
    results_layout->addWidget(m_results_table);
    
    main_layout->addWidget(results_group);
    
    // Connect signals
    connect(m_scan_button, &QPushButton::clicked, this, &LicenseScannerPanel::on_scan_clicked);
    connect(m_stop_button, &QPushButton::clicked, this, &LicenseScannerPanel::on_stop_clicked);
    connect(m_export_button, &QPushButton::clicked, this, &LicenseScannerPanel::on_export_clicked);
    connect(m_clear_button, &QPushButton::clicked, this, &LicenseScannerPanel::on_clear_clicked);
}

void LicenseScannerPanel::on_scan_clicked()
{
    if (m_worker && m_worker->isRunning()) {
        QMessageBox::warning(this, "Scan In Progress", "A scan is already running.");
        return;
    }
    
    // Build configuration from UI
    sak::license_scanner_worker::config config;
    config.scan_registry = m_registry_checkbox->isChecked();
    config.scan_filesystem = m_filesystem_checkbox->isChecked();
    config.include_system_licenses = m_system_licenses_checkbox->isChecked();
    config.validate_keys = false; // Simplified validation
    
    // Parse additional paths
    QString paths_text = m_additional_paths_edit->text().trimmed();
    if (!paths_text.isEmpty()) {
        QStringList paths = paths_text.split(';', Qt::SkipEmptyParts);
        for (const QString& path : paths) {
            config.additional_paths.append(path.trimmed());
        }
    }
    
    if (!config.scan_registry && !config.scan_filesystem && config.additional_paths.isEmpty()) {
        QMessageBox::warning(this, "No Scan Sources", "Please select at least one scan option.");
        return;
    }
    
    // Clear previous results
    m_results_table->setRowCount(0);
    m_results_label->setText("Scanning...");
    m_license_count = 0;
    
    // Create and configure worker
    m_worker = new sak::license_scanner_worker(config, this);
    
    connect(m_worker, &sak::license_scanner_worker::started, this, &LicenseScannerPanel::on_worker_started);
    connect(m_worker, &sak::license_scanner_worker::finished, this, &LicenseScannerPanel::on_worker_finished);
    connect(m_worker, &sak::license_scanner_worker::failed, this, &LicenseScannerPanel::on_worker_failed);
    connect(m_worker, &sak::license_scanner_worker::cancelled, this, &LicenseScannerPanel::on_worker_cancelled);
    connect(m_worker, &sak::license_scanner_worker::scan_progress, this, &LicenseScannerPanel::on_scan_progress);
    connect(m_worker, &sak::license_scanner_worker::license_found, this, &LicenseScannerPanel::on_license_found);
    connect(m_worker, &sak::license_scanner_worker::scan_complete, this, &LicenseScannerPanel::on_scan_complete);
    
    m_worker->start();
}

void LicenseScannerPanel::on_stop_clicked()
{
    if (m_worker && m_worker->isRunning()) {
        m_worker->request_stop();
        m_status_label->setText("Stopping scan...");
        m_stop_button->setEnabled(false);
    }
}

void LicenseScannerPanel::on_export_clicked()
{
    QMessageBox::information(this, "Export", "Export functionality will be implemented with license data access in future update.");
}

void LicenseScannerPanel::on_clear_clicked()
{
    m_results_table->setRowCount(0);
    m_results_label->setText("No licenses found yet");
    m_progress_bar->setValue(0);
    m_status_label->setText("Ready to scan");
    m_export_button->setEnabled(false);
    m_license_count = 0;
}

void LicenseScannerPanel::on_worker_started()
{
    update_ui_state(true);
    m_status_label->setText("Scan started...");
    sak::log_info("License scan started");
}

void LicenseScannerPanel::on_worker_finished()
{
    update_ui_state(false);
    m_status_label->setText("Scan completed successfully");
    sak::log_info("License scan finished");
}

void LicenseScannerPanel::on_worker_failed(int error_code, const QString& message)
{
    Q_UNUSED(error_code);
    update_ui_state(false);
    m_status_label->setText("Scan failed");
    sak::log_error("License scan failed: {}", message.toStdString());
    QMessageBox::critical(this, "Scan Failed", "License scan failed: " + message);
}

void LicenseScannerPanel::on_worker_cancelled()
{
    update_ui_state(false);
    m_status_label->setText("Scan cancelled by user");
    sak::log_info("License scan cancelled");
}

void LicenseScannerPanel::on_scan_progress(int current, int total, const QString& location)
{
    if (total > 0) {
        int percentage = (current * 100) / total;
        m_progress_bar->setValue(percentage);
    }
    m_status_label->setText(QString("Scanning: %1").arg(location));
}

void LicenseScannerPanel::on_license_found(const QString& product_name, const QString& license_key)
{
    ++m_license_count;
    m_results_label->setText(QString("Found %1 license%2").arg(m_license_count).arg(m_license_count != 1 ? "s" : ""));
    
    // Add to results table
    int row = m_results_table->rowCount();
    m_results_table->insertRow(row);
    m_results_table->setItem(row, 0, new QTableWidgetItem(product_name));
    m_results_table->setItem(row, 1, new QTableWidgetItem(license_key));
}

void LicenseScannerPanel::on_scan_complete(int total_count)
{
    m_progress_bar->setValue(100);
    m_results_label->setText(QString("Scan complete: Found %1 license%2").arg(total_count).arg(total_count != 1 ? "s" : ""));
    
    if (total_count > 0) {
        m_export_button->setEnabled(true);
        
        QMessageBox::information(this, "Scan Complete",
            QString("License scan completed successfully.\n\nFound %1 license key%2.")
            .arg(total_count)
            .arg(total_count != 1 ? "s" : ""));
    } else {
        QMessageBox::information(this, "Scan Complete", "No license keys were found.");
    }
}

void LicenseScannerPanel::update_ui_state(bool scanning)
{
    m_scan_button->setEnabled(!scanning);
    m_stop_button->setEnabled(scanning);
    m_registry_checkbox->setEnabled(!scanning);
    m_filesystem_checkbox->setEnabled(!scanning);
    m_system_licenses_checkbox->setEnabled(!scanning);
    m_additional_paths_edit->setEnabled(!scanning);
}

