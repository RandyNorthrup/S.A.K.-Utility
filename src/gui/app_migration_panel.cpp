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
#include <QThread>
#include <QtConcurrent>
#include <atomic>
#include <memory>

using sak::AppScanner;
using sak::AppMigrationPanel;
using sak::ChocolateyManager;
using sak::PackageMatcher;
using sak::MigrationReport;
using sak::AppMigrationWorker;
using sak::MigrationJob;
using sak::MigrationStatus;
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
    ColLockedVersion,
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

AppMigrationPanel::~AppMigrationPanel() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
    }
}

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
    m_selectMatchedButton->setToolTip("Select only apps that have a matched Chocolatey package");
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
    m_scanButton->setToolTip("Detect all installed programs via Windows registry");
    m_toolbar->addWidget(m_scanButton);
    
    m_toolbar->addSeparator();
    
    // Match Packages
    m_matchButton = new QPushButton(QIcon::fromTheme("emblem-synchronizing"), "Match Packages", this);
    m_matchButton->setToolTip("Find matching Chocolatey packages for each scanned app");
    m_matchButton->setEnabled(false);
    m_toolbar->addWidget(m_matchButton);
    
    m_toolbar->addSeparator();
    
    // Backup Data
    m_backupButton = new QPushButton(QIcon::fromTheme("document-save"), "Backup Data", this);
    m_backupButton->setToolTip("Save app settings and user data for selected apps before migration");
    m_backupButton->setEnabled(false);
    m_toolbar->addWidget(m_backupButton);
    
    m_toolbar->addSeparator();
    
    // Install Packages
    m_installButton = new QPushButton(QIcon::fromTheme("system-software-install"), "Install", this);
    m_installButton->setToolTip("Install selected packages on the target system via Chocolatey (requires admin)");
    m_installButton->setEnabled(false);
    m_toolbar->addWidget(m_installButton);
    
    m_toolbar->addSeparator();
    
    // Restore Data
    m_restoreButton = new QPushButton(QIcon::fromTheme("document-open"), "Restore Data", this);
    m_restoreButton->setToolTip("Restore previously backed-up app settings and user data");
    m_restoreButton->setEnabled(false);
    m_toolbar->addWidget(m_restoreButton);
    
    m_toolbar->addSeparator();
    
    // Generate Report
    m_reportButton = new QPushButton(QIcon::fromTheme("document-export"), "Export Report", this);
    m_reportButton->setToolTip("Save a JSON migration report to share with the target machine");
    m_reportButton->setEnabled(false);
    m_toolbar->addWidget(m_reportButton);
    
    m_toolbar->addSeparator();
    
    // Load Report
    m_loadButton = new QPushButton(QIcon::fromTheme("document-import"), "Load Report", this);
    m_loadButton->setToolTip("Import a migration report generated on another machine");
    m_toolbar->addWidget(m_loadButton);
    
    m_toolbar->addSeparator();
    
    // Refresh
    m_refreshButton = new QPushButton(QIcon::fromTheme("view-refresh"), "Refresh", this);
    m_toolbar->addWidget(m_refreshButton);
}

void AppMigrationPanel::setupTable()
{
    m_tableView = new QTableView(this);
    m_tableModel = new QStandardItemModel(0, ColCount, this);
    
    // Set headers
    m_tableModel->setHorizontalHeaderLabels(QStringList()
        << "✓" << "Application" << "Version" << "Publisher"
        << "Choco Package" << "Match" << "Lock" << "Locked Version" << "Status" << "Progress");
    
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
    m_tableView->setColumnWidth(ColLockedVersion, 110);
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

    connect(m_worker.get(), &AppMigrationWorker::migrationStarted, this, [this](int totalJobs) {
        m_progressBar->setVisible(true);
        m_progressBar->setRange(0, totalJobs);
        m_progressBar->setValue(0);
        m_statusLabel->setText("Installing...");
        m_logTextEdit->append(QString("Migration started: %1 job(s)").arg(totalJobs));
    });

    connect(m_worker.get(), &AppMigrationWorker::jobProgress, this,
            [this](int entryIndex, const QString& message) {
                Q_UNUSED(entryIndex);
                m_logTextEdit->append(message);
            });

    connect(m_worker.get(), &AppMigrationWorker::jobStatusChanged, this,
            [this](int entryIndex, const MigrationJob& job) {
                if (entryIndex < 0 || entryIndex >= m_entries.size()) {
                    return;
                }

                auto& entry = m_entries[entryIndex];
                switch (job.status) {
                    case MigrationStatus::Queued:
                        entry.status = "Queued";
                        entry.progress = 0;
                        break;
                    case MigrationStatus::Installing:
                        entry.status = "Installing";
                        entry.progress = 50;
                        break;
                    case MigrationStatus::Success:
                        entry.status = "Installed";
                        entry.progress = 100;
                        break;
                    case MigrationStatus::Failed:
                        entry.status = "Failed";
                        entry.error_message = job.errorMessage;
                        entry.progress = 0;
                        break;
                    case MigrationStatus::Skipped:
                        entry.status = "Skipped";
                        entry.progress = 0;
                        break;
                    case MigrationStatus::Cancelled:
                        entry.status = "Cancelled";
                        entry.progress = 0;
                        break;
                    default:
                        break;
                }

                updateEntry(entryIndex);

                auto stats = m_worker->getStats();
                int completed = stats.success + stats.failed + stats.skipped + stats.cancelled;
                m_progressBar->setValue(completed);
            });

    connect(m_worker.get(), &AppMigrationWorker::migrationCompleted, this,
            [this](const AppMigrationWorker::Stats& stats) {
                m_logTextEdit->append(QString("Installation complete: %1 succeeded, %2 failed, %3 skipped")
                    .arg(stats.success)
                    .arg(stats.failed)
                    .arg(stats.skipped));

                m_statusLabel->setText("Installation complete");
                m_progressBar->setVisible(false);
                m_restoreButton->setEnabled(stats.success > 0);
                enableControls(true);
                m_installInProgress = false;
                m_activeReport.reset();
            });
}

// ============================================================================
// Toolbar Actions
// ============================================================================

void AppMigrationPanel::onScanApps()
{
    if (m_scanInProgress) {
        QMessageBox::warning(this, "Scan In Progress",
            "Please wait for the current scan to complete.");
        return;
    }
    
    m_logTextEdit->append("=== Scanning Installed Applications ===");
    m_statusLabel->setText("Scanning...");
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 0);  // Indeterminate
    
    enableControls(false);
    m_scanInProgress = true;
    
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
        entry.match_score = 0.0;
        entry.match_type = "none";
        entry.available_version.clear();
        entry.status = "Scanned";
        entry.progress = 0;
        
        m_entries.append(entry);
    }
    
    // Reset filters to show all results BEFORE updating table
    // Block signals to prevent filter handlers from running on stale table data
    {
        QSignalBlocker filterBlocker(m_filterEdit);
        QSignalBlocker comboBlocker(m_confidenceFilter);
        m_filterEdit->clear();
        m_confidenceFilter->setCurrentIndex(0);  // Set to "All"
    }
    
    updateTableFromEntries();
    
    m_logTextEdit->append(QString("Scan complete: Found %1 applications").arg(m_entries.size()));
    m_statusLabel->setText("Scan complete");
    m_progressBar->setVisible(false);
    
    m_matchButton->setEnabled(!m_entries.isEmpty());
    m_reportButton->setEnabled(!m_entries.isEmpty());
    
    enableControls(true);
    m_scanInProgress = false;
    
    updateStatusSummary();
}

void AppMigrationPanel::onMatchPackages()
{
    if (m_matchingInProgress || m_entries.isEmpty()) {
        if (m_matchingInProgress) {
            QMessageBox::information(this, "Matching In Progress",
                "Matching is already running. You can continue using the app.");
        }
        return;
    }
    
    m_logTextEdit->append(QString("=== Matching Applications to Chocolatey Packages (Parallel, %1 cores) ===")
                              .arg(QThread::idealThreadCount()));
    m_statusLabel->setText("Matching...");
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, m_entries.size());
    m_progressBar->setValue(0);
    
    m_matchingInProgress = true;
    m_matchButton->setEnabled(false);
    
    // Shared state for thread-safe updates
    auto processed = std::make_shared<std::atomic<int>>(0);
    auto matched = std::make_shared<std::atomic<int>>(0);
    auto available = std::make_shared<std::atomic<int>>(0);
    
    auto matcher = m_matcher;
    auto chocoManager = m_chocoManager;
    const int totalEntries = m_entries.size();
    
    // Process with parallel filter
    m_matchingFuture = QtConcurrent::run([this, matcher, chocoManager, processed, matched, available, totalEntries]() {
        // Process entries in parallel batches
        QtConcurrent::blockingMap(m_entries, [matcher, chocoManager, processed, matched, available, this, totalEntries](MigrationEntry& entry) {
            // Match package - create AppInfo for matching
            AppScanner::AppInfo appInfo;
            appInfo.name = entry.app_name;
            appInfo.version = entry.version;
            appInfo.publisher = entry.publisher;
            appInfo.install_location = entry.install_location;
            
            auto match_result = matcher->findMatch(appInfo, chocoManager.get());
            
            if (match_result.has_value()) {
                entry.choco_package = match_result->choco_package;
                entry.choco_available = match_result->available;
                entry.match_score = match_result->confidence;
                entry.match_type = match_result->match_type;
                entry.available_version = match_result->version;
                
                // Map confidence score to text
                if (match_result->confidence >= 0.9) {
                    entry.match_confidence = "High";
                } else if (match_result->confidence >= 0.7) {
                    entry.match_confidence = "Medium";
                } else {
                    entry.match_confidence = "Low";
                }
                
                entry.status = entry.choco_available ? "Matched" : "Matched (Unavailable)";
                (*matched)++;
                if (entry.choco_available) {
                    (*available)++;
                }
            } else {
                entry.match_confidence = "None";
                entry.match_score = 0.0;
                entry.match_type = "none";
                entry.available_version.clear();
                entry.status = "No Match";
            }
            
            // Update progress periodically
            int current = ++(*processed);
            if (current % 5 == 0 || current == totalEntries) {
                QMetaObject::invokeMethod(this, [this, current]() {
                    m_progressBar->setValue(current);
                }, Qt::QueuedConnection);
            }
        });
        
        // Completion callback on main thread
        QMetaObject::invokeMethod(this, [this, matched, available, totalEntries]() {
            updateTableFromEntries();
            
            m_logTextEdit->append(QString("Matching complete: %1/%2 applications matched (%3%), %4 available")
                                         .arg(matched->load())
                                         .arg(totalEntries)
                                         .arg(QString::number(matched->load() * 100.0 / totalEntries, 'f', 1))
                                         .arg(available->load()));
            
            m_statusLabel->setText("Matching complete");
            m_progressBar->setVisible(false);
            
            m_backupButton->setEnabled(available->load() > 0);
            m_installButton->setEnabled(available->load() > 0);
            
            m_matchButton->setEnabled(true);
            m_matchingInProgress = false;
            
            updateStatusSummary();
        }, Qt::QueuedConnection);
    });
}

void AppMigrationPanel::onBackupData()
{
    // Launch backup wizard
    BackupWizard wizard(this);
    wizard.exec();
}

void AppMigrationPanel::onInstallPackages()
{
    if (!m_chocoManager->isInitialized()) {
        QMessageBox::warning(this, "Chocolatey Not Available",
            "Chocolatey is not initialized. Package installation is unavailable.");
        return;
    }

    if (m_worker->isRunning()) {
        QMessageBox::information(this, "Installation In Progress",
            "An installation operation is already running.");
        return;
    }

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
    m_installInProgress = true;

    auto report = std::make_shared<MigrationReport>();
    for (const auto& entry : m_entries) {
        MigrationReport::MigrationEntry reportEntry;
        reportEntry.app_name = entry.app_name;
        reportEntry.app_version = entry.version;
        reportEntry.app_publisher = entry.publisher;
        reportEntry.install_location = entry.install_location;
        reportEntry.choco_package = entry.choco_package;
        reportEntry.confidence = entry.match_score;
        reportEntry.match_type = entry.match_type;
        reportEntry.available = entry.choco_available;
        reportEntry.available_version = entry.available_version;
        reportEntry.selected = entry.selected;
        reportEntry.version_lock = entry.version_locked;
        reportEntry.status = entry.status;
        report->addEntry(reportEntry);
    }

    m_activeReport = report;
    const int queued = m_worker->startMigration(m_activeReport, 2);
    if (queued == 0) {
        m_logTextEdit->append("No packages queued for installation.");
        m_statusLabel->setText("Ready");
        m_progressBar->setVisible(false);
        enableControls(true);
        m_installInProgress = false;
        m_activeReport.reset();
    }
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
        reportEntry.confidence = entry.match_score;
        reportEntry.match_type = entry.match_type;
        reportEntry.available = entry.choco_available;
        reportEntry.available_version = entry.available_version;
        reportEntry.selected = entry.selected;
        reportEntry.version_lock = entry.version_locked;
        reportEntry.locked_version = entry.locked_version;
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
        "JSON Files (*.json);;All Files (*)"
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
        entry.match_score = reportEntry.confidence;
        entry.match_type = reportEntry.match_type;
        entry.choco_available = reportEntry.available;
        entry.available_version = reportEntry.available_version;
        entry.match_confidence = reportEntry.confidence >= 0.9 ? "High" :
                                reportEntry.confidence >= 0.7 ? "Medium" :
                                reportEntry.confidence > 0.0 ? "Low" : "None";
        entry.selected = reportEntry.selected;
        entry.version_locked = reportEntry.version_lock;
        entry.locked_version = reportEntry.locked_version;
        entry.status = reportEntry.status;
        m_entries.push_back(entry);
    }

    if (m_chocoManager->isInitialized()) {
        for (auto& entry : m_entries) {
            if (entry.choco_package.isEmpty()) {
                entry.choco_available = false;
                continue;
            }
            entry.choco_available = m_chocoManager->isPackageAvailable(entry.choco_package);
            if (!entry.choco_available) {
                entry.status = "Matched (Unavailable)";
            }
        }
    } else {
        m_logTextEdit->append("Chocolatey not initialized. Availability not rechecked.");
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
    
    const int row_count = m_tableModel->rowCount();
    for (int i = 0; i < row_count; ++i) {
        bool visible = true;
        
        if (!filter.isEmpty()) {
            auto* nameItem = m_tableModel->item(i, ColName);
            auto* publisherItem = m_tableModel->item(i, ColPublisher);
            auto* packageItem = m_tableModel->item(i, ColPackage);

            if (!nameItem || !publisherItem || !packageItem) {
                continue;
            }

            QString name = nameItem->text().toLower();
            QString publisher = publisherItem->text().toLower();
            QString package = packageItem->text().toLower();
            
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
            auto* confidenceItem = m_tableModel->item(i, ColConfidence);
            if (!confidenceItem) {
                continue;
            }
            QString confidence = confidenceItem->text();
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
    const bool wasSortingEnabled = m_tableView->isSortingEnabled();
    if (wasSortingEnabled) {
        m_tableView->setSortingEnabled(false);
    }

    // Scope the signal blocker to only block during population
    {
        QSignalBlocker blocker(m_tableModel);
        
        // Clear existing data
        m_tableModel->setRowCount(0);
        
        // Set new row count
        m_tableModel->setRowCount(m_entries.size());

    for (int row = 0; row < m_entries.size(); ++row) {
        const auto& entry = m_entries[row];
        
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

        // Locked version (editable when lock is enabled)
        auto* lockedItem = new QStandardItem(entry.locked_version);
        auto flags = lockedItem->flags();
        if (entry.version_locked) {
            lockedItem->setFlags(flags | Qt::ItemIsEditable);
        } else {
            lockedItem->setFlags(flags & ~Qt::ItemIsEditable);
        }
        m_tableModel->setItem(row, ColLockedVersion, lockedItem);
        
        // Status
        m_tableModel->setItem(row, ColStatus, new QStandardItem(entry.status));
        
        // Progress
        m_tableModel->setItem(row, ColProgress, new QStandardItem(QString::number(entry.progress) + "%"));
    }
    } // End signal blocker scope - signals now unblocked

    for (int i = 0; i < m_tableModel->rowCount(); ++i) {
        m_tableView->setRowHidden(i, false);
    }

    if (wasSortingEnabled) {
        m_tableView->setSortingEnabled(true);
    }

    const QString textFilter = m_filterEdit->text();
    const int confidenceIndex = m_confidenceFilter->currentIndex();
    
    if (!textFilter.isEmpty()) {
        onFilterChanged(textFilter);
    }
    if (confidenceIndex > 0) {
        onConfidenceFilterChanged(confidenceIndex);
    }
    
    // Force the view to update - signals are now unblocked so this should work
    m_tableView->viewport()->update();
    m_tableView->reset();
    m_tableView->scrollToTop();
}

void AppMigrationPanel::updateEntry(int row)
{
    if (row < 0 || row >= m_entries.size()) return;
    
    const auto& entry = m_entries[row];
    
    // Update table row
    if (row < m_tableModel->rowCount()) {
        if (auto* packageItem = m_tableModel->item(row, ColPackage)) {
            packageItem->setText(entry.choco_package);
        }
        auto* confItem = m_tableModel->item(row, ColConfidence);
        if (!confItem) {
            return;
        }
        confItem->setText(entry.match_confidence);
        if (entry.match_confidence == "High") {
            confItem->setForeground(Qt::darkGreen);
        } else if (entry.match_confidence == "Medium") {
            confItem->setForeground(QColor(255, 140, 0));
        } else if (entry.match_confidence == "Low") {
            confItem->setForeground(QColor(200, 100, 0));
        } else {
            confItem->setForeground(QBrush());
        }
        if (auto* lockItem = m_tableModel->item(row, ColVersionLock)) {
            lockItem->setCheckState(entry.version_locked ? Qt::Checked : Qt::Unchecked);
        }
        auto* lockedItem = m_tableModel->item(row, ColLockedVersion);
        if (lockedItem) {
            lockedItem->setText(entry.locked_version);
            auto flags = lockedItem->flags();
            if (entry.version_locked) {
                lockedItem->setFlags(flags | Qt::ItemIsEditable);
            } else {
                lockedItem->setFlags(flags & ~Qt::ItemIsEditable);
            }
        }
        if (auto* statusItem = m_tableModel->item(row, ColStatus)) {
            statusItem->setText(entry.status);
        }
        if (auto* progressItem = m_tableModel->item(row, ColProgress)) {
            progressItem->setText(QString::number(entry.progress) + "%");
        }
    }
}

void AppMigrationPanel::clearTable()
{
    m_tableModel->setRowCount(0);
    m_entries.clear();
}

void AppMigrationPanel::enableControls(bool enabled)
{
    int availableCount = 0;
    for (const auto& entry : m_entries) {
        if (entry.choco_available) {
            availableCount++;
        }
    }

    // Scan button - disabled during scan
    m_scanButton->setEnabled(enabled && !m_scanInProgress);
    
    // Match button - disabled during matching or if no entries
    m_matchButton->setEnabled(enabled && !m_matchingInProgress && !m_entries.isEmpty());
    
    // Install/backup operations - disabled during install or if nothing available
    bool canInstall = enabled && !m_installInProgress && availableCount > 0;
    m_backupButton->setEnabled(canInstall);
    m_installButton->setEnabled(canInstall);
    
    // Restore can run independently
    m_restoreButton->setEnabled(enabled);
    
    // Report operations - can run even during matching
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

    if (item->column() == ColSelect) {
        int row = item->row();
        if (row >= 0 && row < m_entries.size()) {
            m_entries[row].selected = (item->checkState() == Qt::Checked);
        }
        return;
    }

    // Check if this is the version lock column
    if (item->column() == ColVersionLock) {
        int row = item->row();
        if (row >= 0 && row < m_entries.size()) {
            // Update the entry's version_locked state
            m_entries[row].version_locked = (item->checkState() == Qt::Checked);

            if (m_entries[row].version_locked && m_entries[row].locked_version.isEmpty()) {
                m_entries[row].locked_version = !m_entries[row].version.isEmpty()
                    ? m_entries[row].version
                    : m_entries[row].available_version;
            }

            updateEntry(row);
            
            // Log the change
            QString lockStatus = m_entries[row].version_locked ? "locked" : "unlocked";
            m_logTextEdit->append(QString("Version %1 for %2")
                .arg(lockStatus)
                .arg(m_entries[row].app_name));
        }
        return;
    }

    if (item->column() == ColLockedVersion) {
        int row = item->row();
        if (row >= 0 && row < m_entries.size()) {
            m_entries[row].locked_version = item->text().trimmed();
        }
        return;
    }
}

