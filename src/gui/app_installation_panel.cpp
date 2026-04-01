// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file app_installation_panel.cpp
/// @brief Implements the application installation panel UI for software reinstallation

#include "sak/app_installation_panel.h"

#include "sak/app_installation_worker.h"
#include "sak/chocolatey_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/elevation_banner.h"
#include "sak/install_summary_dialog.h"
#include "sak/logger.h"
#include "sak/migration_report.h"
#include "sak/offline_deployment_worker.h"
#include "sak/package_list_manager.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QScrollArea>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QVBoxLayout>

#include <memory>

using sak::AppInstallationPanel;
using sak::AppInstallationWorker;
using sak::ChocolateyManager;
using sak::MigrationJob;
using sak::MigrationReport;
using sak::MigrationStatus;

// Results table columns
enum ResultColumn {
    RColCheck = 0,
    RColPackage,
    RColVersion,
    RColPublisher,
    RColCount
};

AppInstallationPanel::AppInstallationPanel(QWidget* parent)
    : QWidget(parent)
    , m_choco_manager(std::make_shared<ChocolateyManager>())
    , m_worker(std::make_shared<AppInstallationWorker>(m_choco_manager))
    , m_list_manager(std::make_unique<PackageListManager>())
    , m_offline_worker(std::make_unique<OfflineDeploymentWorker>()) {
    setupUi();
    setupConnections();

    // Initialize Chocolatey on startup
    QString chocoPath = QApplication::applicationDirPath() + "/tools/chocolatey";
    sak::logInfo("[AppInstallationPanel] Initializing Chocolatey from: {}",
                 chocoPath.toStdString());
    bool init_success = m_choco_manager->initialize(chocoPath);
    if (!init_success) {
        sak::logWarning("[AppInstallationPanel] Chocolatey initialization failed");
        Q_EMIT logOutput(QString("WARNING: Chocolatey initialization failed"));
        Q_EMIT logOutput("Package installation will not be available.");
    } else {
        sak::logInfo("[AppInstallationPanel] Chocolatey initialized successfully (version: {})",
                     m_choco_manager->getChocoVersion().toStdString());
        Q_EMIT logOutput("Chocolatey initialized successfully");
        Q_EMIT logOutput("Use the search bar to find packages or select a category.");
    }
}

AppInstallationPanel::~AppInstallationPanel() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
    }
    if (m_offline_worker && m_offline_worker->isRunning()) {
        m_offline_worker->cancel();
    }
}

void AppInstallationPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setSpacing(sak::ui::kSpacingDefault);
    mainLayout->setContentsMargins(sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    // Elevation info banner (hidden when already admin)
    if (auto* banner = sak::createElevationBanner(contentWidget)) {
        mainLayout->addWidget(banner);
    }

    // === Tab Widget: Online Install | Offline Deploy ===
    m_tabWidget = new QTabWidget(this);

    // --- Tab 0: Online Install ---
    auto* onlineTab = new QWidget(this);
    auto* onlineLayout = new QVBoxLayout(onlineTab);
    onlineLayout->setContentsMargins(0, sak::ui::kMarginSmall, 0, 0);
    onlineLayout->setSpacing(sak::ui::kSpacingDefault);

    setupUi_searchBar(onlineLayout);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    setupUi_packageTable(splitter);
    setupUi_queueSection(splitter);

    splitter->setHandleWidth(6);
    splitter->setStretchFactor(0, 65);
    splitter->setStretchFactor(1, 35);
    onlineLayout->addWidget(splitter, 1);

    m_tabWidget->addTab(onlineTab, tr("Online Install"));

    // --- Tab 1: Offline Deploy ---
    setupUi_offlineTab(m_tabWidget);

    mainLayout->addWidget(m_tabWidget, 1);

    setupUi_bottomBar(mainLayout);
}

void AppInstallationPanel::setupUi_searchBar(QVBoxLayout* mainLayout) {
    auto* searchGroup = new QGroupBox(tr("Search Packages"), this);
    auto* searchLayout = new QHBoxLayout(searchGroup);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search Chocolatey packages..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName(QStringLiteral("Package Search"));
    m_searchEdit->setToolTip(QStringLiteral("Type to search for Chocolatey packages"));
    searchLayout->addWidget(m_searchEdit, 1);

    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->addItems({tr("All"),
                               tr("Browsers"),
                               tr("Development"),
                               tr("Media"),
                               tr("Utilities"),
                               tr("Security"),
                               tr("Productivity"),
                               tr("Communication")});
    m_categoryCombo->setAccessibleName(QStringLiteral("Package Category"));
    m_categoryCombo->setToolTip(tr("Select a category to browse"));
    searchLayout->addWidget(m_categoryCombo);

    m_searchButton = new QPushButton(tr("Search"), this);
    m_searchButton->setAccessibleName(QStringLiteral("Search Packages"));
    m_searchButton->setToolTip(QStringLiteral("Search the Chocolatey repository"));
    searchLayout->addWidget(m_searchButton);

    mainLayout->addWidget(searchGroup);
}

void AppInstallationPanel::setupUi_packageTable(QSplitter* splitter) {
    Q_ASSERT(splitter);
    auto* resultsWidget = new QWidget(this);
    auto* resultsLayout = new QVBoxLayout(resultsWidget);
    resultsLayout->setContentsMargins(0, 0, 8, 0);

    auto* resultsLabel = new QLabel(tr("Search Results"), this);
    resultsLabel->setStyleSheet("QLabel { font-weight: 600; }");
    resultsLayout->addWidget(resultsLabel);

    m_resultsTable = new QTableView(this);
    m_resultsModel = new QStandardItemModel(0, RColCount, this);
    m_resultsModel->setHorizontalHeaderLabels(
        {tr(""), tr("Package"), tr("Version"), tr("Publisher")});

    m_resultsTable->setModel(m_resultsModel);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_resultsTable->setAlternatingRowColors(true);
    m_resultsTable->setSortingEnabled(true);
    m_resultsTable->verticalHeader()->setVisible(false);
    m_resultsTable->setAccessibleName(QStringLiteral("Search Results Table"));
    m_resultsTable->setToolTip(QStringLiteral("Packages matching your search query"));

    auto* header = m_resultsTable->horizontalHeader();
    header->setSectionResizeMode(RColCheck, QHeaderView::Fixed);
    header->setSectionResizeMode(RColPackage, QHeaderView::Stretch);
    header->setSectionResizeMode(RColVersion, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(RColPublisher, QHeaderView::ResizeToContents);
    m_resultsTable->setColumnWidth(RColCheck, 36);

    m_resultsTable->setStyleSheet(
        QString("QTableView::indicator { width: 16px; height: 16px; border: 1px solid %1; "
                "border-radius: 4px; background: %2; }"
                "QTableView::indicator:checked { background: %3; border: 1px solid %4; }"
                "QTableView::indicator:unchecked { background: %2; border: 1px solid %1; }")
            .arg(sak::ui::kColorBorderMuted,
                 sak::ui::kColorBgSurface,
                 sak::ui::kColorPrimary,
                 sak::ui::kColorPrimaryDark));

    resultsLayout->addWidget(m_resultsTable, 1);

    m_addToQueueButton = new QPushButton(tr("Add Selected to Queue  \u25b6"), this);
    m_addToQueueButton->setEnabled(false);
    m_addToQueueButton->setAccessibleName(QStringLiteral("Add to Queue"));
    m_addToQueueButton->setToolTip(QStringLiteral("Add checked packages to the install queue"));
    resultsLayout->addWidget(m_addToQueueButton);

    splitter->addWidget(resultsWidget);
}

void AppInstallationPanel::setupUi_queueSection(QSplitter* splitter) {
    auto* queueWidget = new QWidget(this);
    auto* queueLayout = new QVBoxLayout(queueWidget);
    queueLayout->setContentsMargins(8, 0, 0, 0);

    auto* queueLabel = new QLabel(tr("Install Queue"), this);
    queueLabel->setStyleSheet("QLabel { font-weight: 600; }");
    queueLayout->addWidget(queueLabel);

    m_queueList = new QListWidget(this);
    m_queueList->setAlternatingRowColors(true);
    m_queueList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queueList->setAccessibleName(QStringLiteral("Install Queue List"));
    m_queueList->setToolTip(QStringLiteral("Packages queued for installation"));
    queueLayout->addWidget(m_queueList, 1);

    auto* queueButtonLayout = new QHBoxLayout();
    m_removeFromQueueButton = new QPushButton(tr("Remove"), this);
    m_removeFromQueueButton->setEnabled(false);
    m_removeFromQueueButton->setAccessibleName(QStringLiteral("Remove from Queue"));
    m_removeFromQueueButton->setToolTip(QStringLiteral("Remove selected packages from the queue"));
    queueButtonLayout->addWidget(m_removeFromQueueButton);

    m_clearQueueButton = new QPushButton(tr("Clear All"), this);
    m_clearQueueButton->setEnabled(false);
    m_clearQueueButton->setAccessibleName(QStringLiteral("Clear Queue"));
    m_clearQueueButton->setToolTip(QStringLiteral("Remove all packages from the queue"));
    queueButtonLayout->addWidget(m_clearQueueButton);
    queueLayout->addLayout(queueButtonLayout);

    // Progress indicator (hidden until installation starts)
    m_progressLabel = new QLabel(this);
    m_progressLabel->setVisible(false);
    queueLayout->addWidget(m_progressLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("%v / %m");
    queueLayout->addWidget(m_progressBar);

    m_installButton = new QPushButton(tr("Install All"), this);
    m_installButton->setEnabled(false);
    m_installButton->setAccessibleName(QStringLiteral("Install All Packages"));
    m_installButton->setToolTip(QStringLiteral("Install all queued packages"));
    m_installButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    queueLayout->addWidget(m_installButton);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    m_cancelButton->setAccessibleName(QStringLiteral("Cancel Installation"));
    m_cancelButton->setToolTip(QStringLiteral("Cancel the current installation process"));
    queueLayout->addWidget(m_cancelButton);

    // Save/Load queue buttons
    auto* saveLoadLayout = new QHBoxLayout();
    m_saveQueueButton = new QPushButton(tr("Save List"), this);
    m_saveQueueButton->setAccessibleName(QStringLiteral("Save Install List"));
    m_saveQueueButton->setToolTip(
        tr("Save the current install queue to a JSON file for later use"));
    m_saveQueueButton->setEnabled(false);
    connect(m_saveQueueButton, &QPushButton::clicked, this, &AppInstallationPanel::saveQueueToFile);
    saveLoadLayout->addWidget(m_saveQueueButton);

    auto* loadQueueBtn = new QPushButton(tr("Load List"), this);
    loadQueueBtn->setAccessibleName(QStringLiteral("Load Install List"));
    loadQueueBtn->setToolTip(tr("Load a previously saved app list into the install queue"));
    connect(loadQueueBtn, &QPushButton::clicked, this, &AppInstallationPanel::loadQueueFromFile);
    saveLoadLayout->addWidget(loadQueueBtn);
    queueLayout->addLayout(saveLoadLayout);

    splitter->addWidget(queueWidget);
}

void AppInstallationPanel::setupUi_bottomBar(QVBoxLayout* mainLayout) {
    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(0, 4, 0, 0);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();
    mainLayout->addLayout(bottomLayout);
}

void AppInstallationPanel::setupConnections() {
    setupSearchAndQueueConnections();
    setupWorkerConnections();
    setupOfflineConnections();
}

void AppInstallationPanel::setupSearchAndQueueConnections() {
    // Search
    connect(m_searchButton, &QPushButton::clicked, this, &AppInstallationPanel::onSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &AppInstallationPanel::onSearch);
    connect(m_categoryCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &AppInstallationPanel::onCategoryChanged);

    // Queue management
    connect(m_addToQueueButton, &QPushButton::clicked, this, &AppInstallationPanel::onAddToQueue);
    connect(m_removeFromQueueButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onRemoveFromQueue);
    connect(m_clearQueueButton, &QPushButton::clicked, this, &AppInstallationPanel::onClearQueue);

    // Install
    connect(m_installButton, &QPushButton::clicked, this, &AppInstallationPanel::onInstallAll);
    connect(m_cancelButton, &QPushButton::clicked, this, &AppInstallationPanel::onCancelInstall);

    // Queue selection
    connect(m_queueList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_removeFromQueueButton->setEnabled(!m_queueList->selectedItems().isEmpty() &&
                                            !m_install_in_progress);
    });

    // Results selection
    connect(m_resultsModel, &QStandardItemModel::itemChanged, this, [this](QStandardItem* item) {
        if (!item || item->column() != RColCheck) {
            return;
        }
        bool anyChecked = false;
        for (int i = 0; i < m_resultsModel->rowCount(); ++i) {
            auto* checkItem = m_resultsModel->item(i, RColCheck);
            if (checkItem && checkItem->checkState() == Qt::Checked) {
                anyChecked = true;
                break;
            }
        }
        m_addToQueueButton->setEnabled(anyChecked && !m_install_in_progress);
    });
}

void AppInstallationPanel::setupWorkerConnections() {
    connect(m_worker.get(), &AppInstallationWorker::migrationStarted, this, [this](int totalJobs) {
        m_progressBar->setRange(0, totalJobs);
        m_progressBar->setValue(0);
        m_progressBar->setVisible(true);
        m_progressLabel->setText(tr("Installing 0 of %1...").arg(totalJobs));
        m_progressLabel->setVisible(true);
        Q_EMIT progressUpdated(0, totalJobs);
        Q_EMIT statusMessage("App Installation: Installing packages...", 0);
        Q_EMIT logOutput(QString("Installation started: %1 package(s)").arg(totalJobs));
    });

    connect(m_worker.get(),
            &AppInstallationWorker::jobProgress,
            this,
            [this](int entryIndex, const QString& message) {
                Q_UNUSED(entryIndex);
                Q_EMIT logOutput(message);
                Q_EMIT statusMessage(message, 0);
            });

    connect(
        m_worker.get(),
        &AppInstallationWorker::jobStatusChanged,
        this,
        [this](int entryIndex, const MigrationJob& job) {
            Q_UNUSED(entryIndex);
            switch (job.status) {
            case MigrationStatus::Installing:
                Q_EMIT statusMessage(tr("Installing %1...").arg(job.packageId), 0);
                break;
            case MigrationStatus::Success:
                Q_EMIT logOutput(QString("[x] %1 installed successfully").arg(job.packageId));
                break;
            case MigrationStatus::Failed:
                Q_EMIT logOutput(QString("[ ] %1 failed: %2").arg(job.packageId, job.errorMessage));
                break;
            default:
                break;
            }

            auto stats = m_worker->getStats();
            int completed = stats.success + stats.failed + stats.skipped + stats.cancelled;
            m_progressBar->setValue(completed);
            m_progressLabel->setText(tr("Installing %1 of %2...").arg(completed).arg(stats.total));
            Q_EMIT progressUpdated(completed, stats.total);
        });

    connect(m_worker.get(),
            &AppInstallationWorker::migrationCompleted,
            this,
            [this](const AppInstallationWorker::Stats& stats) {
                sak::logInfo(
                    "[AppInstallationPanel] Installation complete: {} succeeded, "
                    "{} failed, {} skipped",
                    stats.success,
                    stats.failed,
                    stats.skipped);
                Q_EMIT logOutput(QString("Installation complete: %1 succeeded, %2 failed, %3 "
                                         "skipped")
                                     .arg(stats.success)
                                     .arg(stats.failed)
                                     .arg(stats.skipped));
                Q_EMIT statusMessage(QString("App Installation: %1 succeeded, %2 failed")
                                         .arg(stats.success)
                                         .arg(stats.failed),
                                     5000);

                Q_EMIT statusMessage(tr("Installation complete"), 5000);
                m_progressBar->setVisible(false);
                m_progressLabel->setVisible(false);
                m_install_in_progress = false;
                m_cancelButton->setVisible(false);
                m_cancelButton->setEnabled(false);
                m_installButton->setVisible(true);
                enableControls(true);

                // Show summary modal with per-package results
                auto jobs = m_worker->getJobs();
                sak::InstallSummaryDialog dialog(stats, jobs, this);
                dialog.exec();
            });
}

// ============================================================================
// Offline Deployment Tab
// ============================================================================

void AppInstallationPanel::setupUi_offlineTab(QTabWidget* tabs) {
    auto* offlineTab = new QWidget(this);
    auto* offlineLayout = new QVBoxLayout(offlineTab);
    offlineLayout->setContentsMargins(0, sak::ui::kMarginSmall, 0, 0);
    offlineLayout->setSpacing(sak::ui::kSpacingDefault);

    // --- Preset selection ---
    auto* presetGroup = new QGroupBox(tr("Package List"), this);
    auto* presetLayout = new QVBoxLayout(presetGroup);

    auto* presetRow = new QHBoxLayout();
    auto* presetLabel = new QLabel(tr("Preset:"), this);
    presetRow->addWidget(presetLabel);

    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItem(tr("-- Select Preset --"));
    for (const auto& name : m_list_manager->presetNames()) {
        m_presetCombo->addItem(name);
    }
    m_presetCombo->setAccessibleName(QStringLiteral("Preset Package List"));
    m_presetCombo->setToolTip(tr("Select a preset package list to populate"));
    presetRow->addWidget(m_presetCombo, 1);
    presetLayout->addLayout(presetRow);

    // Add single package
    auto* addRow = new QHBoxLayout();
    m_offlinePackageEdit = new QLineEdit(this);
    m_offlinePackageEdit->setPlaceholderText(tr("Add package by ID (e.g., googlechrome)"));
    m_offlinePackageEdit->setClearButtonEnabled(true);
    m_offlinePackageEdit->setAccessibleName(QStringLiteral("Package ID Input"));
    addRow->addWidget(m_offlinePackageEdit, 1);

    m_offlineAddButton = new QPushButton(tr("Add"), this);
    m_offlineAddButton->setAccessibleName(QStringLiteral("Add Package to List"));
    m_offlineAddButton->setToolTip(tr("Add this package to the offline deployment list"));
    addRow->addWidget(m_offlineAddButton);
    presetLayout->addLayout(addRow);

    offlineLayout->addWidget(presetGroup);

    // --- Package list ---
    auto* listGroup = new QGroupBox(tr("Packages to Deploy"), this);
    auto* listLayout = new QVBoxLayout(listGroup);

    m_offlineListWidget = new QListWidget(this);
    m_offlineListWidget->setAlternatingRowColors(true);
    m_offlineListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_offlineListWidget->setAccessibleName(QStringLiteral("Offline Package List"));
    m_offlineListWidget->setToolTip(tr("Packages that will be included in the deployment bundle"));
    listLayout->addWidget(m_offlineListWidget, 1);

    auto* listBtnRow = new QHBoxLayout();
    m_offlineRemoveButton = new QPushButton(tr("Remove"), this);
    m_offlineRemoveButton->setEnabled(false);
    m_offlineRemoveButton->setAccessibleName(QStringLiteral("Remove from List"));
    listBtnRow->addWidget(m_offlineRemoveButton);

    m_offlineClearButton = new QPushButton(tr("Clear All"), this);
    m_offlineClearButton->setEnabled(false);
    m_offlineClearButton->setAccessibleName(QStringLiteral("Clear Offline List"));
    listBtnRow->addWidget(m_offlineClearButton);

    listBtnRow->addStretch();

    m_saveOfflineListButton = new QPushButton(tr("Save List"), this);
    m_saveOfflineListButton->setAccessibleName(QStringLiteral("Save Offline List"));
    m_saveOfflineListButton->setToolTip(tr("Save the offline package list to a JSON file"));
    m_saveOfflineListButton->setEnabled(false);
    listBtnRow->addWidget(m_saveOfflineListButton);

    m_loadOfflineListButton = new QPushButton(tr("Load List"), this);
    m_loadOfflineListButton->setAccessibleName(QStringLiteral("Load Offline List"));
    m_loadOfflineListButton->setToolTip(tr("Load a package list from a JSON file"));
    listBtnRow->addWidget(m_loadOfflineListButton);

    listLayout->addLayout(listBtnRow);
    offlineLayout->addWidget(listGroup, 1);

    // --- Actions ---
    auto* actionsGroup = new QGroupBox(tr("Deployment Actions"), this);
    auto* actionsLayout = new QVBoxLayout(actionsGroup);

    auto* actionBtnRow = new QHBoxLayout();

    m_buildBundleButton = new QPushButton(tr("Build Offline Bundle"), this);
    m_buildBundleButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_buildBundleButton->setEnabled(false);
    m_buildBundleButton->setAccessibleName(QStringLiteral("Build Offline Bundle"));
    m_buildBundleButton->setToolTip(
        tr("Download and internalize all listed packages into a portable bundle"));
    actionBtnRow->addWidget(m_buildBundleButton);

    m_directDownloadButton = new QPushButton(tr("Direct Download"), this);
    m_directDownloadButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_directDownloadButton->setEnabled(false);
    m_directDownloadButton->setAccessibleName(QStringLiteral("Direct Download"));
    m_directDownloadButton->setToolTip(tr("Download .nupkg files without internalization"));
    actionBtnRow->addWidget(m_directDownloadButton);

    m_installFromBundleButton = new QPushButton(tr("Install from Bundle"), this);
    m_installFromBundleButton->setStyleSheet(sak::ui::kSuccessButtonStyle);
    m_installFromBundleButton->setAccessibleName(QStringLiteral("Install from Bundle"));
    m_installFromBundleButton->setToolTip(
        tr("Install packages from a previously built offline bundle"));
    actionBtnRow->addWidget(m_installFromBundleButton);

    actionsLayout->addLayout(actionBtnRow);

    // Progress
    m_offlineProgressLabel = new QLabel(this);
    m_offlineProgressLabel->setVisible(false);
    actionsLayout->addWidget(m_offlineProgressLabel);

    m_offlineProgressBar = new QProgressBar(this);
    m_offlineProgressBar->setVisible(false);
    m_offlineProgressBar->setTextVisible(true);
    m_offlineProgressBar->setFormat("%v / %m");
    actionsLayout->addWidget(m_offlineProgressBar);

    m_offlineStatusLabel = new QLabel(this);
    m_offlineStatusLabel->setVisible(false);
    m_offlineStatusLabel->setStyleSheet(QString("color: %1;").arg(sak::ui::kColorTextMuted));
    actionsLayout->addWidget(m_offlineStatusLabel);

    m_cancelOfflineButton = new QPushButton(tr("Cancel"), this);
    m_cancelOfflineButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    m_cancelOfflineButton->setVisible(false);
    m_cancelOfflineButton->setAccessibleName(QStringLiteral("Cancel Offline Operation"));
    actionsLayout->addWidget(m_cancelOfflineButton);

    offlineLayout->addWidget(actionsGroup);

    tabs->addTab(offlineTab, tr("Offline Deploy"));
}

// ============================================================================
// Offline Deployment Connections
// ============================================================================

void AppInstallationPanel::setupOfflineConnections() {
    connect(m_presetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &AppInstallationPanel::onPresetSelected);

    connect(
        m_offlineAddButton, &QPushButton::clicked, this, &AppInstallationPanel::onAddToOfflineList);
    connect(m_offlinePackageEdit,
            &QLineEdit::returnPressed,
            this,
            &AppInstallationPanel::onAddToOfflineList);

    connect(m_offlineRemoveButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onRemoveFromOfflineList);
    connect(m_offlineClearButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onClearOfflineList);

    connect(m_buildBundleButton, &QPushButton::clicked, this, &AppInstallationPanel::onBuildBundle);
    connect(m_directDownloadButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onDirectDownload);
    connect(m_installFromBundleButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onInstallFromBundle);
    connect(m_cancelOfflineButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onCancelOfflineOperation);

    connect(m_saveOfflineListButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onSaveOfflineList);
    connect(m_loadOfflineListButton,
            &QPushButton::clicked,
            this,
            &AppInstallationPanel::onLoadOfflineList);

    connect(m_offlineListWidget, &QListWidget::itemSelectionChanged, this, [this]() {
        m_offlineRemoveButton->setEnabled(!m_offlineListWidget->selectedItems().isEmpty() &&
                                          !m_offline_in_progress);
    });

    // Worker signals
    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::operationStarted,
            this,
            [this](int total) {
                m_offlineProgressBar->setRange(0, total);
                m_offlineProgressBar->setValue(0);
                m_offlineProgressBar->setVisible(true);
                m_offlineProgressLabel->setText(tr("Processing 0 of %1...").arg(total));
                m_offlineProgressLabel->setVisible(true);
                m_cancelOfflineButton->setVisible(true);
                Q_EMIT progressUpdated(0, total);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::batchProgress,
            this,
            [this](int completed, int total, const QString& current) {
                m_offlineProgressBar->setValue(completed);
                m_offlineProgressLabel->setText(
                    tr("Processing %1 of %2: %3").arg(completed).arg(total).arg(current));
                Q_EMIT progressUpdated(completed, total);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::packageProgress,
            this,
            [this](const QString& pkg_id, bool success, const QString& msg) {
                QString log_line = success ? QString("[OK] %1: %2").arg(pkg_id, msg)
                                           : QString("[FAIL] %1: %2").arg(pkg_id, msg);
                Q_EMIT logOutput(log_line);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::logMessage,
            this,
            [this](const QString& msg) { Q_EMIT logOutput(msg); });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::operationCompleted,
            this,
            [this](const BatchStats& stats) {
                m_offlineProgressBar->setVisible(false);
                m_offlineProgressLabel->setVisible(false);
                m_cancelOfflineButton->setVisible(false);

                m_offlineStatusLabel->setText(
                    tr("Complete: %1 succeeded, %2 failed").arg(stats.completed).arg(stats.failed));
                m_offlineStatusLabel->setVisible(true);

                m_offline_in_progress = false;
                enableOfflineControls(true);

                Q_EMIT statusMessage(tr("Offline operation complete: %1 succeeded, %2 failed")
                                         .arg(stats.completed)
                                         .arg(stats.failed),
                                     5000);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::operationError,
            this,
            [this](const QString& error) {
                sak::logError("[AppInstallationPanel] Offline error: {}", error.toStdString());
                Q_EMIT logOutput(QString("ERROR: %1").arg(error));
                m_offline_in_progress = false;
                enableOfflineControls(true);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::manifestWritten,
            this,
            [this](const QString& path) {
                Q_EMIT logOutput(QString("Manifest written: %1").arg(path));
            });
}
