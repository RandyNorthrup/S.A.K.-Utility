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

    auto* sideBySide = new QHBoxLayout();
    sideBySide->setSpacing(sak::ui::kSpacingDefault);
    setupUi_packageTable(sideBySide);
    setupUi_queueSection(sideBySide);
    onlineLayout->addLayout(sideBySide, 1);

    setupUi_installActions(onlineLayout);

    m_tabWidget->addTab(onlineTab, tr("Online Install"));

    // --- Tab 1: Offline Deploy ---
    setupUi_offlineTab(m_tabWidget);

    mainLayout->addWidget(m_tabWidget, 1);

    setupUi_bottomBar(mainLayout);
}

void AppInstallationPanel::setupUi_searchBar(QVBoxLayout* mainLayout) {
    auto* topRow = new QHBoxLayout();

    auto* presetLabel = new QLabel(tr("Preset:"), this);
    topRow->addWidget(presetLabel);

    m_onlinePresetCombo = new QComboBox(this);
    m_onlinePresetCombo->addItem(tr("-- Select Preset --"));
    for (const auto& name : m_list_manager->presetNames()) {
        m_onlinePresetCombo->addItem(name);
    }
    m_onlinePresetCombo->setAccessibleName(QStringLiteral("Preset Package List"));
    m_onlinePresetCombo->setToolTip(tr("Select a preset package list to populate the queue"));
    topRow->addWidget(m_onlinePresetCombo, 1);

    mainLayout->addLayout(topRow);
}

void AppInstallationPanel::setupUi_packageTable(QHBoxLayout* sideBySide) {
    auto* searchGroup = new QGroupBox(tr("Search Packages"), this);
    auto* searchLayout = new QVBoxLayout(searchGroup);

    auto* searchRow = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search for packages (e.g., chrome, firefox)"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName(QStringLiteral("Package Search"));
    searchRow->addWidget(m_searchEdit, 1);

    m_searchButton = new QPushButton(tr("Search"), this);
    m_searchButton->setAccessibleName(QStringLiteral("Search Packages"));
    m_searchButton->setToolTip(QStringLiteral("Search the Chocolatey repository"));
    searchRow->addWidget(m_searchButton);
    searchLayout->addLayout(searchRow);

    m_onlineResultsModel = new QStandardItemModel(0, 3, this);
    m_onlineResultsModel->setHorizontalHeaderLabels(
        {tr("Package"), tr("Version"), tr("Publisher")});

    m_onlineResultsTable = new QTableView(this);
    m_onlineResultsTable->setModel(m_onlineResultsModel);
    m_onlineResultsTable->setAlternatingRowColors(true);
    m_onlineResultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_onlineResultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_onlineResultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_onlineResultsTable->verticalHeader()->setVisible(false);
    m_onlineResultsTable->horizontalHeader()->setStretchLastSection(true);
    m_onlineResultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_onlineResultsTable->horizontalHeader()->setSectionResizeMode(1,
                                                                   QHeaderView::ResizeToContents);
    m_onlineResultsTable->horizontalHeader()->setSectionResizeMode(2,
                                                                   QHeaderView::ResizeToContents);
    m_onlineResultsTable->setAccessibleName(QStringLiteral("Package Search Results"));
    m_onlineResultsTable->setToolTip(tr("Select a package and click Add to Queue"));
    searchLayout->addWidget(m_onlineResultsTable, 1);

    m_addToQueueButton = new QPushButton(tr("Add Selected to Queue  \u25b6"), this);
    m_addToQueueButton->setEnabled(false);
    m_addToQueueButton->setAccessibleName(QStringLiteral("Add to Queue"));
    m_addToQueueButton->setToolTip(QStringLiteral("Add the selected package to the install queue"));
    searchLayout->addWidget(m_addToQueueButton);

    sideBySide->addWidget(searchGroup, 1);
}

void AppInstallationPanel::setupUi_queueSection(QHBoxLayout* sideBySide) {
    auto* queueGroup = new QGroupBox(tr("Install Queue"), this);
    auto* queueLayout = new QVBoxLayout(queueGroup);

    m_queueList = new QListWidget(this);
    m_queueList->setAlternatingRowColors(true);
    m_queueList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queueList->setAccessibleName(QStringLiteral("Install Queue List"));
    m_queueList->setToolTip(QStringLiteral("Packages queued for installation"));
    queueLayout->addWidget(m_queueList, 1);

    auto* queueBtnRow = new QHBoxLayout();
    m_removeFromQueueButton = new QPushButton(tr("Remove"), this);
    m_removeFromQueueButton->setEnabled(false);
    m_removeFromQueueButton->setAccessibleName(QStringLiteral("Remove from Queue"));
    m_removeFromQueueButton->setToolTip(QStringLiteral("Remove selected packages from the queue"));
    queueBtnRow->addWidget(m_removeFromQueueButton);

    m_clearQueueButton = new QPushButton(tr("Clear All"), this);
    m_clearQueueButton->setEnabled(false);
    m_clearQueueButton->setAccessibleName(QStringLiteral("Clear Queue"));
    m_clearQueueButton->setToolTip(QStringLiteral("Remove all packages from the queue"));
    queueBtnRow->addWidget(m_clearQueueButton);

    queueBtnRow->addStretch();

    m_saveQueueButton = new QPushButton(tr("Save List"), this);
    m_saveQueueButton->setAccessibleName(QStringLiteral("Save Install List"));
    m_saveQueueButton->setToolTip(
        tr("Save the current install queue to a JSON file for later use"));
    m_saveQueueButton->setEnabled(false);
    connect(m_saveQueueButton, &QPushButton::clicked, this, &AppInstallationPanel::saveQueueToFile);
    queueBtnRow->addWidget(m_saveQueueButton);

    auto* loadQueueBtn = new QPushButton(tr("Load List"), this);
    loadQueueBtn->setAccessibleName(QStringLiteral("Load Install List"));
    loadQueueBtn->setToolTip(tr("Load a previously saved app list into the install queue"));
    connect(loadQueueBtn, &QPushButton::clicked, this, &AppInstallationPanel::loadQueueFromFile);
    queueBtnRow->addWidget(loadQueueBtn);

    queueLayout->addLayout(queueBtnRow);
    sideBySide->addWidget(queueGroup, 1);
}

void AppInstallationPanel::setupUi_installActions(QVBoxLayout* mainLayout) {
    auto* actionsGroup = new QGroupBox(tr("Installation Actions"), this);
    auto* actionsLayout = new QVBoxLayout(actionsGroup);

    m_installButton = new QPushButton(tr("Install All"), this);
    m_installButton->setEnabled(false);
    m_installButton->setAccessibleName(QStringLiteral("Install All Packages"));
    m_installButton->setToolTip(QStringLiteral("Install all queued packages"));
    m_installButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    actionsLayout->addWidget(m_installButton);

    m_progressLabel = new QLabel(this);
    m_progressLabel->setVisible(false);
    actionsLayout->addWidget(m_progressLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("%v / %m");
    actionsLayout->addWidget(m_progressBar);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    m_cancelButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    m_cancelButton->setAccessibleName(QStringLiteral("Cancel Installation"));
    m_cancelButton->setToolTip(QStringLiteral("Cancel the current installation process"));
    actionsLayout->addWidget(m_cancelButton);

    mainLayout->addWidget(actionsGroup);
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
    // Preset
    connect(m_onlinePresetCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &AppInstallationPanel::onOnlinePresetSelected);

    // Search
    connect(m_searchButton, &QPushButton::clicked, this, &AppInstallationPanel::onSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &AppInstallationPanel::onSearch);

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

    // Results selection — enable Add button when a row is selected
    connect(m_onlineResultsTable->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this]() {
                m_addToQueueButton->setEnabled(
                    m_onlineResultsTable->selectionModel()->hasSelection() &&
                    !m_install_in_progress);
            });

    // Double-click a search result to add it directly
    connect(m_onlineResultsTable, &QTableView::doubleClicked, this, [this](const QModelIndex&) {
        onAddToQueue();
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

    // --- Preset row (spans full width) ---
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
    offlineLayout->addLayout(presetRow);

    // --- Side-by-side: Search (left) | Deploy List (right) ---
    auto* sideBySide = new QHBoxLayout();
    sideBySide->setSpacing(sak::ui::kSpacingDefault);

    // Left: Search panel
    auto* searchGroup = new QGroupBox(tr("Search Packages"), this);
    auto* searchLayout = new QVBoxLayout(searchGroup);

    auto* searchRow = new QHBoxLayout();
    m_offlinePackageEdit = new QLineEdit(this);
    m_offlinePackageEdit->setPlaceholderText(tr("Search for packages (e.g., chrome, firefox)"));
    m_offlinePackageEdit->setClearButtonEnabled(true);
    m_offlinePackageEdit->setAccessibleName(QStringLiteral("Package Search Input"));
    searchRow->addWidget(m_offlinePackageEdit, 1);

    m_offlineSearchButton = new QPushButton(tr("Search"), this);
    m_offlineSearchButton->setAccessibleName(QStringLiteral("Search Packages"));
    m_offlineSearchButton->setToolTip(tr("Search the Chocolatey repository for matching packages"));
    searchRow->addWidget(m_offlineSearchButton);
    searchLayout->addLayout(searchRow);

    m_offlineResultsModel = new QStandardItemModel(0, 3, this);
    m_offlineResultsModel->setHorizontalHeaderLabels(
        {tr("Package"), tr("Version"), tr("Publisher")});

    m_offlineResultsTable = new QTableView(this);
    m_offlineResultsTable->setModel(m_offlineResultsModel);
    m_offlineResultsTable->setAlternatingRowColors(true);
    m_offlineResultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_offlineResultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_offlineResultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_offlineResultsTable->verticalHeader()->setVisible(false);
    m_offlineResultsTable->horizontalHeader()->setStretchLastSection(true);
    m_offlineResultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_offlineResultsTable->horizontalHeader()->setSectionResizeMode(1,
                                                                    QHeaderView::ResizeToContents);
    m_offlineResultsTable->horizontalHeader()->setSectionResizeMode(2,
                                                                    QHeaderView::ResizeToContents);
    m_offlineResultsTable->setAccessibleName(QStringLiteral("Package Search Results"));
    m_offlineResultsTable->setToolTip(tr("Select a package from the search results and click Add"));
    searchLayout->addWidget(m_offlineResultsTable, 1);

    m_offlineAddButton = new QPushButton(tr("Add Selected"), this);
    m_offlineAddButton->setAccessibleName(QStringLiteral("Add Package to List"));
    m_offlineAddButton->setToolTip(
        tr("Add the selected search result to the offline deployment list"));
    m_offlineAddButton->setEnabled(false);
    searchLayout->addWidget(m_offlineAddButton);

    sideBySide->addWidget(searchGroup, 1);

    // Right: Deploy list
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
    sideBySide->addWidget(listGroup, 1);

    offlineLayout->addLayout(sideBySide, 1);

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

    // Search button and Enter trigger offline search
    connect(
        m_offlineSearchButton, &QPushButton::clicked, this, &AppInstallationPanel::onOfflineSearch);
    connect(m_offlinePackageEdit,
            &QLineEdit::returnPressed,
            this,
            &AppInstallationPanel::onOfflineSearch);

    // Double-click a search result to add it directly
    connect(m_offlineResultsTable, &QTableView::doubleClicked, this, [this](const QModelIndex&) {
        onAddToOfflineList();
    });

    // Enable/disable Add button based on selection
    connect(m_offlineResultsTable->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this]() {
                m_offlineAddButton->setEnabled(
                    m_offlineResultsTable->selectionModel()->hasSelection());
            });

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
