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

struct ToolbarUi {
    QLineEdit* search_edit = nullptr;
    QComboBox* view_filter_combo = nullptr;
    QPushButton* refresh_button = nullptr;
    QPushButton* uninstall_button = nullptr;
    QPushButton* forced_uninstall_button = nullptr;
    QPushButton* batch_button = nullptr;
    QPushButton* settings_button = nullptr;
};

ToolbarUi buildToolbarUi(AdvancedUninstallPanel* panel, QHBoxLayout* toolbar)
{
    ToolbarUi ui;

    ui.search_edit = new QLineEdit(panel);
    ui.search_edit->setPlaceholderText(QObject::tr("Search programs..."));
    ui.search_edit->setClearButtonEnabled(true);
    ui.search_edit->setToolTip(QObject::tr(
        "Filter programs by name, publisher, or version"));
    setAccessible(ui.search_edit, QObject::tr("Search programs"),
        QObject::tr("Filter the program list by name, publisher, or version"));
    toolbar->addWidget(ui.search_edit, 1);

    ui.view_filter_combo = new QComboBox(panel);
    ui.view_filter_combo->addItem(QObject::tr("All Programs"),
        static_cast<int>(ViewFilter::All));
    ui.view_filter_combo->addItem(QObject::tr("Win32 Only"),
        static_cast<int>(ViewFilter::Win32Only));
    ui.view_filter_combo->addItem(QObject::tr("UWP Only"),
        static_cast<int>(ViewFilter::UwpOnly));
    ui.view_filter_combo->addItem(QObject::tr("Bloatware"),
        static_cast<int>(ViewFilter::BloatwareOnly));
    ui.view_filter_combo->addItem(QObject::tr("Orphaned"),
        static_cast<int>(ViewFilter::OrphanedOnly));
    ui.view_filter_combo->setToolTip(QObject::tr("Filter programs by category"));
    setAccessible(ui.view_filter_combo, QObject::tr("View filter"),
        QObject::tr(
            "Show all programs, Win32 only, UWP only, bloatware, or orphaned"));
    toolbar->addWidget(ui.view_filter_combo);

    toolbar->addSpacing(ui::kSpacingMedium);

    ui.refresh_button = new QPushButton(QObject::tr("Refresh"), panel);
    ui.refresh_button->setStyleSheet(ui::kPrimaryButtonStyle);
    ui.refresh_button->setToolTip(QObject::tr("Refresh the list of installed programs"));
    setAccessible(ui.refresh_button, QObject::tr("Refresh programs"),
        QObject::tr("Re-scan for installed programs"));
    toolbar->addWidget(ui.refresh_button);

    ui.uninstall_button = new QPushButton(QObject::tr("Uninstall"), panel);
    ui.uninstall_button->setStyleSheet(ui::kDangerButtonStyle);
    ui.uninstall_button->setToolTip(QObject::tr(
        "Uninstall selected program and scan for leftovers"));
    ui.uninstall_button->setEnabled(false);
    setAccessible(ui.uninstall_button, QObject::tr("Uninstall program"),
        QObject::tr(
            "Run the native uninstaller then scan for leftover files and registry entries"));
    toolbar->addWidget(ui.uninstall_button);

    ui.forced_uninstall_button = new QPushButton(QObject::tr("Forced"), panel);
    ui.forced_uninstall_button->setStyleSheet(ui::kDangerButtonStyle);
    ui.forced_uninstall_button->setToolTip(QObject::tr(
        "Force uninstall — skip native uninstaller, scan and remove all traces"));
    ui.forced_uninstall_button->setEnabled(false);
    setAccessible(ui.forced_uninstall_button, QObject::tr("Forced uninstall"),
        QObject::tr("Skip native uninstaller and remove all program traces directly"));
    toolbar->addWidget(ui.forced_uninstall_button);

    ui.batch_button = new QPushButton(QObject::tr("Batch"), panel);
    ui.batch_button->setToolTip(QObject::tr(
        "Queue multiple programs for sequential uninstall"));
    ui.batch_button->setEnabled(false);
    setAccessible(ui.batch_button, QObject::tr("Batch uninstall"),
        QObject::tr(
            "Add checked programs to the uninstall queue and process them sequentially"));
    toolbar->addWidget(ui.batch_button);

    ui.settings_button = new QPushButton(QObject::tr("Settings"), panel);
    ui.settings_button->setToolTip(QObject::tr(
        "Configure uninstall preferences — recycle bin, restore points, "
        "default scan level, and more"));
    setAccessible(ui.settings_button, QObject::tr("Uninstall settings"),
        QObject::tr("Open the settings dialog to configure uninstall behavior"));
    toolbar->addWidget(ui.settings_button);

    return ui;
}

struct ProgramTableColumns {
    int check = 0;
    int icon = 0;
    int name = 0;
    int publisher = 0;
    int version = 0;
    int size = 0;
    int date = 0;
    int count = 0;
};

struct ProgramTableUi {
    QTableWidget* program_table = nullptr;
    QLabel* program_count_label = nullptr;
    QLabel* total_size_label = nullptr;
};

ProgramTableUi buildProgramTableUi(AdvancedUninstallPanel* panel,
    QVBoxLayout* groupLayout, const ProgramTableColumns& cols)
{
    ProgramTableUi ui;

    ui.program_table = new QTableWidget(panel);
    ui.program_table->setColumnCount(cols.count);
    ui.program_table->setHorizontalHeaderLabels({
        QString(),          // Check column (no header text)
        QString(),          // Icon column
        QObject::tr("Name"),
        QObject::tr("Publisher"),
        QObject::tr("Version"),
        QObject::tr("Size"),
        QObject::tr("Install Date")
    });

    ui.program_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui.program_table->setSelectionMode(QAbstractItemView::SingleSelection);
    ui.program_table->setAlternatingRowColors(true);
    ui.program_table->setSortingEnabled(true);
    ui.program_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui.program_table->setContextMenuPolicy(Qt::CustomContextMenu);
    ui.program_table->verticalHeader()->setVisible(false);

    auto* header = ui.program_table->horizontalHeader();
    header->setSectionResizeMode(cols.check, QHeaderView::Fixed);
    header->resizeSection(cols.check, 30);
    header->setSectionResizeMode(cols.icon, QHeaderView::Fixed);
    header->resizeSection(cols.icon, 28);
    header->setSectionResizeMode(cols.name, QHeaderView::Stretch);
    header->setSectionResizeMode(cols.publisher, QHeaderView::Interactive);
    header->resizeSection(cols.publisher, 160);
    header->setSectionResizeMode(cols.version, QHeaderView::Interactive);
    header->resizeSection(cols.version, 100);
    header->setSectionResizeMode(cols.size, QHeaderView::Interactive);
    header->resizeSection(cols.size, 80);
    header->setSectionResizeMode(cols.date, QHeaderView::Interactive);
    header->resizeSection(cols.date, 100);

    setAccessible(ui.program_table, QObject::tr("Program list"),
        QObject::tr("List of installed programs with details"));
    groupLayout->addWidget(ui.program_table);

    auto* countRow = new QHBoxLayout();
    ui.program_count_label = new QLabel(QObject::tr("0 programs"), panel);
    ui.program_count_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    countRow->addWidget(ui.program_count_label);
    countRow->addStretch();

    ui.total_size_label = new QLabel(panel);
    ui.total_size_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    countRow->addWidget(ui.total_size_label);
    groupLayout->addLayout(countRow);

    return ui;
}

struct LeftoverTableColumns {
    int check = 0;
    int risk = 0;
    int type = 0;
    int path = 0;
    int size = 0;
    int count = 0;
};

struct LeftoverSectionUi {
    QWidget* section = nullptr;
    QTableWidget* table = nullptr;
    QLabel* header_label = nullptr;
    QLabel* count_label = nullptr;
    QPushButton* select_all_button = nullptr;
    QPushButton* select_safe_button = nullptr;
    QPushButton* deselect_all_button = nullptr;
    QPushButton* delete_selected_button = nullptr;
};

LeftoverSectionUi buildLeftoverSectionUi(AdvancedUninstallPanel* panel,
    const LeftoverTableColumns& cols)
{
    LeftoverSectionUi ui;

    ui.section = new QWidget(panel);
    auto* sectionLayout = new QVBoxLayout(ui.section);
    sectionLayout->setContentsMargins(0, 0, 0, 0);
    sectionLayout->setSpacing(ui::kSpacingSmall);

    auto* headerRow = new QHBoxLayout();
    ui.header_label = new QLabel(QObject::tr("Leftover Items"), panel);
    QFont headerFont = ui.header_label->font();
    headerFont.setBold(true);
    headerFont.setPointSize(ui::kFontSizeStatus);
    ui.header_label->setFont(headerFont);
    headerRow->addWidget(ui.header_label);
    headerRow->addStretch();

    ui.count_label = new QLabel(panel);
    ui.count_label->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    headerRow->addWidget(ui.count_label);
    sectionLayout->addLayout(headerRow);

    ui.table = new QTableWidget(panel);
    ui.table->setColumnCount(cols.count);
    ui.table->setHorizontalHeaderLabels({
        QString(),          // Check
        QObject::tr("Risk"),
        QObject::tr("Type"),
        QObject::tr("Path"),
        QObject::tr("Size")
    });
    ui.table->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui.table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    ui.table->setAlternatingRowColors(true);
    ui.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui.table->setSortingEnabled(true);
    ui.table->verticalHeader()->setVisible(false);

    auto* header = ui.table->horizontalHeader();
    header->setSectionResizeMode(cols.check, QHeaderView::Fixed);
    header->resizeSection(cols.check, 30);
    header->setSectionResizeMode(cols.risk, QHeaderView::Interactive);
    header->resizeSection(cols.risk, 70);
    header->setSectionResizeMode(cols.type, QHeaderView::Interactive);
    header->resizeSection(cols.type, 100);
    header->setSectionResizeMode(cols.path, QHeaderView::Stretch);
    header->setSectionResizeMode(cols.size, QHeaderView::Interactive);
    header->resizeSection(cols.size, 80);

    setAccessible(ui.table, QObject::tr("Leftover items"),
        QObject::tr(
            "Files, folders, registry entries, and system objects left behind after uninstall"));
    sectionLayout->addWidget(ui.table);

    auto* buttonRow = new QHBoxLayout();
    ui.select_all_button = new QPushButton(QObject::tr("Select All"), panel);
    ui.select_all_button->setToolTip(
        QObject::tr("Select all leftover items regardless of risk level"));
    setAccessible(ui.select_all_button, QObject::tr("Select all items"));
    buttonRow->addWidget(ui.select_all_button);

    ui.select_safe_button = new QPushButton(QObject::tr("Select Safe"), panel);
    ui.select_safe_button->setStyleSheet(ui::kSuccessButtonStyle);
    ui.select_safe_button->setToolTip(
        QObject::tr("Select all items classified as Safe for removal"));
    setAccessible(ui.select_safe_button, QObject::tr("Select safe items"));
    buttonRow->addWidget(ui.select_safe_button);

    ui.deselect_all_button = new QPushButton(QObject::tr("Deselect All"), panel);
    ui.deselect_all_button->setToolTip(QObject::tr("Deselect all leftover items"));
    setAccessible(ui.deselect_all_button, QObject::tr("Deselect all"));
    buttonRow->addWidget(ui.deselect_all_button);

    buttonRow->addStretch();

    ui.delete_selected_button = new QPushButton(QObject::tr("Delete Selected"), panel);
    ui.delete_selected_button->setStyleSheet(ui::kDangerButtonStyle);
    ui.delete_selected_button->setToolTip(
        QObject::tr("Permanently delete all checked leftover items"));
    ui.delete_selected_button->setEnabled(false);
    setAccessible(ui.delete_selected_button, QObject::tr("Delete selected leftovers"),
        QObject::tr("Permanently remove checked files, folders, and registry entries"));
    buttonRow->addWidget(ui.delete_selected_button);

    sectionLayout->addLayout(buttonRow);
    return ui;
}
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
    connect(m_controller.get(), &AdvancedUninstallController::rebootPendingItems,
            this, &AdvancedUninstallPanel::onRebootPendingItems);

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

    {
        const ToolbarUi ui = buildToolbarUi(this, toolbar);
        m_search_edit = ui.search_edit;
        m_view_filter_combo = ui.view_filter_combo;
        m_refresh_button = ui.refresh_button;
        m_uninstall_button = ui.uninstall_button;
        m_forced_uninstall_button = ui.forced_uninstall_button;
        m_batch_button = ui.batch_button;
        m_settings_button = ui.settings_button;
    }

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
    connect(m_settings_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::showSettingsDialog);
}

void AdvancedUninstallPanel::createProgramTable(QVBoxLayout* layout)
{
    auto* group = new QGroupBox(tr("Installed Programs"), this);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->setSpacing(ui::kSpacingSmall);

    const ProgramTableColumns cols{
        .check = kColCheck,
        .icon = kColIcon,
        .name = kColName,
        .publisher = kColPublisher,
        .version = kColVersion,
        .size = kColSize,
        .date = kColDate,
        .count = kProgramColumnCount,
    };

    {
        const ProgramTableUi ui = buildProgramTableUi(this, groupLayout, cols);
        m_program_table = ui.program_table;
        m_program_count_label = ui.program_count_label;
        m_total_size_label = ui.total_size_label;
    }

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
    const LeftoverTableColumns cols{
        .check = kLeftoverColCheck,
        .risk = kLeftoverColRisk,
        .type = kLeftoverColType,
        .path = kLeftoverColPath,
        .size = kLeftoverColSize,
        .count = kLeftoverColumnCount,
    };

    {
        const LeftoverSectionUi ui = buildLeftoverSectionUi(this, cols);
        m_leftover_section = ui.section;
        m_leftover_table = ui.table;
        m_leftover_header_label = ui.header_label;
        m_leftover_count_label = ui.count_label;
        m_select_all_button = ui.select_all_button;
        m_select_safe_button = ui.select_safe_button;
        m_deselect_all_button = ui.deselect_all_button;
        m_delete_selected_button = ui.delete_selected_button;
    }

    layout->addWidget(m_leftover_section, 2);

    // Initially hidden
    m_leftover_section->setVisible(false);

    // Connections
    connect(m_select_all_button, &QPushButton::clicked,
            this, &AdvancedUninstallPanel::onSelectAll);
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

    // Auto-select items based on user preference
    if (!leftovers.isEmpty()) {
        if (m_controller->selectAllByDefault()) {
            onSelectAll();
        } else {
            onSelectAllSafe();
        }
    }

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

void AdvancedUninstallPanel::onRebootPendingItems(QStringList paths)
{
    logMessage(QString("Locked files scheduled for removal on reboot: %1")
                   .arg(paths.size()));
    for (const auto& path : paths) {
        logMessage(QString("  Reboot removal: %1").arg(path));
    }

    QMessageBox::information(this,
        tr("Reboot Required"),
        tr("<b>%1 locked item(s)</b> could not be removed immediately.<br><br>"
           "They have been scheduled for automatic removal on the next "
           "Windows restart. This is independent of this application — "
           "Windows will handle the deletion at boot time.")
            .arg(paths.size()));
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

void AdvancedUninstallPanel::onSelectAll()
{
    m_leftover_table->blockSignals(true);
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* checkItem = m_leftover_table->item(row, kLeftoverColCheck);
        if (!checkItem) {
            continue;
        }

        checkItem->setCheckState(Qt::Checked);

        auto* typeItem = m_leftover_table->item(row, kLeftoverColType);
        if (!typeItem) {
            continue;
        }

        const int idx = typeItem->data(kOriginalIndexRole).toInt();
        if (idx < 0 || idx >= m_currentLeftovers.size()) {
            continue;
        }
        m_currentLeftovers[idx].selected = true;
    }
    m_leftover_table->blockSignals(false);
    onLeftoverSelectionChanged();
    logMessage("Selected all leftover items.");
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
        if (!checkItem) {
            continue;
        }

        checkItem->setCheckState(Qt::Unchecked);

        auto* typeItem = m_leftover_table->item(row, kLeftoverColType);
        if (!typeItem) {
            continue;
        }

        const int idx = typeItem->data(kOriginalIndexRole).toInt();
        if (idx < 0 || idx >= m_currentLeftovers.size()) {
            continue;
        }
        m_currentLeftovers[idx].selected = false;
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
