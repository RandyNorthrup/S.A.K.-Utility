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
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/migration_report.h"
#include "sak/offline_deployment_worker.h"
#include "sak/package_list_manager.h"
#include "sak/style_constants.h"
#include "sak/view_empty_state.h"
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

constexpr int kPackageResultColumnCount = 3;
constexpr int kPublisherResultColumn = 2;

namespace {

void applyCompactQueueButton(QPushButton* button, const QString& style) {
    button->setMinimumHeight(sak::ui::kUiButtonHeightDialog);
    button->setStyleSheet(style);
}

}  // namespace

AppInstallationPanel::AppInstallationPanel(QWidget* parent)
    : QWidget(parent)
    , m_list_manager(std::make_unique<PackageListManager>())
    , m_choco_manager(std::make_shared<ChocolateyManager>())
    , m_worker(std::make_shared<AppInstallationWorker>(m_choco_manager))
    , m_offline_worker(std::make_unique<OfflineDeploymentWorker>()) {
    sak::logInfo("[AppInstallationPanel] constructing");
    sak::logInfo("[AppInstallationPanel] setupUi start");
    setupUi();
    sak::logInfo("[AppInstallationPanel] setupUi complete");
    if (qApp->property("sakAccessibilityAudit").toBool()) {
        sak::logInfo(
            "[AppInstallationPanel] Accessibility audit: connections and external init skipped");
        return;
    }
    sak::logInfo("[AppInstallationPanel] setupConnections start");
    setupConnections();
    sak::logInfo("[AppInstallationPanel] setupConnections complete");

    // Initialize Chocolatey on startup
    const QString choco_path = QApplication::applicationDirPath() + "/tools/chocolatey";
    sak::logInfo("[AppInstallationPanel] Initializing Chocolatey from: {}",
                 choco_path.toStdString());
    const bool init_success = m_choco_manager->initialize(choco_path);
    sak::logInfo("[AppInstallationPanel] Chocolatey initialize returned");
    if (!init_success) {
        sak::logWarning("[AppInstallationPanel] Chocolatey initialization failed");
        Q_EMIT logOutput(QString("WARNING: Chocolatey initialization failed"));
        Q_EMIT logOutput("Package installation will not be available.");
    } else {
        sak::logInfo("[AppInstallationPanel] Chocolatey initialized successfully");
        Q_EMIT logOutput("Chocolatey initialized successfully");
        Q_EMIT logOutput("Use the search bar to find packages or select a category.");
    }
    sak::logInfo("[AppInstallationPanel] constructed");
}

AppInstallationPanel::~AppInstallationPanel() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
    }
    if (m_offline_worker && m_offline_worker->isRunning()) {
        m_offline_worker->cancel();
    }
    // Join the background search jobs before members are destroyed: their pool-
    // thread lambdas capture raw `this` and deref m_choco_manager, so a search
    // still running at teardown would use freed state. QtConcurrent::run futures
    // are not cancelable; waitForFinished() on a finished/null future is a no-op.
    // SAK-ALLOW-BLOCKING: a QtConcurrent::run future is not cancelable, so there is nothing
    // to give up TO -- the only alternative to waiting is the use-after-free above.
    m_searchFuture.waitForFinished();
    // SAK-ALLOW-BLOCKING: same contract as the search future above.
    m_offlineSearchFuture.waitForFinished();
}

void AppInstallationPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setSpacing(sak::ui::kSpacingDefault);
    main_layout->setContentsMargins(sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium);

    // Elevation info banner (hidden when already admin)
    if (auto* banner = sak::createElevationBanner(content_widget)) {
        main_layout->addWidget(banner);
    }

    // === Tab Widget: Online Install | Offline Deploy ===
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setAccessibleName(tr("Application installation mode tabs"));

    // --- Tab 0: Online Install ---
    auto* online_tab = new QWidget(this);
    auto* online_layout = new QVBoxLayout(online_tab);
    online_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginSmall, sak::ui::kMarginNone, sak::ui::kMarginNone);
    online_layout->setSpacing(sak::ui::kSpacingDefault);

    setupUi_searchBar(online_layout);

    auto* side_by_side = new QHBoxLayout();
    side_by_side->setSpacing(sak::ui::kSpacingDefault);
    setupUi_packageTable(side_by_side);
    setupUi_queueSection(side_by_side);
    online_layout->addLayout(side_by_side, 1);

    m_tabWidget->addTab(online_tab, tr("Online Install"));

    setupUi_offlineTab(m_tabWidget);

    main_layout->addWidget(m_tabWidget, 1);

    setupUi_bottomBar(main_layout);

    scroll_area->setWidget(content_widget);
    root_layout->addWidget(scroll_area);
}

void AppInstallationPanel::setupUi_searchBar(QVBoxLayout* main_layout) {
    auto* top_row = new QHBoxLayout();

    auto* preset_label = new QLabel(tr("Preset:"), this);
    top_row->addWidget(preset_label);

    m_onlinePresetCombo = new QComboBox(this);
    m_onlinePresetCombo->addItem(tr("-- Select Preset --"));
    for (const auto& name : m_list_manager->presetNames()) {
        m_onlinePresetCombo->addItem(name);
    }
    m_onlinePresetCombo->setAccessibleName(QStringLiteral("Preset Package List"));
    m_onlinePresetCombo->setToolTip(tr("Select a preset package list to populate the queue"));
    top_row->addWidget(m_onlinePresetCombo, 1);

    main_layout->addLayout(top_row);
}

void AppInstallationPanel::setupUi_packageTable(QHBoxLayout* side_by_side) {
    auto* search_group = new QGroupBox(tr("Search Packages"), this);
    auto* search_layout = new QVBoxLayout(search_group);

    auto* search_row = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search for packages (e.g., chrome, firefox)"));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName(QStringLiteral("Package Search"));
    search_row->addWidget(m_searchEdit, 1);

    m_searchButton = new QPushButton(tr("Search"), this);
    m_searchButton->setAccessibleName(QStringLiteral("Search Packages"));
    m_searchButton->setToolTip(QStringLiteral("Search the Chocolatey repository"));
    m_searchButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    search_row->addWidget(m_searchButton);
    search_layout->addLayout(search_row);

    m_onlineResultsModel = new QStandardItemModel(0, kPackageResultColumnCount, this);
    m_onlineResultsModel->setHorizontalHeaderLabels(
        {tr("Package"), tr("Version"), tr("Publisher")});

    m_onlineResultsTable = new QTableView(this);
    m_onlineResultsTable->setModel(m_onlineResultsModel);
    sak::configureStandardTable(m_onlineResultsTable, QAbstractItemView::SingleSelection);
    m_onlineResultsTable->horizontalHeader()->setStretchLastSection(true);
    m_onlineResultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_onlineResultsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_onlineResultsTable->horizontalHeader()->setSectionResizeMode(kPublisherResultColumn,
                                                                   QHeaderView::Interactive);
    m_onlineResultsTable->setAccessibleName(QStringLiteral("Package Search Results"));
    m_onlineResultsTable->setToolTip(tr("Select a package and click Add to Queue"));
    // Designed empty/loading overlay (constructed after setModel, per the helper contract).
    m_onlineResultsState = new sak::ui::ViewEmptyState(
        m_onlineResultsTable, tr("No results yet - search for packages to install"));
    search_layout->addWidget(m_onlineResultsTable, 1);

    m_addToQueueButton = new QPushButton(tr("Add Selected to Queue  \u25b6"), this);
    m_addToQueueButton->setEnabled(false);
    m_addToQueueButton->setAccessibleName(QStringLiteral("Add to Queue"));
    m_addToQueueButton->setToolTip(QStringLiteral("Add the selected package to the install queue"));
    m_addToQueueButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    search_layout->addWidget(m_addToQueueButton);

    side_by_side->addWidget(search_group, 1);
}

void AppInstallationPanel::setupUi_queueSection(QHBoxLayout* side_by_side) {
    auto* queue_group = new QGroupBox(tr("Install Queue"), this);
    auto* queue_layout = new QVBoxLayout(queue_group);

    m_queueList = new QListWidget(this);
    m_queueList->setAlternatingRowColors(true);
    m_queueList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_queueList->setAccessibleName(QStringLiteral("Install Queue List"));
    m_queueList->setToolTip(QStringLiteral("Packages queued for installation"));
    // Empty-only overlay (no scan): self-managed via the view parent, no stored member.
    new sak::ui::ViewEmptyState(m_queueList, tr("No packages queued - add from search results"));
    queue_layout->addWidget(m_queueList, 1);

    setupUi_queueButtons(queue_layout);
    setupUi_queueProgress(queue_layout);
    side_by_side->addWidget(queue_group, 1);
}

void AppInstallationPanel::setupUi_queueButtons(QVBoxLayout* queue_layout) {
    auto* queue_btn_row = new QHBoxLayout();
    m_removeFromQueueButton = new QPushButton(tr("Remove"), this);
    m_removeFromQueueButton->setEnabled(false);
    m_removeFromQueueButton->setAccessibleName(QStringLiteral("Remove from Queue"));
    m_removeFromQueueButton->setToolTip(QStringLiteral("Remove selected packages from the queue"));
    applyCompactQueueButton(m_removeFromQueueButton, sak::ui::kCompactPrimaryButtonStyle);
    queue_btn_row->addWidget(m_removeFromQueueButton);

    m_clearQueueButton = new QPushButton(tr("Clear All"), this);
    m_clearQueueButton->setEnabled(false);
    m_clearQueueButton->setAccessibleName(QStringLiteral("Clear Queue"));
    m_clearQueueButton->setToolTip(QStringLiteral("Remove all packages from the queue"));
    applyCompactQueueButton(m_clearQueueButton, sak::ui::kCompactPrimaryButtonStyle);
    queue_btn_row->addWidget(m_clearQueueButton);

    m_installButton = new QPushButton(tr("Install All"), this);
    m_installButton->setEnabled(false);
    m_installButton->setAccessibleName(QStringLiteral("Install All Packages"));
    m_installButton->setToolTip(QStringLiteral("Install all queued packages"));
    applyCompactQueueButton(m_installButton, sak::ui::kCompactPrimaryButtonStyle);
    queue_btn_row->addWidget(m_installButton);

    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setEnabled(false);
    m_cancelButton->setVisible(false);
    m_cancelButton->setAccessibleName(QStringLiteral("Cancel Installation"));
    m_cancelButton->setToolTip(QStringLiteral("Cancel the current installation process"));
    applyCompactQueueButton(m_cancelButton, sak::ui::kCompactDangerButtonStyle);
    queue_btn_row->addWidget(m_cancelButton);

    queue_btn_row->addStretch();

    m_saveQueueButton = new QPushButton(tr("Save List"), this);
    m_saveQueueButton->setAccessibleName(QStringLiteral("Save Install List"));
    m_saveQueueButton->setToolTip(
        tr("Save the current install queue to a JSON file for later use"));
    m_saveQueueButton->setEnabled(false);
    applyCompactQueueButton(m_saveQueueButton, sak::ui::kCompactSecondaryButtonStyle);
    connect(m_saveQueueButton, &QPushButton::clicked, this, &AppInstallationPanel::saveQueueToFile);
    queue_btn_row->addWidget(m_saveQueueButton);

    auto* load_queue_btn = new QPushButton(tr("Load List"), this);
    load_queue_btn->setAccessibleName(QStringLiteral("Load Install List"));
    load_queue_btn->setToolTip(tr("Load a previously saved app list into the install queue"));
    applyCompactQueueButton(load_queue_btn, sak::ui::kCompactSecondaryButtonStyle);
    connect(load_queue_btn, &QPushButton::clicked, this, &AppInstallationPanel::loadQueueFromFile);
    queue_btn_row->addWidget(load_queue_btn);

    queue_layout->addLayout(queue_btn_row);
}

void AppInstallationPanel::setupUi_queueProgress(QVBoxLayout* queue_layout) {
    m_progressLabel = new QLabel(this);
    m_progressLabel->setAccessibleName(QStringLiteral("Installation Progress Status"));
    m_progressLabel->setVisible(false);
    queue_layout->addWidget(m_progressLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setAccessibleName(QStringLiteral("Installation Progress"));
    m_progressBar->setVisible(false);
    m_progressBar->setTextVisible(true);
    m_progressBar->setFormat("%v / %m");
    queue_layout->addWidget(m_progressBar);
}

void AppInstallationPanel::setupUi_bottomBar(QVBoxLayout* main_layout) {
    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);
    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kSpacingTight, sak::ui::kMarginNone, sak::ui::kMarginNone);
    bottom_layout->addWidget(m_logToggle);
    bottom_layout->addStretch();
    main_layout->addLayout(bottom_layout);
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
    // Selection-driven enablement reads the single in-flight authority, so a running
    // offline deployment operation keeps these online controls disabled too.
    connect(m_queueList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_removeFromQueueButton->setEnabled(!m_queueList->selectedItems().isEmpty() &&
                                            !packageOperationInFlight());
    });

    // Results selection - enable Add button when a row is selected
    connect(m_onlineResultsTable->selectionModel(),
            &QItemSelectionModel::selectionChanged,
            this,
            [this]() {
                m_addToQueueButton->setEnabled(
                    m_onlineResultsTable->selectionModel()->hasSelection() &&
                    !packageOperationInFlight());
            });

    // Double-click a search result to add it directly
    connect(m_onlineResultsTable, &QTableView::doubleClicked, this, [this](const QModelIndex&) {
        onAddToQueue();
    });
}

void AppInstallationPanel::setupWorkerConnections() {
    setupWorkerLifecycleConnections();
    setupWorkerJobConnections();
}

void AppInstallationPanel::setupWorkerLifecycleConnections() {
    connect(m_worker.get(), &AppInstallationWorker::migrationStarted, this, [this](int total_jobs) {
        m_progressBar->setRange(0, total_jobs);
        m_progressBar->setValue(0);
        m_progressBar->setVisible(true);
        m_progressLabel->setText(tr("Installing 0 of %1...").arg(total_jobs));
        m_progressLabel->setVisible(true);
        Q_EMIT progressUpdated(0, total_jobs);
        Q_EMIT statusMessage("App Installation: Installing packages...", 0);
        Q_EMIT logOutput(QString("Installation started: %1 package(s)").arg(total_jobs));
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
                                     sak::kTimerStatusDefaultMs);

                Q_EMIT statusMessage(tr("Installation complete"), sak::kTimerStatusDefaultMs);
                m_progressBar->setVisible(false);
                m_progressLabel->setVisible(false);
                // Releases the online claim and refreshes BOTH control groups from the
                // single in-flight authority (so the offline group comes back too).
                setInstallInProgressUi(false);

                // Show summary modal with per-package results
                auto jobs = m_worker->getJobs();
                sak::InstallSummaryDialog dialog(stats, jobs, this);
                dialog.exec();
            });
}

void AppInstallationPanel::setupWorkerJobConnections() {
    connect(m_worker.get(),
            &AppInstallationWorker::jobProgress,
            this,
            [this](int entry_index, const QString& message) {
                Q_UNUSED(entry_index);
                Q_EMIT logOutput(message);
                Q_EMIT statusMessage(message, 0);
            });

    connect(
        m_worker.get(),
        &AppInstallationWorker::jobStatusChanged,
        this,
        [this](int entry_index, const MigrationJob& job) {
            Q_UNUSED(entry_index);
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
            const int completed = stats.success + stats.failed + stats.skipped + stats.cancelled;
            m_progressBar->setValue(completed);
            m_progressLabel->setText(tr("Installing %1 of %2...").arg(completed).arg(stats.total));
            Q_EMIT progressUpdated(completed, stats.total);
        });
}

// ============================================================================
// Offline Deployment Tab
// ============================================================================

void AppInstallationPanel::setupOfflinePresetRow(QVBoxLayout* offline_layout) {
    auto* preset_row = new QHBoxLayout();
    auto* preset_label = new QLabel(tr("Preset:"), this);
    preset_row->addWidget(preset_label);

    m_presetCombo = new QComboBox(this);
    m_presetCombo->addItem(tr("-- Select Preset --"));
    for (const auto& name : m_list_manager->presetNames()) {
        m_presetCombo->addItem(name);
    }
    m_presetCombo->setAccessibleName(QStringLiteral("Preset Package List"));
    m_presetCombo->setToolTip(tr("Select a preset package list to populate"));
    preset_row->addWidget(m_presetCombo, 1);
    offline_layout->addLayout(preset_row);
}

void AppInstallationPanel::setupOfflineSearchGroup(QHBoxLayout* side_by_side) {
    auto* search_group = new QGroupBox(tr("Search Packages"), this);
    auto* search_layout = new QVBoxLayout(search_group);

    auto* search_row = new QHBoxLayout();
    m_offlinePackageEdit = new QLineEdit(this);
    m_offlinePackageEdit->setPlaceholderText(tr("Search for packages (e.g., chrome, firefox)"));
    m_offlinePackageEdit->setClearButtonEnabled(true);
    m_offlinePackageEdit->setAccessibleName(QStringLiteral("Package Search Input"));
    search_row->addWidget(m_offlinePackageEdit, 1);

    m_offlineSearchButton = new QPushButton(tr("Search"), this);
    m_offlineSearchButton->setAccessibleName(QStringLiteral("Search Packages"));
    m_offlineSearchButton->setToolTip(tr("Search the Chocolatey repository for matching packages"));
    m_offlineSearchButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    search_row->addWidget(m_offlineSearchButton);
    search_layout->addLayout(search_row);

    m_offlineResultsModel = new QStandardItemModel(0, kPackageResultColumnCount, this);
    m_offlineResultsModel->setHorizontalHeaderLabels(
        {tr("Package"), tr("Version"), tr("Publisher")});

    m_offlineResultsTable = new QTableView(this);
    m_offlineResultsTable->setModel(m_offlineResultsModel);
    sak::configureStandardTable(m_offlineResultsTable, QAbstractItemView::SingleSelection);
    m_offlineResultsTable->horizontalHeader()->setStretchLastSection(true);
    m_offlineResultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_offlineResultsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_offlineResultsTable->horizontalHeader()->setSectionResizeMode(kPublisherResultColumn,
                                                                    QHeaderView::Interactive);
    m_offlineResultsTable->setAccessibleName(QStringLiteral("Package Search Results"));
    m_offlineResultsTable->setToolTip(tr("Select a package from the search results and click Add"));
    // Designed empty/loading overlay (constructed after setModel, per the helper contract).
    m_offlineResultsState = new sak::ui::ViewEmptyState(
        m_offlineResultsTable, tr("No results yet - search for packages to bundle"));
    search_layout->addWidget(m_offlineResultsTable, 1);

    m_offlineAddButton = new QPushButton(tr("Add Selected"), this);
    m_offlineAddButton->setAccessibleName(QStringLiteral("Add Package to List"));
    m_offlineAddButton->setToolTip(
        tr("Add the selected search result to the offline deployment list"));
    m_offlineAddButton->setEnabled(false);
    m_offlineAddButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    search_layout->addWidget(m_offlineAddButton);

    side_by_side->addWidget(search_group, 1);
}

void AppInstallationPanel::setupOfflineDeployListGroup(QHBoxLayout* side_by_side) {
    auto* list_group = new QGroupBox(tr("Packages to Deploy"), this);
    auto* list_layout = new QVBoxLayout(list_group);

    m_offlineListWidget = new QListWidget(this);
    m_offlineListWidget->setAlternatingRowColors(true);
    m_offlineListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_offlineListWidget->setAccessibleName(QStringLiteral("Offline Package List"));
    m_offlineListWidget->setToolTip(tr("Packages that will be included in the deployment bundle"));
    // Empty-only overlay (no scan): self-managed via the view parent, no stored member.
    new sak::ui::ViewEmptyState(m_offlineListWidget,
                                tr("No packages selected - add packages to deploy"));
    list_layout->addWidget(m_offlineListWidget, 1);

    auto* list_btn_row = new QHBoxLayout();
    m_offlineRemoveButton = new QPushButton(tr("Remove"), this);
    m_offlineRemoveButton->setEnabled(false);
    m_offlineRemoveButton->setAccessibleName(QStringLiteral("Remove from List"));
    m_offlineRemoveButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    list_btn_row->addWidget(m_offlineRemoveButton);

    m_offlineClearButton = new QPushButton(tr("Clear All"), this);
    m_offlineClearButton->setEnabled(false);
    m_offlineClearButton->setAccessibleName(QStringLiteral("Clear Offline List"));
    m_offlineClearButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    list_btn_row->addWidget(m_offlineClearButton);
    list_btn_row->addStretch();

    m_saveOfflineListButton = new QPushButton(tr("Save List"), this);
    m_saveOfflineListButton->setAccessibleName(QStringLiteral("Save Offline List"));
    m_saveOfflineListButton->setToolTip(tr("Save the offline package list to a JSON file"));
    m_saveOfflineListButton->setEnabled(false);
    m_saveOfflineListButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    list_btn_row->addWidget(m_saveOfflineListButton);

    m_loadOfflineListButton = new QPushButton(tr("Load List"), this);
    m_loadOfflineListButton->setAccessibleName(QStringLiteral("Load Offline List"));
    m_loadOfflineListButton->setToolTip(tr("Load a package list from a JSON file"));
    m_loadOfflineListButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    list_btn_row->addWidget(m_loadOfflineListButton);

    list_layout->addLayout(list_btn_row);
    side_by_side->addWidget(list_group, 1);
}

void AppInstallationPanel::setupPayloadModeControls(QVBoxLayout* actions_layout) {
    // Payload type selector + air-gap switch. (Provisional labels -- the final
    // user-facing naming is being workshopped; see docs Batch 14 B14-14.)
    auto* payload_row = new QHBoxLayout();
    payload_row->addWidget(new QLabel(tr("Payload type:"), this));
    m_payloadModeCombo = new QComboBox(this);
    m_payloadModeCombo->addItem(tr("Full Bundle (installers packed in)"),
                                static_cast<int>(sak::PayloadMode::Bundle));
    m_payloadModeCombo->addItem(tr("Thin Bundle (names only; downloaded at install)"),
                                static_cast<int>(sak::PayloadMode::List));
    m_payloadModeCombo->setToolTip(
        tr("Full Bundle: download and pack every installer now (bundle once, deploy many; "
           "minimal bandwidth at deploy).\nThin Bundle: record only package names/versions; the "
           "target fetches each installer at install time."));
    m_payloadModeCombo->setAccessibleName(tr("Deployment payload type"));
    payload_row->addWidget(m_payloadModeCombo, 1);

    m_airGapCheck = new QCheckBox(tr("Air-gap install (packed only)"), this);
    m_airGapCheck->setAccessibleName(tr("Air-gap install (packed only)"));
    m_airGapCheck->setToolTip(
        tr("When installing on a disconnected machine, install only fully-packed packages and "
           "skip any that would still need to download."));
    payload_row->addWidget(m_airGapCheck);
    actions_layout->addLayout(payload_row);
}

void AppInstallationPanel::setupOfflineActionsGroup(QVBoxLayout* offline_layout) {
    auto* actions_group = new QGroupBox(tr("Deployment Actions"), this);
    auto* actions_layout = new QVBoxLayout(actions_group);

    setupPayloadModeControls(actions_layout);

    auto* action_btn_row = new QHBoxLayout();

    m_buildBundleButton = new QPushButton(tr("Build Offline Bundle"), this);
    m_buildBundleButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_buildBundleButton->setEnabled(false);
    m_buildBundleButton->setAccessibleName(QStringLiteral("Build Offline Bundle"));
    m_buildBundleButton->setToolTip(
        tr("Download and internalize all listed packages into a portable bundle"));
    action_btn_row->addWidget(m_buildBundleButton);

    m_directDownloadButton = new QPushButton(tr("Direct Download"), this);
    m_directDownloadButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_directDownloadButton->setEnabled(false);
    m_directDownloadButton->setAccessibleName(QStringLiteral("Direct Download"));
    m_directDownloadButton->setToolTip(
        tr("Download the raw installer binaries (.exe/.msi) for each package -- for manual "
           "install, not a deployable offline bundle"));
    action_btn_row->addWidget(m_directDownloadButton);

    m_installFromBundleButton = new QPushButton(tr("Install from Bundle"), this);
    m_installFromBundleButton->setStyleSheet(sak::ui::kSuccessButtonStyle);
    m_installFromBundleButton->setAccessibleName(QStringLiteral("Install from Bundle"));
    m_installFromBundleButton->setToolTip(
        tr("Install packages from a previously built offline bundle"));
    action_btn_row->addWidget(m_installFromBundleButton);

    actions_layout->addLayout(action_btn_row);

    m_offlineProgressLabel = new QLabel(this);
    m_offlineProgressLabel->setVisible(false);
    actions_layout->addWidget(m_offlineProgressLabel);

    m_offlineProgressBar = new QProgressBar(this);
    m_offlineProgressBar->setVisible(false);
    m_offlineProgressBar->setTextVisible(true);
    m_offlineProgressBar->setFormat(QStringLiteral("%v / %m"));
    actions_layout->addWidget(m_offlineProgressBar);

    m_offlineStatusLabel = new QLabel(this);
    m_offlineStatusLabel->setVisible(false);
    m_offlineStatusLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    actions_layout->addWidget(m_offlineStatusLabel);

    m_cancelOfflineButton = new QPushButton(tr("Cancel"), this);
    m_cancelOfflineButton->setStyleSheet(sak::ui::kDangerButtonStyle);
    m_cancelOfflineButton->setVisible(false);
    m_cancelOfflineButton->setAccessibleName(QStringLiteral("Cancel Offline Operation"));
    actions_layout->addWidget(m_cancelOfflineButton);

    offline_layout->addWidget(actions_group);
}

void AppInstallationPanel::setupUi_offlineTab(QTabWidget* tabs) {
    auto* offline_tab = new QWidget(this);
    auto* offline_layout = new QVBoxLayout(offline_tab);
    offline_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginSmall, sak::ui::kMarginNone, sak::ui::kMarginNone);
    offline_layout->setSpacing(sak::ui::kSpacingDefault);

    setupOfflinePresetRow(offline_layout);

    auto* side_by_side = new QHBoxLayout();
    side_by_side->setSpacing(sak::ui::kSpacingDefault);
    setupOfflineSearchGroup(side_by_side);
    setupOfflineDeployListGroup(side_by_side);
    offline_layout->addLayout(side_by_side, 1);
    setupOfflineActionsGroup(offline_layout);

    tabs->addTab(offline_tab, tr("Offline Deploy"));
}

// ============================================================================
// Offline Deployment Connections
// ============================================================================

void AppInstallationPanel::setupOfflineConnections() {
    setupOfflineInputConnections();
    setupOfflineWorkerConnections();
}

void AppInstallationPanel::setupOfflineInputConnections() {
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

    // Same single in-flight authority as the online side (see setupQueueConnections).
    connect(m_offlineListWidget, &QListWidget::itemSelectionChanged, this, [this]() {
        m_offlineRemoveButton->setEnabled(!m_offlineListWidget->selectedItems().isEmpty() &&
                                          !packageOperationInFlight());
    });
}

void AppInstallationPanel::setupOfflineWorkerConnections() {
    setupOfflineWorkerProgressConnections();
    setupOfflineWorkerCompletionConnections();
}

void AppInstallationPanel::setupOfflineWorkerProgressConnections() {
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
                const QString log_line = success ? QString("[OK] %1: %2").arg(pkg_id, msg)
                                                 : QString("[FAIL] %1: %2").arg(pkg_id, msg);
                Q_EMIT logOutput(log_line);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::logMessage,
            this,
            [this](const QString& msg) { Q_EMIT logOutput(msg); });
}

void AppInstallationPanel::setupOfflineWorkerCompletionConnections() {
    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::operationCompleted,
            this,
            [this](const BatchStats& stats) {
                m_offlineProgressBar->setVisible(false);
                m_offlineProgressLabel->setVisible(false);
                m_cancelOfflineButton->setVisible(false);

                // Surface skipped/cancelled/pending too: an air-gap run can skip every
                // requires-network package, which "%1 succeeded, %2 failed" alone would
                // misreport as a clean completion (e.g. "0 succeeded, 0 failed"). Fail
                // closed on the wording -- only call it "Complete" when nothing was left
                // undone.
                const int unfinished = stats.skipped + stats.cancelled + stats.pending;
                QString summary =
                    tr("%1 succeeded, %2 failed").arg(stats.completed).arg(stats.failed);
                if (stats.skipped > 0) {
                    summary += tr(", %1 skipped").arg(stats.skipped);
                }
                if (stats.cancelled > 0) {
                    summary += tr(", %1 cancelled").arg(stats.cancelled);
                }
                if (stats.pending > 0) {
                    summary += tr(", %1 not attempted").arg(stats.pending);
                }
                const QString headline = unfinished > 0 ? tr("Incomplete: %1") : tr("Complete: %1");
                m_offlineStatusLabel->setText(headline.arg(summary));
                m_offlineStatusLabel->setVisible(true);

                setOfflineInProgressUi(false);

                Q_EMIT statusMessage(tr("Offline operation: %1").arg(summary),
                                     sak::kTimerStatusDefaultMs);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::operationError,
            this,
            [this](const QString& error) {
                sak::logError("[AppInstallationPanel] Offline error: {}", error.toStdString());
                Q_EMIT logOutput(QString("ERROR: %1").arg(error));
                // operationError is terminal (no operationCompleted follows). Mirror the
                // completion teardown so an error after operationStarted does not strand the
                // progress bar, its label, and the Cancel button; show the reason in the
                // status label too, so the error state is visible without opening the log.
                m_offlineProgressBar->setVisible(false);
                m_offlineProgressLabel->setVisible(false);
                m_cancelOfflineButton->setVisible(false);
                m_offlineStatusLabel->setText(tr("Failed: %1").arg(error));
                m_offlineStatusLabel->setVisible(true);
                setOfflineInProgressUi(false);
            });

    connect(m_offline_worker.get(),
            &OfflineDeploymentWorker::manifestWritten,
            this,
            [this](const QString& path) {
                Q_EMIT logOutput(QString("Manifest written: %1").arg(path));
            });
}
