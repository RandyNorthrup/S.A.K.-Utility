// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/duplicate_finder_panel.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/logger.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QScrollArea>
#include <QFrame>
#include <QDialog>
#include <QFormLayout>

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
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(12, 12, 12, 12);
    main_layout->setSpacing(10);

    scroll_area->setWidget(content_widget);
    root_layout->addWidget(scroll_area);

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

    // Options widgets (hidden — managed via Settings modal)
    m_min_size_spinbox = new QSpinBox(this);
    m_min_size_spinbox->setMinimum(0);
    m_min_size_spinbox->setMaximum(1000000);
    m_min_size_spinbox->setValue(0);
    m_min_size_spinbox->setToolTip("Skip tiny files to speed up scanning (0 = check all files)");
    m_min_size_spinbox->setVisible(false);

    m_recursive_checkbox = new QCheckBox("Recursive Scan", this);
    m_recursive_checkbox->setChecked(true);
    m_recursive_checkbox->setToolTip("Include all nested subfolders, not just the top-level directory");
    m_recursive_checkbox->setVisible(false);

    // Control buttons
    auto* control_layout = new QHBoxLayout();

    auto* settingsBtn = new QPushButton("Settings", this);
    connect(settingsBtn, &QPushButton::clicked, this, &DuplicateFinderPanel::onSettingsClicked);
    control_layout->addWidget(settingsBtn);

    control_layout->addStretch();
    
    m_scan_button = new QPushButton("Start Scan", this);
    m_scan_button->setMinimumWidth(120);
    control_layout->addWidget(m_scan_button);
    
    m_cancel_button = new QPushButton("Cancel", this);
    m_cancel_button->setMinimumWidth(120);
    m_cancel_button->setEnabled(false);
    control_layout->addWidget(m_cancel_button);
    
    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);
    control_layout->insertWidget(1, m_logToggle);

    main_layout->addLayout(control_layout);

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
    Q_EMIT statusMessage("Starting scan...", 0);
    m_worker->start();

    sak::logInfo("Duplicate finder scan initiated");
}

void DuplicateFinderPanel::onCancelClicked()
{
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage("Cancellation requested...");
        Q_EMIT statusMessage("Cancelling...", 0);
        sak::logInfo("Duplicate finder cancellation requested by user");
    }
}

void DuplicateFinderPanel::onWorkerStarted()
{
    logMessage("Duplicate file scan started");
    Q_EMIT statusMessage("Duplicate scan in progress", 0);
}

void DuplicateFinderPanel::onWorkerFinished()
{
    setOperationRunning(false);
    Q_EMIT statusMessage("Scan complete", 5000);
    Q_EMIT progressUpdate(100, 100);
    logMessage("Scan completed successfully");
    sak::logInfo("Duplicate finder scan completed successfully");
}

void DuplicateFinderPanel::onWorkerFailed(int error_code, const QString& error_message)
{
    setOperationRunning(false);
    Q_EMIT statusMessage("Scan failed", 5000);
    Q_EMIT progressUpdate(0, 100);
    logMessage(QString("Scan failed: Error %1: %2").arg(error_code).arg(error_message));
    QMessageBox::warning(this, "Scan Failed", QString("Error %1: %2").arg(error_code).arg(error_message));
    sak::logError("Duplicate finder scan failed: {}", error_message.toStdString());
}

void DuplicateFinderPanel::onWorkerCancelled()
{
    setOperationRunning(false);
    logMessage("Scan cancelled by user");
    Q_EMIT statusMessage("Scan cancelled", 3000);
    Q_EMIT progressUpdate(0, 100);
}

void DuplicateFinderPanel::onScanProgress(int current, int total, const QString& path)
{
    Q_EMIT progressUpdate(current, total);
    
    QFileInfo info(path);
    Q_EMIT statusMessage(QString("Scanning: %1").arg(info.fileName()), 0);
}

void DuplicateFinderPanel::onResultsReady(const QString& summary, int duplicate_count, qint64 wasted_space)
{
    QString results_text = QString("Found %1 duplicate files, %2 MB wasted space")
        .arg(duplicate_count)
        .arg(wasted_space / (1024.0 * 1024.0), 0, 'f', 2);
    
    Q_EMIT statusMessage(results_text, 10000);
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
    Q_EMIT logOutput(message);
}

void DuplicateFinderPanel::onSettingsClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Duplicate Finder Settings"));
    dialog.setMinimumWidth(380);

    auto* layout = new QFormLayout(&dialog);

    auto* minSizeSpin = new QSpinBox(&dialog);
    minSizeSpin->setMinimum(0);
    minSizeSpin->setMaximum(1000000);
    minSizeSpin->setValue(m_min_size_spinbox->value());
    minSizeSpin->setSuffix(tr(" KB"));
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Minimum File Size:"),
            tr("Skip tiny files to speed up scanning (0 = check all files)"), &dialog),
        minSizeSpin);

    auto* recursiveCheck = new QCheckBox(tr("Include all nested subfolders"), &dialog);
    recursiveCheck->setChecked(m_recursive_checkbox->isChecked());
    layout->addRow(
        sak::InfoButton::createInfoLabel(tr("Recursive Scan:"),
            tr("Scan all subdirectories recursively, not just the top-level folder"), &dialog),
        recursiveCheck);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto* okBtn = new QPushButton(tr("OK"), &dialog);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dialog);
    connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addRow(btnLayout);

    if (dialog.exec() == QDialog::Accepted) {
        m_min_size_spinbox->setValue(minSizeSpin->value());
        m_recursive_checkbox->setChecked(recursiveCheck->isChecked());
    }
}

