// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_panel.cpp
/// @brief Main UI panel for Advanced Uninstall — program list, leftover
///        scanner, and cleanup workflow

#include "sak/advanced_uninstall_panel.h"
#include "sak/advanced_uninstall_controller.h"
#include "sak/detachable_log_window.h"
#include "sak/restore_point_manager.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QPixmap>
#include <QProcess>
#include <QRadioButton>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace sak {

namespace {
constexpr int kOriginalIndexRole = Qt::UserRole + 100;

/// @brief Table item that sorts numerically by Qt::UserRole data
class NumericSortItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem& other) const override
    {
        return data(Qt::UserRole).toLongLong()
             < other.data(Qt::UserRole).toLongLong();
    }
};
}  // namespace

// ── Construction ────────────────────────────────────────────────────────────

AdvancedUninstallPanel::AdvancedUninstallPanel(QWidget* parent)
    : QWidget(parent)
    , m_controller(std::make_unique<AdvancedUninstallController>(this))
{
    setupUi();

    // Wire controller signals
    connect(m_controller.get(), &AdvancedUninstallController::enumerationStarted,
            this, &AdvancedUninstallPanel::onEnumerationStarted);
    connect(m_controller.get(), &AdvancedUninstallController::enumerationProgress,
            this, &AdvancedUninstallPanel::onEnumerationProgress);
    connect(m_controller.get(), &AdvancedUninstallController::enumerationFinished,
            this, &AdvancedUninstallPanel::onEnumerationFinished);
    connect(m_controller.get(), &AdvancedUninstallController::enumerationFailed,
            this, &AdvancedUninstallPanel::onEnumerationFailed);

    connect(m_controller.get(), &AdvancedUninstallController::uninstallStarted,
            this, &AdvancedUninstallPanel::onUninstallStarted);
    connect(m_controller.get(), &AdvancedUninstallController::uninstallProgress,
            this, &AdvancedUninstallPanel::onUninstallProgress);
    connect(m_controller.get(), &AdvancedUninstallController::leftoverScanFinished,
            this, &AdvancedUninstallPanel::onLeftoverScanFinished);
    connect(m_controller.get(), &AdvancedUninstallController::uninstallFinished,
            this, &AdvancedUninstallPanel::onUninstallFinished);
    connect(m_controller.get(), &AdvancedUninstallController::uninstallFailed,
            this, &AdvancedUninstallPanel::onUninstallFailed);
    connect(m_controller.get(), &AdvancedUninstallController::uninstallCancelled,
            this, &AdvancedUninstallPanel::onUninstallCancelled);

    connect(m_controller.get(), &AdvancedUninstallController::cleanupStarted,
            this, &AdvancedUninstallPanel::onCleanupStarted);
    connect(m_controller.get(), &AdvancedUninstallController::itemCleaned,
            this, &AdvancedUninstallPanel::onItemCleaned);
    connect(m_controller.get(), &AdvancedUninstallController::cleanupFinished,
            this, &AdvancedUninstallPanel::onCleanupFinished);

    connect(m_controller.get(), &AdvancedUninstallController::statusMessage,
            this, &AdvancedUninstallPanel::statusMessage);
    connect(m_controller.get(), &AdvancedUninstallController::progressUpdate,
            this, &AdvancedUninstallPanel::progressUpdate);
    connect(m_controller.get(), &AdvancedUninstallController::batchFinished,
            this, [this](int succeeded, int failed) {
                setOperationRunning(false);
                logMessage(QString("Batch complete: %1 succeeded, %2 failed")
                               .arg(succeeded).arg(failed));
            });

    logMessage("Advanced Uninstall panel initialized.");
}

AdvancedUninstallPanel::~AdvancedUninstallPanel()
{
    logMessage("Advanced Uninstall panel destroyed.");
}

// ── UI Setup ────────────────────────────────────────────────────────────────

void AdvancedUninstallPanel::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* contentWidget = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium,
        ui::kMarginMedium, ui::kMarginMedium);
    mainLayout->setSpacing(ui::kSpacingDefault);

    rootLayout->addWidget(contentWidget);

    // Panel header
    createPanelHeader(contentWidget, tr("Advanced Uninstall"),
        tr("Deep application removal with registry cleanup, leftover scanning, "
           "and batch uninstall support"),
        mainLayout);

    createToolbar(mainLayout);
    createProgramTable(mainLayout);
    createLeftoverSection(mainLayout);
    createStatusBar(mainLayout);
}

void AdvancedUninstallPanel::createToolbar(QVBoxLayout* layout)
{
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(ui::kSpacingSmall);

    // Search
    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText(tr("Search programs..."));
    m_search_edit->setClearButtonEnabled(true);
    m_search_edit->setToolTip(
        tr("Filter programs by name, publisher, or version"));
    setAccessible(m_search_edit, tr("Search programs"),
        tr("Filter the program list by name, publisher, or version"));
    toolbar->addWidget(m_search_edit, 1);

    // View filter
    m_view_filter_combo = new QComboBox(this);
    m_view_filter_combo->addItem(tr("All Programs"), static_cast<int>(ViewFilter::All));
    m_view_filter_combo->addItem(tr("Win32 Only"), static_cast<int>(ViewFilter::Win32Only));
    m_view_filter_combo->addItem(tr("UWP Only"), static_cast<int>(ViewFilter::UwpOnly));
    m_view_filter_combo->addItem(tr("Bloatware"), static_cast<int>(ViewFilter::BloatwareOnly));
    m_view_filter_combo->addItem(tr("Orphaned"), static_cast<int>(ViewFilter::OrphanedOnly));
    m_view_filter_combo->setToolTip(tr("Filter programs by category"));
    setAccessible(m_view_filter_combo, tr("View filter"),
        tr("Show all programs, Win32 only, UWP only, bloatware, or orphaned"));
    toolbar->addWidget(m_view_filter_combo);

    toolbar->addSpacing(ui::kSpacingMedium);

    // Refresh
    m_refresh_button = new QPushButton(tr("Refresh"), this);
    m_refresh_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_refresh_button->setToolTip(tr("Refresh the list of installed programs"));
    setAccessible(m_refresh_button, tr("Refresh programs"),
        tr("Re-scan for installed programs"));
    toolbar->addWidget(m_refresh_button);

    // Standard Uninstall
    m_uninstall_button = new QPushButton(tr("Uninstall"), this);
    m_uninstall_button->setStyleSheet(ui::kDangerButtonStyle);
    m_uninstall_button->setToolTip(
        tr("Uninstall selected program and scan for leftovers"));
    m_uninstall_button->setEnabled(false);
    setAccessible(m_uninstall_button, tr("Uninstall program"),
        tr("Run the native uninstaller then scan for leftover files and registry entries"));
    toolbar->addWidget(m_uninstall_button);

    // Forced Uninstall
    m_forced_uninstall_button = new QPushButton(tr("Forced"), this);
    m_forced_uninstall_button->setStyleSheet(ui::kDangerButtonStyle);
    m_forced_uninstall_button->setToolTip(
        tr("Force uninstall — skip native uninstaller, scan and remove all traces"));
    m_forced_uninstall_button->setEnabled(false);
    setAccessible(m_forced_uninstall_button, tr("Forced uninstall"),
        tr("Skip native uninstaller and remove all program traces directly"));
    toolbar->addWidget(m_forced_uninstall_button);

    // Batch Uninstall
    m_batch_button = new QPushButton(tr("Batch"), this);
    m_batch_button->setToolTip(
        tr("Queue multiple programs for sequential uninstall"));
    m_batch_button->setEnabled(false);
    setAccessible(m_batch_button, tr("Batch uninstall"),
        tr("Add checked programs to the uninstall queue and process them sequentially"));
    toolbar->addWidget(m_batch_button);

    layout->addLayout(toolbar);

    // Connect toolbar signals
    connect(m_search_edit, &QLineEdit::textChanged,
            this, &AdvancedUninstallPanel::onSearchTextChanged);
    connect(m_view_filter_combo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AdvancedUninstallPanel::onViewFilterChanged);
    connect(m_refresh_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onRefreshClicked);
    connect(m_uninstall_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onUninstallClicked);
    connect(m_forced_uninstall_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onForcedUninstallClicked);
    connect(m_batch_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onBatchUninstallClicked);
}

void AdvancedUninstallPanel::createProgramTable(QVBoxLayout* layout)
{
    auto* group = new QGroupBox(tr("Installed Programs"), this);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(ui::kSpacingSmall);

    m_program_table = new QTableWidget(this);
    m_program_table->setColumnCount(kProgramColumnCount);
    m_program_table->setHorizontalHeaderLabels({
        QString(),          // Check column (no header text)
        QString(),          // Icon column
        tr("Name"),
        tr("Publisher"),
        tr("Version"),
        tr("Size"),
        tr("Install Date")
    });

    // Configure table
    m_program_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_program_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_program_table->setAlternatingRowColors(true);
    m_program_table->setSortingEnabled(true);
    m_program_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_program_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_program_table->verticalHeader()->setVisible(false);

    // Column widths
    auto* header = m_program_table->horizontalHeader();
    header->setSectionResizeMode(kColCheck, QHeaderView::Fixed);
    header->resizeSection(kColCheck, 30);
    header->setSectionResizeMode(kColIcon, QHeaderView::Fixed);
    header->resizeSection(kColIcon, 28);
    header->setSectionResizeMode(kColName, QHeaderView::Stretch);
    header->setSectionResizeMode(kColPublisher, QHeaderView::Interactive);
    header->resizeSection(kColPublisher, 160);
    header->setSectionResizeMode(kColVersion, QHeaderView::Interactive);
    header->resizeSection(kColVersion, 100);
    header->setSectionResizeMode(kColSize, QHeaderView::Interactive);
    header->resizeSection(kColSize, 80);
    header->setSectionResizeMode(kColDate, QHeaderView::Interactive);
    header->resizeSection(kColDate, 100);

    setAccessible(m_program_table, tr("Program list"),
        tr("List of installed programs with details"));

    groupLayout->addWidget(m_program_table);

    // Count labels
    auto* countRow = new QHBoxLayout();
    m_program_count_label = new QLabel(tr("0 programs"), this);
    m_program_count_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    countRow->addWidget(m_program_count_label);
    countRow->addStretch();
    m_total_size_label = new QLabel(this);
    m_total_size_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    countRow->addWidget(m_total_size_label);
    groupLayout->addLayout(countRow);

    layout->addWidget(group, 3);  // Give program table more stretch

    // Connections
    connect(m_program_table, &QTableWidget::itemSelectionChanged,
            this, &AdvancedUninstallPanel::onProgramSelectionChanged);
    connect(m_program_table, &QTableWidget::cellDoubleClicked,
            this, &AdvancedUninstallPanel::onProgramDoubleClicked);
    connect(m_program_table, &QTableWidget::customContextMenuRequested,
            this, &AdvancedUninstallPanel::onProgramContextMenu);
}

void AdvancedUninstallPanel::createLeftoverSection(QVBoxLayout* layout)
{
    m_leftover_section = new QWidget(this);
    auto* sectionLayout = new QVBoxLayout(m_leftover_section);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(ui::kSpacingSmall);

    // Header with count
    auto* headerRow = new QHBoxLayout();
    m_leftover_header_label = new QLabel(tr("Leftover Items"), this);
    QFont headerFont = m_leftover_header_label->font();
    headerFont.setBold(true);
    headerFont.setPointSize(ui::kFontSizeStatus);
    m_leftover_header_label->setFont(headerFont);
    headerRow->addWidget(m_leftover_header_label);

    headerRow->addStretch();

    m_leftover_count_label = new QLabel(this);
    m_leftover_count_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    headerRow->addWidget(m_leftover_count_label);

    sectionLayout->addLayout(headerRow);

    // Leftover table
    m_leftover_table = new QTableWidget(this);
    m_leftover_table->setColumnCount(kLeftoverColumnCount);
    m_leftover_table->setHorizontalHeaderLabels({
        QString(),          // Check
        tr("Risk"),
        tr("Type"),
        tr("Path"),
        tr("Size")
    });

    m_leftover_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_leftover_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_leftover_table->setAlternatingRowColors(true);
    m_leftover_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_leftover_table->setSortingEnabled(true);
    m_leftover_table->verticalHeader()->setVisible(false);

    auto* lheader = m_leftover_table->horizontalHeader();
    lheader->setSectionResizeMode(kLeftoverColCheck, QHeaderView::Fixed);
    lheader->resizeSection(kLeftoverColCheck, 30);
    lheader->setSectionResizeMode(kLeftoverColRisk, QHeaderView::Interactive);
    lheader->resizeSection(kLeftoverColRisk, 70);
    lheader->setSectionResizeMode(kLeftoverColType, QHeaderView::Interactive);
    lheader->resizeSection(kLeftoverColType, 100);
    lheader->setSectionResizeMode(kLeftoverColPath, QHeaderView::Stretch);
    lheader->setSectionResizeMode(kLeftoverColSize, QHeaderView::Interactive);
    lheader->resizeSection(kLeftoverColSize, 80);

    setAccessible(m_leftover_table, tr("Leftover items"),
        tr("Files, folders, registry entries, and system objects left behind after uninstall"));

    sectionLayout->addWidget(m_leftover_table);

    // Action buttons
    auto* buttonRow = new QHBoxLayout();
    m_select_safe_button = new QPushButton(tr("Select Safe"), this);
    m_select_safe_button->setStyleSheet(ui::kSuccessButtonStyle);
    m_select_safe_button->setToolTip(
        tr("Select all items classified as Safe for removal"));
    setAccessible(m_select_safe_button, tr("Select safe items"));
    buttonRow->addWidget(m_select_safe_button);

    m_deselect_all_button = new QPushButton(tr("Deselect All"), this);
    m_deselect_all_button->setToolTip(tr("Deselect all leftover items"));
    setAccessible(m_deselect_all_button, tr("Deselect all"));
    buttonRow->addWidget(m_deselect_all_button);

    buttonRow->addStretch();

    m_delete_selected_button = new QPushButton(tr("Delete Selected"), this);
    m_delete_selected_button->setStyleSheet(ui::kDangerButtonStyle);
    m_delete_selected_button->setToolTip(
        tr("Permanently delete all checked leftover items"));
    m_delete_selected_button->setEnabled(false);
    setAccessible(m_delete_selected_button, tr("Delete selected leftovers"),
        tr("Permanently remove checked files, folders, and registry entries"));
    buttonRow->addWidget(m_delete_selected_button);

    sectionLayout->addLayout(buttonRow);
    layout->addWidget(m_leftover_section, 2);

    // Initially hidden
    m_leftover_section->setVisible(false);

    // Connections
    connect(m_select_safe_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onSelectAllSafe);
    connect(m_deselect_all_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onDeselectAll);
    connect(m_delete_selected_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onDeleteSelectedLeftovers);
    connect(m_leftover_table, &QTableWidget::itemChanged,
            this, &AdvancedUninstallPanel::onLeftoverSelectionChanged);
}

void AdvancedUninstallPanel::createStatusBar(QVBoxLayout* layout)
{
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 4, 0, 0);

    // Log toggle on the left
    m_log_toggle = new LogToggleSwitch(tr("Log"), this);
    statusRow->addWidget(m_log_toggle);

    statusRow->addSpacing(ui::kSpacingMedium);

    m_status_label = new QLabel(tr("Ready"), this);
    m_status_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    statusRow->addWidget(m_status_label);
    statusRow->addStretch();

    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setRange(0, 100);
    m_progress_bar->setValue(0);
    m_progress_bar->setFixedWidth(200);
    m_progress_bar->setVisible(false);
    statusRow->addWidget(m_progress_bar);

    layout->addLayout(statusRow);
}

// ── Toolbar Slots ───────────────────────────────────────────────────────────

void AdvancedUninstallPanel::onRefreshClicked()
{
    logMessage("Refreshing installed programs...");
    m_controller->refreshPrograms();
}

void AdvancedUninstallPanel::onUninstallClicked()
{
    auto program = selectedProgram();
    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage(tr("No program selected."), 3000);
        return;
    }
    showUninstallConfirmation(program);
}

void AdvancedUninstallPanel::onForcedUninstallClicked()
{
    auto program = selectedProgram();
    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage(tr("No program selected."), 3000);
        return;
    }
    showForcedUninstallDialog(program);
}

void AdvancedUninstallPanel::onBatchUninstallClicked()
{
    // Collect checked programs
    auto programs = selectedPrograms();
    if (programs.isEmpty()) {
        Q_EMIT statusMessage(tr("No programs checked for batch uninstall."), 3000);
        return;
    }

    // Add all checked to queue
    m_controller->clearQueue();
    for (const auto& p : programs) {
        m_controller->addToQueue(p, m_controller->defaultScanLevel(),
                                 m_controller->autoCleanSafe());
    }

    showBatchUninstallDialog();
}

void AdvancedUninstallPanel::onSearchTextChanged(const QString& text)
{
    m_searchFilter = text;
    applyFilter();
}

void AdvancedUninstallPanel::onViewFilterChanged(int index)
{
    m_viewFilter = static_cast<ViewFilter>(
        m_view_filter_combo->itemData(index).toInt());
    applyFilter();
}

// ── Controller Signal Handlers ──────────────────────────────────────────────

void AdvancedUninstallPanel::onEnumerationStarted()
{
    setOperationRunning(true);
    m_status_label->setText(tr("Scanning installed programs..."));
    m_progress_bar->setVisible(true);
    m_progress_bar->setRange(0, 0);  // Indeterminate
    logMessage("Program enumeration started.");
}

void AdvancedUninstallPanel::onEnumerationProgress(int current, int total)
{
    m_progress_bar->setRange(0, total);
    m_progress_bar->setValue(current);
}

void AdvancedUninstallPanel::onEnumerationFinished(
    QVector<ProgramInfo> programs)
{
    m_allPrograms = programs;
    applyFilter();
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    m_status_label->setText(tr("Ready"));
    logMessage(QString("Found %1 installed programs.").arg(programs.size()));
}

void AdvancedUninstallPanel::onEnumerationFailed(const QString& error)
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    m_status_label->setText(tr("Enumeration failed"));
    logMessage(QString("ERROR: Enumeration failed: %1").arg(error));
    QMessageBox::warning(this, tr("Enumeration Error"),
        tr("Failed to enumerate installed programs:\n%1").arg(error));
}

void AdvancedUninstallPanel::onUninstallStarted(const QString& programName)
{
    setOperationRunning(true);
    m_status_label->setText(tr("Uninstalling: %1").arg(programName));
    m_progress_bar->setVisible(true);
    m_progress_bar->setRange(0, 100);
    m_progress_bar->setValue(0);
    logMessage(QString("Uninstall started: %1").arg(programName));
}

void AdvancedUninstallPanel::onUninstallProgress(int percent, const QString& phase)
{
    m_progress_bar->setValue(percent);
    m_status_label->setText(phase);
}

void AdvancedUninstallPanel::onLeftoverScanFinished(
    QVector<LeftoverItem> leftovers)
{
    m_currentLeftovers = leftovers;
    populateLeftoverTable(leftovers);
    m_leftover_section->setVisible(!leftovers.isEmpty());

    logMessage(QString("Leftover scan complete: %1 items found.")
                   .arg(leftovers.size()));
}

void AdvancedUninstallPanel::onUninstallFinished(UninstallReport report)
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);

    QString resultStr;
    switch (report.uninstallResult) {
    case UninstallReport::UninstallResult::Success:
        resultStr = "Success";
        break;
    case UninstallReport::UninstallResult::Failed:
        resultStr = "Failed";
        break;
    case UninstallReport::UninstallResult::Cancelled:
        resultStr = "Cancelled";
        break;
    case UninstallReport::UninstallResult::Skipped:
        resultStr = "Skipped";
        break;
    }

    m_status_label->setText(
        tr("Uninstall %1: %2").arg(resultStr, report.programName));

    logMessage(QString("Uninstall complete: %1 — %2 (exit code: %3)")
                   .arg(report.programName, resultStr)
                   .arg(report.nativeExitCode));
    const int totalCleaned = report.filesDeleted + report.foldersDeleted
                            + report.registryKeysDeleted + report.registryValuesDeleted
                            + report.servicesRemoved + report.tasksRemoved
                            + report.firewallRulesRemoved + report.startupEntriesRemoved;
    logMessage(QString("  Leftovers found: %1, Cleaned: %2, Failed: %3")
                   .arg(report.foundLeftovers.size())
                   .arg(totalCleaned)
                   .arg(report.failedDeletions));

    // Refresh program list to reflect removal
    m_controller->refreshPrograms();
}

void AdvancedUninstallPanel::onUninstallFailed(const QString& error)
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    m_status_label->setText(tr("Uninstall failed"));
    logMessage(QString("ERROR: Uninstall failed: %1").arg(error));
    QMessageBox::warning(this, tr("Uninstall Error"),
        tr("The uninstall operation failed:\n%1").arg(error));
}

void AdvancedUninstallPanel::onUninstallCancelled()
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);
    m_status_label->setText(tr("Uninstall cancelled"));
    logMessage("Uninstall operation cancelled by user.");
}

void AdvancedUninstallPanel::onCleanupStarted(int totalItems)
{
    setOperationRunning(true);
    m_progress_bar->setVisible(true);
    m_progress_bar->setRange(0, totalItems);
    m_progress_bar->setValue(0);
    m_status_label->setText(tr("Cleaning %1 items...").arg(totalItems));
    logMessage(QString("Cleanup started: %1 items to remove.").arg(totalItems));
}

void AdvancedUninstallPanel::onItemCleaned(const QString& path, bool success)
{
    m_progress_bar->setValue(m_progress_bar->value() + 1);
    if (success) {
        logMessage(QString("  Removed: %1").arg(path));
    } else {
        logMessage(QString("  FAILED to remove: %1").arg(path));
    }
}

void AdvancedUninstallPanel::onCleanupFinished(
    int succeeded, int failed, qint64 bytesRecovered)
{
    setOperationRunning(false);
    m_progress_bar->setVisible(false);

    QString msg = tr("Cleanup complete: %1 removed").arg(succeeded);
    if (failed > 0) {
        msg += tr(", %1 failed").arg(failed);
    }
    if (bytesRecovered > 0) {
        msg += tr(" (%1 recovered)").arg(formatSize(bytesRecovered));
    }
    m_status_label->setText(msg);
    logMessage(msg);

    // Clear leftover section after cleanup
    clearLeftoverTable();
    m_leftover_section->setVisible(false);

    // Refresh program list
    m_controller->refreshPrograms();
}

// ── Program Table Slots ─────────────────────────────────────────────────────

void AdvancedUninstallPanel::onProgramSelectionChanged()
{
    bool hasSelection = m_program_table->currentRow() >= 0;
    m_uninstall_button->setEnabled(hasSelection);
    m_forced_uninstall_button->setEnabled(hasSelection);
}

void AdvancedUninstallPanel::onProgramDoubleClicked(int row, int /*column*/)
{
    auto* nameItem = m_program_table->item(row, kColName);
    if (!nameItem) return;
    int idx = nameItem->data(kOriginalIndexRole).toInt();
    if (idx >= 0 && idx < m_filteredPrograms.size()) {
        showProgramProperties(m_filteredPrograms[idx]);
    }
}

void AdvancedUninstallPanel::onProgramContextMenu(const QPoint& pos)
{
    auto* item = m_program_table->itemAt(pos);
    if (!item) return;

    int row = item->row();
    auto* nameItem = m_program_table->item(row, kColName);
    if (!nameItem) return;
    int idx = nameItem->data(kOriginalIndexRole).toInt();
    if (idx < 0 || idx >= m_filteredPrograms.size()) return;

    // Select this row so context actions work correctly
    m_program_table->setCurrentCell(row, 0);

    const auto& program = m_filteredPrograms[idx];

    QMenu menu(this);
    menu.addAction(tr("Uninstall"), this,
                   &AdvancedUninstallPanel::contextUninstall);
    menu.addAction(tr("Forced Uninstall"), this,
                   &AdvancedUninstallPanel::contextForcedUninstall);
    menu.addSeparator();
    menu.addAction(tr("Add to Batch Queue"), this,
                   &AdvancedUninstallPanel::contextAddToQueue);
    menu.addSeparator();

    auto* openLocAction = menu.addAction(tr("Open Install Location"), this,
        &AdvancedUninstallPanel::contextOpenInstallLocation);
    openLocAction->setEnabled(!program.installLocation.isEmpty());

    menu.addAction(tr("Copy Program Name"), this,
                   &AdvancedUninstallPanel::contextCopyProgramName);

    auto* copyUninstallAction = menu.addAction(tr("Copy Uninstall Command"), this,
        &AdvancedUninstallPanel::contextCopyUninstallCommand);
    copyUninstallAction->setEnabled(!program.uninstallString.isEmpty());

    menu.addSeparator();
    menu.addAction(tr("Remove Registry Entry"), this,
                   &AdvancedUninstallPanel::contextRemoveRegistryEntry);
    menu.addSeparator();
    menu.addAction(tr("Properties..."), this,
                   &AdvancedUninstallPanel::contextShowProperties);

    menu.exec(m_program_table->viewport()->mapToGlobal(pos));
}

// ── Leftover Table Slots ────────────────────────────────────────────────────

void AdvancedUninstallPanel::onLeftoverSelectionChanged()
{
    // Update delete button state based on checked items
    bool anyChecked = false;
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* checkItem = m_leftover_table->item(row, kLeftoverColCheck);
        if (checkItem && checkItem->checkState() == Qt::Checked) {
            anyChecked = true;
            break;
        }
    }
    m_delete_selected_button->setEnabled(anyChecked);
    updateStatusCounts();
}

void AdvancedUninstallPanel::onSelectAllSafe()
{
    m_leftover_table->blockSignals(true);
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* typeItem = m_leftover_table->item(row, kLeftoverColType);
        if (!typeItem) continue;
        int idx = typeItem->data(kOriginalIndexRole).toInt();
        if (idx >= 0 && idx < m_currentLeftovers.size()
            && m_currentLeftovers[idx].risk == LeftoverItem::RiskLevel::Safe) {
            auto* checkItem = m_leftover_table->item(row, kLeftoverColCheck);
            if (checkItem) {
                checkItem->setCheckState(Qt::Checked);
                m_currentLeftovers[idx].selected = true;
            }
        }
    }
    m_leftover_table->blockSignals(false);
    onLeftoverSelectionChanged();
    logMessage("Selected all safe leftover items.");
}

void AdvancedUninstallPanel::onDeselectAll()
{
    m_leftover_table->blockSignals(true);
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* checkItem = m_leftover_table->item(row, kLeftoverColCheck);
        if (checkItem) {
            checkItem->setCheckState(Qt::Unchecked);
            auto* typeItem = m_leftover_table->item(row, kLeftoverColType);
            if (typeItem) {
                int idx = typeItem->data(kOriginalIndexRole).toInt();
                if (idx >= 0 && idx < m_currentLeftovers.size()) {
                    m_currentLeftovers[idx].selected = false;
                }
            }
        }
    }
    m_leftover_table->blockSignals(false);
    onLeftoverSelectionChanged();
}

void AdvancedUninstallPanel::onDeleteSelectedLeftovers()
{
    auto leftovers = selectedLeftovers();
    if (leftovers.isEmpty()) return;

    // Count items by risk
    int safe = 0, review = 0, risky = 0;
    for (const auto& item : leftovers) {
        switch (item.risk) {
        case LeftoverItem::RiskLevel::Safe:   ++safe; break;
        case LeftoverItem::RiskLevel::Review: ++review; break;
        case LeftoverItem::RiskLevel::Risky:  ++risky; break;
        }
    }

    QString msg = tr("Delete %1 selected leftover items?\n\n"
                     "Safe: %2  |  Review: %3  |  Risky: %4\n\n"
                     "This action cannot be undone.")
                      .arg(leftovers.size()).arg(safe).arg(review).arg(risky);

    if (risky > 0) {
        msg += tr("\n\nWARNING: %1 item(s) are classified as RISKY. "
                  "Removing them may affect system stability.").arg(risky);
    }

    auto result = QMessageBox::warning(this, tr("Confirm Deletion"), msg,
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_controller->cleanLeftovers(leftovers);
    }
}

// ── Context Menu Actions ────────────────────────────────────────────────────

void AdvancedUninstallPanel::contextUninstall()
{
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        showUninstallConfirmation(program);
    }
}

void AdvancedUninstallPanel::contextForcedUninstall()
{
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        showForcedUninstallDialog(program);
    }
}

void AdvancedUninstallPanel::contextAddToQueue()
{
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        m_controller->addToQueue(program, m_controller->defaultScanLevel(),
                                 m_controller->autoCleanSafe());
        logMessage(QString("Added to batch queue: %1").arg(program.displayName));
        Q_EMIT statusMessage(
            tr("Added %1 to batch queue.").arg(program.displayName), 3000);
    }
}

void AdvancedUninstallPanel::contextOpenInstallLocation()
{
    auto program = selectedProgram();
    if (!program.installLocation.isEmpty()) {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(program.installLocation));
    }
}

void AdvancedUninstallPanel::contextCopyProgramName()
{
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        QApplication::clipboard()->setText(program.displayName);
        Q_EMIT statusMessage(tr("Program name copied to clipboard."), 2000);
    }
}

void AdvancedUninstallPanel::contextCopyUninstallCommand()
{
    auto program = selectedProgram();
    if (!program.uninstallString.isEmpty()) {
        QApplication::clipboard()->setText(program.uninstallString);
        Q_EMIT statusMessage(tr("Uninstall command copied to clipboard."), 2000);
    }
}

void AdvancedUninstallPanel::contextShowProperties()
{
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        showProgramProperties(program);
    }
}

void AdvancedUninstallPanel::contextRemoveRegistryEntry()
{
    auto program = selectedProgram();
    if (program.displayName.isEmpty()) return;

    auto result = QMessageBox::warning(this, tr("Remove Registry Entry"),
        tr("Remove the registry entry for '%1'?\n\n"
           "This will only remove the program's registration in Windows. "
           "Program files will NOT be deleted.\n\n"
           "This is useful for orphaned entries where the program "
           "is already uninstalled.")
            .arg(program.displayName),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_controller->removeRegistryEntry(program);
    }
}

// ── Table Population ────────────────────────────────────────────────────────

void AdvancedUninstallPanel::populateProgramTable(
    const QVector<ProgramInfo>& programs)
{
    m_program_table->setSortingEnabled(false);
    m_program_table->setRowCount(0);
    m_program_table->setRowCount(programs.size());

    for (int row = 0; row < programs.size(); ++row) {
        const auto& prog = programs[row];

        // Check column
        auto* checkItem = new QTableWidgetItem();
        checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        checkItem->setCheckState(Qt::Unchecked);
        m_program_table->setItem(row, kColCheck, checkItem);

        // Icon column
        auto* iconItem = new QTableWidgetItem();
        if (!prog.cachedImage.isNull()) {
            iconItem->setIcon(QIcon(QPixmap::fromImage(prog.cachedImage)));
        }
        iconItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColIcon, iconItem);

        // Name
        auto* nameItem = new QTableWidgetItem(prog.displayName);
        nameItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        nameItem->setData(kOriginalIndexRole, row);  // Store original index for sort-safe lookup
        // Color code bloatware and orphaned
        if (prog.isBloatware) {
            nameItem->setForeground(QColor(ui::kColorWarning));
            nameItem->setToolTip(tr("Potential bloatware"));
        }
        if (prog.isOrphaned) {
            nameItem->setForeground(QColor(ui::kColorError));
            nameItem->setToolTip(tr("Orphaned — program files not found"));
        }
        m_program_table->setItem(row, kColName, nameItem);

        // Publisher
        auto* pubItem = new QTableWidgetItem(prog.publisher);
        pubItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColPublisher, pubItem);

        // Version
        auto* verItem = new QTableWidgetItem(prog.displayVersion);
        verItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColVersion, verItem);

        // Size (uses NumericSortItem for correct numeric sorting)
        auto* sizeItem = new NumericSortItem(formatSize(prog.estimatedSizeKB * 1024));
        sizeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        sizeItem->setData(Qt::UserRole, prog.estimatedSizeKB * 1024);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_program_table->setItem(row, kColSize, sizeItem);

        // Install date
        auto* dateItem = new QTableWidgetItem(prog.installDate);
        dateItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColDate, dateItem);
    }

    m_program_table->setSortingEnabled(true);
    m_program_table->sortByColumn(kColName, Qt::AscendingOrder);

    updateStatusCounts();
}

void AdvancedUninstallPanel::populateLeftoverTable(
    const QVector<LeftoverItem>& leftovers)
{
    m_leftover_table->setSortingEnabled(false);
    m_leftover_table->setRowCount(0);
    m_leftover_table->setRowCount(leftovers.size());

    for (int row = 0; row < leftovers.size(); ++row) {
        const auto& item = leftovers[row];

        // Check column
        auto* checkItem = new QTableWidgetItem();
        checkItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        checkItem->setCheckState(item.selected ? Qt::Checked : Qt::Unchecked);
        m_leftover_table->setItem(row, kLeftoverColCheck, checkItem);

        // Risk level
        QString riskText;
        QColor riskColor;
        switch (item.risk) {
        case LeftoverItem::RiskLevel::Safe:
            riskText = tr("Safe");
            riskColor = QColor(ui::kColorSuccess);
            break;
        case LeftoverItem::RiskLevel::Review:
            riskText = tr("Review");
            riskColor = QColor(ui::kColorWarning);
            break;
        case LeftoverItem::RiskLevel::Risky:
            riskText = tr("Risky");
            riskColor = QColor(ui::kColorError);
            break;
        }
        auto* riskItem = new QTableWidgetItem(riskText);
        riskItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        riskItem->setForeground(riskColor);
        QFont riskFont = riskItem->font();
        riskFont.setBold(true);
        riskItem->setFont(riskFont);
        m_leftover_table->setItem(row, kLeftoverColRisk, riskItem);

        // Type
        QString typeText;
        switch (item.type) {
        case LeftoverItem::Type::File:          typeText = tr("File"); break;
        case LeftoverItem::Type::Folder:        typeText = tr("Folder"); break;
        case LeftoverItem::Type::RegistryKey:   typeText = tr("Registry Key"); break;
        case LeftoverItem::Type::RegistryValue: typeText = tr("Registry Value"); break;
        case LeftoverItem::Type::Service:       typeText = tr("Service"); break;
        case LeftoverItem::Type::ScheduledTask: typeText = tr("Scheduled Task"); break;
        case LeftoverItem::Type::FirewallRule:  typeText = tr("Firewall Rule"); break;
        case LeftoverItem::Type::StartupEntry:  typeText = tr("Startup Entry"); break;
        case LeftoverItem::Type::ShellExtension:typeText = tr("Shell Extension"); break;
        }
        auto* typeItem = new QTableWidgetItem(typeText);
        typeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        typeItem->setData(kOriginalIndexRole, row);  // Store original index for sort-safe lookup
        m_leftover_table->setItem(row, kLeftoverColType, typeItem);

        // Path
        auto* pathItem = new QTableWidgetItem(item.path);
        pathItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        pathItem->setToolTip(item.path);
        m_leftover_table->setItem(row, kLeftoverColPath, pathItem);

        // Size (uses NumericSortItem for correct numeric sorting)
        auto* sizeItem = new NumericSortItem(formatSize(item.sizeBytes));
        sizeItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        sizeItem->setData(Qt::UserRole, item.sizeBytes);
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_leftover_table->setItem(row, kLeftoverColSize, sizeItem);
    }

    m_leftover_table->setSortingEnabled(true);

    // Update count label
    int safeCount = 0;
    for (const auto& item : leftovers) {
        if (item.risk == LeftoverItem::RiskLevel::Safe) ++safeCount;
    }
    m_leftover_count_label->setText(
        tr("%1 items (%2 safe)").arg(leftovers.size()).arg(safeCount));
}

void AdvancedUninstallPanel::clearLeftoverTable()
{
    m_leftover_table->setRowCount(0);
    m_currentLeftovers.clear();
    m_leftover_count_label->clear();
}

// ── Filtering ───────────────────────────────────────────────────────────────

void AdvancedUninstallPanel::applyFilter()
{
    m_filteredPrograms.clear();

    for (const auto& prog : m_allPrograms) {
        if (matchesFilter(prog)) {
            m_filteredPrograms.append(prog);
        }
    }

    populateProgramTable(m_filteredPrograms);

    // Update batch button
    m_batch_button->setEnabled(!m_filteredPrograms.isEmpty());
}

bool AdvancedUninstallPanel::matchesFilter(const ProgramInfo& program) const
{
    // View filter
    switch (m_viewFilter) {
    case ViewFilter::All:
        break;
    case ViewFilter::Win32Only:
        if (program.source == ProgramInfo::Source::UWP
            || program.source == ProgramInfo::Source::Provisioned) {
            return false;
        }
        break;
    case ViewFilter::UwpOnly:
        if (program.source != ProgramInfo::Source::UWP
            && program.source != ProgramInfo::Source::Provisioned) {
            return false;
        }
        break;
    case ViewFilter::BloatwareOnly:
        if (!program.isBloatware) return false;
        break;
    case ViewFilter::OrphanedOnly:
        if (!program.isOrphaned) return false;
        break;
    }

    // System components filter
    if (program.isSystemComponent && !m_controller->showSystemComponents()) {
        return false;
    }

    // Text search
    if (!m_searchFilter.isEmpty()) {
        bool matches =
            program.displayName.contains(m_searchFilter, Qt::CaseInsensitive)
            || program.publisher.contains(m_searchFilter, Qt::CaseInsensitive)
            || program.displayVersion.contains(m_searchFilter, Qt::CaseInsensitive);
        if (!matches) return false;
    }

    return true;
}

// ── Utility ─────────────────────────────────────────────────────────────────

QString AdvancedUninstallPanel::formatSize(qint64 bytes) const
{
    if (bytes <= 0) return QString();
    if (bytes < 1024) return tr("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return tr("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return tr("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return tr("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

void AdvancedUninstallPanel::logMessage(const QString& message)
{
    Q_EMIT logOutput(message);
}

void AdvancedUninstallPanel::setOperationRunning(bool running)
{
    m_refresh_button->setEnabled(!running);
    m_batch_button->setEnabled(!running);
    m_search_edit->setEnabled(!running);
    m_view_filter_combo->setEnabled(!running);

    if (running) {
        m_uninstall_button->setEnabled(false);
        m_forced_uninstall_button->setEnabled(false);
    } else {
        // Re-sync button states based on actual selection
        onProgramSelectionChanged();
    }
}

ProgramInfo AdvancedUninstallPanel::selectedProgram() const
{
    int row = m_program_table->currentRow();
    if (row < 0) return {};
    auto* nameItem = m_program_table->item(row, kColName);
    if (!nameItem) return {};
    int idx = nameItem->data(kOriginalIndexRole).toInt();
    if (idx >= 0 && idx < m_filteredPrograms.size()) {
        return m_filteredPrograms[idx];
    }
    return {};
}

QVector<ProgramInfo> AdvancedUninstallPanel::selectedPrograms() const
{
    QVector<ProgramInfo> result;
    for (int row = 0; row < m_program_table->rowCount(); ++row) {
        auto* checkItem = m_program_table->item(row, kColCheck);
        if (checkItem && checkItem->checkState() == Qt::Checked) {
            auto* nameItem = m_program_table->item(row, kColName);
            if (!nameItem) continue;
            int idx = nameItem->data(kOriginalIndexRole).toInt();
            if (idx >= 0 && idx < m_filteredPrograms.size()) {
                result.append(m_filteredPrograms[idx]);
            }
        }
    }
    return result;
}

QVector<LeftoverItem> AdvancedUninstallPanel::selectedLeftovers() const
{
    QVector<LeftoverItem> result;
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* checkItem = m_leftover_table->item(row, kLeftoverColCheck);
        if (checkItem && checkItem->checkState() == Qt::Checked) {
            auto* typeItem = m_leftover_table->item(row, kLeftoverColType);
            if (!typeItem) continue;
            int idx = typeItem->data(kOriginalIndexRole).toInt();
            if (idx >= 0 && idx < m_currentLeftovers.size()) {
                auto item = m_currentLeftovers[idx];
                item.selected = true;
                result.append(item);
            }
        }
    }
    return result;
}

void AdvancedUninstallPanel::updateStatusCounts()
{
    m_program_count_label->setText(
        tr("%1 programs").arg(m_filteredPrograms.size()));

    qint64 totalSize = 0;
    for (const auto& prog : m_filteredPrograms) {
        totalSize += prog.estimatedSizeKB * 1024;
    }

    if (totalSize > 0) {
        m_total_size_label->setText(
            tr("Total: %1").arg(formatSize(totalSize)));
    } else {
        m_total_size_label->clear();
    }
}

} // namespace sak
