// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file organizer_panel.cpp
/// @brief Unified file management panel -- organizer + duplicate finder in tabs

#include "sak/organizer_panel.h"

#include "sak/detachable_log_window.h"
#include "sak/duplicate_finder_worker.h"
#include "sak/file_management_explorer_panel.h"
#include "sak/info_button.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/organizer_worker.h"
#include "sak/storage_inventory_worker.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QThread>
#include <QVBoxLayout>

namespace sak {

namespace {

constexpr int kCategoryTableMinHeight = 200;
constexpr int kOrganizerWorkerStopTimeoutMs = sak::kTimeoutThreadShutdownMs;
constexpr int kCategoryTableColumnCount = 2;
constexpr int kProgressMaximum = 100;
constexpr int kDedupMinSizeMaxKb = 1'000'000;
constexpr int kDedupThreadCountMax = 64;
constexpr int kSizeGbPrecision = 2;
constexpr int kFallbackCpuCoreCount = 4;

struct DedupSettingsDefaults {
    int minSizeKb;
    bool recursive;
    bool parallelHashing;
    int threadCount;
};

struct DedupSettingsWidgets {
    QSpinBox* minSizeSpin{nullptr};
    QCheckBox* recursiveCheck{nullptr};
    QCheckBox* parallelCheck{nullptr};
    QSpinBox* threadSpin{nullptr};
};

QString dedupTr(const char* text) {
    return QCoreApplication::translate("OrganizerPanel", text);
}

void addDialogButtonRow(QFormLayout* layout,
                        QDialog* dialog,
                        const QString& okText,
                        const QString& cancelText) {
    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    auto* okBtn = new QPushButton(okText, dialog);
    okBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    QObject::connect(okBtn, &QPushButton::clicked, dialog, &QDialog::accept);
    btnLayout->addWidget(okBtn);

    auto* cancelBtn = new QPushButton(cancelText, dialog);
    cancelBtn->setStyleSheet(ui::kSecondaryButtonStyle);
    QObject::connect(cancelBtn, &QPushButton::clicked, dialog, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    layout->addRow(btnLayout);
}

DedupSettingsWidgets addDedupSettingsRows(QFormLayout* layout,
                                          QDialog* dialog,
                                          const DedupSettingsDefaults& defaults) {
    DedupSettingsWidgets widgets;
    widgets.minSizeSpin = new QSpinBox(dialog);
    widgets.minSizeSpin->setMinimum(0);
    widgets.minSizeSpin->setMaximum(kDedupMinSizeMaxKb);
    widgets.minSizeSpin->setValue(defaults.minSizeKb);
    widgets.minSizeSpin->setSuffix(dedupTr(" KB"));
    layout->addRow(InfoButton::createInfoLabel(
                       dedupTr("Minimum File Size:"),
                       dedupTr("Skip tiny files to speed up scanning (0 = check all files)"),
                       dialog),
                   widgets.minSizeSpin);

    widgets.recursiveCheck = new QCheckBox(dedupTr("Include all nested subfolders"), dialog);
    widgets.recursiveCheck->setChecked(defaults.recursive);
    layout->addRow(
        InfoButton::createInfoLabel(
            dedupTr("Recursive Scan:"),
            dedupTr("Scan all subdirectories recursively, not just the top-level folder"),
            dialog),
        widgets.recursiveCheck);

    widgets.parallelCheck = new QCheckBox(dedupTr("Use parallel hashing"), dialog);
    widgets.parallelCheck->setChecked(defaults.parallelHashing);
    layout->addRow(
        InfoButton::createInfoLabel(dedupTr("Parallel Hashing:"),
                                    dedupTr("Use multiple CPU cores for faster hash calculation. "
                                            "Disable for debugging or low-resource systems."),
                                    dialog),
        widgets.parallelCheck);

    const int cpuCores = QThread::idealThreadCount();
    widgets.threadSpin = new QSpinBox(dialog);
    widgets.threadSpin->setMinimum(0);
    widgets.threadSpin->setMaximum(kDedupThreadCountMax);
    widgets.threadSpin->setValue(defaults.threadCount);
    widgets.threadSpin->setSpecialValueText(
        dedupTr("Auto (%1 cores)").arg(cpuCores > 0 ? cpuCores : kFallbackCpuCoreCount));
    widgets.threadSpin->setEnabled(widgets.parallelCheck->isChecked());
    QObject::connect(
        widgets.parallelCheck, &QCheckBox::toggled, widgets.threadSpin, &QSpinBox::setEnabled);
    layout->addRow(InfoButton::createInfoLabel(dedupTr("Thread Count:"),
                                               dedupTr("Number of threads for parallel hashing. "
                                                       "0 = auto-detect from CPU cores."),
                                               dialog),
                   widgets.threadSpin);
    return widgets;
}

}  // namespace

OrganizerPanel::OrganizerPanel(QWidget* parent) : QWidget(parent), m_worker(nullptr) {
    setupUi();
    refreshMountedFileSystemTargets();
    setupDefaultCategories();
    logInfo("OrganizerPanel initialized");
}

OrganizerPanel::~OrganizerPanel() {
    if (m_worker) {
        m_worker->requestStop();
        if (!m_worker->wait(kOrganizerWorkerStopTimeoutMs)) {
            logError("OrganizerWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    if (m_dedup_worker) {
        m_dedup_worker->requestStop();
        if (!m_dedup_worker->wait(kOrganizerWorkerStopTimeoutMs)) {
            logError(
                "DuplicateFinderWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    logInfo("OrganizerPanel destroyed");
}

// ============================================================================
// Main setup -- panel header, tabbed layout, shared status bar
// ============================================================================

void OrganizerPanel::updateHeaderForTab(int index) {
    struct TabMeta {
        const char* icon;
        const char* title;
        const char* subtitle;
    };
    static constexpr TabMeta kTabs[] = {
        {":/icons/icons/panel_organizer.svg",
         "File Organizer",
         "Organize files into categorized folders"},
        {":/icons/icons/icons8-duplicate.svg",
         "Duplicate Finder",
         "Find duplicate files to reclaim disk space"},
        {":/icons/icons/panel_search.svg",
         "File Explorer",
         "Browse mounted and supported non-Windows file systems"},
        {":/icons/icons/panel_search.svg",
         "Advanced Search",
         "Search file contents, metadata, archives, and binary data across directory trees"},
    };
    if (index >= 0 && index < static_cast<int>(std::size(kTabs))) {
        const auto& meta = kTabs[index];
        sak::updatePanelHeader(
            m_headerWidgets, QString::fromUtf8(meta.icon), tr(meta.title), tr(meta.subtitle));
    }
}

void OrganizerPanel::setupUi() {
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    rootLayout->setSpacing(ui::kSpacingDefault);

    // Dynamic panel header -- updates when sub-tab changes
    m_headerWidgets =
        sak::createDynamicPanelHeader(this,
                                      QStringLiteral(":/icons/icons/panel_organizer.svg"),
                                      tr("File Organizer"),
                                      tr("Organize files into categorized folders and "
                                         "find duplicate files to reclaim disk space"),
                                      rootLayout);

    // Tabbed content
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(createOrganizerTab(), tr("File Organizer"));
    m_tabs->addTab(createDuplicateFinderTab(), tr("Duplicate Finder"));
    m_file_explorer_panel = new FileManagementExplorerPanel(this);
    m_tabs->addTab(m_file_explorer_panel, tr("File Explorer"));
    setAccessible(
        m_tabs,
        tr("File management tools"),
        tr("Switch between file organizer, duplicate finder, file explorer, and advanced search"));
    rootLayout->addWidget(m_tabs, 1);

    connect(m_file_explorer_panel,
            &FileManagementExplorerPanel::statusMessage,
            this,
            &OrganizerPanel::statusMessage);
    connect(m_file_explorer_panel,
            &FileManagementExplorerPanel::progressUpdate,
            this,
            &OrganizerPanel::progressUpdate);
    connect(m_file_explorer_panel,
            &FileManagementExplorerPanel::logOutput,
            this,
            &OrganizerPanel::logOutput);

    // Update header when sub-tab changes
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        updateHeaderForTab(index);
    });

    // Shared status bar with log toggle
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(
        sak::ui::kMarginNone, ui::kSpacingTight, sak::ui::kMarginNone, sak::ui::kMarginNone);
    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    statusRow->addWidget(m_logToggle);
    statusRow->addStretch();
    rootLayout->addLayout(statusRow);
}

void OrganizerPanel::refreshMountedFileSystemTargets() {
    m_file_system_targets = FileManagementFileSystemBridge::mountedTargets();
    populateFileSystemTargetCombos();
}

void OrganizerPanel::scanFileSystemTargets() {
    Q_EMIT statusMessage(tr("Scanning disk and partition file-system targets..."), 0);
    setEnabled(false);
    const auto inventory = StorageInventoryWorker::scanCurrentSystem();
    setEnabled(true);
    m_file_system_targets = FileManagementFileSystemBridge::targetsFromInventory(inventory);
    populateFileSystemTargetCombos();
    Q_EMIT statusMessage(tr("File-system targets scanned"), sak::kTimerStatusDefaultMs);
}

void OrganizerPanel::addManualFileSystemTarget() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add Raw or Image Target"));
    dialog.setMinimumWidth(sak::kDialogWidthLarge);
    auto* layout = new QFormLayout(&dialog);

    auto* path = new QLineEdit(&dialog);
    path->setAccessibleName(tr("Raw or image target path"));
    auto* browse = new QPushButton(tr("Browse"), &dialog);
    browse->setStyleSheet(ui::kSecondaryButtonStyle);
    auto* row = new QHBoxLayout();
    row->addWidget(path, 1);
    row->addWidget(browse);
    layout->addRow(tr("Target path:"), row);

    auto* fs = new QComboBox(&dialog);
    fs->addItems({QStringLiteral("ext2"),
                  QStringLiteral("ext3"),
                  QStringLiteral("ext4"),
                  QStringLiteral("HFS+"),
                  QStringLiteral("HFSX"),
                  QStringLiteral("APFS"),
                  QStringLiteral("XFS"),
                  QStringLiteral("Btrfs")});
    layout->addRow(tr("File system:"), fs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add Target"));
    layout->addRow(buttons);
    connect(browse, &QPushButton::clicked, &dialog, [this, path]() {
        const QString file = QFileDialog::getOpenFileName(this, tr("Select Raw or Image Target"));
        if (!file.isEmpty()) {
            path->setText(file);
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted || path->text().trimmed().isEmpty()) {
        return;
    }
    m_file_system_targets.append(
        FileManagementFileSystemBridge::manualTarget(path->text(), fs->currentText()));
    populateFileSystemTargetCombos();
}

void OrganizerPanel::populateFileSystemTargetCombos() {
    const auto fillCombo = [this](QComboBox* combo) {
        if (!combo) {
            return;
        }
        QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto& target : m_file_system_targets) {
            combo->addItem(target.label);
        }
        if (!m_file_system_targets.isEmpty()) {
            combo->setCurrentIndex(0);
        }
    };
    fillCombo(m_organizer_target_combo);
    fillCombo(m_dedup_target_combo);
    onOrganizerTargetChanged(m_organizer_target_combo ? m_organizer_target_combo->currentIndex()
                                                      : -1);
    onDedupTargetChanged(m_dedup_target_combo ? m_dedup_target_combo->currentIndex() : -1);
}

FileManagementTarget OrganizerPanel::currentTargetForCombo(const QComboBox* combo) const {
    const int index = combo ? combo->currentIndex() : -1;
    if (index < 0 || index >= m_file_system_targets.size()) {
        return {};
    }
    return m_file_system_targets.at(index);
}

FileManagementTarget OrganizerPanel::currentOrganizerTarget() const {
    return currentTargetForCombo(m_organizer_target_combo);
}

FileManagementTarget OrganizerPanel::currentDedupTarget() const {
    return currentTargetForCombo(m_dedup_target_combo);
}

// ============================================================================
// Tab 1 -- File Organizer
// ============================================================================

// ============================================================================
// Organizer Tab -- Input Groups
// ============================================================================

QGroupBox* OrganizerPanel::createTargetDirectoryGroup() {
    auto* group = new QGroupBox(tr("File-System Target"), this);
    auto* group_layout = new QVBoxLayout(group);

    auto* target_row = new QHBoxLayout();
    m_organizer_target_combo = new QComboBox(this);
    m_organizer_target_combo->setAccessibleName(tr("Organizer file-system target"));
    m_organizer_target_combo->setToolTip(
        tr("Choose a mounted volume or supported raw/image target"));
    target_row->addWidget(m_organizer_target_combo, 1);

    m_target_refresh_button = new QPushButton(tr("Refresh"), this);
    m_target_refresh_button->setAccessibleName(tr("Refresh organizer targets"));
    m_target_refresh_button->setStyleSheet(ui::kSecondaryButtonStyle);
    target_row->addWidget(m_target_refresh_button);

    m_target_scan_button = new QPushButton(tr("Scan Disks"), this);
    m_target_scan_button->setAccessibleName(tr("Scan disk and partition organizer targets"));
    m_target_scan_button->setStyleSheet(ui::kPrimaryButtonStyle);
    target_row->addWidget(m_target_scan_button);

    m_target_manual_button = new QPushButton(tr("Add Raw/Image"), this);
    m_target_manual_button->setAccessibleName(tr("Add raw or image organizer target"));
    m_target_manual_button->setStyleSheet(ui::kPrimaryButtonStyle);
    target_row->addWidget(m_target_manual_button);
    group_layout->addLayout(target_row);

    auto* path_row = new QHBoxLayout();
    m_target_path = new QLineEdit(this);
    m_target_path->setPlaceholderText(tr("Select directory to organize..."));
    m_target_path->setAccessibleName(QStringLiteral("Target Directory Path"));
    m_target_path->setToolTip(QStringLiteral("Path to the directory that will be organized"));
    path_row->addWidget(m_target_path, 1);

    m_browse_button = new QPushButton(tr("Browse..."), this);
    m_browse_button->setAccessibleName(QStringLiteral("Browse Directory"));
    m_browse_button->setToolTip(QStringLiteral("Browse for a directory to organize"));
    m_browse_button->setStyleSheet(ui::kPrimaryButtonStyle);
    path_row->addWidget(m_browse_button);
    group_layout->addLayout(path_row);

    m_dir_summary_label = new QLabel(tr("No directory selected"), this);
    m_dir_summary_label->setStyleSheet(
        ui::paddedStatusTextStyle(ui::kColorTextMuted, ui::kFontSizeNote));
    m_dir_summary_label->setAccessibleName(QStringLiteral("Directory Summary"));
    group_layout->addWidget(m_dir_summary_label);

    return group;
}

QGroupBox* OrganizerPanel::createCategoryMappingGroup() {
    auto* group = new QGroupBox(tr("Category Mapping"), this);
    auto* group_layout = new QVBoxLayout(group);

    m_category_table = new QTableWidget(this);
    m_category_table->setColumnCount(kCategoryTableColumnCount);
    m_category_table->setHorizontalHeaderLabels(
        {tr("Category"), tr("Extensions (comma-separated)")});
    m_category_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_category_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_category_table->setAlternatingRowColors(true);
    m_category_table->setMinimumHeight(kCategoryTableMinHeight);
    m_category_table->setAccessibleName(QStringLiteral("Category Mappings Table"));
    m_category_table->setToolTip(QStringLiteral("File categories and their associated extensions"));
    group_layout->addWidget(m_category_table);

    auto* btn_row = new QHBoxLayout();
    m_add_category_button = new QPushButton(tr("Add Category"), this);
    m_add_category_button->setAccessibleName(QStringLiteral("Add Category"));
    m_add_category_button->setToolTip(QStringLiteral("Add a new file category row"));
    m_add_category_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_remove_category_button = new QPushButton(tr("Remove Selected"), this);
    m_remove_category_button->setAccessibleName(QStringLiteral("Remove Category"));
    m_remove_category_button->setToolTip(
        QStringLiteral("Remove the selected category from the list"));
    m_remove_category_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_reset_categories_button = new QPushButton(tr("Reset to Defaults"), this);
    m_reset_categories_button->setAccessibleName(QStringLiteral("Reset Categories"));
    m_reset_categories_button->setToolTip(
        QStringLiteral("Restore the default category-to-extension mappings"));
    m_reset_categories_button->setStyleSheet(ui::kPrimaryButtonStyle);
    btn_row->addWidget(m_add_category_button);
    btn_row->addWidget(m_remove_category_button);
    btn_row->addWidget(m_reset_categories_button);
    btn_row->addStretch();
    group_layout->addLayout(btn_row);

    return group;
}

// ============================================================================
// Tab 1 -- File Organizer
// ============================================================================

QWidget* OrganizerPanel::createOrganizerTab() {
    auto* tab = new QWidget(this);
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);

    layout->addWidget(createTargetDirectoryGroup());
    layout->addWidget(createCategoryMappingGroup());

    // Hidden options (managed via Settings dialog)
    m_collision_strategy = new QComboBox(this);
    m_collision_strategy->addItems({tr("Rename"), tr("Skip"), tr("Overwrite")});
    m_collision_strategy->setAccessibleName(QStringLiteral("Collision Strategy"));
    m_collision_strategy->setVisible(false);

    m_preview_mode_checkbox = new QCheckBox(tr("Preview Mode (Dry Run)"), this);
    m_preview_mode_checkbox->setAccessibleName(QStringLiteral("Preview mode dry run"));
    m_preview_mode_checkbox->setChecked(true);
    m_preview_mode_checkbox->setVisible(false);

    m_create_subdirs_checkbox = new QCheckBox(tr("Create Subdirectories"), this);
    m_create_subdirs_checkbox->setAccessibleName(QStringLiteral("Create category subdirectories"));
    m_create_subdirs_checkbox->setChecked(true);
    m_create_subdirs_checkbox->setVisible(false);

    // Progress bar
    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setRange(0, kProgressMaximum);
    m_progress_bar->setValue(0);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setVisible(false);
    m_progress_bar->setAccessibleName(QStringLiteral("Organization Progress"));
    layout->addWidget(m_progress_bar);

    QPushButton* settingsBtn = nullptr;
    createOrganizerControls(layout, settingsBtn);

    connectOrganizerTabSignals(settingsBtn);

    scrollArea->setWidget(tab);
    return scrollArea;
}

void OrganizerPanel::connectOrganizerTabSignals(QPushButton* settingsBtn) {
    connect(m_target_path, &QLineEdit::textChanged, this, &OrganizerPanel::onTargetPathChanged);
    connect(m_organizer_target_combo,
            &QComboBox::currentIndexChanged,
            this,
            &OrganizerPanel::onOrganizerTargetChanged);
    connect(m_target_refresh_button,
            &QPushButton::clicked,
            this,
            &OrganizerPanel::refreshMountedFileSystemTargets);
    connect(
        m_target_scan_button, &QPushButton::clicked, this, &OrganizerPanel::scanFileSystemTargets);
    connect(m_target_manual_button,
            &QPushButton::clicked,
            this,
            &OrganizerPanel::addManualFileSystemTarget);
    connect(m_browse_button, &QPushButton::clicked, this, &OrganizerPanel::onBrowseClicked);
    connect(m_preview_button, &QPushButton::clicked, this, &OrganizerPanel::onPreviewClicked);
    connect(m_execute_button, &QPushButton::clicked, this, &OrganizerPanel::onExecuteClicked);
    connect(m_cancel_button, &QPushButton::clicked, this, &OrganizerPanel::onCancelClicked);
    connect(settingsBtn, &QPushButton::clicked, this, &OrganizerPanel::onSettingsClicked);
    connect(
        m_add_category_button, &QPushButton::clicked, this, &OrganizerPanel::onAddCategoryClicked);
    connect(m_remove_category_button,
            &QPushButton::clicked,
            this,
            &OrganizerPanel::onRemoveCategoryClicked);
    connect(m_reset_categories_button,
            &QPushButton::clicked,
            this,
            &OrganizerPanel::onResetCategoriesClicked);
}

void OrganizerPanel::createOrganizerControls(QVBoxLayout* layout, QPushButton*& settingsBtn) {
    Q_ASSERT(layout);
    auto* row = new QHBoxLayout();

    settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(QStringLiteral("Organizer Settings"));
    settingsBtn->setToolTip(QStringLiteral("Configure collision strategy and preview mode"));
    settingsBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    row->addWidget(settingsBtn);
    row->addStretch();

    m_preview_button = new QPushButton(tr("Preview"), this);
    m_preview_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_preview_button->setAccessibleName(QStringLiteral("Preview Organization"));
    m_preview_button->setToolTip(
        QStringLiteral("Preview file organization without making changes"));
    m_preview_button->setStyleSheet(ui::kSecondaryButtonStyle);
    row->addWidget(m_preview_button);

    m_execute_button = new QPushButton(tr("Organize Files"), this);
    m_execute_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_execute_button->setAccessibleName(QStringLiteral("Execute Organization"));
    m_execute_button->setToolTip(QStringLiteral("Organize files into category folders"));
    m_execute_button->setStyleSheet(ui::kPrimaryButtonStyle);
    row->addWidget(m_execute_button);

    m_cancel_button = new QPushButton(tr("Cancel"), this);
    m_cancel_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_cancel_button->setEnabled(false);
    m_cancel_button->setAccessibleName(QStringLiteral("Cancel Organization"));
    m_cancel_button->setToolTip(QStringLiteral("Cancel the current organization operation"));
    m_cancel_button->setStyleSheet(ui::kDangerButtonStyle);
    row->addWidget(m_cancel_button);

    layout->addLayout(row);
}

// ============================================================================
// Duplicate Finder -- Group Builders
// ============================================================================

QGroupBox* OrganizerPanel::createScanDirectoriesGroup() {
    auto* group = new QGroupBox(tr("Scan Directories"), this);
    auto* group_layout = new QVBoxLayout(group);

    auto* target_row = new QHBoxLayout();
    m_dedup_target_combo = new QComboBox(this);
    m_dedup_target_combo->setAccessibleName(tr("Duplicate finder file-system target"));
    m_dedup_target_combo->setToolTip(tr("Choose a mounted volume or supported raw/image target"));
    target_row->addWidget(m_dedup_target_combo, 1);
    group_layout->addLayout(target_row);

    m_dedup_directory_list = new QListWidget(this);
    m_dedup_directory_list->setMinimumHeight(sak::kListAreaMinH);
    m_dedup_directory_list->setAccessibleName(QStringLiteral("Duplicate Scan Directories List"));
    m_dedup_directory_list->setToolTip(QStringLiteral("Directories to scan for duplicate files"));
    group_layout->addWidget(m_dedup_directory_list);

    auto* btn_row = new QHBoxLayout();
    m_dedup_add_button = new QPushButton(tr("Add Directory"), this);
    m_dedup_add_button->setAccessibleName(QStringLiteral("Add Scan Directory"));
    m_dedup_add_button->setToolTip(QStringLiteral("Add a directory to the scan list"));
    m_dedup_add_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_dedup_remove_button = new QPushButton(tr("Remove Selected"), this);
    m_dedup_remove_button->setAccessibleName(QStringLiteral("Remove Selected Directory"));
    m_dedup_remove_button->setToolTip(QStringLiteral("Remove highlighted directory from the list"));
    m_dedup_remove_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_dedup_clear_button = new QPushButton(tr("Clear All"), this);
    m_dedup_clear_button->setAccessibleName(QStringLiteral("Clear All Directories"));
    m_dedup_clear_button->setToolTip(QStringLiteral("Remove all directories from the scan list"));
    m_dedup_clear_button->setStyleSheet(ui::kPrimaryButtonStyle);
    btn_row->addWidget(m_dedup_add_button);
    btn_row->addWidget(m_dedup_remove_button);
    btn_row->addWidget(m_dedup_clear_button);
    btn_row->addStretch();
    group_layout->addLayout(btn_row);

    m_dedup_summary_label = new QLabel(tr("No directories added"), this);
    m_dedup_summary_label->setStyleSheet(
        ui::paddedStatusTextStyle(ui::kColorTextMuted, ui::kFontSizeNote));
    m_dedup_summary_label->setAccessibleName(QStringLiteral("Scan Directories Summary"));
    group_layout->addWidget(m_dedup_summary_label);

    return group;
}

void OrganizerPanel::createDedupControls(QVBoxLayout* layout, QPushButton*& settingsBtn) {
    Q_ASSERT(layout);
    auto* row = new QHBoxLayout();

    settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(QStringLiteral("Duplicate Finder Settings"));
    settingsBtn->setToolTip(
        QStringLiteral("Configure minimum file size, recursion,"
                       " and hashing options"));
    settingsBtn->setStyleSheet(ui::kPrimaryButtonStyle);
    row->addWidget(settingsBtn);
    row->addStretch();

    m_dedup_scan_button = new QPushButton(tr("Find Duplicates"), this);
    m_dedup_scan_button->setMinimumWidth(sak::kButtonWidthMedium);
    m_dedup_scan_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_dedup_scan_button->setAccessibleName(QStringLiteral("Start Duplicate Scan"));
    m_dedup_scan_button->setToolTip(
        QStringLiteral("Scan the listed directories for duplicate files"
                       " using content hashing"));
    row->addWidget(m_dedup_scan_button);

    m_dedup_cancel_button = new QPushButton(tr("Cancel"), this);
    m_dedup_cancel_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_dedup_cancel_button->setEnabled(false);
    m_dedup_cancel_button->setAccessibleName(QStringLiteral("Cancel Duplicate Scan"));
    m_dedup_cancel_button->setToolTip(QStringLiteral("Cancel the duplicate scan in progress"));
    m_dedup_cancel_button->setStyleSheet(ui::kDangerButtonStyle);
    row->addWidget(m_dedup_cancel_button);

    layout->addLayout(row);
}

// ============================================================================
// Tab 2 -- Duplicate Finder
// ============================================================================

QWidget* OrganizerPanel::createDuplicateFinderTab() {
    auto* tab = new QWidget(this);
    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    auto* layout = new QVBoxLayout(tab);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingDefault);
    layout->addWidget(createScanDirectoriesGroup());
    // Hidden options (managed via Settings dialog)
    m_dedup_min_size = new QSpinBox(this);
    m_dedup_min_size->setAccessibleName(QStringLiteral("Duplicate finder minimum file size"));
    m_dedup_min_size->setMinimum(0);
    m_dedup_min_size->setMaximum(kDedupMinSizeMaxKb);
    m_dedup_min_size->setValue(0);
    m_dedup_min_size->setVisible(false);
    m_dedup_recursive = new QCheckBox(tr("Recursive Scan"), this);
    m_dedup_recursive->setAccessibleName(QStringLiteral("Recursive duplicate scan"));
    m_dedup_recursive->setChecked(true);
    m_dedup_recursive->setVisible(false);
    m_dedup_parallel_hashing = new QCheckBox(tr("Parallel Hashing"), this);
    m_dedup_parallel_hashing->setAccessibleName(QStringLiteral("Parallel duplicate hashing"));
    m_dedup_parallel_hashing->setChecked(true);
    m_dedup_parallel_hashing->setVisible(false);
    m_dedup_thread_count = new QSpinBox(this);
    m_dedup_thread_count->setAccessibleName(QStringLiteral("Duplicate finder thread count"));
    m_dedup_thread_count->setMinimum(0);
    m_dedup_thread_count->setMaximum(kDedupThreadCountMax);
    m_dedup_thread_count->setValue(0);
    m_dedup_thread_count->setVisible(false);

    // Last scan results
    m_dedup_results_label = new QLabel(this);
    m_dedup_results_label->setWordWrap(true);
    m_dedup_results_label->setVisible(false);
    m_dedup_results_label->setAccessibleName(QStringLiteral("Last Scan Results Summary"));
    layout->addWidget(m_dedup_results_label);

    // Progress bar
    m_dedup_progress_bar = new QProgressBar(this);
    m_dedup_progress_bar->setRange(0, kProgressMaximum);
    m_dedup_progress_bar->setValue(0);
    m_dedup_progress_bar->setTextVisible(true);
    m_dedup_progress_bar->setVisible(false);
    m_dedup_progress_bar->setAccessibleName(QStringLiteral("Duplicate Scan Progress"));
    layout->addWidget(m_dedup_progress_bar);

    QPushButton* settingsBtn = nullptr;
    createDedupControls(layout, settingsBtn);

    connectDuplicateFinderTabSignals(settingsBtn);

    scrollArea->setWidget(tab);
    return scrollArea;
}

void OrganizerPanel::connectDuplicateFinderTabSignals(QPushButton* settingsBtn) {
    connect(m_dedup_add_button,
            &QPushButton::clicked,
            this,
            &OrganizerPanel::onDedupAddDirectoryClicked);
    connect(m_dedup_target_combo,
            &QComboBox::currentIndexChanged,
            this,
            &OrganizerPanel::onDedupTargetChanged);
    connect(m_dedup_remove_button,
            &QPushButton::clicked,
            this,
            &OrganizerPanel::onDedupRemoveDirectoryClicked);
    connect(
        m_dedup_clear_button, &QPushButton::clicked, this, &OrganizerPanel::onDedupClearAllClicked);
    connect(m_dedup_scan_button, &QPushButton::clicked, this, &OrganizerPanel::onDedupScanClicked);
    connect(
        m_dedup_cancel_button, &QPushButton::clicked, this, &OrganizerPanel::onDedupCancelClicked);
    connect(settingsBtn, &QPushButton::clicked, this, &OrganizerPanel::onDedupSettingsClicked);
}

// ============================================================================
// File Organizer -- logic
// ============================================================================

void OrganizerPanel::setupDefaultCategories() {
    Q_ASSERT(m_category_table);
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

void OrganizerPanel::onTargetPathChanged(const QString& path) {
    Q_UNUSED(path)
    updateDirectorySummary();
}

void OrganizerPanel::onOrganizerTargetChanged(int index) {
    Q_UNUSED(index)
    const auto target = currentOrganizerTarget();
    if (!m_target_path || target.root_path.isEmpty()) {
        return;
    }
    m_browse_button->setEnabled(target.local_file_system && !m_operation_running);
    m_target_path->setText(target.local_file_system ? target.root_path : QStringLiteral("/"));
    updateDirectorySummary();
}

void OrganizerPanel::updateDirectorySummary() {
    Q_ASSERT(m_target_path);
    Q_ASSERT(m_dir_summary_label);
    const QString path = m_target_path->text().trimmed();
    const auto target = currentOrganizerTarget();
    if (path.isEmpty()) {
        m_dir_summary_label->setText(tr("No directory selected"));
        return;
    }

    if (!target.root_path.isEmpty() && !target.can_organize) {
        m_dir_summary_label->setText(target.blockers.join(QStringLiteral("; ")));
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
        sizeStr = QString("%1 GB").arg(totalSize / sak::kBytesPerGBf, 0, 'f', kSizeGbPrecision);
    } else if (totalSize >= sak::kBytesPerMB) {
        sizeStr = QString("%1 MB").arg(totalSize / sak::kBytesPerMBf, 0, 'f', 1);
    } else {
        sizeStr = QString("%1 KB").arg(totalSize / sak::kBytesPerKBf, 0, 'f', 0);
    }

    int subdirCount = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
    m_dir_summary_label->setText(tr("%1 files (%2) \u2022 %3 subdirectories")
                                     .arg(entries.size())
                                     .arg(sizeStr)
                                     .arg(subdirCount));
}

void OrganizerPanel::onBrowseClicked() {
    Q_ASSERT(m_target_path);
    QString dir = QFileDialog::getExistingDirectory(this,
                                                    tr("Select Directory to Organize"),
                                                    m_target_path->text(),
                                                    QFileDialog::ShowDirsOnly |
                                                        QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_target_path->setText(dir);
        logMessage(QString("Target directory selected: %1").arg(dir));
    }
}

void OrganizerPanel::onPreviewClicked() {
    m_preview_mode_checkbox->setChecked(true);
    onExecuteClicked();
}

bool OrganizerPanel::validateOrganizerTarget() {
    const auto target = currentOrganizerTarget();
    if (!target.root_path.isEmpty() && !target.can_organize) {
        sak::showWarningLogged(this,
                               tr("Organizer Target Blocked"),
                               target.blockers.join(QStringLiteral("\n")));
        return false;
    }

    if (m_target_path->text().isEmpty()) {
        sak::logWarning("Organization: no target directory selected");
        sak::showWarningLogged(this,
                               tr("Validation Error"),
                               tr("Please select a target directory."));
        return false;
    }

    QDir targetDir(m_target_path->text());
    if (!targetDir.exists()) {
        sak::logWarning("Organization: target directory does not exist: {}",
                        m_target_path->text().toStdString());
        sak::showWarningLogged(this,
                               tr("Validation Error"),
                               tr("Target directory does not exist."));
        return false;
    }
    return validateCategoryMapping();
}

bool OrganizerPanel::confirmOrganizerExecution(const QDir& targetDir) {
    if (m_preview_mode_checkbox->isChecked()) {
        return true;
    }

    const int fileCount = targetDir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot).size();
    auto result = sak::showQuestionLogged(
        this,
        tr("Confirm Organization"),
        tr("This will move up to %1 files in:\n%2\n\n"
           "Collision strategy: %3\n\n"
           "This operation cannot be automatically undone. Continue?")
            .arg(fileCount)
            .arg(m_target_path->text(), m_collision_strategy->currentText()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    return result == QMessageBox::Yes;
}

OrganizerWorker::Config OrganizerPanel::buildOrganizerConfig() const {
    OrganizerWorker::Config config;
    config.target_directory = m_target_path->text();
    config.category_mapping = getCategoryMapping();
    config.preview_mode = m_preview_mode_checkbox->isChecked();
    config.create_subdirectories = m_create_subdirs_checkbox->isChecked();

    QString strategy = m_collision_strategy->currentText().toLower();
    config.collision_strategy = strategy;
    return config;
}

void OrganizerPanel::connectOrganizerWorkerSignals() {
    Q_ASSERT(m_worker);
    connect(m_worker.get(), &OrganizerWorker::started, this, &OrganizerPanel::onWorkerStarted);
    connect(m_worker.get(), &OrganizerWorker::finished, this, &OrganizerPanel::onWorkerFinished);
    connect(m_worker.get(), &OrganizerWorker::failed, this, &OrganizerPanel::onWorkerFailed);
    connect(m_worker.get(), &OrganizerWorker::cancelled, this, &OrganizerPanel::onWorkerCancelled);
    connect(m_worker.get(), &OrganizerWorker::fileProgress, this, &OrganizerPanel::onFileProgress);
    connect(
        m_worker.get(), &OrganizerWorker::previewResults, this, &OrganizerPanel::onPreviewResults);
}

void OrganizerPanel::connectDedupWorkerSignals() {
    Q_ASSERT(m_dedup_worker);
    connect(m_dedup_worker.get(),
            &DuplicateFinderWorker::started,
            this,
            &OrganizerPanel::onDedupWorkerStarted);
    connect(m_dedup_worker.get(),
            &DuplicateFinderWorker::finished,
            this,
            &OrganizerPanel::onDedupWorkerFinished);
    connect(m_dedup_worker.get(),
            &DuplicateFinderWorker::failed,
            this,
            &OrganizerPanel::onDedupWorkerFailed);
    connect(m_dedup_worker.get(),
            &DuplicateFinderWorker::cancelled,
            this,
            &OrganizerPanel::onDedupWorkerCancelled);
    connect(m_dedup_worker.get(),
            &DuplicateFinderWorker::scanProgress,
            this,
            &OrganizerPanel::onDedupScanProgress);
    connect(m_dedup_worker.get(),
            &DuplicateFinderWorker::resultsReady,
            this,
            &OrganizerPanel::onDedupResultsReady);
}

void OrganizerPanel::startOrganizerWorker(const OrganizerWorker::Config& config) {
    m_worker = std::make_unique<OrganizerWorker>(config, this);
    connectOrganizerWorkerSignals();

    setOperationRunning(true);
    m_progress_bar->setValue(0);
    m_progress_bar->setVisible(true);
    Q_EMIT statusMessage(tr("Starting..."), 0);
    m_worker->start();

    QString mode = config.preview_mode ? "Preview" : "Execute";
    logInfo("Organization operation initiated ({}): {}",
            mode.toStdString(),
            config.target_directory.toStdString());
}

void OrganizerPanel::onExecuteClicked() {
    Q_ASSERT(m_target_path);
    Q_ASSERT(m_preview_mode_checkbox);
    if (!validateOrganizerTarget()) {
        return;
    }

    QDir targetDir(m_target_path->text());
    if (!confirmOrganizerExecution(targetDir)) {
        return;
    }

    m_worker.reset();
    startOrganizerWorker(buildOrganizerConfig());
}

void OrganizerPanel::onCancelClicked() {
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage(tr("Cancellation requested..."));
        Q_EMIT statusMessage(tr("Cancelling..."), 0);
        logInfo("Organization cancellation requested by user");
    }
}

void OrganizerPanel::onAddCategoryClicked() {
    int row = m_category_table->rowCount();
    m_category_table->insertRow(row);
    m_category_table->setItem(row, 0, new QTableWidgetItem(tr("New Category")));
    m_category_table->setItem(row, 1, new QTableWidgetItem(QString()));
    m_category_table->editItem(m_category_table->item(row, 0));
}

void OrganizerPanel::onResetCategoriesClicked() {
    auto result = sak::showQuestionLogged(this,
                                          tr("Reset Categories"),
                                          tr("Reset all categories to their default values?\n"
                                             "Any custom categories will be lost."),
                                          QMessageBox::Yes | QMessageBox::No,
                                          QMessageBox::No);
    if (result == QMessageBox::Yes) {
        setupDefaultCategories();
        logMessage(tr("Category mappings reset to defaults"));
    }
}

void OrganizerPanel::onRemoveCategoryClicked() {
    Q_ASSERT(m_category_table);
    auto selected = m_category_table->selectedItems();
    if (selected.isEmpty()) {
        sak::showInformationLogged(this,
                                   tr("No Selection"),
                                   tr("Please select a category to remove."));
        return;
    }

    int row = m_category_table->currentRow();
    if (row >= 0) {
        m_category_table->removeRow(row);
    }
}

void OrganizerPanel::onWorkerStarted() {
    QString mode = m_preview_mode_checkbox->isChecked() ? "preview" : "organization";
    logMessage(QString("Starting %1...").arg(mode));
    Q_EMIT statusMessage(QString("%1 in progress").arg(mode), 0);
}

void OrganizerPanel::onWorkerFinished() {
    Q_ASSERT(m_progress_bar);
    Q_ASSERT(m_preview_mode_checkbox);
    setOperationRunning(false);
    m_progress_bar->setValue(kProgressMaximum);
    QString mode = m_preview_mode_checkbox->isChecked() ? "Preview" : "Organization";
    Q_EMIT statusMessage(QString("%1 complete").arg(mode), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(kProgressMaximum, kProgressMaximum);
    logMessage(QString("%1 completed successfully").arg(mode));
    sak::showInformationLogged(this,
                               QString("%1 Complete").arg(mode),
                               QString("%1 operation completed successfully.").arg(mode));
    logInfo("Organization operation completed successfully");
    updateDirectorySummary();
}

void OrganizerPanel::onWorkerFailed(int errorCode, const QString& errorMessage) {
    Q_ASSERT(m_progress_bar);
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    const QString safe_error = errorMessage.trimmed().isEmpty() ? tr("Unknown error")
                                                                : errorMessage;
    Q_EMIT statusMessage(tr("Organization failed"), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(0, kProgressMaximum);
    logMessage(QString("Organization failed: Error %1: %2").arg(errorCode).arg(safe_error));
    sak::showWarningLogged(this,
                           tr("Organization Failed"),
                           QString("Error %1: %2").arg(errorCode).arg(safe_error));
    logError("Organization failed: {}", safe_error.toStdString());
}

void OrganizerPanel::onWorkerCancelled() {
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    logMessage(tr("Organization cancelled by user"));
    Q_EMIT statusMessage(tr("Organization cancelled"), sak::kTimerStatusMessageMs);
    Q_EMIT progressUpdate(0, kProgressMaximum);
}

void OrganizerPanel::onFileProgress(int current, int total, const QString& filePath) {
    Q_ASSERT(m_progress_bar);
    Q_EMIT progressUpdate(current, total);

    if (total > 0) {
        int pct = static_cast<int>(static_cast<double>(current) / total * kProgressMaximum);
        m_progress_bar->setValue(pct);
    }

    QString filename = QFileInfo(filePath).fileName();
    Q_EMIT statusMessage(QString("Processing: %1").arg(filename), 0);
}

void OrganizerPanel::onPreviewResults(const QString& summary, int operationCount) {
    Q_ASSERT(m_progress_bar);
    m_progress_bar->setVisible(false);
    showScrollableResultsDialog(tr("Preview Results"), summary);
    logMessage(QString("Preview completed: %1 operations planned").arg(operationCount));
}

QMap<QString, QStringList> OrganizerPanel::getCategoryMapping() const {
    Q_ASSERT(m_category_table);
    QMap<QString, QStringList> mapping;

    for (int row = 0; row < m_category_table->rowCount(); ++row) {
        auto* categoryItem = m_category_table->item(row, 0);
        auto* extensionsItem = m_category_table->item(row, 1);

        if (!categoryItem || !extensionsItem) {
            continue;
        }

        QString category = categoryItem->text().trimmed();
        QString extensionsStr = extensionsItem->text().trimmed();

        if (category.isEmpty() || extensionsStr.isEmpty()) {
            continue;
        }

        QStringList extensions = extensionsStr.split(',', Qt::SkipEmptyParts);
        for (auto& ext : extensions) {
            ext = ext.trimmed();
        }
        mapping[category] = extensions;
    }

    return mapping;
}

void OrganizerPanel::setOperationRunning(bool running) {
    Q_ASSERT(m_target_path);
    Q_ASSERT(m_browse_button);
    m_operation_running = running;
    const auto organizerTarget = currentOrganizerTarget();

    // Organizer controls
    m_organizer_target_combo->setEnabled(!running);
    m_target_refresh_button->setEnabled(!running);
    m_target_scan_button->setEnabled(!running);
    m_target_manual_button->setEnabled(!running);
    m_target_path->setEnabled(!running);
    m_browse_button->setEnabled(!running && organizerTarget.local_file_system);
    m_category_table->setEnabled(!running);
    m_add_category_button->setEnabled(!running);
    m_remove_category_button->setEnabled(!running);
    m_reset_categories_button->setEnabled(!running);

    m_preview_button->setEnabled(!running);
    m_execute_button->setEnabled(!running);
    m_cancel_button->setEnabled(running);

    // Cross-lock: disable other file-management tabs while mutating organizer runs.
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (index != 0) {
            m_tabs->setTabEnabled(index, !running);
        }
    }
}

void OrganizerPanel::logMessage(const QString& message) {
    if (message.trimmed().isEmpty()) {
        return;
    }
    Q_EMIT logOutput(message);
}

bool OrganizerPanel::validateCategoryMapping() const {
    auto mapping = getCategoryMapping();
    if (mapping.isEmpty()) {
        sak::logWarning("No valid category mappings defined in organizer");
        sak::showWarningLogged(
            const_cast<OrganizerPanel*>(this),
            tr("Validation Error"),
            tr("No valid category mappings defined.\n"
               "Add at least one category with extensions, or reset to defaults."));
        return false;
    }

    QSet<QString> seen;
    for (auto it = mapping.cbegin(); it != mapping.cend(); ++it) {
        QString lower = it.key().toLower();
        if (seen.contains(lower)) {
            sak::logWarning("Duplicate category name in organizer: {}", it.key().toStdString());
            sak::showWarningLogged(const_cast<OrganizerPanel*>(this),
                                   tr("Validation Error"),
                                   tr("Duplicate category name: \"%1\".\n"
                                      "Each category must have a unique name.")
                                       .arg(it.key()));
            return false;
        }
        seen.insert(lower);
    }
    return true;
}

void OrganizerPanel::showScrollableResultsDialog(const QString& title, const QString& text) {
    const QString safe_title = title.trimmed().isEmpty() ? tr("Results") : title;
    const QString safe_text = text.isEmpty() ? tr("No results to display.") : text;
    QDialog dialog(this);
    dialog.setWindowTitle(safe_title);
    dialog.setMinimumSize(sak::kDialogWidthLarge, sak::kDialogHeightMedium);
    dialog.resize(sak::kDialogWidthLarge, sak::kDialogHeightLarge);

    auto* layout = new QVBoxLayout(&dialog);

    auto* textEdit = new QTextEdit(&dialog);
    textEdit->setReadOnly(true);
    textEdit->setPlainText(safe_text);
    textEdit->setAccessibleName(QStringLiteral("Scan Results"));
    layout->addWidget(textEdit);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void OrganizerPanel::onSettingsClicked() {
    Q_ASSERT(m_collision_strategy);
    Q_ASSERT(m_preview_mode_checkbox);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("File Organizer Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthSmall);

    auto* layout = new QFormLayout(&dialog);

    auto* collisionCombo = new QComboBox(&dialog);
    collisionCombo->addItems({tr("Rename"), tr("Skip"), tr("Overwrite")});
    collisionCombo->setCurrentIndex(m_collision_strategy->currentIndex());
    layout->addRow(
        InfoButton::createInfoLabel(
            tr("Collision Strategy:"),
            tr("How to handle files when a file with the same name already exists in the "
               "destination folder"),
            &dialog),
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

    addDialogButtonRow(layout, &dialog, tr("OK"), tr("Cancel"));

    if (dialog.exec() == QDialog::Accepted) {
        m_collision_strategy->setCurrentIndex(collisionCombo->currentIndex());
        m_preview_mode_checkbox->setChecked(previewCheck->isChecked());
        m_create_subdirs_checkbox->setChecked(subdirCheck->isChecked());
    }
}

// ============================================================================
// Duplicate Finder -- logic
// ============================================================================

void OrganizerPanel::updateDedupDirectorySummary() {
    Q_ASSERT(m_dedup_directory_list);
    Q_ASSERT(m_dedup_summary_label);
    int count = m_dedup_directory_list->count();
    if (count == 0) {
        m_dedup_summary_label->setText(tr("No directories added"));
        return;
    }

    const auto target = currentDedupTarget();
    if (!target.root_path.isEmpty() && !target.local_file_system) {
        m_dedup_summary_label->setText(
            tr("%1 virtual directories on %2 - %3")
                .arg(count)
                .arg(target.label, FileManagementFileSystemBridge::capabilitySummary(target)));
        return;
    }

    qint64 totalSize = 0;
    int totalFiles = 0;
    for (int i = 0; i < count; ++i) {
        QDir dir(m_dedup_directory_list->item(i)->text());
        if (dir.exists()) {
            QDirIterator it(dir.absolutePath(),
                            QDir::Files,
                            m_dedup_recursive->isChecked() ? QDirIterator::Subdirectories
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
        sizeStr = QString("%1 GB").arg(totalSize / sak::kBytesPerGBf, 0, 'f', kSizeGbPrecision);
    } else if (totalSize >= sak::kBytesPerMB) {
        sizeStr = QString("%1 MB").arg(totalSize / sak::kBytesPerMBf, 0, 'f', 1);
    } else {
        sizeStr = QString("%1 KB").arg(totalSize / sak::kBytesPerKBf, 0, 'f', 0);
    }

    m_dedup_summary_label->setText(tr("%1 directory(ies) \u2022 %2 files (%3) to scan")
                                       .arg(count)
                                       .arg(totalFiles)
                                       .arg(sizeStr));
}

void OrganizerPanel::onDedupAddDirectoryClicked() {
    Q_ASSERT(m_dedup_directory_list);
    const auto target = currentDedupTarget();
    if (!target.root_path.isEmpty() && !target.local_file_system) {
        if (!target.can_duplicate_scan) {
            sak::showWarningLogged(this,
                                   tr("Duplicate Finder Target Blocked"),
                                   target.blockers.join(QStringLiteral("\n")));
            return;
        }
        bool ok = false;
        const QString virtualPath =
            QInputDialog::getText(this,
                                  tr("Add Target Directory"),
                                  tr("Directory path inside %1:").arg(target.label),
                                  QLineEdit::Normal,
                                  QStringLiteral("/"),
                                  &ok)
                .trimmed();
        if (!ok || virtualPath.isEmpty()) {
            return;
        }
        m_dedup_directory_list->addItem(virtualPath);
        updateDedupDirectorySummary();
        logMessage(QString("Added virtual scan directory: %1").arg(virtualPath));
        return;
    }

    QString dir = QFileDialog::getExistingDirectory(this,
                                                    tr("Select Directory to Scan"),
                                                    QString(),
                                                    QFileDialog::ShowDirsOnly |
                                                        QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        // Prevent duplicate directory entries
        for (int i = 0; i < m_dedup_directory_list->count(); ++i) {
            if (QDir(m_dedup_directory_list->item(i)->text()) == QDir(dir)) {
                sak::showInformationLogged(this,
                                           tr("Duplicate Directory"),
                                           tr("This directory is already in the scan list."));
                return;
            }
        }
        m_dedup_directory_list->addItem(dir);
        updateDedupDirectorySummary();
        logMessage(QString("Added scan directory: %1").arg(dir));
    }
}

void OrganizerPanel::onDedupRemoveDirectoryClicked() {
    Q_ASSERT(m_dedup_directory_list);
    auto selected = m_dedup_directory_list->selectedItems();
    if (selected.isEmpty()) {
        sak::showInformationLogged(this,
                                   tr("No Selection"),
                                   tr("Please select a directory to remove."));
        return;
    }

    for (auto* item : selected) {
        delete item;
    }
    updateDedupDirectorySummary();
}

void OrganizerPanel::onDedupClearAllClicked() {
    Q_ASSERT(m_dedup_directory_list);
    Q_ASSERT(m_dedup_results_label);
    if (m_dedup_directory_list->count() == 0) {
        return;
    }

    auto result = sak::showQuestionLogged(this,
                                          tr("Clear All"),
                                          tr("Remove all directories from the scan list?"),
                                          QMessageBox::Yes | QMessageBox::No,
                                          QMessageBox::No);
    if (result == QMessageBox::Yes) {
        m_dedup_directory_list->clear();
        updateDedupDirectorySummary();
        m_dedup_results_label->setVisible(false);
        logMessage(tr("Scan directory list cleared"));
    }
}

void OrganizerPanel::onDedupScanClicked() {
    Q_ASSERT(m_dedup_directory_list);
    Q_ASSERT(m_dedup_min_size);
    if (m_dedup_directory_list->count() == 0) {
        sak::logWarning("Duplicate scan attempted with no directories selected");
        sak::showWarningLogged(this,
                               tr("Validation Error"),
                               tr("Please add at least one directory to scan for duplicates."));
        return;
    }

    m_dedup_worker.reset();

    DuplicateFinderWorker::Config config;
    const auto target = currentDedupTarget();
    if (!target.root_path.isEmpty() && !target.local_file_system) {
        if (!target.can_duplicate_scan) {
            sak::showWarningLogged(this,
                                   tr("Duplicate Finder Target Blocked"),
                                   target.blockers.join(QStringLiteral("\n")));
            return;
        }
        config.use_file_system_target = true;
        config.file_system_target = target;
        for (int i = 0; i < m_dedup_directory_list->count(); ++i) {
            config.virtual_directories.push_back(m_dedup_directory_list->item(i)->text());
        }
    } else {
        for (int i = 0; i < m_dedup_directory_list->count(); ++i) {
            config.scanDirectories.push_back(m_dedup_directory_list->item(i)->text());
        }
    }
    config.minimum_file_size = m_dedup_min_size->value() * sak::kBytesPerKB;
    config.recursive_scan = m_dedup_recursive->isChecked();
    config.parallel_hashing = m_dedup_parallel_hashing->isChecked();
    config.hash_thread_count = m_dedup_thread_count->value();

    m_dedup_worker = std::make_unique<DuplicateFinderWorker>(config, this);

    connectDedupWorkerSignals();

    setDedupRunning(true);
    m_dedup_progress_bar->setValue(0);
    m_dedup_progress_bar->setVisible(true);
    m_dedup_results_label->setVisible(false);
    Q_EMIT statusMessage(tr("Starting duplicate scan..."), 0);
    m_dedup_worker->start();

    logInfo("Duplicate finder scan initiated from organizer panel");
}

void OrganizerPanel::onDedupCancelClicked() {
    if (m_dedup_worker) {
        m_dedup_worker->requestStop();
        logMessage(tr("Duplicate scan cancellation requested..."));
        Q_EMIT statusMessage(tr("Cancelling duplicate scan..."), 0);
    }
}

void OrganizerPanel::onDedupSettingsClicked() {
    Q_ASSERT(m_dedup_min_size);
    Q_ASSERT(m_dedup_recursive);
    Q_ASSERT(m_dedup_parallel_hashing);
    Q_ASSERT(m_dedup_thread_count);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Duplicate Finder Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthSmall);

    auto* layout = new QFormLayout(&dialog);
    const DedupSettingsDefaults defaults{m_dedup_min_size->value(),
                                         m_dedup_recursive->isChecked(),
                                         m_dedup_parallel_hashing->isChecked(),
                                         m_dedup_thread_count->value()};
    const auto widgets = addDedupSettingsRows(layout, &dialog, defaults);
    addDialogButtonRow(layout, &dialog, tr("OK"), tr("Cancel"));

    if (dialog.exec() == QDialog::Accepted) {
        m_dedup_min_size->setValue(widgets.minSizeSpin->value());
        m_dedup_recursive->setChecked(widgets.recursiveCheck->isChecked());
        m_dedup_parallel_hashing->setChecked(widgets.parallelCheck->isChecked());
        m_dedup_thread_count->setValue(widgets.threadSpin->value());
        updateDedupDirectorySummary();
    }
}

void OrganizerPanel::onDedupTargetChanged(int index) {
    Q_UNUSED(index)
    if (m_dedup_directory_list) {
        m_dedup_directory_list->clear();
    }
    updateDedupDirectorySummary();
}

void OrganizerPanel::onDedupWorkerStarted() {
    logMessage(tr("Duplicate file scan started"));
    Q_EMIT statusMessage(tr("Duplicate scan in progress"), 0);
}

void OrganizerPanel::onDedupWorkerFinished() {
    setDedupRunning(false);
    m_dedup_progress_bar->setValue(kProgressMaximum);
    Q_EMIT statusMessage(tr("Duplicate scan complete"), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(kProgressMaximum, kProgressMaximum);
    logMessage(tr("Duplicate scan completed successfully"));
    logInfo("Duplicate finder scan completed successfully");
}

void OrganizerPanel::onDedupWorkerFailed(int errorCode, const QString& errorMessage) {
    Q_ASSERT(m_dedup_progress_bar);
    setDedupRunning(false);
    m_dedup_progress_bar->setVisible(false);
    const QString safe_error = errorMessage.trimmed().isEmpty() ? tr("Unknown error")
                                                                : errorMessage;
    Q_EMIT statusMessage(tr("Duplicate scan failed"), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(0, kProgressMaximum);
    logMessage(QString("Duplicate scan failed: Error %1: %2").arg(errorCode).arg(safe_error));
    sak::showWarningLogged(this,
                           tr("Scan Failed"),
                           QString("Error %1: %2").arg(errorCode).arg(safe_error));
    logError("Duplicate finder scan failed: {}", safe_error.toStdString());
}

void OrganizerPanel::onDedupWorkerCancelled() {
    setDedupRunning(false);
    m_dedup_progress_bar->setVisible(false);
    logMessage(tr("Duplicate scan cancelled by user"));
    Q_EMIT statusMessage(tr("Duplicate scan cancelled"), sak::kTimerStatusMessageMs);
    Q_EMIT progressUpdate(0, kProgressMaximum);
}

void OrganizerPanel::onDedupScanProgress(int current, int total, const QString& path) {
    Q_ASSERT(m_dedup_progress_bar);
    Q_EMIT progressUpdate(current, total);

    if (total > 0) {
        int pct = static_cast<int>(static_cast<double>(current) / total * kProgressMaximum);
        m_dedup_progress_bar->setValue(pct);
    }

    QFileInfo info(path);
    Q_EMIT statusMessage(QString("Scanning: %1").arg(info.fileName()), 0);
}

void OrganizerPanel::onDedupResultsReady(const QString& summary,
                                         int duplicateCount,
                                         qint64 wastedSpace) {
    QString sizeStr;
    if (wastedSpace >= sak::kBytesPerGB) {
        sizeStr = QString("%1 GB").arg(wastedSpace / sak::kBytesPerGBf, 0, 'f', kSizeGbPrecision);
    } else {
        sizeStr = QString("%1 MB").arg(wastedSpace / sak::kBytesPerMBf, 0, 'f', kSizeGbPrecision);
    }

    QString resultsText =
        tr("Found %1 duplicate file(s), %2 wasted space").arg(duplicateCount).arg(sizeStr);

    // Show persistent results label
    if (duplicateCount > 0) {
        m_dedup_results_label->setStyleSheet(
            ui::resultBadgeStyle(ui::kColorBgWarningPanel, ui::kColorWarning));
    } else {
        m_dedup_results_label->setStyleSheet(
            ui::resultBadgeStyle(ui::kColorBgInfoPanel, ui::kColorSuccess));
    }
    m_dedup_results_label->setText(resultsText);
    m_dedup_results_label->setVisible(true);

    Q_EMIT statusMessage(resultsText, sak::kTimerHealthPollMs);
    logMessage(resultsText);

    showScrollableResultsDialog(tr("Duplicate Scan Results"), summary);
}

void OrganizerPanel::setDedupRunning(bool running) {
    Q_ASSERT(m_dedup_directory_list);
    Q_ASSERT(m_dedup_add_button);
    m_dedup_running = running;

    m_dedup_target_combo->setEnabled(!running);
    m_dedup_directory_list->setEnabled(!running);
    m_dedup_add_button->setEnabled(!running);
    m_dedup_remove_button->setEnabled(!running);
    m_dedup_clear_button->setEnabled(!running);
    m_dedup_scan_button->setEnabled(!running);
    m_dedup_cancel_button->setEnabled(running);

    // Cross-lock: disable other file-management tabs while duplicate scan runs.
    for (int index = 0; index < m_tabs->count(); ++index) {
        if (index != 1) {
            m_tabs->setTabEnabled(index, !running);
        }
    }
}

}  // namespace sak
