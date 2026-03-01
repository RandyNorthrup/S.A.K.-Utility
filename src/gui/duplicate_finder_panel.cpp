// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file duplicate_finder_panel.cpp
/// @brief Implements the duplicate file finder panel UI with scan controls

#include "sak/duplicate_finder_panel.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/logger.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

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

namespace sak {

DuplicateFinderPanel::DuplicateFinderPanel(QWidget* parent)
    : QWidget(parent)
    , m_worker(nullptr)
{
    setupUi();
    logInfo("DuplicateFinderPanel initialized");
}

DuplicateFinderPanel::~DuplicateFinderPanel()
{
    if (m_worker) {
        m_worker->requestStop();
        if (!m_worker->wait(15000)) {
            logError("DuplicateFinderWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    logInfo("DuplicateFinderPanel destroyed");
}

void DuplicateFinderPanel::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(10);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(contentWidget, tr("Duplicate Finder"),
        tr("Scan directories for duplicate files using content-based hashing"), mainLayout);

    createDirectoryGroup(mainLayout);
    createOptionsWidgets();
    createControlButtons(mainLayout);
}

void DuplicateFinderPanel::createDirectoryGroup(QVBoxLayout* layout)
{
    auto* dirGroup = new QGroupBox("Scan Directories", this);
    auto* dirLayout = new QVBoxLayout(dirGroup);
    
    m_directory_list = new QListWidget(this);
    m_directory_list->setMinimumHeight(100);
    m_directory_list->setAccessibleName(QStringLiteral("Scan Directories List"));
    dirLayout->addWidget(m_directory_list);
    
    auto* buttonLayout = new QHBoxLayout();
    m_add_directory_button = new QPushButton("Add Directory", this);
    m_add_directory_button->setAccessibleName(QStringLiteral("Add Scan Directory"));
    m_remove_directory_button = new QPushButton("Remove Selected", this);
    m_remove_directory_button->setAccessibleName(QStringLiteral("Remove Selected Directory"));
    buttonLayout->addWidget(m_add_directory_button);
    buttonLayout->addWidget(m_remove_directory_button);
    buttonLayout->addStretch();
    dirLayout->addLayout(buttonLayout);
    
    layout->addWidget(dirGroup);
}

void DuplicateFinderPanel::createOptionsWidgets()
{
    m_min_size_spinbox = new QSpinBox(this);
    m_min_size_spinbox->setMinimum(0);
    m_min_size_spinbox->setMaximum(1000000);
    m_min_size_spinbox->setValue(0);
    m_min_size_spinbox->setToolTip("Skip tiny files to speed up scanning (0 = check all files)");
    m_min_size_spinbox->setAccessibleName(QStringLiteral("Minimum File Size (KB)"));
    m_min_size_spinbox->setVisible(false);

    m_recursive_checkbox = new QCheckBox("Recursive Scan", this);
    m_recursive_checkbox->setChecked(true);
    m_recursive_checkbox->setToolTip("Include all nested subfolders, not just the top-level directory");
    m_recursive_checkbox->setAccessibleName(QStringLiteral("Recursive Scan"));
    m_recursive_checkbox->setVisible(false);
}

void DuplicateFinderPanel::createControlButtons(QVBoxLayout* layout)
{
    auto* controlLayout = new QHBoxLayout();

    auto* settingsBtn = new QPushButton("Settings", this);
    settingsBtn->setAccessibleName(QStringLiteral("Duplicate Finder Settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &DuplicateFinderPanel::onSettingsClicked);
    controlLayout->addWidget(settingsBtn);

    controlLayout->addStretch();
    
    m_scan_button = new QPushButton("Start Scan", this);
    m_scan_button->setMinimumWidth(120);
    m_scan_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_scan_button->setAccessibleName(QStringLiteral("Start Duplicate Scan"));
    controlLayout->addWidget(m_scan_button);
    
    m_cancel_button = new QPushButton("Cancel", this);
    m_cancel_button->setMinimumWidth(120);
    m_cancel_button->setEnabled(false);
    m_cancel_button->setAccessibleName(QStringLiteral("Cancel Scan"));
    controlLayout->addWidget(m_cancel_button);
    
    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    controlLayout->insertWidget(1, m_logToggle);

    layout->addLayout(controlLayout);

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

    logInfo("Duplicate finder scan initiated");
}

void DuplicateFinderPanel::onCancelClicked()
{
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage("Cancellation requested...");
        Q_EMIT statusMessage("Cancelling...", 0);
        logInfo("Duplicate finder cancellation requested by user");
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
    logInfo("Duplicate finder scan completed successfully");
}

void DuplicateFinderPanel::onWorkerFailed(int errorCode, const QString& errorMessage)
{
    setOperationRunning(false);
    Q_EMIT statusMessage("Scan failed", 5000);
    Q_EMIT progressUpdate(0, 100);
    logMessage(QString("Scan failed: Error %1: %2").arg(errorCode).arg(errorMessage));
    QMessageBox::warning(this, "Scan Failed", QString("Error %1: %2").arg(errorCode).arg(errorMessage));
    logError("Duplicate finder scan failed: {}", errorMessage.toStdString());
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

void DuplicateFinderPanel::onResultsReady(const QString& summary, int duplicateCount, qint64 wastedSpace)
{
    QString resultsText = QString("Found %1 duplicate files, %2 MB wasted space")
        .arg(duplicateCount)
        .arg(wastedSpace / (1024.0 * 1024.0), 0, 'f', 2);
    
    Q_EMIT statusMessage(resultsText, 10000);
    logMessage(resultsText);
    
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
        InfoButton::createInfoLabel(tr("Minimum File Size:"),
            tr("Skip tiny files to speed up scanning (0 = check all files)"), &dialog),
        minSizeSpin);

    auto* recursiveCheck = new QCheckBox(tr("Include all nested subfolders"), &dialog);
    recursiveCheck->setChecked(m_recursive_checkbox->isChecked());
    layout->addRow(
        InfoButton::createInfoLabel(tr("Recursive Scan:"),
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

} // namespace sak
