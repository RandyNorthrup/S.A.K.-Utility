#include "sak/duplicate_finder_panel.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>

DuplicateFinderPanel::DuplicateFinderPanel(QWidget* parent)
    : QWidget(parent)
    , m_worker(nullptr)
{
    setup_ui();
    sak::log_info("DuplicateFinderPanel initialized");
}

DuplicateFinderPanel::~DuplicateFinderPanel()
{
    if (m_worker) {
        m_worker->request_stop();
        if (!m_worker->wait(15000)) {
            sak::log_error("DuplicateFinderWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    sak::log_info("DuplicateFinderPanel destroyed");
}

void DuplicateFinderPanel::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(10, 10, 10, 10);
    main_layout->setSpacing(10);

    // Scan directories group
    auto* dir_group = new QGroupBox("Scan Directories", this);
    auto* dir_layout = new QVBoxLayout(dir_group);
    
    m_directory_list = new QListWidget(this);
    m_directory_list->setMinimumHeight(100);
    dir_layout->addWidget(m_directory_list);
    
    auto* button_layout = new QHBoxLayout();
    m_add_directory_button = new QPushButton("Add Directory", this);
    m_remove_directory_button = new QPushButton("Remove Selected", this);
    button_layout->addWidget(m_add_directory_button);
    button_layout->addWidget(m_remove_directory_button);
    button_layout->addStretch();
    dir_layout->addLayout(button_layout);
    
    main_layout->addWidget(dir_group);

    // Options group
    auto* options_group = new QGroupBox("Scan Options", this);
    auto* options_layout = new QHBoxLayout(options_group);
    
    options_layout->addWidget(new QLabel("Minimum Size (KB):", this));
    m_min_size_spinbox = new QSpinBox(this);
    m_min_size_spinbox->setMinimum(0);
    m_min_size_spinbox->setMaximum(1000000);
    m_min_size_spinbox->setValue(0);
    m_min_size_spinbox->setToolTip("Skip tiny files to speed up scanning (0 = check all files)");
    options_layout->addWidget(m_min_size_spinbox);
    
    m_recursive_checkbox = new QCheckBox("Recursive Scan", this);
    m_recursive_checkbox->setChecked(true);
    m_recursive_checkbox->setToolTip("Include all nested subfolders, not just the top-level directory");
    options_layout->addWidget(m_recursive_checkbox);
    
    options_layout->addStretch();
    main_layout->addWidget(options_group);

    // Progress group
    auto* progress_group = new QGroupBox("Progress", this);
    auto* progress_layout = new QVBoxLayout(progress_group);
    
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setFormat("%v / %m (%p%)");
    progress_layout->addWidget(m_progress_bar);
    
    m_status_label = new QLabel("Ready", this);
    m_status_label->setStyleSheet("font-weight: 600; color: #1e293b;");
    progress_layout->addWidget(m_status_label);
    
    m_results_label = new QLabel("", this);
    m_results_label->setStyleSheet("color: #16a34a; font-weight: 600;");
    progress_layout->addWidget(m_results_label);
    
    main_layout->addWidget(progress_group);

    // Control buttons
    auto* control_layout = new QHBoxLayout();
    control_layout->addStretch();
    
    m_scan_button = new QPushButton("Start Scan", this);
    m_scan_button->setMinimumWidth(120);
    control_layout->addWidget(m_scan_button);
    
    m_cancel_button = new QPushButton("Cancel", this);
    m_cancel_button->setMinimumWidth(120);
    m_cancel_button->setEnabled(false);
    control_layout->addWidget(m_cancel_button);
    
    main_layout->addLayout(control_layout);

    // Log viewer
    auto* log_group = new QGroupBox("Log", this);
    auto* log_layout = new QVBoxLayout(log_group);
    
    m_log_viewer = new QTextEdit(this);
    m_log_viewer->setReadOnly(true);
    m_log_viewer->setMaximumHeight(150);
    m_log_viewer->setPlaceholderText("Operation log will appear here...");
    log_layout->addWidget(m_log_viewer);
    
    main_layout->addWidget(log_group);

    // Connect signals
    connect(m_add_directory_button, &QPushButton::clicked, this, &DuplicateFinderPanel::on_add_directory_clicked);
    connect(m_remove_directory_button, &QPushButton::clicked, this, &DuplicateFinderPanel::on_remove_directory_clicked);
    connect(m_scan_button, &QPushButton::clicked, this, &DuplicateFinderPanel::on_scan_clicked);
    connect(m_cancel_button, &QPushButton::clicked, this, &DuplicateFinderPanel::on_cancel_clicked);
}

void DuplicateFinderPanel::on_add_directory_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Directory to Scan",
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_directory_list->addItem(dir);
        log_message(QString("Added directory: %1").arg(dir));
    }
}

void DuplicateFinderPanel::on_remove_directory_clicked()
{
    auto selected = m_directory_list->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Please select a directory to remove.");
        return;
    }

    for (auto* item : selected) {
        delete item;
    }
}

void DuplicateFinderPanel::on_scan_clicked()
{
    if (m_directory_list->count() == 0) {
        QMessageBox::warning(this, "Validation Error", "Please add at least one directory to scan.");
        return;
    }

    // Clean up previous worker
    m_worker.reset();

    // Build configuration
    DuplicateFinderWorker::Config config;
    for (int i = 0; i < m_directory_list->count(); ++i) {
        config.scan_directories.push_back(m_directory_list->item(i)->text());
    }
    config.minimum_file_size = m_min_size_spinbox->value() * 1024; // Convert KB to bytes
    config.recursive_scan = m_recursive_checkbox->isChecked();

    // Create and configure worker
    m_worker = std::make_unique<DuplicateFinderWorker>(config, this);

    connect(m_worker.get(), &DuplicateFinderWorker::started, this, &DuplicateFinderPanel::on_worker_started);
    connect(m_worker.get(), &DuplicateFinderWorker::finished, this, &DuplicateFinderPanel::on_worker_finished);
    connect(m_worker.get(), &DuplicateFinderWorker::failed, this, &DuplicateFinderPanel::on_worker_failed);
    connect(m_worker.get(), &DuplicateFinderWorker::cancelled, this, &DuplicateFinderPanel::on_worker_cancelled);
    connect(m_worker.get(), &DuplicateFinderWorker::scan_progress, this, &DuplicateFinderPanel::on_scan_progress);
    connect(m_worker.get(), &DuplicateFinderWorker::results_ready, this, &DuplicateFinderPanel::on_results_ready);

    set_operation_running(true);
    m_status_label->setText("Status: Starting scan...");
    m_results_label->clear();
    m_worker->start();

    sak::log_info("Duplicate finder scan initiated");
}

void DuplicateFinderPanel::on_cancel_clicked()
{
    if (m_worker != nullptr) {
        m_worker->request_stop();
        log_message("Cancellation requested...");
        m_status_label->setText("Status: Cancelling...");
        sak::log_info("Duplicate finder cancellation requested by user");
    }
}

void DuplicateFinderPanel::on_worker_started()
{
    log_message("Duplicate file scan started");
    m_status_label->setText("Status: Scanning...");
    Q_EMIT status_message("Duplicate scan in progress", 0);
}

void DuplicateFinderPanel::on_worker_finished()
{
    set_operation_running(false);
    m_status_label->setText("Status: Scan complete");
    log_message("Scan completed successfully");
    sak::log_info("Duplicate finder scan completed successfully");
}

void DuplicateFinderPanel::on_worker_failed(int error_code, const QString& error_message)
{
    set_operation_running(false);
    m_status_label->setText("Status: Failed");
    m_results_label->clear();
    log_message(QString("Scan failed: Error %1: %2").arg(error_code).arg(error_message));
    QMessageBox::warning(this, "Scan Failed", QString("Error %1: %2").arg(error_code).arg(error_message));
    sak::log_error("Duplicate finder scan failed: {}", error_message.toStdString());
}

void DuplicateFinderPanel::on_worker_cancelled()
{
    set_operation_running(false);
    log_message("Scan cancelled by user");
    m_status_label->setText("Status: Cancelled");
    m_results_label->clear();
    Q_EMIT status_message("Scan cancelled", 3000);
}

void DuplicateFinderPanel::on_scan_progress(int current, int total, const QString& path)
{
    m_progress_bar->setMaximum(total);
    m_progress_bar->setValue(current);
    
    QFileInfo info(path);
    m_status_label->setText(QString("Scanning: %1").arg(info.fileName()));
}

void DuplicateFinderPanel::on_results_ready(const QString& summary, int duplicate_count, qint64 wasted_space)
{
    QString results_text = QString("Found %1 duplicate files, %2 MB wasted space")
        .arg(duplicate_count)
        .arg(wasted_space / (1024.0 * 1024.0), 0, 'f', 2);
    
    m_results_label->setText(results_text);
    log_message(results_text);
    
    QMessageBox::information(this, "Scan Results", summary);
}

void DuplicateFinderPanel::set_operation_running(bool running)
{
    m_operation_running = running;
    
    m_directory_list->setEnabled(!running);
    m_add_directory_button->setEnabled(!running);
    m_remove_directory_button->setEnabled(!running);
    m_min_size_spinbox->setEnabled(!running);
    m_recursive_checkbox->setEnabled(!running);
    
    m_scan_button->setEnabled(!running);
    m_cancel_button->setEnabled(running);
}

void DuplicateFinderPanel::log_message(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m_log_viewer->append(QString("[%1] %2").arg(timestamp, message));
}

