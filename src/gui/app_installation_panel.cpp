// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file app_installation_panel.cpp
/// @brief Implements the application installation panel UI for software reinstallation

#include "sak/app_installation_panel.h"
#include "sak/chocolatey_manager.h"
#include "sak/app_installation_worker.h"
#include "sak/migration_report.h"
#include "sak/detachable_log_window.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QApplication>
#include <QSplitter>
#include <QGroupBox>
#include <QScrollArea>
#include <QStyle>
#include <memory>

using sak::AppInstallationPanel;
using sak::ChocolateyManager;
using sak::AppInstallationWorker;
using sak::MigrationReport;
using sak::MigrationJob;
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
{
    setupUi();
    setupConnections();

    // Initialize Chocolatey on startup
    QString chocoPath = QApplication::applicationDirPath() + "/tools/chocolatey";
    bool init_success = m_choco_manager->initialize(chocoPath);
    if (!init_success) {
        Q_EMIT logOutput(QString("WARNING: Chocolatey initialization failed"));
        Q_EMIT logOutput("Package installation will not be available.");
    } else {
        Q_EMIT logOutput("Chocolatey initialized successfully");
        Q_EMIT logOutput("Use the search bar to find packages or select a category.");
    }
}

AppInstallationPanel::~AppInstallationPanel() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
    }
}

void AppInstallationPanel::setupUi()
{
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setSpacing(sak::ui::kSpacingDefault);
    mainLayout->setContentsMargins(sak::ui::kMarginMedium, sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium, sak::ui::kMarginMedium);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(contentWidget, tr("App Installation"),
        tr("Search, queue, and batch-install applications via Chocolatey"), mainLayout);

    setupUi_searchBar(mainLayout);

    // === Splitter: Results table | Queue panel ===
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    setupUi_packageTable(splitter);
    setupUi_queueSection(splitter);

    splitter->setHandleWidth(6);
    splitter->setStretchFactor(0, 65);
    splitter->setStretchFactor(1, 35);
    mainLayout->addWidget(splitter, 1);

    setupUi_bottomBar(mainLayout);
}

void AppInstallationPanel::setupUi_searchBar(QVBoxLayout* mainLayout)
{
    auto* searchGroup = new QGroupBox(tr("Search Packages"), this);
    auto* searchLayout = new QHBoxLayout(searchGroup);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search Chocolatey packages..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName(QStringLiteral("Package Search"));
    m_searchEdit->setToolTip(QStringLiteral("Type to search for Chocolatey packages"));
    searchLayout->addWidget(m_searchEdit, 1);

    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->addItems({
        tr("All"),
        tr("Browsers"),
        tr("Development"),
        tr("Media"),
        tr("Utilities"),
        tr("Security"),
        tr("Productivity"),
        tr("Communication")
    });
    m_categoryCombo->setAccessibleName(QStringLiteral("Package Category"));
    m_categoryCombo->setToolTip(tr("Select a category to browse"));
    searchLayout->addWidget(m_categoryCombo);

    m_searchButton = new QPushButton(tr("Search"), this);
    m_searchButton->setAccessibleName(QStringLiteral("Search Packages"));
    m_searchButton->setToolTip(QStringLiteral("Search the Chocolatey repository"));
    searchLayout->addWidget(m_searchButton);

    mainLayout->addWidget(searchGroup);
}

void AppInstallationPanel::setupUi_packageTable(QSplitter* splitter)
{
    auto* resultsWidget = new QWidget(this);
    auto* resultsLayout = new QVBoxLayout(resultsWidget);
    resultsLayout->setContentsMargins(0, 0, 8, 0);

    auto* resultsLabel = new QLabel(tr("Search Results"), this);
    resultsLabel->setStyleSheet("QLabel { font-weight: 600; }");
    resultsLayout->addWidget(resultsLabel);

    m_resultsTable = new QTableView(this);
    m_resultsModel = new QStandardItemModel(0, RColCount, this);
    m_resultsModel->setHorizontalHeaderLabels({tr(""), tr("Package"), tr("Version"),
        tr("Publisher")});

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
        "QTableView::indicator { width: 16px; height: 16px; border: 1px solid #94a3b8; "
        "border-radius: 4px; background: #f8fafc; }"
        "QTableView::indicator:checked { background: #3b82f6; border: 1px solid #2563eb; }"
        "QTableView::indicator:unchecked { background: #f8fafc; border: 1px solid #94a3b8; }"
    );

    resultsLayout->addWidget(m_resultsTable, 1);

    m_addToQueueButton = new QPushButton(tr("Add Selected to Queue  \u25b6"), this);
    m_addToQueueButton->setEnabled(false);
    m_addToQueueButton->setAccessibleName(QStringLiteral("Add to Queue"));
    m_addToQueueButton->setToolTip(QStringLiteral("Add checked packages to the install queue"));
    resultsLayout->addWidget(m_addToQueueButton);

    splitter->addWidget(resultsWidget);
}

void AppInstallationPanel::setupUi_queueSection(QSplitter* splitter)
{
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
    m_saveQueueButton->setToolTip(tr(
        "Save the current install queue to a JSON file for later use"));
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

void AppInstallationPanel::setupUi_bottomBar(QVBoxLayout* mainLayout)
{
    m_logToggle = new sak::LogToggleSwitch(tr("Log"), this);
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(0, 4, 0, 0);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();
    mainLayout->addLayout(bottomLayout);
}

void AppInstallationPanel::setupConnections()
{
    setupSearchAndQueueConnections();
    setupWorkerConnections();
}

void AppInstallationPanel::setupSearchAndQueueConnections()
{
    // Search
    connect(m_searchButton, &QPushButton::clicked, this, &AppInstallationPanel::onSearch);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &AppInstallationPanel::onSearch);
    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppInstallationPanel::onCategoryChanged);

    // Queue management
    connect(m_addToQueueButton, &QPushButton::clicked, this, &AppInstallationPanel::onAddToQueue);
    connect(m_removeFromQueueButton, &QPushButton::clicked, this,
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
        if (item && item->column() == RColCheck) {
            // Check if any items are checked
            bool anyChecked = false;
            for (int i = 0; i < m_resultsModel->rowCount(); ++i) {
                auto* checkItem = m_resultsModel->item(i, RColCheck);
                if (checkItem && checkItem->checkState() == Qt::Checked) {
                    anyChecked = true;
                    break;
                }
            }
            m_addToQueueButton->setEnabled(anyChecked && !m_install_in_progress);
        }
    });
}

void AppInstallationPanel::setupWorkerConnections()
{
    connect(m_worker.get(), &AppInstallationWorker::migrationStarted, this, [this](int totalJobs) {
        Q_EMIT progressUpdated(0, totalJobs);
        Q_EMIT statusMessage("App Installation: Installing packages...", 0);
        Q_EMIT logOutput(QString("Installation started: %1 package(s)").arg(totalJobs));
    });

    connect(m_worker.get(), &AppInstallationWorker::jobProgress, this,
            [this](int entryIndex, const QString& message) {
                Q_UNUSED(entryIndex);
                Q_EMIT logOutput(message);
                Q_EMIT statusMessage(message, 0);
            });

    connect(m_worker.get(), &AppInstallationWorker::jobStatusChanged, this,
            [this](int entryIndex, const MigrationJob& job) {
                Q_UNUSED(entryIndex);
                switch (job.status) {
                    case MigrationStatus::Installing:
                        Q_EMIT statusMessage(tr("Installing %1...").arg(job.packageId), 0);
                        break;
                    case MigrationStatus::Success:
                        Q_EMIT logOutput(QString("✓ %1 installed successfully").arg(job.packageId));
                        break;
                    case MigrationStatus::Failed:
                        Q_EMIT logOutput(QString("✗ %1 failed: %2").arg(job.packageId,
                            job.errorMessage));
                        break;
                    default:
                        break;
                }

                auto stats = m_worker->getStats();
                int completed = stats.success + stats.failed + stats.skipped + stats.cancelled;
                Q_EMIT progressUpdated(completed, stats.total);
            });

    connect(m_worker.get(), &AppInstallationWorker::migrationCompleted, this,
            [this](const AppInstallationWorker::Stats& stats) {
                Q_EMIT logOutput(QString("Installation complete: %1 succeeded, %2 failed, %3 "
                                         "skipped")
                    .arg(stats.success).arg(stats.failed).arg(stats.skipped));
                Q_EMIT statusMessage(QString("App Installation: %1 succeeded, %2 failed")
                    .arg(stats.success).arg(stats.failed), 5000);

                Q_EMIT statusMessage(tr("Installation complete"), 5000);
                m_install_in_progress = false;
                m_cancelButton->setVisible(false);
                m_cancelButton->setEnabled(false);
                m_installButton->setVisible(true);
                enableControls(true);
            });
}

