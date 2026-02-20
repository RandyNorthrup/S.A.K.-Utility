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
    setupUi();
    sak::logInfo("DuplicateFinderPanel initialized");
}

DuplicateFinderPanel::~DuplicateFinderPanel()
{
    if (m_worker) {
        m_worker->requestStop();
        if (!m_worker->wait(15000)) {
            sak::logError("DuplicateFinderWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    sak::logInfo("DuplicateFinderPanel destroyed");
}

void DuplicateFinderPanel::setupUi()
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
    connect(m_add_directory_button, &QPushButton::clicked, this, &DuplicateFinderPanel::onAddDirectoryClicked);
    connect(m_remove_directory_button, &QPushButton::clicked, this, &DuplicateFinderPanel::onRemoveDirectoryClicked);
    connect(m_scan_button, &QPushButton::clicked, this, &DuplicateFinderPanel::onScanClicked);
    connect(m_cancel_button, &QPushButton::clicked, this, &DuplicateFinderPanel::onCancelClicked);
}

void DuplicateFinderPanel::onAddDirectoryClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Directory to Scan",
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_directory_list->addItem(dir);
        logMessage(QString("Added directory: %1").arg(dir));
    }
}

void DuplicateFinderPanel::onRemoveDirectoryClicked()
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

void DuplicateFinderPanel::onScanClicked()
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
        config.scanDirectories.push_back(m_directory_list->item(i)->text());
    }
    config.minimum_file_size = m_min_size_spinbox->value() * 1024; // Convert KB to bytes
    config.recursive_scan = m_recursive_checkbox->isChecked();

    // Create and configure worker
    m_worker = std::make_unique<DuplicateFinderWorker>(config, this);

    connect(m_worker.get(), &DuplicateFinderWorker::started, this, &DuplicateFinderPanel::onWorkerStarted);
    connect(m_worker.get(), &DuplicateFinderWorker::finished, this, &DuplicateFinderPanel::onWorkerFinished);
    connect(m_worker.get(), &DuplicateFinderWorker::failed, this, &DuplicateFinderPanel::onWorkerFailed);
    connect(m_worker.get(), &DuplicateFinderWorker::cancelled, this, &DuplicateFinderPanel::onWorkerCancelled);
    connect(m_worker.get(), &DuplicateFinderWorker::scanProgress, this, &DuplicateFinderPanel::onScanProgress);
    connect(m_worker.get(), &DuplicateFinderWorker::resultsReady, this, &DuplicateFinderPanel::onResultsReady);

    setOperationRunning(true);
    m_status_label->setText("Status: Starting scan...");
    m_results_label->clear();
    m_worker->start();

    sak::logInfo("Duplicate finder scan initiated");
}

void DuplicateFinderPanel::onCancelClicked()
{
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage("Cancellation requested...");
        m_status_label->setText("Status: Cancelling...");
        sak::logInfo("Duplicate finder cancellation requested by user");
    }
}

void DuplicateFinderPanel::onWorkerStarted()
{
    logMessage("Duplicate file scan started");
    m_status_label->setText("Status: Scanning...");
    Q_EMIT statusMessage("Duplicate scan in progress", 0);
}

void DuplicateFinderPanel::onWorkerFinished()
{
    setOperationRunning(false);
    m_status_label->setText("Status: Scan complete");
    logMessage("Scan completed successfully");
    sak::logInfo("Duplicate finder scan completed successfully");
}

void DuplicateFinderPanel::onWorkerFailed(int error_code, const QString& error_message)
{
    setOperationRunning(false);
    m_status_label->setText("Status: Failed");
    m_results_label->clear();
    logMessage(QString("Scan failed: Error %1: %2").arg(error_code).arg(error_message));
    QMessageBox::warning(this, "Scan Failed", QString("Error %1: %2").arg(error_code).arg(error_message));
    sak::logError("Duplicate finder scan failed: {}", error_message.toStdString());
}

void DuplicateFinderPanel::onWorkerCancelled()
{
    setOperationRunning(false);
    logMessage("Scan cancelled by user");
    m_status_label->setText("Status: Cancelled");
    m_results_label->clear();
    Q_EMIT statusMessage("Scan cancelled", 3000);
}

void DuplicateFinderPanel::onScanProgress(int current, int total, const QString& path)
{
    m_progress_bar->setMaximum(total);
    m_progress_bar->setValue(current);
    
    QFileInfo info(path);
    m_status_label->setText(QString("Scanning: %1").arg(info.fileName()));
}

void DuplicateFinderPanel::onResultsReady(const QString& summary, int duplicate_count, qint64 wasted_space)
{
    QString results_text = QString("Found %1 duplicate files, %2 MB wasted space")
        .arg(duplicate_count)
        .arg(wasted_space / (1024.0 * 1024.0), 0, 'f', 2);
    
    m_results_label->setText(results_text);
    logMessage(results_text);
    
    QMessageBox::information(this, "Scan Results", summary);
}

void DuplicateFinderPanel::setOperationRunning(bool running)
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

void DuplicateFinderPanel::logMessage(const QString& message)
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    m_log_viewer->append(QString("[%1] %2").arg(timestamp, message));
}

