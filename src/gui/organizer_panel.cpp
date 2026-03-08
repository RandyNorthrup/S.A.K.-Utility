// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file organizer_panel.cpp
/// @brief Unified file management panel — organizer + duplicate finder in tabs

#include "sak/organizer_panel.h"
#include "sak/organizer_worker.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/logger.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/layout_constants.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QDateTime>
#include <QScrollArea>
#include <QFrame>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QSet>
#include <QThread>
#include <QFileInfo>

namespace sak {

OrganizerPanel::OrganizerPanel(QWidget* parent)
    : QWidget(parent)
    , m_worker(nullptr)
{
    setupUi();
    setupDefaultCategories();
    logInfo("OrganizerPanel initialized");
}

OrganizerPanel::~OrganizerPanel()
{
    if (m_worker) {
        m_worker->requestStop();
        if (!m_worker->wait(15000)) {
            logError("OrganizerWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    if (m_dedup_worker) {
        m_dedup_worker->requestStop();
        if (!m_dedup_worker->wait(15000)) {
            logError(
                "DuplicateFinderWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    logInfo("OrganizerPanel destroyed");
}

// ============================================================================
// Main setup — panel header, tabbed layout, shared status bar
// ============================================================================

void OrganizerPanel::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(ui::kMarginMedium, ui::kMarginMedium,
                                   ui::kMarginMedium, ui::kMarginMedium);
    rootLayout->setSpacing(ui::kSpacingDefault);

    // Dynamic panel header — updates when sub-tab changes
    m_headerWidgets = sak::createDynamicPanelHeader(this,
        QStringLiteral(":/icons/icons/panel_organizer.svg"),
        tr("File Organizer"),
        tr("Organize files into categorized folders and find duplicate files to reclaim disk space"),
        rootLayout);

    // Tabbed content
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createOrganizerTab(), tr("File Organizer"));
    m_tabs->addTab(createDuplicateFinderTab(), tr("Duplicate Finder"));
    setAccessible(m_tabs, tr("File management tools"),
        tr("Switch between file organizer and duplicate finder"));
    rootLayout->addWidget(m_tabs, 1);

    // Update header when sub-tab changes
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        struct TabMeta { const char* icon; const char* title; const char* subtitle; };
        static constexpr TabMeta kTabs[] = {
            {":/icons/icons/panel_organizer.svg",
             "File Organizer",
             "Organize files into categorized folders"},
            {":/icons/icons/panel_organizer.svg",
             "Duplicate Finder",
             "Find duplicate files to reclaim disk space"},
            {":/icons/icons/panel_search.svg",
             "Advanced Search",
             "Search file contents, metadata, archives, and binary data across directory trees"},
        };
        if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
            const auto& meta = kTabs[index];
            sak::updatePanelHeader(m_headerWidgets,
                QString::fromUtf8(meta.icon),
                tr(meta.title), tr(meta.subtitle));
        }
    });

    // Shared status bar with log toggle
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, ui::kSpacingTight, 0, 0);
    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    statusRow->addWidget(m_logToggle);
    statusRow->addStretch();
    rootLayout->addLayout(statusRow);
}

// ============================================================================
// Tab 1 — File Organizer
// ============================================================================

// ============================================================================
// Organizer Tab — Input Groups
// ============================================================================

QGroupBox* OrganizerPanel::createTargetDirectoryGroup()
{
    auto* group = new QGroupBox(tr("Target Directory"), this);
    auto* group_layout = new QVBoxLayout(group);

    auto* path_row = new QHBoxLayout();
    m_target_path = new QLineEdit(this);
    m_target_path->setPlaceholderText(
        tr("Select directory to organize..."));
    m_target_path->setAccessibleName(
        QStringLiteral("Target Directory Path"));
    m_target_path->setToolTip(QStringLiteral(
        "Path to the directory that will be organized"));
    path_row->addWidget(m_target_path, 1);

    m_browse_button = new QPushButton(tr("Browse..."), this);
    m_browse_button->setAccessibleName(
        QStringLiteral("Browse Directory"));
    m_browse_button->setToolTip(
        QStringLiteral("Browse for a directory to organize"));
    path_row->addWidget(m_browse_button);
    group_layout->addLayout(path_row);

    m_dir_summary_label = new QLabel(
        tr("No directory selected"), this);
    m_dir_summary_label->setStyleSheet(
        QString("color: %1; font-size: %2pt; padding: 2px 0;")
            .arg(ui::kColorTextMuted).arg(ui::kFontSizeNote));
    m_dir_summary_label->setAccessibleName(
        QStringLiteral("Directory Summary"));
    group_layout->addWidget(m_dir_summary_label);

    return group;
}

QGroupBox* OrganizerPanel::createCategoryMappingGroup()
{
    auto* group = new QGroupBox(tr("Category Mapping"), this);
    auto* group_layout = new QVBoxLayout(group);

    m_category_table = new QTableWidget(this);
    m_category_table->setColumnCount(2);
    m_category_table->setHorizontalHeaderLabels(
        {tr("Category"), tr("Extensions (comma-separated)")});
    m_category_table->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    m_category_table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    m_category_table->setAlternatingRowColors(true);
    m_category_table->setMinimumHeight(200);
    m_category_table->setAccessibleName(
        QStringLiteral("Category Mappings Table"));
    m_category_table->setToolTip(QStringLiteral(
        "File categories and their associated extensions"));
    group_layout->addWidget(m_category_table);

    auto* btn_row = new QHBoxLayout();
    m_add_category_button = new QPushButton(
        tr("Add Category"), this);
    m_add_category_button->setAccessibleName(
        QStringLiteral("Add Category"));
    m_add_category_button->setToolTip(
        QStringLiteral("Add a new file category row"));
    m_remove_category_button = new QPushButton(
        tr("Remove Selected"), this);
    m_remove_category_button->setAccessibleName(
        QStringLiteral("Remove Category"));
    m_remove_category_button->setToolTip(QStringLiteral(
        "Remove the selected category from the list"));
    m_reset_categories_button = new QPushButton(
        tr("Reset to Defaults"), this);
    m_reset_categories_button->setAccessibleName(
        QStringLiteral("Reset Categories"));
    m_reset_categories_button->setToolTip(QStringLiteral(
        "Restore the default category-to-extension mappings"));
    btn_row->addWidget(m_add_category_button);
    btn_row->addWidget(m_remove_category_button);
    btn_row->addWidget(m_reset_categories_button);
    btn_row->addStretch();
    group_layout->addLayout(btn_row);

    return group;
}

// ============================================================================
// Tab 1 — File Organizer
// ============================================================================

QWidget* OrganizerPanel::createOrganizerTab()
{
    auto* tab = new QWidget(this);
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(ui::kMarginSmall, ui::kMarginSmall,
                               ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    layout->addWidget(createTargetDirectoryGroup());
    layout->addWidget(createCategoryMappingGroup());

    // Hidden options (managed via Settings dialog)
    m_collision_strategy = new QComboBox(this);
    m_collision_strategy->addItems(
        {tr("Rename"), tr("Skip"), tr("Overwrite")});
    m_collision_strategy->setAccessibleName(
        QStringLiteral("Collision Strategy"));
    m_collision_strategy->setVisible(false);

    m_preview_mode_checkbox = new QCheckBox(
        tr("Preview Mode (Dry Run)"), this);
    m_preview_mode_checkbox->setChecked(true);
    m_preview_mode_checkbox->setVisible(false);

    m_create_subdirs_checkbox = new QCheckBox(
        tr("Create Subdirectories"), this);
    m_create_subdirs_checkbox->setChecked(true);
    m_create_subdirs_checkbox->setVisible(false);

    // Progress bar
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setRange(0, 100);
    m_progress_bar->setValue(0);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setVisible(false);
    m_progress_bar->setAccessibleName(
        QStringLiteral("Organization Progress"));
    layout->addWidget(m_progress_bar);

    QPushButton* settingsBtn = nullptr;
    createOrganizerControls(layout, settingsBtn);

    // Connections
    connect(m_target_path, &QLineEdit::textChanged,
        this, &OrganizerPanel::onTargetPathChanged);
    connect(m_browse_button, &QPushButton::clicked,
        this, &OrganizerPanel::onBrowseClicked);
    connect(m_preview_button, &QPushButton::clicked,
        this, &OrganizerPanel::onPreviewClicked);
    connect(m_execute_button, &QPushButton::clicked,
        this, &OrganizerPanel::onExecuteClicked);
    connect(m_cancel_button, &QPushButton::clicked,
        this, &OrganizerPanel::onCancelClicked);
    connect(settingsBtn, &QPushButton::clicked,
        this, &OrganizerPanel::onSettingsClicked);
    connect(m_add_category_button, &QPushButton::clicked,
        this, &OrganizerPanel::onAddCategoryClicked);
    connect(m_remove_category_button, &QPushButton::clicked,
        this, &OrganizerPanel::onRemoveCategoryClicked);
    connect(m_reset_categories_button, &QPushButton::clicked,
        this, &OrganizerPanel::onResetCategoriesClicked);

    scrollArea->setWidget(tab);
    return scrollArea;
}

void OrganizerPanel::createOrganizerControls(
    QVBoxLayout* layout, QPushButton*& settingsBtn)
{
    auto* row = new QHBoxLayout();

    settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(
        QStringLiteral("Organizer Settings"));
    settingsBtn->setToolTip(QStringLiteral(
        "Configure collision strategy and preview mode"));
    row->addWidget(settingsBtn);
    row->addStretch();

    m_preview_button = new QPushButton(tr("Preview"), this);
    m_preview_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_preview_button->setAccessibleName(
        QStringLiteral("Preview Organization"));
    m_preview_button->setToolTip(QStringLiteral(
        "Preview file organization without making changes"));
    m_preview_button->setStyleSheet(ui::kSecondaryButtonStyle);
    row->addWidget(m_preview_button);

    m_execute_button = new QPushButton(
        tr("Organize Files"), this);
    m_execute_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_execute_button->setAccessibleName(
        QStringLiteral("Execute Organization"));
    m_execute_button->setToolTip(QStringLiteral(
        "Organize files into category folders"));
    m_execute_button->setStyleSheet(ui::kPrimaryButtonStyle);
    row->addWidget(m_execute_button);

    m_cancel_button = new QPushButton(tr("Cancel"), this);
    m_cancel_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_cancel_button->setEnabled(false);
    m_cancel_button->setAccessibleName(
        QStringLiteral("Cancel Organization"));
    m_cancel_button->setToolTip(QStringLiteral(
        "Cancel the current organization operation"));
    row->addWidget(m_cancel_button);

    layout->addLayout(row);
}

// ============================================================================
// Duplicate Finder — Group Builders
// ============================================================================

QGroupBox* OrganizerPanel::createScanDirectoriesGroup()
{
    auto* group = new QGroupBox(tr("Scan Directories"), this);
    auto* group_layout = new QVBoxLayout(group);

    m_dedup_directory_list = new QListWidget(this);
    m_dedup_directory_list->setMinimumHeight(sak::kListAreaMinH);
    m_dedup_directory_list->setAccessibleName(
        QStringLiteral("Duplicate Scan Directories List"));
    m_dedup_directory_list->setToolTip(QStringLiteral(
        "Directories to scan for duplicate files"));
    group_layout->addWidget(m_dedup_directory_list);

    auto* btn_row = new QHBoxLayout();
    m_dedup_add_button = new QPushButton(
        tr("Add Directory"), this);
    m_dedup_add_button->setAccessibleName(
        QStringLiteral("Add Scan Directory"));
    m_dedup_add_button->setToolTip(
        QStringLiteral("Add a directory to the scan list"));
    m_dedup_remove_button = new QPushButton(
        tr("Remove Selected"), this);
    m_dedup_remove_button->setAccessibleName(
        QStringLiteral("Remove Selected Directory"));
    m_dedup_remove_button->setToolTip(QStringLiteral(
        "Remove highlighted directory from the list"));
    m_dedup_clear_button = new QPushButton(
        tr("Clear All"), this);
    m_dedup_clear_button->setAccessibleName(
        QStringLiteral("Clear All Directories"));
    m_dedup_clear_button->setToolTip(QStringLiteral(
        "Remove all directories from the scan list"));
    btn_row->addWidget(m_dedup_add_button);
    btn_row->addWidget(m_dedup_remove_button);
    btn_row->addWidget(m_dedup_clear_button);
    btn_row->addStretch();
    group_layout->addLayout(btn_row);

    m_dedup_summary_label = new QLabel(
        tr("No directories added"), this);
    m_dedup_summary_label->setStyleSheet(
        QString("color: %1; font-size: %2pt; padding: 2px 0;")
            .arg(ui::kColorTextMuted).arg(ui::kFontSizeNote));
    m_dedup_summary_label->setAccessibleName(
        QStringLiteral("Scan Directories Summary"));
    group_layout->addWidget(m_dedup_summary_label);

    return group;
}

void OrganizerPanel::createDedupControls(
    QVBoxLayout* layout, QPushButton*& settingsBtn)
{
    auto* row = new QHBoxLayout();

    settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(
        QStringLiteral("Duplicate Finder Settings"));
    settingsBtn->setToolTip(QStringLiteral(
        "Configure minimum file size, recursion,"
        " and hashing options"));
    row->addWidget(settingsBtn);
    row->addStretch();

    m_dedup_scan_button = new QPushButton(
        tr("Find Duplicates"), this);
    m_dedup_scan_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_dedup_scan_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_dedup_scan_button->setAccessibleName(
        QStringLiteral("Start Duplicate Scan"));
    m_dedup_scan_button->setToolTip(QStringLiteral(
        "Scan the listed directories for duplicate files"
        " using content hashing"));
    row->addWidget(m_dedup_scan_button);

    m_dedup_cancel_button = new QPushButton(
        tr("Cancel"), this);
    m_dedup_cancel_button->setMinimumWidth(
        sak::kButtonWidthSmall);
    m_dedup_cancel_button->setEnabled(false);
    m_dedup_cancel_button->setAccessibleName(
        QStringLiteral("Cancel Duplicate Scan"));
    m_dedup_cancel_button->setToolTip(QStringLiteral(
        "Cancel the duplicate scan in progress"));
    row->addWidget(m_dedup_cancel_button);

    layout->addLayout(row);
}

// ============================================================================
// Tab 2 — Duplicate Finder
// ============================================================================

QWidget* OrganizerPanel::createDuplicateFinderTab()
{
    auto* tab = new QWidget(this);
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(ui::kMarginSmall, ui::kMarginSmall,
                               ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    layout->addWidget(createScanDirectoriesGroup());

    // Hidden options (managed via Settings dialog)
    m_dedup_min_size = new QSpinBox(this);
    m_dedup_min_size->setMinimum(0);
    m_dedup_min_size->setMaximum(1000000);
    m_dedup_min_size->setValue(0);
    m_dedup_min_size->setVisible(false);

    m_dedup_recursive = new QCheckBox(
        tr("Recursive Scan"), this);
    m_dedup_recursive->setChecked(true);
    m_dedup_recursive->setVisible(false);

    m_dedup_parallel_hashing = new QCheckBox(
        tr("Parallel Hashing"), this);
    m_dedup_parallel_hashing->setChecked(true);
    m_dedup_parallel_hashing->setVisible(false);

    m_dedup_thread_count = new QSpinBox(this);
    m_dedup_thread_count->setMinimum(0);
    m_dedup_thread_count->setMaximum(64);
    m_dedup_thread_count->setValue(0);
    m_dedup_thread_count->setVisible(false);

    // Last scan results
    m_dedup_results_label = new QLabel(this);
    m_dedup_results_label->setWordWrap(true);
    m_dedup_results_label->setVisible(false);
    m_dedup_results_label->setAccessibleName(
        QStringLiteral("Last Scan Results Summary"));
    layout->addWidget(m_dedup_results_label);

    // Progress bar
    m_dedup_progress_bar = new QProgressBar(this);
    m_dedup_progress_bar->setRange(0, 100);
    m_dedup_progress_bar->setValue(0);
    m_dedup_progress_bar->setTextVisible(true);
    m_dedup_progress_bar->setVisible(false);
    m_dedup_progress_bar->setAccessibleName(
        QStringLiteral("Duplicate Scan Progress"));
    layout->addWidget(m_dedup_progress_bar);

    QPushButton* settingsBtn = nullptr;
    createDedupControls(layout, settingsBtn);

    // Connections
    connect(m_dedup_add_button, &QPushButton::clicked,
        this, &OrganizerPanel::onDedupAddDirectoryClicked);
    connect(m_dedup_remove_button, &QPushButton::clicked,
        this, &OrganizerPanel::onDedupRemoveDirectoryClicked);
    connect(m_dedup_clear_button, &QPushButton::clicked,
        this, &OrganizerPanel::onDedupClearAllClicked);
    connect(m_dedup_scan_button, &QPushButton::clicked,
        this, &OrganizerPanel::onDedupScanClicked);
    connect(m_dedup_cancel_button, &QPushButton::clicked,
        this, &OrganizerPanel::onDedupCancelClicked);
    connect(settingsBtn, &QPushButton::clicked,
        this, &OrganizerPanel::onDedupSettingsClicked);

    scrollArea->setWidget(tab);
    return scrollArea;
}

// ============================================================================
// File Organizer — logic
// ============================================================================

void OrganizerPanel::setupDefaultCategories()
{
    QMap<QString, QString> defaults = {
        {"Images", "jpg,jpeg,png,gif,bmp,svg,webp,ico,tiff,raw"},
        {"Documents", "pdf,doc,docx,txt,rtf,odt,xls,xlsx,ppt,pptx,csv"},
        {"Audio", "mp3,wav,flac,aac,ogg,m4a,wma,opus"},
        {"Video", "mp4,avi,mkv,mov,wmv,flv,webm,m4v"},
        {"Archives", "zip,rar,7z,tar,gz,bz2,xz,iso"},
        {"Code", "cpp,h,py,js,ts,java,cs,html,css,json,xml,yaml,yml"},
        {"Executables", "exe,msi,bat,cmd,ps1,sh"},
        {"Fonts", "ttf,otf,woff,woff2,eot"},
    };

    m_category_table->setRowCount(defaults.size());
    int row = 0;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        m_category_table->setItem(row, 0, new QTableWidgetItem(it.key()));
        m_category_table->setItem(row, 1, new QTableWidgetItem(it.value()));
        ++row;
    }
}

void OrganizerPanel::onTargetPathChanged(const QString& path)
{
    Q_UNUSED(path)
    updateDirectorySummary();
}

void OrganizerPanel::updateDirectorySummary()
{
    const QString path = m_target_path->text().trimmed();
    if (path.isEmpty()) {
        m_dir_summary_label->setText(tr("No directory selected"));
        return;
    }

    QDir dir(path);
    if (!dir.exists()) {
        m_dir_summary_label->setText(tr("Directory does not exist"));
        return;
    }

    const auto entries = dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    qint64 totalSize = 0;
    for (const auto& entry : entries) {
        totalSize += entry.size();
    }

    QString sizeStr;
    if (totalSize >= sak::kBytesPerGB) {
        sizeStr = QString("%1 GB").arg(totalSize / sak::kBytesPerGBf, 0, 'f', 2);
    } else if (totalSize >= sak::kBytesPerMB) {
        sizeStr = QString("%1 MB").arg(totalSize / sak::kBytesPerMBf, 0, 'f', 1);
    } else {
        sizeStr = QString("%1 KB").arg(totalSize / sak::kBytesPerKBf, 0, 'f', 0);
    }

    int subdirCount = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    m_dir_summary_label->setText(
        tr("%1 files (%2) \u2022 %3 subdirectories")
            .arg(entries.size()).arg(sizeStr).arg(subdirCount));
}

void OrganizerPanel::onBrowseClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Directory to Organize"),
        m_target_path->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_target_path->setText(dir);
        logMessage(QString("Target directory selected: %1").arg(dir));
    }
}

void OrganizerPanel::onPreviewClicked()
{
    m_preview_mode_checkbox->setChecked(true);
    onExecuteClicked();
}

void OrganizerPanel::onExecuteClicked()
{
    if (m_target_path->text().isEmpty()) {
        QMessageBox::warning(this, tr("Validation Error"),
            tr("Please select a target directory."));
        return;
    }

    QDir targetDir(m_target_path->text());
    if (!targetDir.exists()) {
        QMessageBox::warning(this, tr("Validation Error"),
            tr("Target directory does not exist."));
        return;
    }

    if (!validateCategoryMapping()) {
        return;
    }

    // Confirmation dialog for destructive (non-preview) operations
    if (!m_preview_mode_checkbox->isChecked()) {
        const auto entries = targetDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        int fileCount = entries.size();

        auto result = QMessageBox::question(this, tr("Confirm Organization"),
            tr("This will move up to %1 files in:\n%2\n\n"
               "Collision strategy: %3\n\n"
               "This operation cannot be automatically undone. Continue?")
                .arg(fileCount)
                .arg(m_target_path->text(),
                     m_collision_strategy->currentText()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (result != QMessageBox::Yes) {
            return;
        }
    }

    m_worker.reset();

    OrganizerWorker::Config config;
    config.target_directory = m_target_path->text();
    config.category_mapping = getCategoryMapping();
    config.preview_mode = m_preview_mode_checkbox->isChecked();
    config.create_subdirectories = m_create_subdirs_checkbox->isChecked();

    QString strategy = m_collision_strategy->currentText().toLower();
    config.collision_strategy = strategy;

    m_worker = std::make_unique<OrganizerWorker>(config, this);

    connect(m_worker.get(), &OrganizerWorker::started, this, &OrganizerPanel::onWorkerStarted);
    connect(m_worker.get(), &OrganizerWorker::finished, this, &OrganizerPanel::onWorkerFinished);
    connect(m_worker.get(), &OrganizerWorker::failed, this, &OrganizerPanel::onWorkerFailed);
    connect(m_worker.get(), &OrganizerWorker::cancelled, this, &OrganizerPanel::onWorkerCancelled);
    connect(m_worker.get(), &OrganizerWorker::fileProgress, this,
        &OrganizerPanel::onFileProgress);
    connect(m_worker.get(), &OrganizerWorker::previewResults, this,
        &OrganizerPanel::onPreviewResults);

    setOperationRunning(true);
    m_progress_bar->setValue(0);
    m_progress_bar->setVisible(true);
    Q_EMIT statusMessage(tr("Starting..."), 0);
    m_worker->start();

    QString mode = config.preview_mode ? "Preview" : "Execute";
    logInfo("Organization operation initiated ({}): {}", mode.toStdString(),
            config.target_directory.toStdString());
}

void OrganizerPanel::onCancelClicked()
{
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage(tr("Cancellation requested..."));
        Q_EMIT statusMessage(tr("Cancelling..."), 0);
        logInfo("Organization cancellation requested by user");
    }
}

void OrganizerPanel::onAddCategoryClicked()
{
    int row = m_category_table->rowCount();
    m_category_table->insertRow(row);
    m_category_table->setItem(row, 0, new QTableWidgetItem(tr("New Category")));
    m_category_table->setItem(row, 1, new QTableWidgetItem(QString()));
    m_category_table->editItem(m_category_table->item(row, 0));
}

void OrganizerPanel::onResetCategoriesClicked()
{
    auto result = QMessageBox::question(this, tr("Reset Categories"),
        tr("Reset all categories to their default values?\n"
           "Any custom categories will be lost."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (result == QMessageBox::Yes) {
        setupDefaultCategories();
        logMessage(tr("Category mappings reset to defaults"));
    }
}

void OrganizerPanel::onRemoveCategoryClicked()
{
    auto selected = m_category_table->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("No Selection"),
            tr("Please select a category to remove."));
        return;
    }

    int row = m_category_table->currentRow();
    if (row >= 0) {
        m_category_table->removeRow(row);
    }
}

void OrganizerPanel::onWorkerStarted()
{
    QString mode = m_preview_mode_checkbox->isChecked() ? "preview" : "organization";
    logMessage(QString("Starting %1...").arg(mode));
    Q_EMIT statusMessage(QString("%1 in progress").arg(mode), 0);
}

void OrganizerPanel::onWorkerFinished()
{
    setOperationRunning(false);
    m_progress_bar->setValue(100);
    QString mode = m_preview_mode_checkbox->isChecked() ? "Preview" : "Organization";
    Q_EMIT statusMessage(QString("%1 complete").arg(mode), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(100, 100);
    logMessage(QString("%1 completed successfully").arg(mode));
    QMessageBox::information(this, QString("%1 Complete").arg(mode),
                            QString("%1 operation completed successfully.").arg(mode));
    logInfo("Organization operation completed successfully");
    updateDirectorySummary();
}

void OrganizerPanel::onWorkerFailed(int errorCode, const QString& errorMessage)
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    Q_EMIT statusMessage(tr("Organization failed"), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(0, 100);
    logMessage(QString("Organization failed: Error %1: %2").arg(errorCode).arg(errorMessage));
    QMessageBox::warning(this, tr("Organization Failed"),
                        QString("Error %1: %2").arg(errorCode).arg(errorMessage));
    logError("Organization failed: {}", errorMessage.toStdString());
}

void OrganizerPanel::onWorkerCancelled()
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    logMessage(tr("Organization cancelled by user"));
    Q_EMIT statusMessage(tr("Organization cancelled"), sak::kTimerStatusMessageMs);
    Q_EMIT progressUpdate(0, 100);
}

void OrganizerPanel::onFileProgress(int current, int total, const QString& filePath)
{
    Q_EMIT progressUpdate(current, total);

    if (total > 0) {
        int pct = static_cast<int>(static_cast<double>(current) / total * 100);
        m_progress_bar->setValue(pct);
    }

    QString filename = QFileInfo(filePath).fileName();
    Q_EMIT statusMessage(QString("Processing: %1").arg(filename), 0);
}

void OrganizerPanel::onPreviewResults(const QString& summary, int operationCount)
{
    m_progress_bar->setVisible(false);
    showScrollableResultsDialog(tr("Preview Results"), summary);
    logMessage(QString("Preview completed: %1 operations planned").arg(operationCount));
}

QMap<QString, QStringList> OrganizerPanel::getCategoryMapping() const
{
    QMap<QString, QStringList> mapping;

    for (int row = 0; row < m_category_table->rowCount(); ++row) {
        auto* categoryItem = m_category_table->item(row, 0);
        auto* extensionsItem = m_category_table->item(row, 1);

        if (!categoryItem || !extensionsItem) continue;

        QString category = categoryItem->text().trimmed();
        QString extensionsStr = extensionsItem->text().trimmed();

        if (category.isEmpty() || extensionsStr.isEmpty()) continue;

        QStringList extensions = extensionsStr.split(',', Qt::SkipEmptyParts);
        for (auto& ext : extensions) {
            ext = ext.trimmed();
        }
        mapping[category] = extensions;
    }

    return mapping;
}

void OrganizerPanel::setOperationRunning(bool running)
{
    m_operation_running = running;

    // Organizer controls
    m_target_path->setEnabled(!running);
    m_browse_button->setEnabled(!running);
    m_category_table->setEnabled(!running);
    m_add_category_button->setEnabled(!running);
    m_remove_category_button->setEnabled(!running);
    m_reset_categories_button->setEnabled(!running);

    m_preview_button->setEnabled(!running);
    m_execute_button->setEnabled(!running);
    m_cancel_button->setEnabled(running);

    // Cross-lock: disable dedup tab and other tab while running
    m_tabs->setTabEnabled(1, !running);
}

void OrganizerPanel::logMessage(const QString& message)
{
    Q_EMIT logOutput(message);
}

bool OrganizerPanel::validateCategoryMapping() const
{
    auto mapping = getCategoryMapping();
    if (mapping.isEmpty()) {
        QMessageBox::warning(const_cast<OrganizerPanel*>(this), tr("Validation Error"),
            tr("No valid category mappings defined.\n"
               "Add at least one category with extensions, or reset to defaults."));
        return false;
    }

    QSet<QString> seen;
    for (auto it = mapping.cbegin(); it != mapping.cend(); ++it) {
        QString lower = it.key().toLower();
        if (seen.contains(lower)) {
            QMessageBox::warning(const_cast<OrganizerPanel*>(this), tr("Validation Error"),
                tr("Duplicate category name: \"%1\".\n"
                   "Each category must have a unique name.").arg(it.key()));
            return false;
        }
        seen.insert(lower);
    }
    return true;
}

void OrganizerPanel::showScrollableResultsDialog(const QString& title, const QString& text)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.setMinimumSize(sak::kDialogWidthLarge, sak::kDialogHeightMedium);
    dialog.resize(sak::kDialogWidthLarge, sak::kDialogHeightLarge);

    auto* layout = new QVBoxLayout(&dialog);

    auto* textEdit = new QTextEdit(&dialog);
    textEdit->setReadOnly(true);
    textEdit->setPlainText(text);
    textEdit->setAccessibleName(QStringLiteral("Scan Results"));
    layout->addWidget(textEdit);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void OrganizerPanel::onSettingsClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("File Organizer Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthSmall);

    auto* layout = new QFormLayout(&dialog);

    auto* collisionCombo = new QComboBox(&dialog);
    collisionCombo->addItems({tr("Rename"), tr("Skip"), tr("Overwrite")});
    collisionCombo->setCurrentIndex(m_collision_strategy->currentIndex());
    layout->addRow(
        InfoButton::createInfoLabel(tr("Collision Strategy:"),
            tr("How to handle files when a file with the same name already exists in the "
               "destination folder"), &dialog),
        collisionCombo);

    auto* previewCheck = new QCheckBox(tr("Preview Mode (Dry Run)"), &dialog);
    previewCheck->setChecked(m_preview_mode_checkbox->isChecked());
    auto* previewRow = new QHBoxLayout();
    previewRow->addWidget(previewCheck);
    previewRow->addWidget(new InfoButton(
        tr("When enabled, shows what would happen without actually moving any files"), &dialog));
    previewRow->addStretch();
    layout->addRow(previewRow);

    auto* subdirCheck = new QCheckBox(tr("Create Subdirectories"), &dialog);
    subdirCheck->setChecked(m_create_subdirs_checkbox->isChecked());
    auto* subdirRow = new QHBoxLayout();
    subdirRow->addWidget(subdirCheck);
    subdirRow->addWidget(new InfoButton(
        tr("Create category subdirectories automatically if they don't exist"), &dialog));
    subdirRow->addStretch();
    layout->addRow(subdirRow);

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
        m_collision_strategy->setCurrentIndex(collisionCombo->currentIndex());
        m_preview_mode_checkbox->setChecked(previewCheck->isChecked());
        m_create_subdirs_checkbox->setChecked(subdirCheck->isChecked());
    }
}

// ============================================================================
// Duplicate Finder — logic
// ============================================================================

void OrganizerPanel::updateDedupDirectorySummary()
{
    int count = m_dedup_directory_list->count();
    if (count == 0) {
        m_dedup_summary_label->setText(tr("No directories added"));
        return;
    }

    qint64 totalSize = 0;
    int totalFiles = 0;
    for (int i = 0; i < count; ++i) {
        QDir dir(m_dedup_directory_list->item(i)->text());
        if (dir.exists()) {
            QDirIterator it(dir.absolutePath(), QDir::Files,
                m_dedup_recursive->isChecked()
                    ? QDirIterator::Subdirectories
                    : QDirIterator::NoIteratorFlags);
            while (it.hasNext()) {
                it.next();
                totalSize += it.fileInfo().size();
                ++totalFiles;
            }
        }
    }

    QString sizeStr;
    if (totalSize >= sak::kBytesPerGB) {
        sizeStr = QString("%1 GB").arg(totalSize / sak::kBytesPerGBf, 0, 'f', 2);
    } else if (totalSize >= sak::kBytesPerMB) {
        sizeStr = QString("%1 MB").arg(totalSize / sak::kBytesPerMBf, 0, 'f', 1);
    } else {
        sizeStr = QString("%1 KB").arg(totalSize / sak::kBytesPerKBf, 0, 'f', 0);
    }

    m_dedup_summary_label->setText(
        tr("%1 directory(ies) \u2022 %2 files (%3) to scan")
            .arg(count).arg(totalFiles).arg(sizeStr));
}

void OrganizerPanel::onDedupAddDirectoryClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Directory to Scan"),
        QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        // Prevent duplicate directory entries
        for (int i = 0; i < m_dedup_directory_list->count(); ++i) {
            if (QDir(m_dedup_directory_list->item(i)->text()) == QDir(dir)) {
                QMessageBox::information(this, tr("Duplicate Directory"),
                    tr("This directory is already in the scan list."));
                return;
            }
        }
        m_dedup_directory_list->addItem(dir);
        updateDedupDirectorySummary();
        logMessage(QString("Added scan directory: %1").arg(dir));
    }
}

void OrganizerPanel::onDedupRemoveDirectoryClicked()
{
    auto selected = m_dedup_directory_list->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, tr("No Selection"),
            tr("Please select a directory to remove."));
        return;
    }

    for (auto* item : selected) {
        delete item;
    }
    updateDedupDirectorySummary();
}

void OrganizerPanel::onDedupClearAllClicked()
{
    if (m_dedup_directory_list->count() == 0) return;

    auto result = QMessageBox::question(this, tr("Clear All"),
        tr("Remove all directories from the scan list?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (result == QMessageBox::Yes) {
        m_dedup_directory_list->clear();
        updateDedupDirectorySummary();
        m_dedup_results_label->setVisible(false);
        logMessage(tr("Scan directory list cleared"));
    }
}

void OrganizerPanel::onDedupScanClicked()
{
    if (m_dedup_directory_list->count() == 0) {
        QMessageBox::warning(this, tr("Validation Error"),
            tr("Please add at least one directory to scan for duplicates."));
        return;
    }

    m_dedup_worker.reset();

    DuplicateFinderWorker::Config config;
    for (int i = 0; i < m_dedup_directory_list->count(); ++i) {
        config.scanDirectories.push_back(m_dedup_directory_list->item(i)->text());
    }
    config.minimum_file_size =
        m_dedup_min_size->value() * sak::kBytesPerKB;
    config.recursive_scan = m_dedup_recursive->isChecked();
    config.parallel_hashing = m_dedup_parallel_hashing->isChecked();
    config.hash_thread_count = m_dedup_thread_count->value();

    m_dedup_worker = std::make_unique<DuplicateFinderWorker>(config, this);

    connect(m_dedup_worker.get(), &DuplicateFinderWorker::started,
        this, &OrganizerPanel::onDedupWorkerStarted);
    connect(m_dedup_worker.get(), &DuplicateFinderWorker::finished,
        this, &OrganizerPanel::onDedupWorkerFinished);
    connect(m_dedup_worker.get(), &DuplicateFinderWorker::failed,
        this, &OrganizerPanel::onDedupWorkerFailed);
    connect(m_dedup_worker.get(), &DuplicateFinderWorker::cancelled,
        this, &OrganizerPanel::onDedupWorkerCancelled);
    connect(m_dedup_worker.get(), &DuplicateFinderWorker::scanProgress,
        this, &OrganizerPanel::onDedupScanProgress);
    connect(m_dedup_worker.get(), &DuplicateFinderWorker::resultsReady,
        this, &OrganizerPanel::onDedupResultsReady);

    setDedupRunning(true);
    m_dedup_progress_bar->setValue(0);
    m_dedup_progress_bar->setVisible(true);
    m_dedup_results_label->setVisible(false);
    Q_EMIT statusMessage(tr("Starting duplicate scan..."), 0);
    m_dedup_worker->start();

    logInfo("Duplicate finder scan initiated from organizer panel");
}

void OrganizerPanel::onDedupCancelClicked()
{
    if (m_dedup_worker) {
        m_dedup_worker->requestStop();
        logMessage(tr("Duplicate scan cancellation requested..."));
        Q_EMIT statusMessage(tr("Cancelling duplicate scan..."), 0);
    }
}

void OrganizerPanel::onDedupSettingsClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Duplicate Finder Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthSmall);

    auto* layout = new QFormLayout(&dialog);

    auto* minSizeSpin = new QSpinBox(&dialog);
    minSizeSpin->setMinimum(0);
    minSizeSpin->setMaximum(1000000);
    minSizeSpin->setValue(m_dedup_min_size->value());
    minSizeSpin->setSuffix(tr(" KB"));
    layout->addRow(
        InfoButton::createInfoLabel(tr("Minimum File Size:"),
            tr("Skip tiny files to speed up scanning (0 = check all files)"), &dialog),
        minSizeSpin);

    auto* recursiveCheck = new QCheckBox(tr("Include all nested subfolders"), &dialog);
    recursiveCheck->setChecked(m_dedup_recursive->isChecked());
    layout->addRow(
        InfoButton::createInfoLabel(tr("Recursive Scan:"),
            tr("Scan all subdirectories recursively, not just the top-level folder"), &dialog),
        recursiveCheck);

    auto* parallelCheck = new QCheckBox(tr("Use parallel hashing"), &dialog);
    parallelCheck->setChecked(m_dedup_parallel_hashing->isChecked());
    layout->addRow(
        InfoButton::createInfoLabel(tr("Parallel Hashing:"),
            tr("Use multiple CPU cores for faster hash calculation. "
               "Disable for debugging or low-resource systems."), &dialog),
        parallelCheck);

    int cpuCores = QThread::idealThreadCount();
    auto* threadSpin = new QSpinBox(&dialog);
    threadSpin->setMinimum(0);
    threadSpin->setMaximum(64);
    threadSpin->setValue(m_dedup_thread_count->value());
    threadSpin->setSpecialValueText(tr("Auto (%1 cores)").arg(cpuCores > 0 ? cpuCores : 4));
    threadSpin->setEnabled(parallelCheck->isChecked());
    connect(parallelCheck, &QCheckBox::toggled, threadSpin, &QSpinBox::setEnabled);
    layout->addRow(
        InfoButton::createInfoLabel(tr("Thread Count:"),
            tr("Number of threads for parallel hashing. "
               "0 = auto-detect from CPU cores."), &dialog),
        threadSpin);

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
        m_dedup_min_size->setValue(minSizeSpin->value());
        m_dedup_recursive->setChecked(recursiveCheck->isChecked());
        m_dedup_parallel_hashing->setChecked(parallelCheck->isChecked());
        m_dedup_thread_count->setValue(threadSpin->value());
        updateDedupDirectorySummary();
    }
}

void OrganizerPanel::onDedupWorkerStarted()
{
    logMessage(tr("Duplicate file scan started"));
    Q_EMIT statusMessage(tr("Duplicate scan in progress"), 0);
}

void OrganizerPanel::onDedupWorkerFinished()
{
    setDedupRunning(false);
    m_dedup_progress_bar->setValue(100);
    Q_EMIT statusMessage(tr("Duplicate scan complete"), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(100, 100);
    logMessage(tr("Duplicate scan completed successfully"));
    logInfo("Duplicate finder scan completed successfully");
}

void OrganizerPanel::onDedupWorkerFailed(int errorCode, const QString& errorMessage)
{
    setDedupRunning(false);
    m_dedup_progress_bar->setVisible(false);
    Q_EMIT statusMessage(tr("Duplicate scan failed"), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(0, 100);
    logMessage(QString("Duplicate scan failed: Error %1: %2").arg(errorCode).arg(errorMessage));
    QMessageBox::warning(this, tr("Scan Failed"),
        QString("Error %1: %2").arg(errorCode).arg(errorMessage));
    logError("Duplicate finder scan failed: {}", errorMessage.toStdString());
}

void OrganizerPanel::onDedupWorkerCancelled()
{
    setDedupRunning(false);
    m_dedup_progress_bar->setVisible(false);
    logMessage(tr("Duplicate scan cancelled by user"));
    Q_EMIT statusMessage(tr("Duplicate scan cancelled"), sak::kTimerStatusMessageMs);
    Q_EMIT progressUpdate(0, 100);
}

void OrganizerPanel::onDedupScanProgress(int current, int total, const QString& path)
{
    Q_EMIT progressUpdate(current, total);

    if (total > 0) {
        int pct = static_cast<int>(static_cast<double>(current) / total * 100);
        m_dedup_progress_bar->setValue(pct);
    }

    QFileInfo info(path);
    Q_EMIT statusMessage(QString("Scanning: %1").arg(info.fileName()), 0);
}

void OrganizerPanel::onDedupResultsReady(const QString& summary, int duplicateCount,
    qint64 wastedSpace)
{
    QString sizeStr;
    if (wastedSpace >= sak::kBytesPerGB) {
        sizeStr = QString("%1 GB").arg(wastedSpace / sak::kBytesPerGBf, 0, 'f', 2);
    } else {
        sizeStr = QString("%1 MB").arg(wastedSpace / sak::kBytesPerMBf, 0, 'f', 2);
    }

    QString resultsText = tr("Found %1 duplicate file(s), %2 wasted space")
        .arg(duplicateCount).arg(sizeStr);

    // Show persistent results label
    if (duplicateCount > 0) {
        m_dedup_results_label->setStyleSheet(
            QString("background: %1; color: %2; padding: 8px; border-radius: 6px; font-weight: 600;")
                .arg(ui::kColorBgWarningPanel).arg(ui::kColorWarning));
    } else {
        m_dedup_results_label->setStyleSheet(
            QString("background: %1; color: %2; padding: 8px; border-radius: 6px; font-weight: 600;")
                .arg(ui::kColorBgInfoPanel).arg(ui::kColorSuccess));
    }
    m_dedup_results_label->setText(resultsText);
    m_dedup_results_label->setVisible(true);

    Q_EMIT statusMessage(resultsText, sak::kTimerHealthPollMs);
    logMessage(resultsText);

    showScrollableResultsDialog(tr("Duplicate Scan Results"), summary);
}

void OrganizerPanel::setDedupRunning(bool running)
{
    m_dedup_running = running;

    m_dedup_directory_list->setEnabled(!running);
    m_dedup_add_button->setEnabled(!running);
    m_dedup_remove_button->setEnabled(!running);
    m_dedup_clear_button->setEnabled(!running);
    m_dedup_scan_button->setEnabled(!running);
    m_dedup_cancel_button->setEnabled(running);

    // Cross-lock: disable organizer tab while scanning
    m_tabs->setTabEnabled(0, !running);
}

} // namespace sak
