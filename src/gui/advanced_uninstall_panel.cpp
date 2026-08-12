// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_panel.cpp
/// @brief Main UI panel for Advanced Uninstall -- program list, leftover
///        scanner, and cleanup workflow

#include "sak/advanced_uninstall_panel.h"

#include "sak/advanced_uninstall_controller.h"
#include "sak/detachable_log_window.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/restore_point_manager.h"
#include "sak/rich_text_safety.h"
#include "sak/style_constants.h"
#include "sak/view_empty_state.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
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
constexpr int kLeftoverCheckColumnWidth = 30;
constexpr int kLeftoverRiskColumnWidth = 70;
constexpr int kLeftoverTypeColumnWidth = 100;
constexpr int kLeftoverSizeColumnWidth = 80;
constexpr int kProgramCheckColumnWidth = sak::kCheckboxColumnW;
constexpr int kProgramIconColumnWidth = sak::ui::kUiButtonSizeInline;
constexpr int kProgramPublisherColumnWidth = 160;
constexpr int kProgramVersionColumnWidth = sak::ui::kUiWideColumnWidth;
constexpr int kProgramSizeColumnWidth = sak::ui::kUiMediumColumnWidth;
constexpr int kProgramInstallDateColumnWidth = sak::ui::kUiWideColumnWidth;
constexpr int kProgramTableStretch = 3;
constexpr int kLeftoverSectionStretch = 2;
constexpr qint64 kEstimatedSizeBytesPerKb = sak::kBytesPerKB;

// Reads a row's original-index payload fail-closed. A missing or wrong-typed
// kOriginalIndexRole value must NOT silently coerce to 0 (QVariant::toInt()'s
// default), which would target the FIRST program/leftover instead of the one the
// row actually maps to. Returns -1 on any missing/invalid payload so the caller's
// range check rejects it rather than acting on a mis-resolved row.
int originalRowIndex(const QTableWidgetItem* item) {
    if (item == nullptr) {
        return -1;
    }
    bool ok = false;
    const int idx = item->data(kOriginalIndexRole).toInt(&ok);
    return ok ? idx : -1;
}

/// @brief Table item that sorts numerically by Qt::UserRole data
class NumericSortItem : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    bool operator<(const QTableWidgetItem& other) const override {
        return data(Qt::UserRole).toLongLong() < other.data(Qt::UserRole).toLongLong();
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

void addToolbarFilters(AdvancedUninstallPanel* panel, QHBoxLayout* toolbar, ToolbarUi* controls) {
    controls->search_edit = new QLineEdit(panel);
    controls->search_edit->setPlaceholderText(QObject::tr("Search programs..."));
    controls->search_edit->setClearButtonEnabled(true);
    controls->search_edit->setToolTip(
        QObject::tr("Filter programs by name, publisher, or version"));
    setAccessible(controls->search_edit,
                  QObject::tr("Search programs"),
                  QObject::tr("Filter the program list by name, publisher, or version"));
    toolbar->addWidget(controls->search_edit, 1);

    controls->view_filter_combo = new QComboBox(panel);
    controls->view_filter_combo->addItem(QObject::tr("All Programs"),
                                         static_cast<int>(ViewFilter::All));
    controls->view_filter_combo->addItem(QObject::tr("Win32 Only"),
                                         static_cast<int>(ViewFilter::Win32Only));
    controls->view_filter_combo->addItem(QObject::tr("UWP Only"),
                                         static_cast<int>(ViewFilter::UwpOnly));
    controls->view_filter_combo->addItem(QObject::tr("Bloatware"),
                                         static_cast<int>(ViewFilter::BloatwareOnly));
    controls->view_filter_combo->addItem(QObject::tr("Orphaned"),
                                         static_cast<int>(ViewFilter::OrphanedOnly));
    controls->view_filter_combo->setToolTip(QObject::tr("Filter programs by category"));
    setAccessible(controls->view_filter_combo,
                  QObject::tr("View filter"),
                  QObject::tr("Show all programs, Win32 only, UWP only, bloatware, or orphaned"));
    toolbar->addWidget(controls->view_filter_combo);
}

void addToolbarActions(AdvancedUninstallPanel* panel, QHBoxLayout* toolbar, ToolbarUi* controls) {
    toolbar->addSpacing(ui::kSpacingMedium);

    controls->refresh_button = new QPushButton(QObject::tr("Refresh"), panel);
    controls->refresh_button->setStyleSheet(ui::kPrimaryButtonStyle);
    controls->refresh_button->setToolTip(QObject::tr("Refresh the list of installed programs"));
    setAccessible(controls->refresh_button,
                  QObject::tr("Refresh programs"),
                  QObject::tr("Re-scan for installed programs"));
    toolbar->addWidget(controls->refresh_button);

    controls->uninstall_button = new QPushButton(QObject::tr("Uninstall"), panel);
    controls->uninstall_button->setStyleSheet(ui::kDangerButtonStyle);
    controls->uninstall_button->setToolTip(
        QObject::tr("Uninstall selected program and scan for leftovers"));
    controls->uninstall_button->setEnabled(false);
    setAccessible(
        controls->uninstall_button,
        QObject::tr("Uninstall program"),
        QObject::tr(
            "Run the native uninstaller then scan for leftover files and registry entries"));
    toolbar->addWidget(controls->uninstall_button);

    controls->forced_uninstall_button = new QPushButton(QObject::tr("Forced"), panel);
    controls->forced_uninstall_button->setStyleSheet(ui::kDangerButtonStyle);
    controls->forced_uninstall_button->setToolTip(
        QObject::tr("Force uninstall -- skip native uninstaller, scan and remove all traces"));
    controls->forced_uninstall_button->setEnabled(false);
    setAccessible(controls->forced_uninstall_button,
                  QObject::tr("Forced uninstall"),
                  QObject::tr("Skip native uninstaller and remove all program traces directly"));
    toolbar->addWidget(controls->forced_uninstall_button);

    controls->batch_button = new QPushButton(QObject::tr("Batch"), panel);
    controls->batch_button->setStyleSheet(ui::kPrimaryButtonStyle);
    controls->batch_button->setToolTip(
        QObject::tr("Queue multiple programs for sequential uninstall"));
    controls->batch_button->setEnabled(false);
    setAccessible(controls->batch_button,
                  QObject::tr("Batch uninstall"),
                  QObject::tr(
                      "Add checked programs to the uninstall queue and process them sequentially"));
    toolbar->addWidget(controls->batch_button);
}

ToolbarUi buildToolbarUi(AdvancedUninstallPanel* panel, QHBoxLayout* toolbar) {
    ToolbarUi ui;
    addToolbarFilters(panel, toolbar, &ui);
    addToolbarActions(panel, toolbar, &ui);

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
                                   QVBoxLayout* group_layout,
                                   const ProgramTableColumns& cols) {
    ProgramTableUi ui;

    ui.program_table = new QTableWidget(panel);
    ui.program_table->setColumnCount(cols.count);
    ui.program_table->setHorizontalHeaderLabels({QString(),  // Check column (no header text)
                                                 QString(),  // Icon column
                                                 QObject::tr("Name"),
                                                 QObject::tr("Publisher"),
                                                 QObject::tr("Version"),
                                                 QObject::tr("Size"),
                                                 QObject::tr("Install Date")});

    configureStandardTable(ui.program_table, QAbstractItemView::SingleSelection);
    ui.program_table->setSortingEnabled(true);
    ui.program_table->setContextMenuPolicy(Qt::CustomContextMenu);

    auto* header = ui.program_table->horizontalHeader();
    header->setSectionResizeMode(cols.check, QHeaderView::Fixed);
    header->resizeSection(cols.check, kProgramCheckColumnWidth);
    header->setSectionResizeMode(cols.icon, QHeaderView::Fixed);
    header->resizeSection(cols.icon, kProgramIconColumnWidth);
    header->setSectionResizeMode(cols.name, QHeaderView::Stretch);
    header->setSectionResizeMode(cols.publisher, QHeaderView::Interactive);
    header->resizeSection(cols.publisher, kProgramPublisherColumnWidth);
    header->setSectionResizeMode(cols.version, QHeaderView::Interactive);
    header->resizeSection(cols.version, kProgramVersionColumnWidth);
    header->setSectionResizeMode(cols.size, QHeaderView::Interactive);
    header->resizeSection(cols.size, kProgramSizeColumnWidth);
    header->setSectionResizeMode(cols.date, QHeaderView::Interactive);
    header->resizeSection(cols.date, kProgramInstallDateColumnWidth);

    setAccessible(ui.program_table,
                  QObject::tr("Program list"),
                  QObject::tr("List of installed programs with details"));
    group_layout->addWidget(ui.program_table);

    auto* count_row = new QHBoxLayout();
    ui.program_count_label = new QLabel(QObject::tr("0 programs"), panel);
    ui.program_count_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    count_row->addWidget(ui.program_count_label);
    count_row->addStretch();

    ui.total_size_label = new QLabel(panel);
    ui.total_size_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    count_row->addWidget(ui.total_size_label);
    group_layout->addLayout(count_row);

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

void addLeftoverHeader(AdvancedUninstallPanel* panel,
                       QVBoxLayout* section_layout,
                       LeftoverSectionUi* ui) {
    auto* header_row = new QHBoxLayout();
    ui->header_label = new QLabel(QObject::tr("Leftover Items"), panel);
    QFont header_font = ui->header_label->font();
    header_font.setBold(true);
    header_font.setPointSize(ui::kFontSizeStatus);
    ui->header_label->setFont(header_font);
    header_row->addWidget(ui->header_label);
    header_row->addStretch();

    ui->count_label = new QLabel(panel);
    ui->count_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    header_row->addWidget(ui->count_label);
    section_layout->addLayout(header_row);
}

void configureLeftoverTableHeader(QTableWidget* table, const LeftoverTableColumns& cols) {
    auto* header = table->horizontalHeader();
    header->setSectionResizeMode(cols.check, QHeaderView::Fixed);
    header->resizeSection(cols.check, kLeftoverCheckColumnWidth);
    header->setSectionResizeMode(cols.risk, QHeaderView::Interactive);
    header->resizeSection(cols.risk, kLeftoverRiskColumnWidth);
    header->setSectionResizeMode(cols.type, QHeaderView::Interactive);
    header->resizeSection(cols.type, kLeftoverTypeColumnWidth);
    header->setSectionResizeMode(cols.path, QHeaderView::Stretch);
    header->setSectionResizeMode(cols.size, QHeaderView::Interactive);
    header->resizeSection(cols.size, kLeftoverSizeColumnWidth);
}

void addLeftoverTable(AdvancedUninstallPanel* panel,
                      QVBoxLayout* section_layout,
                      const LeftoverTableColumns& cols,
                      LeftoverSectionUi* ui) {
    ui->table = new QTableWidget(panel);
    ui->table->setColumnCount(cols.count);
    ui->table->setHorizontalHeaderLabels({QString(),
                                          QObject::tr("Risk"),
                                          QObject::tr("Type"),
                                          QObject::tr("Path"),
                                          QObject::tr("Size")});
    configureStandardTable(ui->table, QAbstractItemView::ExtendedSelection);
    ui->table->setSortingEnabled(true);
    configureLeftoverTableHeader(ui->table, cols);
    setAccessible(
        ui->table,
        QObject::tr("Leftover items"),
        QObject::tr(
            "Files, folders, registry entries, and system objects left behind after uninstall"));
    section_layout->addWidget(ui->table);
}

void addLeftoverButtons(AdvancedUninstallPanel* panel,
                        QVBoxLayout* section_layout,
                        LeftoverSectionUi* ui) {
    auto* button_row = new QHBoxLayout();
    ui->select_all_button = new QPushButton(QObject::tr("Select All"), panel);
    ui->select_all_button->setStyleSheet(ui::kPrimaryButtonStyle);
    ui->select_all_button->setToolTip(
        QObject::tr("Select all leftover items regardless of risk level"));
    setAccessible(ui->select_all_button, QObject::tr("Select all items"));
    button_row->addWidget(ui->select_all_button);

    ui->select_safe_button = new QPushButton(QObject::tr("Select Safe"), panel);
    ui->select_safe_button->setStyleSheet(ui::kSuccessButtonStyle);
    ui->select_safe_button->setToolTip(
        QObject::tr("Select all items classified as Safe for removal"));
    setAccessible(ui->select_safe_button, QObject::tr("Select safe items"));
    button_row->addWidget(ui->select_safe_button);

    ui->deselect_all_button = new QPushButton(QObject::tr("Deselect All"), panel);
    ui->deselect_all_button->setStyleSheet(ui::kPrimaryButtonStyle);
    ui->deselect_all_button->setToolTip(QObject::tr("Deselect all leftover items"));
    setAccessible(ui->deselect_all_button, QObject::tr("Deselect all"));
    button_row->addWidget(ui->deselect_all_button);

    button_row->addStretch();

    ui->delete_selected_button = new QPushButton(QObject::tr("Delete Selected"), panel);
    ui->delete_selected_button->setStyleSheet(ui::kDangerButtonStyle);
    ui->delete_selected_button->setToolTip(
        QObject::tr("Permanently delete all checked leftover items"));
    ui->delete_selected_button->setEnabled(false);
    setAccessible(ui->delete_selected_button,
                  QObject::tr("Delete selected leftovers"),
                  QObject::tr("Permanently remove checked files, folders, and registry entries"));
    button_row->addWidget(ui->delete_selected_button);

    section_layout->addLayout(button_row);
}

LeftoverSectionUi buildLeftoverSectionUi(AdvancedUninstallPanel* panel,
                                         const LeftoverTableColumns& cols) {
    LeftoverSectionUi ui;

    ui.section = new QWidget(panel);
    auto* section_layout = new QVBoxLayout(ui.section);
    section_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    section_layout->setSpacing(ui::kSpacingSmall);

    addLeftoverHeader(panel, section_layout, &ui);
    addLeftoverTable(panel, section_layout, cols, &ui);
    addLeftoverButtons(panel, section_layout, &ui);
    return ui;
}
}  // namespace

// -- Construction ------------------------------------------------------------

AdvancedUninstallPanel::AdvancedUninstallPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<AdvancedUninstallController>(this)) {
    setupUi();
    connectEnumerationSignals();
    connectUninstallSignals();
    connectCleanupSignals();
    connectStatusSignals();

    logMessage("Advanced Uninstall panel initialized.");
}

AdvancedUninstallPanel::~AdvancedUninstallPanel() {
    logMessage("Advanced Uninstall panel destroyed.");
}

void AdvancedUninstallPanel::connectEnumerationSignals() {
    connect(m_controller.get(),
            &AdvancedUninstallController::enumerationStarted,
            this,
            &AdvancedUninstallPanel::onEnumerationStarted);
    connect(m_controller.get(),
            &AdvancedUninstallController::enumerationProgress,
            this,
            &AdvancedUninstallPanel::onEnumerationProgress);
    connect(m_controller.get(),
            &AdvancedUninstallController::enumerationFinished,
            this,
            &AdvancedUninstallPanel::onEnumerationFinished);
    connect(m_controller.get(),
            &AdvancedUninstallController::enumerationFailed,
            this,
            &AdvancedUninstallPanel::onEnumerationFailed);
}

void AdvancedUninstallPanel::connectUninstallSignals() {
    connect(m_controller.get(),
            &AdvancedUninstallController::uninstallStarted,
            this,
            &AdvancedUninstallPanel::onUninstallStarted);
    connect(m_controller.get(),
            &AdvancedUninstallController::uninstallProgress,
            this,
            &AdvancedUninstallPanel::onUninstallProgress);
    connect(m_controller.get(),
            &AdvancedUninstallController::leftoverScanFinished,
            this,
            &AdvancedUninstallPanel::onLeftoverScanFinished);
    connect(m_controller.get(),
            &AdvancedUninstallController::uninstallFinished,
            this,
            &AdvancedUninstallPanel::onUninstallFinished);
    connect(m_controller.get(),
            &AdvancedUninstallController::uninstallFailed,
            this,
            &AdvancedUninstallPanel::onUninstallFailed);
    connect(m_controller.get(),
            &AdvancedUninstallController::uninstallCancelled,
            this,
            &AdvancedUninstallPanel::onUninstallCancelled);
}

void AdvancedUninstallPanel::connectCleanupSignals() {
    connect(m_controller.get(),
            &AdvancedUninstallController::cleanupStarted,
            this,
            &AdvancedUninstallPanel::onCleanupStarted);
    connect(m_controller.get(),
            &AdvancedUninstallController::itemCleaned,
            this,
            &AdvancedUninstallPanel::onItemCleaned);
    connect(m_controller.get(),
            &AdvancedUninstallController::cleanupFinished,
            this,
            &AdvancedUninstallPanel::onCleanupFinished);
    connect(m_controller.get(),
            &AdvancedUninstallController::cleanupCancelled,
            this,
            &AdvancedUninstallPanel::onCleanupCancelled);
    connect(m_controller.get(),
            &AdvancedUninstallController::rebootPendingItems,
            this,
            &AdvancedUninstallPanel::onRebootPendingItems);
    connect(m_controller.get(),
            &AdvancedUninstallController::recycleFallbackItems,
            this,
            &AdvancedUninstallPanel::onRecycleFallbackItems);
}

void AdvancedUninstallPanel::connectStatusSignals() {
    connect(m_controller.get(),
            &AdvancedUninstallController::statusMessage,
            this,
            &AdvancedUninstallPanel::statusMessage);
    connect(m_controller.get(),
            &AdvancedUninstallController::progressUpdate,
            this,
            &AdvancedUninstallPanel::progressUpdate);
    connect(m_controller.get(),
            &AdvancedUninstallController::batchFinished,
            this,
            [this](int succeeded, int failed) {
                setOperationRunning(false);
                logMessage(
                    QString("Batch complete: %1 succeeded, %2 failed").arg(succeeded).arg(failed));
            });
}

// -- UI Setup ----------------------------------------------------------------

void AdvancedUninstallPanel::setupUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    root_layout->setSpacing(sak::ui::kSpacingNone);

    auto* content_widget = new QWidget(this);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    main_layout->setSpacing(ui::kSpacingDefault);

    root_layout->addWidget(content_widget);

    createToolbar(main_layout);
    createProgramTable(main_layout);
    createLeftoverSection(main_layout);
    createStatusBar(main_layout);
}

void AdvancedUninstallPanel::createToolbar(QVBoxLayout* layout) {
    Q_ASSERT(layout);
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
    }

    layout->addLayout(toolbar);

    // Connect toolbar signals
    connect(
        m_search_edit, &QLineEdit::textChanged, this, &AdvancedUninstallPanel::onSearchTextChanged);
    connect(m_view_filter_combo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &AdvancedUninstallPanel::onViewFilterChanged);
    connect(
        m_refresh_button, &QPushButton::clicked, this, &AdvancedUninstallPanel::onRefreshClicked);
    connect(m_uninstall_button,
            &QPushButton::clicked,
            this,
            &AdvancedUninstallPanel::onUninstallClicked);
    connect(m_forced_uninstall_button,
            &QPushButton::clicked,
            this,
            &AdvancedUninstallPanel::onForcedUninstallClicked);
    connect(m_batch_button,
            &QPushButton::clicked,
            this,
            &AdvancedUninstallPanel::onBatchUninstallClicked);
}

void AdvancedUninstallPanel::createProgramTable(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    auto* group = new QGroupBox(tr("Installed Programs"), this);
    auto* group_layout = new QVBoxLayout(group);
    group_layout->setSpacing(ui::kSpacingSmall);

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
        const ProgramTableUi ui = buildProgramTableUi(this, group_layout, cols);
        m_program_table = ui.program_table;
        m_program_count_label = ui.program_count_label;
        m_total_size_label = ui.total_size_label;
    }

    // Designed empty state over the table body; the loading message is raised/cleared from the
    // enumeration handlers. The QTableWidget's model exists as soon as the widget is built.
    m_program_empty_state = new sak::ui::ViewEmptyState(
        m_program_table, tr("No programs found - click Refresh to scan"));

    layout->addWidget(group, kProgramTableStretch);

    // Connections
    connect(m_program_table,
            &QTableWidget::itemSelectionChanged,
            this,
            &AdvancedUninstallPanel::onProgramSelectionChanged);
    connect(m_program_table,
            &QTableWidget::cellDoubleClicked,
            this,
            &AdvancedUninstallPanel::onProgramDoubleClicked);
    connect(m_program_table,
            &QTableWidget::customContextMenuRequested,
            this,
            &AdvancedUninstallPanel::onProgramContextMenu);
}

void AdvancedUninstallPanel::createLeftoverSection(QVBoxLayout* layout) {
    Q_ASSERT(layout);
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

    // Empty state for the leftover table. No scan runs on this table directly, so it needs no
    // loading state; parented to the table, the overlay self-manages its lifetime.
    new sak::ui::ViewEmptyState(m_leftover_table, tr("No leftover items found"));

    layout->addWidget(m_leftover_section, kLeftoverSectionStretch);

    // Initially hidden
    m_leftover_section->setVisible(false);

    // Connections
    connect(m_select_all_button, &QPushButton::clicked, this, &AdvancedUninstallPanel::onSelectAll);
    connect(m_select_safe_button,
            &QPushButton::clicked,
            this,
            &AdvancedUninstallPanel::onSelectAllSafe);
    connect(
        m_deselect_all_button, &QPushButton::clicked, this, &AdvancedUninstallPanel::onDeselectAll);
    connect(m_delete_selected_button,
            &QPushButton::clicked,
            this,
            &AdvancedUninstallPanel::onDeleteSelectedLeftovers);
    connect(m_leftover_table,
            &QTableWidget::itemChanged,
            this,
            &AdvancedUninstallPanel::onLeftoverSelectionChanged);
}

void AdvancedUninstallPanel::createStatusBar(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    auto* status_row = new QHBoxLayout();
    status_row->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kSpacingTight, sak::ui::kMarginNone, sak::ui::kMarginNone);

    // Log toggle on the left
    m_log_toggle = new LogToggleSwitch(tr("Log"), this);
    status_row->addWidget(m_log_toggle);

    // Settings button next to log toggle
    m_settings_button = new QPushButton(tr("Settings"), this);
    m_settings_button->setToolTip(
        tr("Configure uninstall preferences -- recycle bin, restore points, "
           "default scan level, and more"));
    m_settings_button->setAccessibleName(tr("Uninstall settings"));
    m_settings_button->setStyleSheet(ui::kPrimaryButtonStyle);
    status_row->addWidget(m_settings_button);
    connect(m_settings_button,
            &QPushButton::clicked,
            this,
            &AdvancedUninstallPanel::showSettingsDialog);

    status_row->addStretch();

    // R5-G20-2: in-panel Stop for the running enumeration/uninstall/cleanup. It is the single
    // control setOperationRunning(true) leaves enabled (every other control is disabled), so the
    // technician can halt work mid-run. Clicking it calls the controller's cooperative
    // cancelOperation() (requestCancel/requestStop on the workers -- a real stop, not a detach).
    // Hidden+disabled until an op starts; setOperationRunning drives its visible/enabled state.
    m_cancel_button = new QPushButton(tr("Cancel"), this);
    m_cancel_button->setToolTip(tr("Stop the running operation"));
    m_cancel_button->setAccessibleName(tr("Cancel current operation"));
    m_cancel_button->setStyleSheet(ui::kDangerButtonStyle);
    m_cancel_button->setVisible(false);
    m_cancel_button->setEnabled(false);
    status_row->addWidget(m_cancel_button);
    connect(m_cancel_button,
            &QPushButton::clicked,
            m_controller.get(),
            &AdvancedUninstallController::cancelOperation);

    layout->addLayout(status_row);
}

// -- Toolbar Slots -----------------------------------------------------------

void AdvancedUninstallPanel::onRefreshClicked() {
    logMessage("Refreshing installed programs...");
    m_controller->refreshPrograms();
}

void AdvancedUninstallPanel::onUninstallClicked() {
    auto program = selectedProgram();
    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage(tr("No program selected."), sak::kTimerStatusMessageMs);
        return;
    }
    showUninstallConfirmation(program);
}

void AdvancedUninstallPanel::onForcedUninstallClicked() {
    auto program = selectedProgram();
    if (program.displayName.isEmpty()) {
        Q_EMIT statusMessage(tr("No program selected."), sak::kTimerStatusMessageMs);
        return;
    }
    showForcedUninstallDialog(program);
}

void AdvancedUninstallPanel::onBatchUninstallClicked() {
    // Collect checked programs
    bool all_resolved = true;
    auto programs = selectedPrograms(&all_resolved);
    if (!all_resolved) {
        // Fail closed: queueing only the rows that still resolve would uninstall a SUBSET of the
        // checked programs without telling anyone which ones were dropped.
        sak::showWarningLogged(
            this,
            tr("Selection Out Of Date"),
            tr("One or more checked programs could not be resolved from the current list. "
               "Refresh and check them again -- no batch was queued."));
        return;
    }
    if (programs.isEmpty()) {
        Q_EMIT statusMessage(tr("No programs checked for batch uninstall."),
                             sak::kTimerStatusMessageMs);
        return;
    }

    // Add all checked to queue
    m_controller->clearQueue();
    for (const auto& p : programs) {
        m_controller->addToQueue(p,
                                 m_controller->defaultScanLevel(),
                                 m_controller->autoCleanSafe());
    }

    showBatchUninstallDialog();
}

void AdvancedUninstallPanel::onSearchTextChanged(const QString& text) {
    m_searchFilter = text;
    applyFilter();
}

void AdvancedUninstallPanel::onViewFilterChanged(int index) {
    m_viewFilter = static_cast<ViewFilter>(m_view_filter_combo->itemData(index).toInt());
    applyFilter();
}

// -- Controller Signal Handlers ----------------------------------------------

void AdvancedUninstallPanel::onEnumerationStarted() {
    setOperationRunning(true);
    m_program_empty_state->setLoading(tr("Loading installed programs..."));
    Q_EMIT statusMessage(tr("Scanning installed programs..."), 0);
    Q_EMIT progressUpdate(0, 0);
    logMessage("Program enumeration started.");
}

void AdvancedUninstallPanel::onEnumerationProgress(int current, int total) {
    Q_EMIT progressUpdate(current, total);
}

void AdvancedUninstallPanel::onEnumerationFinished(QVector<ProgramInfo> programs) {
    m_allPrograms = programs;
    applyFilter();
    m_program_empty_state->clearLoading();
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);
    Q_EMIT statusMessage(tr("Ready"), sak::kTimerStatusMessageMs);
    logMessage(QString("Found %1 installed programs.").arg(programs.size()));
}

void AdvancedUninstallPanel::onEnumerationFailed(const QString& error) {
    m_program_empty_state->clearLoading();
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);
    Q_EMIT statusMessage(tr("Enumeration failed"), sak::kTimerStatusDefaultMs);
    logMessage(QString("ERROR: Enumeration failed: %1").arg(error));
    sak::logError("Enumeration failed: {}", error.toStdString());
    // error may carry registry-derived (HKCU-writable) text, and the warning box renders in
    // AutoText mode, so escape it to markup rather than let it spoof the dialog.
    sak::showWarningLogged(
        this,
        tr("Enumeration Error"),
        tr("Failed to enumerate installed programs:\n%1").arg(error.toHtmlEscaped()));
}

void AdvancedUninstallPanel::onUninstallStarted(const QString& program_name) {
    setOperationRunning(true);
    // Invalidate any leftovers still shown from a PRIOR scan before this operation runs: they
    // belong to a different program, and if this uninstall later fails or is cancelled the UI
    // re-enables with those stale, now-misattributed delete targets still present. A fresh scan
    // repopulates the section for this program.
    clearLeftoverTable();
    m_leftover_section->setVisible(false);
    Q_EMIT statusMessage(tr("Uninstalling: %1").arg(program_name), 0);
    Q_EMIT progressUpdate(0, kPercentMax);
    logMessage(QString("Uninstall started: %1").arg(program_name));
}

void AdvancedUninstallPanel::onUninstallProgress(int percent, const QString& phase) {
    Q_EMIT progressUpdate(percent, kPercentMax);
    Q_EMIT statusMessage(phase, 0);
}

void AdvancedUninstallPanel::onLeftoverScanFinished(QVector<LeftoverItem> leftovers) {
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

    logMessage(QString("Leftover scan complete: %1 items found.").arg(leftovers.size()));
}

void AdvancedUninstallPanel::onUninstallFinished(UninstallReport report) {
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);

    QString result_str;
    switch (report.uninstallResult) {
    case UninstallReport::UninstallResult::Success:
        result_str = "Success";
        break;
    case UninstallReport::UninstallResult::Failed:
        result_str = "Failed";
        break;
    case UninstallReport::UninstallResult::Cancelled:
        result_str = "Cancelled";
        break;
    case UninstallReport::UninstallResult::Skipped:
        result_str = "Skipped";
        break;
    }

    Q_EMIT statusMessage(tr("Uninstall %1: %2").arg(result_str, report.programName),
                         sak::kTimerStatusDefaultMs);

    logMessage(QString("Uninstall complete: %1 -- %2 (exit code: %3)")
                   .arg(report.programName, result_str)
                   .arg(report.nativeExitCode));
    const int total_cleaned = report.filesDeleted + report.foldersDeleted +
                              report.registryKeysDeleted + report.registryValuesDeleted +
                              report.servicesRemoved + report.tasksRemoved +
                              report.firewallRulesRemoved + report.startupEntriesRemoved;
    logMessage(QString("  Leftovers found: %1, Cleaned: %2, Failed: %3")
                   .arg(report.foundLeftovers.size())
                   .arg(total_cleaned)
                   .arg(report.failedDeletions));

    // Refresh program list to reflect removal
    m_controller->refreshPrograms();
}

void AdvancedUninstallPanel::onUninstallFailed(const QString& error) {
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);
    Q_EMIT statusMessage(tr("Uninstall failed"), sak::kTimerStatusDefaultMs);
    logMessage(QString("ERROR: Uninstall failed: %1").arg(error));
    sak::logError("Uninstall failed: {}", error.toStdString());
    // error may carry registry-derived (HKCU-writable) text, and the warning box renders in
    // AutoText mode, so escape it to markup rather than let it spoof the dialog.
    sak::showWarningLogged(this,
                           tr("Uninstall Error"),
                           tr("The uninstall operation failed:\n%1").arg(error.toHtmlEscaped()));
}

void AdvancedUninstallPanel::onUninstallCancelled() {
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);
    Q_EMIT statusMessage(tr("Uninstall cancelled"), sak::kTimerStatusDefaultMs);
    logMessage("Uninstall operation cancelled by user.");
}

void AdvancedUninstallPanel::onCleanupStarted(int total_items) {
    setOperationRunning(true);
    m_cleanup_progress = 0;
    m_cleanup_total = total_items;
    Q_EMIT progressUpdate(0, total_items);
    Q_EMIT statusMessage(tr("Cleaning %1 items...").arg(total_items), 0);
    logMessage(QString("Cleanup started: %1 items to remove.").arg(total_items));
}

void AdvancedUninstallPanel::onItemCleaned(const QString& path, bool success) {
    ++m_cleanup_progress;
    Q_EMIT progressUpdate(m_cleanup_progress, m_cleanup_total);
    if (success) {
        logMessage(QString("  Removed: %1").arg(path));
    } else {
        logMessage(QString("  FAILED to remove: %1").arg(path));
    }
}

void AdvancedUninstallPanel::onCleanupFinished(int succeeded, int failed, qint64 bytes_recovered) {
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);

    QString msg = tr("Cleanup complete: %1 removed").arg(succeeded);
    if (failed > 0) {
        msg += tr(", %1 failed").arg(failed);
    }
    if (bytes_recovered > 0) {
        msg += tr(" (%1 recovered)").arg(formatSize(bytes_recovered));
    }
    Q_EMIT statusMessage(msg, sak::kTimerStatusDefaultMs);
    logMessage(msg);

    // Only tear down the leftover view when everything actually cleaned. If any item failed, keep
    // the table and section visible so the failures stay on screen and remain retryable instead
    // of being silently erased under a "Cleanup complete" message.
    if (failed == 0) {
        clearLeftoverTable();
        m_leftover_section->setVisible(false);
    }

    // Refresh program list
    m_controller->refreshPrograms();
}

void AdvancedUninstallPanel::onCleanupCancelled() {
    // A cancelled cleanup must un-busy the UI (otherwise the panel stays stuck "running") and leave
    // the leftover table as-is for the user to retry. The controller re-emits the leftovers still
    // present if this cancel ended a deferred auto-clean.
    setOperationRunning(false);
    Q_EMIT progressUpdate(0, 1);
    Q_EMIT statusMessage(tr("Cleanup cancelled"), sak::kTimerStatusDefaultMs);
    logMessage("Cleanup cancelled.");
}

void AdvancedUninstallPanel::onRebootPendingItems(QStringList paths) {
    logMessage(QString("Locked files scheduled for removal on reboot: %1").arg(paths.size()));
    for (const auto& path : paths) {
        logMessage(QString("  Reboot removal: %1").arg(path));
    }

    sak::showInformationLogged(
        this,
        tr("Reboot Required"),
        tr("<b>%1 locked item(s)</b> could not be removed immediately.<br><br>"
           "They have been scheduled for automatic removal on the next "
           "Windows restart. This is independent of this application -- "
           "Windows will handle the deletion at boot time.")
            .arg(paths.size()));
}

void AdvancedUninstallPanel::onRecycleFallbackItems(QStringList paths) {
    logMessage(
        QString("Items that could NOT be sent to the Recycle Bin and were permanently deleted: %1")
            .arg(paths.size()));
    for (const auto& path : paths) {
        logMessage(QString("  Permanently deleted: %1").arg(path));
    }

    sak::showWarningLogged(
        this,
        tr("Permanently Deleted"),
        tr("<b>%1 item(s)</b> could not be sent to the Recycle Bin and were "
           "<b>permanently deleted</b> instead.<br><br>"
           "These items are NOT recoverable from the Recycle Bin. See the log for the "
           "full list.")
            .arg(paths.size()));
}

// -- Program Table Slots -----------------------------------------------------

void AdvancedUninstallPanel::onProgramSelectionChanged() {
    const bool has_selection = m_program_table->currentRow() >= 0;
    m_uninstall_button->setEnabled(has_selection);
    m_forced_uninstall_button->setEnabled(has_selection);
}

void AdvancedUninstallPanel::onProgramDoubleClicked(int row, int /*column*/) {
    auto* name_item = m_program_table->item(row, kColName);
    if (name_item == nullptr) {
        return;
    }
    const int idx = originalRowIndex(name_item);
    if (idx >= 0 && idx < m_filteredPrograms.size()) {
        showProgramProperties(m_filteredPrograms[idx]);
    }
}

void AdvancedUninstallPanel::onProgramContextMenu(const QPoint& pos) {
    auto* item = m_program_table->itemAt(pos);
    if (item == nullptr) {
        return;
    }

    const int row = item->row();
    auto* name_item = m_program_table->item(row, kColName);
    if (name_item == nullptr) {
        return;
    }
    const int idx = originalRowIndex(name_item);
    if (idx < 0 || idx >= m_filteredPrograms.size()) {
        return;
    }

    // Select this row so context actions work correctly
    m_program_table->setCurrentCell(row, 0);

    const auto& program = m_filteredPrograms[idx];

    QMenu menu(this);
    menu.addAction(tr("Uninstall"), this, &AdvancedUninstallPanel::contextUninstall);
    menu.addAction(tr("Forced Uninstall"), this, &AdvancedUninstallPanel::contextForcedUninstall);
    menu.addSeparator();
    menu.addAction(tr("Add to Batch Queue"), this, &AdvancedUninstallPanel::contextAddToQueue);
    menu.addSeparator();

    auto* open_loc_action = menu.addAction(tr("Open Install Location"),
                                           this,
                                           &AdvancedUninstallPanel::contextOpenInstallLocation);
    open_loc_action->setEnabled(!program.installLocation.isEmpty());

    menu.addAction(tr("Copy Program Name"), this, &AdvancedUninstallPanel::contextCopyProgramName);

    auto* copy_uninstall_action = menu.addAction(
        tr("Copy Uninstall Command"), this, &AdvancedUninstallPanel::contextCopyUninstallCommand);
    copy_uninstall_action->setEnabled(!program.uninstallString.isEmpty());

    menu.addSeparator();
    menu.addAction(tr("Remove Registry Entry"),
                   this,
                   &AdvancedUninstallPanel::contextRemoveRegistryEntry);
    menu.addSeparator();
    menu.addAction(tr("Properties..."), this, &AdvancedUninstallPanel::contextShowProperties);

    menu.exec(m_program_table->viewport()->mapToGlobal(pos));
}

// -- Leftover Table Slots ----------------------------------------------------

void AdvancedUninstallPanel::onLeftoverSelectionChanged() {
    // Update delete button state based on checked items
    bool any_checked = false;
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* check_item = m_leftover_table->item(row, kLeftoverColCheck);
        if ((check_item != nullptr) && check_item->checkState() == Qt::Checked) {
            any_checked = true;
            break;
        }
    }
    m_delete_selected_button->setEnabled(any_checked);
    updateStatusCounts();
}

void AdvancedUninstallPanel::onSelectAll() {
    m_leftover_table->blockSignals(true);
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* check_item = m_leftover_table->item(row, kLeftoverColCheck);
        if (check_item == nullptr) {
            continue;
        }

        check_item->setCheckState(Qt::Checked);

        auto* type_item = m_leftover_table->item(row, kLeftoverColType);
        if (type_item == nullptr) {
            continue;
        }

        const int idx = originalRowIndex(type_item);
        if (idx < 0 || idx >= m_currentLeftovers.size()) {
            continue;
        }
        m_currentLeftovers[idx].selected = true;
    }
    m_leftover_table->blockSignals(false);
    onLeftoverSelectionChanged();
    logMessage("Selected all leftover items.");
}

void AdvancedUninstallPanel::onSelectAllSafe() {
    m_leftover_table->blockSignals(true);
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* type_item = m_leftover_table->item(row, kLeftoverColType);
        if (type_item == nullptr) {
            continue;
        }
        const int idx = originalRowIndex(type_item);
        if (idx >= 0 && idx < m_currentLeftovers.size() && m_currentLeftovers[idx].deletable &&
            m_currentLeftovers[idx].risk == LeftoverItem::RiskLevel::Safe) {
            auto* check_item = m_leftover_table->item(row, kLeftoverColCheck);
            if (check_item != nullptr) {
                check_item->setCheckState(Qt::Checked);
                m_currentLeftovers[idx].selected = true;
            }
        }
    }
    m_leftover_table->blockSignals(false);
    onLeftoverSelectionChanged();
    logMessage("Selected all safe leftover items.");
}

void AdvancedUninstallPanel::onDeselectAll() {
    m_leftover_table->blockSignals(true);
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* check_item = m_leftover_table->item(row, kLeftoverColCheck);
        if (check_item == nullptr) {
            continue;
        }

        check_item->setCheckState(Qt::Unchecked);

        auto* type_item = m_leftover_table->item(row, kLeftoverColType);
        if (type_item == nullptr) {
            continue;
        }

        const int idx = originalRowIndex(type_item);
        if (idx < 0 || idx >= m_currentLeftovers.size()) {
            continue;
        }
        m_currentLeftovers[idx].selected = false;
    }
    m_leftover_table->blockSignals(false);
    onLeftoverSelectionChanged();
}

void AdvancedUninstallPanel::onDeleteSelectedLeftovers() {
    bool all_resolved = true;
    auto leftovers = selectedLeftovers(&all_resolved);
    if (!all_resolved) {
        // Fail closed: deleting only the rows that still resolve would act on part of the
        // selection the user confirmed, and the rest would vanish silently.
        sak::showWarningLogged(
            this,
            tr("Selection Out Of Date"),
            tr("One or more checked leftover items could not be resolved from the current scan "
               "results. Re-scan and check them again -- nothing was deleted."));
        return;
    }
    if (leftovers.isEmpty()) {
        return;
    }

    // Count items by risk
    int safe = 0, review = 0, risky = 0;
    for (const auto& item : leftovers) {
        switch (item.risk) {
        case LeftoverItem::RiskLevel::Safe:
            ++safe;
            break;
        case LeftoverItem::RiskLevel::Review:
            ++review;
            break;
        case LeftoverItem::RiskLevel::Risky:
            ++risky;
            break;
        }
    }

    QString msg = tr("Delete %1 selected leftover items?\n\n"
                     "Safe: %2  |  Review: %3  |  Risky: %4\n\n"
                     "This action cannot be undone.")
                      .arg(leftovers.size())
                      .arg(safe)
                      .arg(review)
                      .arg(risky);

    if (risky > 0) {
        msg += tr("\n\nWARNING: %1 item(s) are classified as RISKY. "
                  "Removing them may affect system stability.")
                   .arg(risky);
    }

    auto result = sak::showWarningLogged(
        this, tr("Confirm Deletion"), msg, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_controller->cleanLeftovers(leftovers);
    }
}

// -- Context Menu Actions ----------------------------------------------------

void AdvancedUninstallPanel::contextUninstall() {
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        showUninstallConfirmation(program);
    }
}

void AdvancedUninstallPanel::contextForcedUninstall() {
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        showForcedUninstallDialog(program);
    }
}

void AdvancedUninstallPanel::contextAddToQueue() {
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        m_controller->addToQueue(program,
                                 m_controller->defaultScanLevel(),
                                 m_controller->autoCleanSafe());
        logMessage(QString("Added to batch queue: %1").arg(program.displayName));
        Q_EMIT statusMessage(tr("Added %1 to batch queue.").arg(program.displayName),
                             sak::kTimerStatusMessageMs);
    }
}

void AdvancedUninstallPanel::contextOpenInstallLocation() {
    auto program = selectedProgram();
    if (program.installLocation.isEmpty()) {
        return;
    }

    // installLocation comes from the uninstall registry, and HKCU is writable without elevation,
    // so it is untrusted. "Open Install Location" must open a local DIRECTORY only: handing a
    // file, .exe, .lnk, .url, or a UNC/device path to the shell would launch or fetch attacker-
    // controlled content with this (possibly elevated) process's rights, or trigger network
    // authentication. Fail closed on anything that is not an existing on-disk folder.
    const QString& location = program.installLocation;
    if (location.startsWith(QLatin1String("\\\\")) || location.startsWith(QLatin1String("//"))) {
        sak::showWarningLogged(
            this,
            tr("Cannot Open Location"),
            tr("The install location is a network or device path and was not opened."));
        return;
    }

    const QFileInfo info(location);
    if (!info.exists() || !info.isDir()) {
        sak::showWarningLogged(this,
                               tr("Cannot Open Location"),
                               tr("The install location for this program is not a valid folder."));
        return;
    }

    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() || !QDesktopServices::openUrl(QUrl::fromLocalFile(canonical))) {
        sak::showWarningLogged(this,
                               tr("Cannot Open Location"),
                               tr("The install location could not be opened."));
    }
}

void AdvancedUninstallPanel::contextCopyProgramName() {
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        QApplication::clipboard()->setText(program.displayName);
        Q_EMIT statusMessage(tr("Program name copied to clipboard."), sak::kTimerStatusBriefMs);
    }
}

void AdvancedUninstallPanel::contextCopyUninstallCommand() {
    auto program = selectedProgram();
    if (!program.uninstallString.isEmpty()) {
        QApplication::clipboard()->setText(program.uninstallString);
        Q_EMIT statusMessage(tr("Uninstall command copied to clipboard."),
                             sak::kTimerStatusBriefMs);
    }
}

void AdvancedUninstallPanel::contextShowProperties() {
    auto program = selectedProgram();
    if (!program.displayName.isEmpty()) {
        showProgramProperties(program);
    }
}

void AdvancedUninstallPanel::contextRemoveRegistryEntry() {
    auto program = selectedProgram();
    if (program.displayName.isEmpty()) {
        return;
    }

    auto result =
        sak::showWarningLogged(this,
                               tr("Remove Registry Entry"),
                               tr("Remove the registry entry for '%1'?\n\n"
                                  "This will only remove the program's registration in Windows. "
                                  "Program files will NOT be deleted.\n\n"
                                  "This is useful for orphaned entries where the program "
                                  "is already uninstalled.")
                                   .arg(program.displayName.toHtmlEscaped()),
                               QMessageBox::Yes | QMessageBox::No,
                               QMessageBox::No);

    if (result == QMessageBox::Yes) {
        m_controller->removeRegistryEntry(program);
    }
}

// -- Table Population --------------------------------------------------------

void AdvancedUninstallPanel::populateProgramTable(const QVector<ProgramInfo>& programs) {
    m_program_table->setSortingEnabled(false);
    m_program_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_program_table);
    m_program_table->setRowCount(0);
    m_program_table->setRowCount(static_cast<int>(programs.size()));

    for (int row = 0; row < programs.size(); ++row) {
        const auto& prog = programs[row];

        // Check column
        auto* check_item = new QTableWidgetItem();
        check_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
        check_item->setCheckState(Qt::Unchecked);
        m_program_table->setItem(row, kColCheck, check_item);

        // Icon column
        auto* icon_item = new QTableWidgetItem();
        if (!prog.cachedImage.isNull()) {
            icon_item->setIcon(QIcon(QPixmap::fromImage(prog.cachedImage)));
        }
        icon_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColIcon, icon_item);

        // Name
        auto* name_item = new QTableWidgetItem(prog.displayName);
        name_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        name_item->setData(kOriginalIndexRole, row);  // Store original index for sort-safe lookup
        // Color code bloatware and orphaned
        if (prog.isBloatware) {
            name_item->setForeground(QColor(ui::kColorWarning));
            name_item->setToolTip(tr("Potential bloatware"));
        }
        if (prog.isOrphaned) {
            name_item->setForeground(QColor(ui::kColorError));
            name_item->setToolTip(tr("Orphaned -- program files not found"));
        }
        m_program_table->setItem(row, kColName, name_item);

        // Publisher
        auto* pub_item = new QTableWidgetItem(prog.publisher);
        pub_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColPublisher, pub_item);

        // Version
        auto* ver_item = new QTableWidgetItem(prog.displayVersion);
        ver_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColVersion, ver_item);

        // Size (uses NumericSortItem for correct numeric sorting)
        auto* size_item =
            new NumericSortItem(formatSize(prog.estimatedSizeKB * kEstimatedSizeBytesPerKb));
        size_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        size_item->setData(Qt::UserRole, prog.estimatedSizeKB * kEstimatedSizeBytesPerKb);
        size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_program_table->setItem(row, kColSize, size_item);

        // Install date
        auto* date_item = new QTableWidgetItem(prog.installDate);
        date_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        m_program_table->setItem(row, kColDate, date_item);
    }

    m_program_table->setUpdatesEnabled(true);
    m_program_table->setSortingEnabled(true);
    m_program_table->sortByColumn(kColName, Qt::AscendingOrder);

    updateStatusCounts();
}

void AdvancedUninstallPanel::populateLeftoverTable(const QVector<LeftoverItem>& leftovers) {
    m_leftover_table->setSortingEnabled(false);
    m_leftover_table->setUpdatesEnabled(false);
    const QSignalBlocker leftover_blocker(m_leftover_table);
    m_leftover_table->setRowCount(0);
    m_leftover_table->setRowCount(static_cast<int>(leftovers.size()));

    for (int row = 0; row < leftovers.size(); ++row) {
        populateLeftoverRow(row, leftovers[row]);
    }

    m_leftover_table->setUpdatesEnabled(true);
    m_leftover_table->setSortingEnabled(true);

    int safe_count = 0;
    for (const auto& item : leftovers) {
        if (item.risk == LeftoverItem::RiskLevel::Safe) {
            ++safe_count;
        }
    }
    m_leftover_count_label->setText(tr("%1 items (%2 safe)").arg(leftovers.size()).arg(safe_count));
}

QString AdvancedUninstallPanel::leftoverTypeText(LeftoverItem::Type type) const {
    switch (type) {
    case LeftoverItem::Type::File:
        return tr("File");
    case LeftoverItem::Type::Folder:
        return tr("Folder");
    case LeftoverItem::Type::RegistryKey:
        return tr("Registry Key");
    case LeftoverItem::Type::RegistryValue:
        return tr("Registry Value");
    case LeftoverItem::Type::Service:
        return tr("Service");
    case LeftoverItem::Type::ScheduledTask:
        return tr("Scheduled Task");
    case LeftoverItem::Type::FirewallRule:
        return tr("Firewall Rule");
    case LeftoverItem::Type::StartupEntry:
        return tr("Startup Entry");
    case LeftoverItem::Type::ShellExtension:
        return tr("Shell Extension");
    }
    return {};
}

void AdvancedUninstallPanel::populateLeftoverRow(int row, const LeftoverItem& item) {
    auto* check_item = new QTableWidgetItem();
    // A report-only leftover is shown but must not be checkable at all: the scanner could
    // not tie that path to this program, so it carries no deletion authority (the
    // controller and CleanupWorker refuse it again regardless of what the table says).
    check_item->setFlags(item.deletable ? (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled)
                                        : Qt::ItemIsEnabled);
    check_item->setCheckState(item.selected && item.deletable ? Qt::Checked : Qt::Unchecked);
    m_leftover_table->setItem(row, kLeftoverColCheck, check_item);

    // Risk level
    QString risk_text;
    QColor risk_color;
    switch (item.risk) {
    case LeftoverItem::RiskLevel::Safe:
        risk_text = tr("Safe");
        risk_color = QColor(ui::kColorSuccess);
        break;
    case LeftoverItem::RiskLevel::Review:
        risk_text = tr("Review");
        risk_color = QColor(ui::kColorWarning);
        break;
    case LeftoverItem::RiskLevel::Risky:
        risk_text = tr("Risky");
        risk_color = QColor(ui::kColorError);
        break;
    }
    auto* risk_item = new QTableWidgetItem(risk_text);
    risk_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    risk_item->setForeground(risk_color);
    QFont risk_font = risk_item->font();
    risk_font.setBold(true);
    risk_item->setFont(risk_font);
    m_leftover_table->setItem(row, kLeftoverColRisk, risk_item);

    // Type
    auto* type_item = new QTableWidgetItem(leftoverTypeText(item.type));
    type_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    type_item->setData(kOriginalIndexRole, row);  // Store original index for sort-safe lookup
    m_leftover_table->setItem(row, kLeftoverColType, type_item);

    // Path
    auto* path_item = new QTableWidgetItem(item.path);
    path_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    // Leftover paths come from disk and from the (HKCU-writable) uninstall registry keys, and a
    // tooltip has no plain-text mode, so wrap it to be shown literally.
    path_item->setToolTip(ui::asLiteralRichText(item.path));
    m_leftover_table->setItem(row, kLeftoverColPath, path_item);

    // Size (uses NumericSortItem for correct numeric sorting)
    auto* size_item = new NumericSortItem(formatSize(item.sizeBytes));
    size_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    size_item->setData(Qt::UserRole, item.sizeBytes);
    size_item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_leftover_table->setItem(row, kLeftoverColSize, size_item);
}

void AdvancedUninstallPanel::clearLeftoverTable() {
    m_leftover_table->setRowCount(0);
    m_currentLeftovers.clear();
    m_leftover_count_label->clear();
}

// -- Filtering ---------------------------------------------------------------

void AdvancedUninstallPanel::applyFilter() {
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

static bool passesViewFilter(const ProgramInfo& program, ViewFilter view_filter) {
    switch (view_filter) {
    case ViewFilter::All:
        return true;
    case ViewFilter::Win32Only:
        return program.source != ProgramInfo::Source::UWP &&
               program.source != ProgramInfo::Source::Provisioned;
    case ViewFilter::UwpOnly:
        return program.source == ProgramInfo::Source::UWP ||
               program.source == ProgramInfo::Source::Provisioned;
    case ViewFilter::BloatwareOnly:
        return program.isBloatware;
    case ViewFilter::OrphanedOnly:
        return program.isOrphaned;
    }
    return true;
}

bool AdvancedUninstallPanel::matchesFilter(const ProgramInfo& program) const {
    if (!passesViewFilter(program, m_viewFilter)) {
        return false;
    }

    if (program.isSystemComponent && !m_controller->showSystemComponents()) {
        return false;
    }

    if (!m_searchFilter.isEmpty()) {
        const bool matches = program.displayName.contains(m_searchFilter, Qt::CaseInsensitive) ||
                             program.publisher.contains(m_searchFilter, Qt::CaseInsensitive) ||
                             program.displayVersion.contains(m_searchFilter, Qt::CaseInsensitive);
        if (!matches) {
            return false;
        }
    }

    return true;
}

// -- Utility -----------------------------------------------------------------

QString AdvancedUninstallPanel::formatSize(qint64 bytes) const {
    if (bytes <= 0) {
        return QString();
    }
    return sak::formatBytes(bytes);
}

void AdvancedUninstallPanel::logMessage(const QString& message) {
    Q_EMIT logOutput(message);
}

void AdvancedUninstallPanel::setOperationRunning(bool running) {
    // R5-G20-2: the Cancel/Stop button is the inverse of every other control -- shown and enabled
    // only WHILE an op runs, hidden+disabled otherwise. Every start handler passes running=true
    // and every terminal handler (success, error, and cancel alike) passes running=false, so it is
    // never stranded enabled after the op ends nor stuck disabled while one is in flight.
    m_cancel_button->setVisible(running);
    m_cancel_button->setEnabled(running);

    m_refresh_button->setEnabled(!running);
    m_batch_button->setEnabled(!running);
    m_search_edit->setEnabled(!running);
    m_view_filter_combo->setEnabled(!running);
    m_settings_button->setEnabled(!running);
    m_program_table->setEnabled(!running);

    // Leftover action buttons
    m_select_all_button->setEnabled(!running);
    m_select_safe_button->setEnabled(!running);
    m_deselect_all_button->setEnabled(!running);
    if (running) {
        m_delete_selected_button->setEnabled(false);
    }

    if (running) {
        m_uninstall_button->setEnabled(false);
        m_forced_uninstall_button->setEnabled(false);
    } else {
        // Re-sync button states based on actual selection
        onProgramSelectionChanged();
        onLeftoverSelectionChanged();
    }
}

ProgramInfo AdvancedUninstallPanel::selectedProgram() const {
    const int row = m_program_table->currentRow();
    if (row < 0) {
        return {};
    }
    auto* name_item = m_program_table->item(row, kColName);
    if (name_item == nullptr) {
        return {};
    }
    const int idx = originalRowIndex(name_item);
    if (idx >= 0 && idx < m_filteredPrograms.size()) {
        return m_filteredPrograms[idx];
    }
    return {};
}

QVector<ProgramInfo> AdvancedUninstallPanel::selectedPrograms(bool* all_resolved) const {
    QVector<ProgramInfo> result;
    if (all_resolved != nullptr) {
        *all_resolved = true;
    }
    for (int row = 0; row < m_program_table->rowCount(); ++row) {
        auto* check_item = m_program_table->item(row, kColCheck);
        if ((check_item != nullptr) && check_item->checkState() == Qt::Checked) {
            // A CHECKED row that cannot be resolved back to a program is reported, never
            // quietly skipped: dropping it would run the batch against a subset of what the
            // user checked while the UI still showed the full selection.
            auto* name_item = m_program_table->item(row, kColName);
            const int idx = (name_item != nullptr) ? originalRowIndex(name_item) : -1;
            if (idx >= 0 && idx < m_filteredPrograms.size()) {
                result.append(m_filteredPrograms[idx]);
            } else if (all_resolved != nullptr) {
                *all_resolved = false;
            }
        }
    }
    return result;
}

QVector<LeftoverItem> AdvancedUninstallPanel::selectedLeftovers(bool* all_resolved) const {
    QVector<LeftoverItem> result;
    if (all_resolved != nullptr) {
        *all_resolved = true;
    }
    for (int row = 0; row < m_leftover_table->rowCount(); ++row) {
        auto* check_item = m_leftover_table->item(row, kLeftoverColCheck);
        if ((check_item != nullptr) && check_item->checkState() == Qt::Checked) {
            // Same all-or-nothing contract as selectedPrograms: an unresolvable checked row is
            // surfaced so the deletion can refuse rather than act on part of the selection.
            auto* type_item = m_leftover_table->item(row, kLeftoverColType);
            const int idx = (type_item != nullptr) ? originalRowIndex(type_item) : -1;
            if (idx >= 0 && idx < m_currentLeftovers.size()) {
                auto item = m_currentLeftovers[idx];
                item.selected = true;
                result.append(item);
            } else if (all_resolved != nullptr) {
                *all_resolved = false;
            }
        }
    }
    return result;
}

void AdvancedUninstallPanel::updateStatusCounts() {
    m_program_count_label->setText(tr("%1 programs").arg(m_filteredPrograms.size()));

    qint64 total_size = 0;
    for (const auto& prog : m_filteredPrograms) {
        total_size += prog.estimatedSizeKB * kEstimatedSizeBytesPerKb;
    }

    if (total_size > 0) {
        m_total_size_label->setText(tr("Total: %1").arg(formatSize(total_size)));
    } else {
        m_total_size_label->clear();
    }
}

}  // namespace sak
