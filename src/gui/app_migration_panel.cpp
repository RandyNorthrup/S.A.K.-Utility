// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/app_migration_panel.h"
#include "sak/app_scanner.h"
#include "sak/chocolatey_manager.h"
#include "sak/package_matcher.h"
#include "sak/migration_report.h"
#include "sak/app_migration_worker.h"
#include "sak/user_data_manager.h"
#include "sak/backup_wizard.h"
#include "sak/restore_wizard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QCheckBox>
#include <QApplication>
#include <QStyle>

using sak::AppScanner;
using sak::AppMigrationPanel;
using sak::ChocolateyManager;
using sak::PackageMatcher;
using sak::MigrationReport;
using sak::AppMigrationWorker;
using sak::UserDataManager;
using sak::BackupWizard;
using sak::RestoreWizard;

// Table columns
enum Column {
    ColSelect = 0,
    ColName,
    ColVersion,
    ColPublisher,
    ColPackage,
    ColConfidence,
    ColVersionLock,
    ColStatus,
    ColProgress,
    ColCount  // Total number of columns
};

AppMigrationPanel::AppMigrationPanel(QWidget* parent)
    : QWidget(parent)
    , m_scanner(std::make_shared<AppScanner>())
    , m_chocoManager(std::make_shared<ChocolateyManager>())
    , m_matcher(std::make_shared<PackageMatcher>())
    , m_report(std::make_shared<MigrationReport>())
    , m_worker(std::make_shared<AppMigrationWorker>(m_chocoManager))
    , m_dataManager(std::make_shared<UserDataManager>())
{
    setupUI();
    setupConnections();
    
    // Initialize Chocolatey on startup
    QString chocoPath = QApplication::applicationDirPath() + "/tools/chocolatey";
    bool init_success = m_chocoManager->initialize(chocoPath);
    if (!init_success) {
        m_logTextEdit->append(QString("WARNING: Chocolatey initialization failed"));
        m_logTextEdit->append("Package installation will not be available.");
    } else {
        m_logTextEdit->append("Chocolatey initialized successfully");
    }
}

AppMigrationPanel::~AppMigrationPanel() = default;

void AppMigrationPanel::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    
    // Toolbar
    setupToolbar();
    mainLayout->addWidget(m_toolbar);
    
    // Filter row
    auto* filterLayout = new QHBoxLayout();
    filterLayout->addWidget(new QLabel("Filter:", this));
    
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText("Search by name, publisher, or package...");
    filterLayout->addWidget(m_filterEdit);
    
    filterLayout->addWidget(new QLabel("Confidence:", this));
    m_confidenceFilter = new QComboBox(this);
    m_confidenceFilter->addItems(QStringList() << "All" << "High" << "Medium" << "Low" << "None");
    filterLayout->addWidget(m_confidenceFilter);
    
    // Selection buttons
    m_selectAllButton = new QPushButton("Select All", this);
    m_selectNoneButton = new QPushButton("Select None", this);
    m_selectMatchedButton = new QPushButton("Select Matched", this);
    filterLayout->addWidget(m_selectAllButton);
    filterLayout->addWidget(m_selectNoneButton);
    filterLayout->addWidget(m_selectMatchedButton);
    
    mainLayout->addLayout(filterLayout);
    
    // Table
    setupTable();
    mainLayout->addWidget(m_tableView, 1);  // Stretch factor 1
    
    // Log
    mainLayout->addWidget(new QLabel("Operation Log:", this));
    m_logTextEdit = new QTextEdit(this);
    m_logTextEdit->setReadOnly(true);
    m_logTextEdit->setMaximumHeight(120);
    mainLayout->addWidget(m_logTextEdit);
    
    // Status bar
    auto* statusWidget = setupStatusBar();
    mainLayout->addWidget(statusWidget);
}

void AppMigrationPanel::setupToolbar()
{
    m_toolbar = new QToolBar(this);
    m_toolbar->setMovable(false);
    m_toolbar->setIconSize(QSize(24, 24));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    
    // Scan Apps
    m_scanButton = new QPushButton(QIcon::fromTheme("system-search"), "Scan Apps", this);
    m_scanButton->setToolTip("Scan installed applications on this system");
    m_toolbar->addWidget(m_scanButton);
    
    m_toolbar->addSeparator();
    
    // Match Packages
    m_matchButton = new QPushButton(QIcon::fromTheme("emblem-synchronizing"), "Match Packages", this);
    m_matchButton->setToolTip("Match applications to Chocolatey packages");
    m_matchButton->setEnabled(false);
    m_toolbar->addWidget(m_matchButton);
    
    m_toolbar->addSeparator();
    
    // Backup Data
    m_backupButton = new QPushButton(QIcon::fromTheme("document-save"), "Backup Data", this);
    m_backupButton->setToolTip("Backup user data for selected applications");
    m_backupButton->setEnabled(false);
    m_toolbar->addWidget(m_backupButton);
    
    m_toolbar->addSeparator();
    
    // Install Packages
    m_installButton = new QPushButton(QIcon::fromTheme("system-software-install"), "Install", this);
    m_installButton->setToolTip("Install selected packages via Chocolatey");
    m_installButton->setEnabled(false);
    m_toolbar->addWidget(m_installButton);
    
    m_toolbar->addSeparator();
    
    // Restore Data
    m_restoreButton = new QPushButton(QIcon::fromTheme("document-open"), "Restore Data", this);
    m_restoreButton->setToolTip("Restore user data from backup");
    m_restoreButton->setEnabled(false);
    m_toolbar->addWidget(m_restoreButton);
    
    m_toolbar->addSeparator();
    
    // Generate Report
    m_reportButton = new QPushButton(QIcon::fromTheme("document-export"), "Export Report", this);
    m_reportButton->setToolTip("Generate migration report");
    m_reportButton->setEnabled(false);
    m_toolbar->addWidget(m_reportButton);
    
    m_toolbar->addSeparator();
    
    // Load Report
    m_loadButton = new QPushButton(QIcon::fromTheme("document-import"), "Load Report", this);
    m_loadButton->setToolTip("Load existing migration report");
    m_toolbar->addWidget(m_loadButton);
    
    m_toolbar->addSeparator();
    
    // Refresh
    m_refreshButton = new QPushButton(QIcon::fromTheme("view-refresh"), "Refresh", this);
    m_refreshButton->setToolTip("Refresh display");
    m_toolbar->addWidget(m_refreshButton);
}

void AppMigrationPanel::setupTable()
{
    m_tableView = new QTableView(this);
    m_tableModel = new QStandardItemModel(0, ColCount, this);
    
    // Set headers
    m_tableModel->setHorizontalHeaderLabels(QStringList()
        << "✓" << "Application" << "Version" << "Publisher"
        << "Choco Package" << "Match" << "Lock Ver" << "Status" << "Progress");
    
    m_tableView->setModel(m_tableModel);
    m_tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tableView->setAlternatingRowColors(true);
    m_tableView->setSortingEnabled(true);
    m_tableView->verticalHeader()->setVisible(false);
    
    // Column widths
    m_tableView->horizontalHeader()->setStretchLastSection(false);
    m_tableView->setColumnWidth(ColSelect, 40);
    m_tableView->setColumnWidth(ColName, 200);
    m_tableView->setColumnWidth(ColVersion, 80);
    m_tableView->setColumnWidth(ColPublisher, 150);
    m_tableView->setColumnWidth(ColPackage, 150);
    m_tableView->setColumnWidth(ColConfidence, 80);
    m_tableView->setColumnWidth(ColVersionLock, 80);
    m_tableView->setColumnWidth(ColStatus, 100);
    m_tableView->setColumnWidth(ColProgress, 100);
}

QWidget* AppMigrationPanel::setupStatusBar()
{
    auto* statusLayout = new QHBoxLayout();
    statusLayout->setContentsMargins(0, 0, 0, 0);
    
    m_statusLabel = new QLabel("Ready", this);
    statusLayout->addWidget(m_statusLabel);
    
    statusLayout->addStretch();
    
    m_summaryLabel = new QLabel("Applications: 0 | Matched: 0 | Selected: 0", this);
    statusLayout->addWidget(m_summaryLabel);
    
    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    m_progressBar->setMaximumWidth(200);
    statusLayout->addWidget(m_progressBar);
    
    // Create a widget to hold the status layout
    auto* statusWidget = new QWidget(this);
    statusWidget->setLayout(statusLayout);
    return statusWidget;
}

void AppMigrationPanel::setupConnections()
{
    // Toolbar buttons
    connect(m_scanButton, &QPushButton::clicked, this, &AppMigrationPanel::onScanApps);
    connect(m_matchButton, &QPushButton::clicked, this, &AppMigrationPanel::onMatchPackages);
    connect(m_backupButton, &QPushButton::clicked, this, &AppMigrationPanel::onBackupData);
    connect(m_installButton, &QPushButton::clicked, this, &AppMigrationPanel::onInstallPackages);
    connect(m_restoreButton, &QPushButton::clicked, this, &AppMigrationPanel::onRestoreData);
    
    // Table model changes
    connect(m_tableModel, &QStandardItemModel::itemChanged, this, &AppMigrationPanel::onTableItemChanged);
    connect(m_reportButton, &QPushButton::clicked, this, &AppMigrationPanel::onGenerateReport);
    connect(m_loadButton, &QPushButton::clicked, this, &AppMigrationPanel::onLoadReport);
    connect(m_refreshButton, &QPushButton::clicked, this, &AppMigrationPanel::onRefresh);
    
    // Selection buttons
    connect(m_selectAllButton, &QPushButton::clicked, this, &AppMigrationPanel::onSelectAll);
    connect(m_selectNoneButton, &QPushButton::clicked, this, &AppMigrationPanel::onSelectNone);
    connect(m_selectMatchedButton, &QPushButton::clicked, this, &AppMigrationPanel::onSelectMatched);
    
    // Filters
    connect(m_filterEdit, &QLineEdit::textChanged, this, &AppMigrationPanel::onFilterChanged);
    connect(m_confidenceFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppMigrationPanel::onConfidenceFilterChanged);
    
    // Table
    connect(m_tableModel, &QStandardItemModel::itemChanged, 
            this, [this](QStandardItem* item) {
                if (item->column() == ColSelect) {
                    updateStatusSummary();
                }
            });
}

// ============================================================================
// Toolbar Actions
// ============================================================================

void AppMigrationPanel::onScanApps()
{
    if (m_operationInProgress) {
        QMessageBox::warning(this, "Operation In Progress",
            "Please wait for the current operation to complete.");
        return;
    }
    
    m_logTextEdit->append("=== Scanning Installed Applications ===");
    m_statusLabel->setText("Scanning...");
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 0);  // Indeterminate
    
    enableControls(false);
    m_operationInProgress = true;
    
    // Scan in background
    QApplication::setOverrideCursor(Qt::WaitCursor);
    
    auto apps = m_scanner->scanAll();
    
    QApplication::restoreOverrideCursor();
    
    // Convert to MigrationEntry
    m_entries.clear();
    for (const auto& app : apps) {
        MigrationEntry entry;
        entry.selected = true;
        entry.app_name = app.name;
        entry.version = app.version;
        entry.publisher = app.publisher;
        entry.install_location = app.install_location;
        entry.choco_package = "";
        entry.choco_available = false;
        entry.match_confidence = "None";
        entry.status = "Scanned";
        entry.progress = 0;
        
        m_entries.append(entry);
    }
    
    updateTableFromEntries();
    
    m_logTextEdit->append(QString("Scan complete: Found %1 applications").arg(m_entries.size()));
    m_statusLabel->setText("Scan complete");
    m_progressBar->setVisible(false);
    
    m_matchButton->setEnabled(!m_entries.isEmpty());
    m_reportButton->setEnabled(!m_entries.isEmpty());
    
    enableControls(true);
    m_operationInProgress = false;
    
    updateStatusSummary();
}

void AppMigrationPanel::onMatchPackages()
{
    if (m_operationInProgress || m_entries.isEmpty()) return;
    
    m_logTextEdit->append("=== Matching Applications to Chocolatey Packages ===");
    m_statusLabel->setText("Matching...");
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, m_entries.size());
    m_progressBar->setValue(0);
    
    enableControls(false);
    m_operationInProgress = true;
    
    QApplication::setOverrideCursor(Qt::WaitCursor);
    
    int matched = 0;
    for (int i = 0; i < m_entries.size(); ++i) {
        auto& entry = m_entries[i];
        
        // Match package - create AppInfo for matching
        AppScanner::AppInfo appInfo;
        appInfo.name = entry.app_name;
        appInfo.version = entry.version;
        appInfo.publisher = entry.publisher;
        appInfo.install_location = entry.install_location;
        
        auto match_result = m_matcher->findMatch(appInfo, m_chocoManager.get());
        
        if (match_result.has_value()) {
            entry.choco_package = match_result->choco_package;
            entry.choco_available = match_result->available;
            
            // Map confidence score to text
            if (match_result->confidence >= 0.9) {
                entry.match_confidence = "High";
            } else if (match_result->confidence >= 0.7) {
                entry.match_confidence = "Medium";
            } else {
                entry.match_confidence = "Low";
            }
            
            entry.status = "Matched";
            matched++;
        } else {
            entry.match_confidence = "None";
            entry.status = "No Match";
        }
        
        m_progressBar->setValue(i + 1);
        updateEntry(i);
        
        QApplication::processEvents();
    }
    
    QApplication::restoreOverrideCursor();
    
    m_logTextEdit->append(QString("Matching complete: %1/%2 applications matched (%.1f%%)")
                                 .arg(matched)
                                 .arg(m_entries.size())
                                 .arg(matched * 100.0 / m_entries.size()));
    
    m_statusLabel->setText("Matching complete");
    m_progressBar->setVisible(false);
    
    m_backupButton->setEnabled(matched > 0);
    m_installButton->setEnabled(matched > 0);
    
    enableControls(true);
    m_operationInProgress = false;
    
    updateStatusSummary();
}

void AppMigrationPanel::onBackupData()
{
    // Launch backup wizard
    BackupWizard wizard(this);
    wizard.exec();
}

void AppMigrationPanel::onInstallPackages()
{
    auto selected = getSelectedEntries();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "No Selection",
            "Please select applications to install.");
        return;
    }
    
    // Filter to only matched entries
    QVector<MigrationEntry> toInstall;
    for (const auto& entry : selected) {
        if (entry.choco_available) {
            toInstall.append(entry);
        }
    }
    
    if (toInstall.isEmpty()) {
        QMessageBox::information(this, "No Matched Packages",
            "None of the selected applications have matched Chocolatey packages.");
        return;
    }
    
    auto reply = QMessageBox::question(this, "Confirm Installation",
        QString("Install %1 package(s) via Chocolatey?\n\n"
                "This operation requires administrative privileges and may take several minutes.")
                .arg(toInstall.size()),
        QMessageBox::Yes | QMessageBox::No);
    
    if (reply != QMessageBox::Yes) return;
    
    m_logTextEdit->append(QString("=== Installing %1 Packages ===").arg(toInstall.size()));
    m_statusLabel->setText("Installing...");
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, toInstall.size());
    m_progressBar->setValue(0);
    
    enableControls(false);
    m_operationInProgress = true;
    
    // Install packages synchronously (in a real implementation, use worker thread)
    int installed = 0;
    int failed = 0;
    
    for (int i = 0; i < toInstall.size(); ++i) {
        const auto& entry = toInstall[i];
        
        m_logTextEdit->append(QString("[%1/%2] Installing %3...")
                                     .arg(i + 1)
                                     .arg(toInstall.size())
                                     .arg(entry.choco_package));
        
        // Find entry in main list to update status
        for (int j = 0; j < m_entries.size(); ++j) {
            if (m_entries[j].app_name == entry.app_name) {
                m_entries[j].status = "Installing";
                m_entries[j].progress = 50;
                updateEntry(j);
                break;
            }
        }
        
        QApplication::processEvents();
        
        // Install with version lock if enabled
        ChocolateyManager::InstallConfig config;
        config.package_name = entry.choco_package;
        if (entry.version_locked && !entry.locked_version.isEmpty()) {
            config.version = entry.locked_version;
        }
        config.auto_confirm = true;
        config.timeout_seconds = 300;
        
        auto result = m_chocoManager->installPackage(config);
        
        // Update entry status
        for (int j = 0; j < m_entries.size(); ++j) {
            if (m_entries[j].app_name == entry.app_name) {
                if (result.success) {
                    m_entries[j].status = "Installed";
                    m_entries[j].progress = 100;
                    installed++;
                    m_logTextEdit->append(QString("  SUCCESS: %1").arg(entry.choco_package));
                } else {
                    m_entries[j].status = "Failed";
                    m_entries[j].error_message = result.error_message;
                    failed++;
                    m_logTextEdit->append(QString("  FAILED: %1 - %2")
                                                 .arg(entry.choco_package)
                                                 .arg(result.error_message));
                }
                updateEntry(j);
                break;
            }
        }
        
        m_progressBar->setValue(i + 1);
        QApplication::processEvents();
    }
    
    m_logTextEdit->append(QString("Installation complete: %1 succeeded, %2 failed")
                                 .arg(installed)
                                 .arg(failed));
    
    m_statusLabel->setText("Installation complete");
    m_progressBar->setVisible(false);
    
    m_restoreButton->setEnabled(true);
    
    enableControls(true);
    m_operationInProgress = false;
}

void AppMigrationPanel::onRestoreData()
{
    // Launch restore wizard
    RestoreWizard wizard(this);
    wizard.exec();
}

void AppMigrationPanel::onGenerateReport()
{
    QString fileName = QFileDialog::getSaveFileName(
        this,
        "Save Migration Report",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/migration_report.json",
        "JSON Files (*.json);;CSV Files (*.csv);;All Files (*)"
    );
    
    if (fileName.isEmpty()) return;
    
    m_logTextEdit->append(QString("Generating report: %1").arg(fileName));
    
    // Create migration report from current entries
    MigrationReport report;
    
    // Convert current entries to MigrationEntry format
    for (const auto& entry : m_entries) {
        MigrationReport::MigrationEntry reportEntry;
        reportEntry.app_name = entry.app_name;
        reportEntry.app_version = entry.version;  // MigrationEntry uses 'version' field
        reportEntry.app_publisher = entry.publisher;
        reportEntry.install_location = entry.install_location;
        reportEntry.choco_package = entry.choco_package;
        reportEntry.confidence = entry.match_confidence == "High" ? 0.9 : 
                                 entry.match_confidence == "Medium" ? 0.6 : 0.3;
        reportEntry.match_type = entry.match_confidence;
        reportEntry.selected = entry.selected;
        reportEntry.version_lock = entry.version_locked;
        reportEntry.status = entry.status;
        report.addEntry(reportEntry);
    }
    
    // Export to appropriate format based on file extension
    bool success = false;
    if (fileName.endsWith(".json", Qt::CaseInsensitive)) {
        success = report.exportToJson(fileName);
    } else if (fileName.endsWith(".csv", Qt::CaseInsensitive)) {
        success = report.exportToCsv(fileName);
    } else if (fileName.endsWith(".html", Qt::CaseInsensitive)) {
        success = report.exportToHtml(fileName);
    } else {
        // Default to JSON
        success = report.exportToJson(fileName);
    }
    
    if (success) {
        m_logTextEdit->append(QString("Report successfully generated: %1").arg(fileName));
        QMessageBox::information(this, "Report Generated",
            QString("Migration report saved to:\n%1").arg(fileName));
    } else {
        m_logTextEdit->append(QString("Failed to generate report: %1").arg(fileName));
        QMessageBox::warning(this, "Report Generation Failed",
            QString("Failed to save report to:\n%1").arg(fileName));
    }
}

void AppMigrationPanel::onLoadReport()
{
    QString fileName = QFileDialog::getOpenFileName(
        this,
        "Load Migration Report",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        "JSON Files (*.json);;CSV Files (*.csv);;All Files (*)"
    );
    
    if (fileName.isEmpty()) return;
    
    m_logTextEdit->append(QString("Loading report: %1").arg(fileName));
    
    // Create migration report and load from file
    MigrationReport report;
    
    bool success = false;
    if (fileName.endsWith(".json", Qt::CaseInsensitive)) {
        success = report.importFromJson(fileName);
    } else {
        // Try JSON as default format
        success = report.importFromJson(fileName);
    }
    
    if (!success) {
        m_logTextEdit->append(QString("Failed to load report: %1").arg(fileName));
        QMessageBox::warning(this, "Report Load Failed",
            QString("Failed to load report from:\n%1").arg(fileName));
        return;
    }
    
    // Clear current entries and load from report
    m_entries.clear();
    
    for (const auto& reportEntry : report.getEntries()) {
        MigrationEntry entry;
        entry.app_name = reportEntry.app_name;
        entry.version = reportEntry.app_version;  // MigrationEntry uses 'version' field
        entry.publisher = reportEntry.app_publisher;
        entry.install_location = reportEntry.install_location;
        entry.choco_package = reportEntry.choco_package;
        entry.match_confidence = reportEntry.confidence > 0.8 ? "High" :
                                reportEntry.confidence > 0.5 ? "Medium" : "Low";
        entry.selected = reportEntry.selected;
        entry.version_locked = reportEntry.version_lock;
        entry.status = reportEntry.status;
        m_entries.push_back(entry);
    }
    
    // Update UI
    updateTableFromEntries();
    updateStatusSummary();
    
    m_logTextEdit->append(QString("Successfully loaded %1 entries from report").arg(m_entries.size()));
    QMessageBox::information(this, "Report Loaded",
        QString("Migration report loaded from:\n%1\n\nLoaded %2 application entries.")
            .arg(fileName).arg(m_entries.size()));
}

void AppMigrationPanel::onRefresh()
{
    updateTableFromEntries();
    updateStatusSummary();
}

// ============================================================================
// Selection Actions
// ============================================================================

void AppMigrationPanel::onSelectAll()
{
    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        auto* item = m_tableModel->item(i, ColSelect);
        if (item) {
            item->setCheckState(Qt::Checked);
        }
    }
    updateStatusSummary();
}

void AppMigrationPanel::onSelectNone()
{
    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        auto* item = m_tableModel->item(i, ColSelect);
        if (item) {
            item->setCheckState(Qt::Unchecked);
        }
    }
    updateStatusSummary();
}

void AppMigrationPanel::onSelectMatched()
{
    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        auto* confidenceItem = m_tableModel->item(i, ColConfidence);
        auto* selectItem = m_tableModel->item(i, ColSelect);
        
        if (confidenceItem && selectItem) {
            bool isMatched = confidenceItem->text() != "None";
            selectItem->setCheckState(isMatched ? Qt::Checked : Qt::Unchecked);
        }
    }
    updateStatusSummary();
}

void AppMigrationPanel::onInvertSelection()
{
    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        auto* item = m_tableModel->item(i, ColSelect);
        if (item) {
            item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
        }
    }
    updateStatusSummary();
}

// ============================================================================
// Filter Actions
// ============================================================================

void AppMigrationPanel::onFilterChanged(const QString& text)
{
    QString filter = text.toLower();
    
    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        bool visible = true;
        
        if (!filter.isEmpty()) {
            QString name = m_tableModel->item(i, ColName)->text().toLower();
            QString publisher = m_tableModel->item(i, ColPublisher)->text().toLower();
            QString package = m_tableModel->item(i, ColPackage)->text().toLower();
            
            visible = name.contains(filter) || publisher.contains(filter) || package.contains(filter);
        }
        
        m_tableView->setRowHidden(i, !visible);
    }
}

void AppMigrationPanel::onConfidenceFilterChanged(int index)
{
    QString filter = m_confidenceFilter->itemText(index);
    
    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        bool visible = true;
        
        if (filter != "All") {
            QString confidence = m_tableModel->item(i, ColConfidence)->text();
            visible = (filter == confidence);
        }
        
        m_tableView->setRowHidden(i, !visible);
    }
}

// ============================================================================
// Helper Methods
// ============================================================================

void AppMigrationPanel::updateTableFromEntries()
{
    m_tableModel->setRowCount(0);
    
    for (const auto& entry : m_entries) {
        int row = m_tableModel->rowCount();
        m_tableModel->insertRow(row);
        
        // Checkbox
        auto* checkItem = new QStandardItem();
        checkItem->setCheckable(true);
        checkItem->setCheckState(entry.selected ? Qt::Checked : Qt::Unchecked);
        m_tableModel->setItem(row, ColSelect, checkItem);
        
        // Application name
        m_tableModel->setItem(row, ColName, new QStandardItem(entry.app_name));
        
        // Version
        m_tableModel->setItem(row, ColVersion, new QStandardItem(entry.version));
        
        // Publisher
        m_tableModel->setItem(row, ColPublisher, new QStandardItem(entry.publisher));
        
        // Choco package
        m_tableModel->setItem(row, ColPackage, new QStandardItem(entry.choco_package));
        
        // Match confidence
        auto* confItem = new QStandardItem(entry.match_confidence);
        if (entry.match_confidence == "High") {
            confItem->setForeground(Qt::darkGreen);
        } else if (entry.match_confidence == "Medium") {
            confItem->setForeground(QColor(255, 140, 0));  // Orange
        } else if (entry.match_confidence == "Low") {
            confItem->setForeground(QColor(200, 100, 0));  // Dark orange
        }
        m_tableModel->setItem(row, ColConfidence, confItem);
        
        // Version lock - use checkbox
        auto* lockItem = new QStandardItem();
        lockItem->setCheckable(true);
        lockItem->setCheckState(entry.version_locked ? Qt::Checked : Qt::Unchecked);
        lockItem->setEditable(false);
        m_tableModel->setItem(row, ColVersionLock, lockItem);
        
        // Status
        m_tableModel->setItem(row, ColStatus, new QStandardItem(entry.status));
        
        // Progress
        m_tableModel->setItem(row, ColProgress, new QStandardItem(QString::number(entry.progress) + "%"));
    }
}

void AppMigrationPanel::updateEntry(int row)
{
    if (row < 0 || row >= m_entries.size()) return;
    
    const auto& entry = m_entries[row];
    
    // Update table row
    if (row < m_tableModel->rowCount()) {
        m_tableModel->item(row, ColPackage)->setText(entry.choco_package);
        m_tableModel->item(row, ColConfidence)->setText(entry.match_confidence);
        m_tableModel->item(row, ColVersionLock)->setCheckState(entry.version_locked ? Qt::Checked : Qt::Unchecked);
        m_tableModel->item(row, ColStatus)->setText(entry.status);
        m_tableModel->item(row, ColProgress)->setText(QString::number(entry.progress) + "%");
    }
}

void AppMigrationPanel::clearTable()
{
    m_tableModel->setRowCount(0);
    m_entries.clear();
}

void AppMigrationPanel::enableControls(bool enabled)
{
    m_scanButton->setEnabled(enabled);
    m_matchButton->setEnabled(enabled && !m_entries.isEmpty());
    m_backupButton->setEnabled(enabled && !m_entries.isEmpty());
    m_installButton->setEnabled(enabled && !m_entries.isEmpty());
    m_restoreButton->setEnabled(enabled);
    m_reportButton->setEnabled(enabled && !m_entries.isEmpty());
    m_loadButton->setEnabled(enabled);
    m_refreshButton->setEnabled(enabled);
}

void AppMigrationPanel::updateStatusSummary()
{
    int total = m_entries.size();
    int matched = 0;
    int selected = 0;
    
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].choco_available) matched++;
        
        if (i < m_tableModel->rowCount()) {
            auto* item = m_tableModel->item(i, ColSelect);
            if (item && item->checkState() == Qt::Checked) {
                selected++;
            }
        }
    }
    
    m_summaryLabel->setText(QString("Applications: %1 | Matched: %2 | Selected: %3")
                                   .arg(total)
                                   .arg(matched)
                                   .arg(selected));
}

QVector<AppMigrationPanel::MigrationEntry> AppMigrationPanel::getSelectedEntries() const
{
    QVector<MigrationEntry> selected;
    
    for (int i = 0; i < m_tableModel->rowCount() && i < m_entries.size(); ++i) {
        auto* item = m_tableModel->item(i, ColSelect);
        if (item && item->checkState() == Qt::Checked) {
            selected.append(m_entries[i]);
        }
    }
    
    return selected;
}

void AppMigrationPanel::setEntryStatus(int row, const QString& status, int progress)
{
    if (row < 0 || row >= m_entries.size()) return;
    
    m_entries[row].status = status;
    m_entries[row].progress = progress;
    updateEntry(row);
}

// Placeholder slot implementations
void AppMigrationPanel::onScanStarted() {}
void AppMigrationPanel::onScanProgress(int, int) {}
void AppMigrationPanel::onScanComplete(int) {}
void AppMigrationPanel::onMatchStarted() {}
void AppMigrationPanel::onMatchProgress(int, int) {}
void AppMigrationPanel::onMatchComplete(int, int) {}
void AppMigrationPanel::onInstallStarted(const QString&) {}
void AppMigrationPanel::onInstallProgress(int, int) {}
void AppMigrationPanel::onInstallComplete(const QString&, bool, const QString&) {}
void AppMigrationPanel::onInstallError(const QString&, const QString&) {}
void AppMigrationPanel::onCellChanged(const QModelIndex&) {}
void AppMigrationPanel::onSelectionChanged() {}
void AppMigrationPanel::onVersionLockToggled(int) {}

void AppMigrationPanel::onTableItemChanged(QStandardItem* item)
{
    if (!item) return;
    
    // Check if this is the version lock column
    if (item->column() == ColVersionLock) {
        int row = item->row();
        if (row >= 0 && row < m_entries.size()) {
            // Update the entry's version_locked state
            m_entries[row].version_locked = (item->checkState() == Qt::Checked);
            
            // Log the change
            QString lockStatus = m_entries[row].version_locked ? "locked" : "unlocked";
            m_logTextEdit->append(QString("Version %1 for %2")
                .arg(lockStatus)
                .arg(m_entries[row].app_name));
        }
    }
}

