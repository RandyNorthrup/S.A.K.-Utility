// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_widget.cpp
/// @brief UI widget for the OST/PST Converter tab

#include "sak/ost_converter_widget.h"

#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/ost_converter_constants.h"
#include "sak/ost_converter_controller.h"
#include "sak/portable_settings.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <initializer_list>

namespace sak {

namespace {

constexpr int kQueueTableMinHeight = 120;

void applyOstGroupLayout(QVBoxLayout* layout) {
    Q_ASSERT(layout != nullptr);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kSpacingSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingSmall);
}

void applyCompactOstButton(QPushButton* button, const QString& style) {
    Q_ASSERT(button != nullptr);
    button->setMinimumHeight(ui::kUiButtonHeightDialog);
    button->setMaximumHeight(ui::kUiButtonHeightDialog);
    button->setStyleSheet(style);
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

OstConverterWidget::OstConverterWidget(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<OstConverterController>(this)) {
    setupUi();
    connectController();
    loadSettings();
}

OstConverterWidget::~OstConverterWidget() {
    saveSettings();
}

// ============================================================================
// UI Setup
// ============================================================================

void OstConverterWidget::setupUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginSmall, ui::kMarginMedium, ui::kMarginMedium);
    root_layout->setSpacing(ui::kSpacingSmall);

    // Scroll area for the full form
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* scroll_content = new QWidget(scroll);
    auto* content_layout = new QVBoxLayout(scroll_content);
    content_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    content_layout->setSpacing(ui::kSpacingMedium);

    content_layout->addWidget(createFileQueueSection());
    content_layout->addWidget(createOutputSettingsSection());
    content_layout->addWidget(createFilterSection());
    content_layout->addWidget(createRecoverySection());
    content_layout->addWidget(createButtonBar());
    content_layout->addStretch(1);

    scroll->setWidget(scroll_content);
    root_layout->addWidget(scroll, 1);
}

void OstConverterWidget::connectController() {
    connect(m_controller.get(),
            &OstConverterController::fileAdded,
            this,
            &OstConverterWidget::onFileAdded);
    connect(m_controller.get(),
            &OstConverterController::fileRemoved,
            this,
            &OstConverterWidget::onFileRemoved);
    connect(m_controller.get(),
            &OstConverterController::queueCleared,
            this,
            &OstConverterWidget::onQueueCleared);
    connect(m_controller.get(),
            &OstConverterController::conversionStarted,
            this,
            &OstConverterWidget::onConversionStarted);
    connect(m_controller.get(),
            &OstConverterController::fileConversionStarted,
            this,
            &OstConverterWidget::onFileConversionStarted);
    connect(m_controller.get(),
            &OstConverterController::fileProgressUpdated,
            this,
            &OstConverterWidget::onFileProgressUpdated);
    connect(m_controller.get(),
            &OstConverterController::fileConversionComplete,
            this,
            &OstConverterWidget::onFileConversionComplete);
    connect(m_controller.get(),
            &OstConverterController::allConversionsComplete,
            this,
            &OstConverterWidget::onAllConversionsComplete);
    connect(m_controller.get(),
            &OstConverterController::errorOccurred,
            this,
            &OstConverterWidget::onErrorOccurred);
    connect(m_controller.get(),
            &OstConverterController::statusMessage,
            this,
            &OstConverterWidget::statusMessage);
}

// ============================================================================
// Section Builders
// ============================================================================

QWidget* OstConverterWidget::createFileQueueSection() {
    auto* group = new QGroupBox(tr("Source Files"), this);
    auto* layout = new QVBoxLayout(group);
    applyOstGroupLayout(layout);

    // Button bar
    auto* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(ui::kSpacingSmall);

    m_add_files_button = new QPushButton(tr("+ Add Files"), group);
    m_add_files_button->setAccessibleName(tr("+ Add Files"));
    m_add_files_button->setAccessibleDescription(tr("Add email files"));
    m_add_files_button->setToolTip(tr("Add OST or PST files to the conversion queue"));
    applyCompactOstButton(m_add_files_button, ui::kCompactPrimaryButtonStyle);
    m_remove_button = new QPushButton(tr("Remove"), group);
    m_remove_button->setAccessibleName(tr("Remove selected email file"));
    m_remove_button->setToolTip(tr("Remove selected file from the queue"));
    applyCompactOstButton(m_remove_button, ui::kCompactSecondaryButtonStyle);
    m_clear_button = new QPushButton(tr("Clear All"), group);
    m_clear_button->setAccessibleName(tr("Clear All"));
    m_clear_button->setAccessibleDescription(tr("Clear conversion queue"));
    m_clear_button->setToolTip(tr("Remove all files from the queue"));
    applyCompactOstButton(m_clear_button, ui::kCompactSecondaryButtonStyle);

    btn_layout->addWidget(m_add_files_button);
    btn_layout->addWidget(m_remove_button);
    btn_layout->addWidget(m_clear_button);
    btn_layout->addStretch(1);
    layout->addLayout(btn_layout);

    connect(m_add_files_button,
            &QPushButton::clicked,
            this,
            &OstConverterWidget::onAddFilesClicked,
            Qt::QueuedConnection);
    connect(m_remove_button, &QPushButton::clicked, this, &OstConverterWidget::onRemoveFileClicked);
    connect(m_clear_button, &QPushButton::clicked, this, &OstConverterWidget::onClearQueueClicked);

    // Queue table
    m_queue_table = new QTableWidget(0, ost::ColCount, group);
    m_queue_table->setAccessibleName(tr("Email conversion queue"));
    m_queue_table->setHorizontalHeaderLabels(
        {tr("File"), tr("Size"), tr("Status"), tr("Progress")});
    configureStandardTable(m_queue_table, QAbstractItemView::SingleSelection);
    m_queue_table->horizontalHeader()->setStretchLastSection(true);
    m_queue_table->setMinimumHeight(kQueueTableMinHeight);

    // Column widths
    m_queue_table->setColumnWidth(ost::ColFile, ost::kQueueFileColumnWidth);
    m_queue_table->setColumnWidth(ost::ColSize, ost::kQueueSizeColumnWidth);
    m_queue_table->setColumnWidth(ost::ColStatus, ost::kQueueStatusColumnWidth);

    layout->addWidget(m_queue_table, 1);

    return group;
}

void OstConverterWidget::addOutputFormatRow(QVBoxLayout* layout, QWidget* group) {
    // No format picker. This tab converts a store to MBOX, the mailbox format other mail
    // clients import; per-message output (EML, HTML, Text, PDF, CSV) is the Email Tools
    // inspector's job, from its folder tree.
    auto* destination_row = new QHBoxLayout();
    destination_row->setSpacing(ui::kSpacingSmall);
    destination_row->addWidget(new QLabel(tr("Destination:"), group));
    m_output_dir_edit = new QLineEdit(group);
    m_output_dir_edit->setAccessibleName(tr("Output directory"));
    m_output_dir_edit->setPlaceholderText(tr("Select output directory..."));
    m_output_dir_edit->setReadOnly(true);
    destination_row->addWidget(m_output_dir_edit, 1);

    m_browse_button = new QPushButton(tr("Browse"), group);
    m_browse_button->setAccessibleName(tr("Browse output directory"));
    m_browse_button->setToolTip(tr("Select the folder that will receive converted email files"));
    applyCompactOstButton(m_browse_button, ui::kCompactPrimaryButtonStyle);
    connect(
        m_browse_button,
        &QPushButton::clicked,
        this,
        [this]() {
            const QString dir = QFileDialog::getExistingDirectory(this,
                                                                  tr("Select Output Directory"),
                                                                  m_output_dir_edit->text());
            if (!dir.isEmpty()) {
                m_output_dir_edit->setText(QDir::toNativeSeparators(dir));
            }
        },
        Qt::QueuedConnection);
    destination_row->addWidget(m_browse_button);

    layout->addLayout(destination_row);
}

void OstConverterWidget::addOutputOptionsRow(QVBoxLayout* layout, QWidget* group) {
    auto* options_row = new QHBoxLayout();
    options_row->setSpacing(ui::kSpacingMedium);

    // The one real MBOX choice, and until now it had no control at all: the config field
    // existed and the writer honoured it, but nothing could set it, so every conversion ran
    // on the default.
    m_mbox_per_folder_check = new QCheckBox(tr("One MBOX file per folder"), group);
    m_mbox_per_folder_check->setAccessibleName(tr("One MBOX file per folder"));
    m_mbox_per_folder_check->setChecked(true);
    m_mbox_per_folder_check->setToolTip(
        tr("Write one .mbox per source folder, preserving the folder tree. Unchecked, the "
           "whole store becomes a single mailbox.mbox."));
    options_row->addWidget(m_mbox_per_folder_check);

    options_row->addStretch(1);

    options_row->addWidget(new QLabel(tr("Threads:"), group));
    m_threads_spin = new QSpinBox(group);
    m_threads_spin->setAccessibleName(tr("Conversion thread count"));
    m_threads_spin->setRange(ost::kMinThreads, ost::kMaxThreads);
    m_threads_spin->setValue(ost::kDefaultThreads);
    m_threads_spin->setToolTip(
        tr("Number of concurrent file conversions (1-%1)").arg(ost::kMaxThreads));
    options_row->addWidget(m_threads_spin);

    layout->addLayout(options_row);
}

QWidget* OstConverterWidget::createOutputSettingsSection() {
    auto* group = new QGroupBox(tr("Output Settings"), this);
    auto* layout = new QVBoxLayout(group);
    applyOstGroupLayout(layout);

    addOutputFormatRow(layout, group);
    addOutputOptionsRow(layout, group);
    return group;
}

QWidget* OstConverterWidget::createFilterSection() {
    m_filter_group = new QGroupBox(tr("Filters (optional)"), this);

    auto* layout = new QVBoxLayout(m_filter_group);
    applyOstGroupLayout(layout);

    m_filters_enabled_check = new QCheckBox(tr("Enable filters"), m_filter_group);
    m_filters_enabled_check->setAccessibleName(tr("Enable filters"));
    m_filters_enabled_check->setAccessibleDescription(tr("Enable email conversion filters"));
    m_filters_enabled_check->setToolTip(
        tr("Filter converted messages by date, sender, or recipient"));
    layout->addWidget(m_filters_enabled_check);

    addFilterDateRow(layout);
    addFilterTextRows(layout);

    connect(m_filters_enabled_check,
            &QCheckBox::toggled,
            this,
            &OstConverterWidget::updateFilterControlsEnabled);
    connect(m_date_filter_check,
            &QCheckBox::toggled,
            this,
            &OstConverterWidget::updateFilterControlsEnabled);
    updateFilterControlsEnabled();

    return m_filter_group;
}

void OstConverterWidget::addFilterDateRow(QVBoxLayout* layout) {
    // Date range row
    auto* date_row = new QHBoxLayout();
    date_row->setSpacing(ui::kSpacingSmall);

    m_date_filter_check = new QCheckBox(tr("Date range:"), m_filter_group);
    m_date_filter_check->setAccessibleName(tr("Enable date range filter"));
    date_row->addWidget(m_date_filter_check);

    m_date_from_edit = new QDateEdit(m_filter_group);
    m_date_from_edit->setAccessibleName(tr("Filter start date"));
    m_date_from_edit->setCalendarPopup(true);
    m_date_from_edit->setDate(QDate::currentDate().addYears(-1));
    m_date_from_edit->setEnabled(false);
    date_row->addWidget(m_date_from_edit);

    date_row->addWidget(new QLabel(tr("to"), m_filter_group));

    m_date_to_edit = new QDateEdit(m_filter_group);
    m_date_to_edit->setAccessibleName(tr("Filter end date"));
    m_date_to_edit->setCalendarPopup(true);
    m_date_to_edit->setDate(QDate::currentDate());
    m_date_to_edit->setEnabled(false);
    date_row->addWidget(m_date_to_edit);

    date_row->addStretch(1);
    layout->addLayout(date_row);
}

void OstConverterWidget::addFilterTextRows(QVBoxLayout* layout) {
    // Sender filter row
    auto* sender_row = new QHBoxLayout();
    sender_row->setSpacing(ui::kSpacingSmall);
    auto* sender_label = new QLabel(tr("Sender:"), m_filter_group);
    sender_row->addWidget(sender_label);
    m_sender_filter_edit = new QLineEdit(m_filter_group);
    m_sender_filter_edit->setAccessibleName(tr("Sender filter"));
    m_sender_filter_edit->setPlaceholderText(tr("Filter by sender email (contains)"));
    sender_row->addWidget(m_sender_filter_edit, 1);
    layout->addLayout(sender_row);

    // Recipient filter row
    auto* recip_row = new QHBoxLayout();
    recip_row->setSpacing(ui::kSpacingSmall);
    auto* recipient_label = new QLabel(tr("Recipient:"), m_filter_group);
    recip_row->addWidget(recipient_label);
    m_recipient_filter_edit = new QLineEdit(m_filter_group);
    m_recipient_filter_edit->setAccessibleName(tr("Recipient filter"));
    m_recipient_filter_edit->setPlaceholderText(tr("Filter by recipient email (contains)"));
    recip_row->addWidget(m_recipient_filter_edit, 1);
    layout->addLayout(recip_row);

    // The date range's box and the two labels lead their rows at one width, so the three fields
    // start at one edge; they started at 120, 96 and 108 px (review 2, S17).
    const std::initializer_list<QWidget*> leads{m_date_filter_check, sender_label, recipient_label};
    int lead_width = 0;
    for (QWidget* lead : leads) {
        lead_width = std::max(lead_width, lead->sizeHint().width());
    }
    for (QWidget* lead : leads) {
        lead->setMinimumWidth(lead_width);
    }
}

QWidget* OstConverterWidget::createRecoverySection() {
    m_recovery_group = new QGroupBox(tr("Recovery Options"), this);

    auto* layout = new QVBoxLayout(m_recovery_group);
    applyOstGroupLayout(layout);

    m_recover_deleted_check = new QCheckBox(
        tr("Recover deleted items (scan Recoverable Items folder)"), m_recovery_group);
    m_recover_deleted_check->setAccessibleName(
        tr("Recover deleted items (scan Recoverable Items folder)"));
    m_recover_deleted_check->setAccessibleDescription(tr("Recover deleted email items"));
    layout->addWidget(m_recover_deleted_check);

    m_deep_recovery_check =
        new QCheckBox(tr("Deep recovery (scan orphaned nodes - slow, thorough)"), m_recovery_group);
    m_deep_recovery_check->setAccessibleName(
        tr("Deep recovery (scan orphaned nodes - slow, thorough)"));
    m_deep_recovery_check->setAccessibleDescription(tr("Enable deep email recovery"));
    m_deep_recovery_check->setToolTip(
        tr("Walk all NBT nodes to find hard-deleted messages not in any folder"));
    layout->addWidget(m_deep_recovery_check);

    return m_recovery_group;
}

QWidget* OstConverterWidget::createButtonBar() {
    auto* bar = new QWidget(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    layout->setSpacing(ui::kSpacingMedium);

    m_view_report_button = new QPushButton(tr("View Report"), bar);
    m_view_report_button->setAccessibleName(tr("View Report"));
    m_view_report_button->setAccessibleDescription(tr("View conversion report"));
    m_view_report_button->setEnabled(false);
    m_view_report_button->setToolTip(tr("Open the batch conversion report in your browser"));
    applyCompactOstButton(m_view_report_button, ui::kCompactSecondaryButtonStyle);
    connect(m_view_report_button,
            &QPushButton::clicked,
            this,
            &OstConverterWidget::onViewReportClicked);
    layout->addWidget(m_view_report_button);

    layout->addStretch(1);

    m_convert_button = new QPushButton(tr("Convert All"), bar);
    m_convert_button->setAccessibleName(tr("Convert all queued email files"));
    m_convert_button->setToolTip(tr("Start converting all queued files"));
    applyCompactOstButton(m_convert_button, ui::kCompactPrimaryButtonStyle);
    layout->addWidget(m_convert_button);

    m_cancel_button = new QPushButton(tr("Cancel"), bar);
    m_cancel_button->setAccessibleName(tr("Cancel email conversion"));
    m_cancel_button->setEnabled(false);
    m_cancel_button->setToolTip(tr("Cancel all in-progress conversions"));
    applyCompactOstButton(m_cancel_button, ui::kCompactDangerButtonStyle);
    layout->addWidget(m_cancel_button);

    connect(m_convert_button,
            &QPushButton::clicked,
            this,
            &OstConverterWidget::onConvertClicked,
            Qt::QueuedConnection);
    connect(m_cancel_button,
            &QPushButton::clicked,
            this,
            &OstConverterWidget::onCancelClicked,
            Qt::QueuedConnection);

    return bar;
}

// ============================================================================
// Slot Implementations -- File Queue
// ============================================================================

void OstConverterWidget::onAddFilesClicked() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Select OST/PST Files"),
        QString(),
        tr("Outlook Data Files (*.ost *.pst *.nst);;All Files (*.*)"));

    for (const auto& file : files) {
        m_controller->addFile(file);
    }
}

void OstConverterWidget::onRemoveFileClicked() {
    const int row = m_queue_table->currentRow();
    if (row >= 0) {
        m_controller->removeFile(row);
    }
}

void OstConverterWidget::onClearQueueClicked() {
    m_controller->clearQueue();
}

// ============================================================================
// Slot Implementations -- Conversion Control
// ============================================================================

void OstConverterWidget::onConvertClicked() {
    if (m_output_dir_edit->text().isEmpty()) {
        sak::showWarningLogged(this,
                               tr("Missing Output Directory"),
                               tr("Please select an output directory before converting."));
        sak::logWarning("OST Converter: conversion started without output directory");
        return;
    }

    if (m_controller->queue().isEmpty()) {
        sak::showWarningLogged(this,
                               tr("No Files"),
                               tr("Please add at least one file to the queue."));
        return;
    }

    auto config = buildConfig();
    m_controller->startConversion(config);
}

void OstConverterWidget::onCancelClicked() {
    m_controller->cancelAll();
    setConvertingState(false);
}

// ============================================================================
// Slot Implementations -- Controller Signals
// ============================================================================

void OstConverterWidget::onFileAdded(int /*index*/, OstConversionJob job) {
    const int row = m_queue_table->rowCount();
    m_queue_table->insertRow(row);
    updateQueueRow(row, job);
}

void OstConverterWidget::onFileRemoved(int index) {
    if (index >= 0 && index < m_queue_table->rowCount()) {
        m_queue_table->removeRow(index);
    }
}

void OstConverterWidget::onQueueCleared() {
    m_queue_table->setRowCount(0);
}

void OstConverterWidget::onConversionStarted(int total_files) {
    setConvertingState(true);
    m_total_files = total_files;
    m_files_done = 0;
    Q_EMIT statusMessage(
        tr("Starting conversion of %1...").arg(countOf(total_files, tr("file"), tr("files"))),
        kTimerStatusMessageMs);
    Q_EMIT progressUpdate(0, 0);  // Indeterminate while starting
}

void OstConverterWidget::onFileConversionStarted(int file_index) {
    if (file_index >= 0 && file_index < m_controller->queue().size()) {
        const auto& job = m_controller->queue().at(file_index);
        Q_EMIT statusMessage(tr("Converting: %1").arg(job.display_name), kTimerStatusMessageMs);
    }
}

void OstConverterWidget::onFileProgressUpdated(int file_index,
                                               int items_done,
                                               int items_total,
                                               QString current_folder) {
    Q_EMIT progressUpdate(items_done, items_total);

    // Update the queue table row
    if (file_index >= 0 && file_index < m_queue_table->rowCount()) {
        auto progress_text = tr("%1 / %2").arg(items_done).arg(items_total);
        auto* item = m_queue_table->item(file_index, ost::ColProgress);
        if (item != nullptr) {
            item->setText(progress_text);
        }
        auto* status_item = m_queue_table->item(file_index, ost::ColStatus);
        if (status_item != nullptr) {
            status_item->setText(tr("Converting"));
        }
    }

    Q_EMIT statusMessage(tr("Converting: %1 - %2")
                             .arg(current_folder)
                             .arg(tr("%1 / %2 items").arg(items_done).arg(items_total)),
                         0);
}

void OstConverterWidget::onFileConversionComplete(int file_index, OstConversionResult result) {
    ++m_files_done;

    Q_EMIT progressUpdate(m_files_done, m_total_files);

    if (file_index >= 0 && file_index < m_queue_table->rowCount()) {
        // One rule for what a run's outcome is, the one the batch totals and the report use: a
        // file with any failed item or recorded error did not convert cleanly, however much of
        // it was written.
        auto* status_item = m_queue_table->item(file_index, ost::ColStatus);
        if (status_item != nullptr) {
            status_item->setText(statusLabel(OstConverterController::classifyOutcome(result)));
        }
        auto* progress_item = m_queue_table->item(file_index, ost::ColProgress);
        if (progress_item != nullptr) {
            progress_item->setText(progressSummary(result));
        }
    }
}

void OstConverterWidget::onAllConversionsComplete(OstConversionBatchResult result) {
    setConvertingState(false);

    // Reset progress bar (0/0 hides it in MainWindow)
    Q_EMIT progressUpdate(0, 0);

    // Enable report button if a report was generated
    const QString report = m_controller->reportPath();
    m_view_report_button->setEnabled(!report.isEmpty());

    QString summary = tr("Conversion complete: %1/%2 files, %3 items")
                          .arg(result.files_succeeded)
                          .arg(result.files_total)
                          .arg(result.total_items_converted);
    if (result.total_items_failed > 0) {
        summary += tr(", %1 failed").arg(result.total_items_failed);
    }
    if (result.total_items_skipped > 0) {
        summary += tr(", %1 not mail and left out").arg(result.total_items_skipped);
    }
    Q_EMIT statusMessage(summary, kTimerStatusLongMs);
}

void OstConverterWidget::onErrorOccurred(int file_index, QString message) {
    Q_EMIT statusMessage(tr("Error: %1").arg(message), kTimerStatusWarnMs);

    if (file_index >= 0 && file_index < m_queue_table->rowCount()) {
        auto* status_item = m_queue_table->item(file_index, ost::ColStatus);
        if (status_item != nullptr) {
            status_item->setText(tr("Error"));
        }
    }
}

// ============================================================================
// Helpers
// ============================================================================

void OstConverterWidget::setConvertingState(bool converting) {
    m_add_files_button->setEnabled(!converting);
    m_remove_button->setEnabled(!converting);
    m_clear_button->setEnabled(!converting);
    m_convert_button->setEnabled(!converting);
    m_cancel_button->setEnabled(converting);
    m_browse_button->setEnabled(!converting);
    m_threads_spin->setEnabled(!converting);
    m_filter_group->setEnabled(!converting);
    m_recovery_group->setEnabled(!converting);
    m_view_report_button->setEnabled(false);
    if (!converting) {
        updateFilterControlsEnabled();
    }
}

void OstConverterWidget::updateFilterControlsEnabled() {
    const bool filters_enabled = m_filters_enabled_check != nullptr &&
                                 m_filters_enabled_check->isChecked();
    if (m_date_filter_check != nullptr) {
        m_date_filter_check->setEnabled(filters_enabled);
    }
    const bool date_filter_enabled = filters_enabled && m_date_filter_check != nullptr &&
                                     m_date_filter_check->isChecked();
    if (m_date_from_edit != nullptr) {
        m_date_from_edit->setEnabled(date_filter_enabled);
    }
    if (m_date_to_edit != nullptr) {
        m_date_to_edit->setEnabled(date_filter_enabled);
    }
    if (m_sender_filter_edit != nullptr) {
        m_sender_filter_edit->setEnabled(filters_enabled);
    }
    if (m_recipient_filter_edit != nullptr) {
        m_recipient_filter_edit->setEnabled(filters_enabled);
    }
}

OstConversionConfig OstConverterWidget::buildConfig() const {
    OstConversionConfig config;

    config.one_mbox_per_folder = m_mbox_per_folder_check->isChecked();
    config.output_directory = m_output_dir_edit->text();
    config.max_threads = m_threads_spin->value();
    config.recover_deleted_items = m_recover_deleted_check->isChecked();

    // Recovery mode
    if (m_deep_recovery_check->isChecked()) {
        config.recovery_mode = RecoveryMode::DeepRecovery;
    }

    const bool filters_enabled = m_filters_enabled_check != nullptr &&
                                 m_filters_enabled_check->isChecked();

    // Date filter. The range covers whole days: a QDateEdit's time is midnight, and using it for
    // the end date left out everything sent on that day.
    if (filters_enabled && m_date_filter_check->isChecked()) {
        config.date_from = m_date_from_edit->date().startOfDay();
        config.date_to = m_date_to_edit->date().endOfDay();
    }

    // Sender / recipient filters
    if (filters_enabled) {
        config.sender_filter = m_sender_filter_edit->text().trimmed();
        config.recipient_filter = m_recipient_filter_edit->text().trimmed();
    }

    return config;
}

void OstConverterWidget::updateQueueRow(int row, const OstConversionJob& job) {
    m_queue_table->setItem(row, ost::ColFile, new QTableWidgetItem(job.display_name));
    m_queue_table->setItem(row,
                           ost::ColSize,
                           new QTableWidgetItem(formatBytes(job.file_size_bytes)));
    m_queue_table->setItem(row, ost::ColStatus, new QTableWidgetItem(statusLabel(job.status)));
    m_queue_table->setItem(row, ost::ColProgress, new QTableWidgetItem(QString()));
}

QString OstConverterWidget::formatBytes(qint64 bytes) {
    // The shared compact byte style. This class carried its own copy; five existed across
    // the tree, differing only in which unit got which precision.
    return sak::formatBytesCompact(bytes);
}

QString OstConverterWidget::progressSummary(const OstConversionResult& result) {
    // A store can be whole and hold nothing, as a group-mail cache often does.
    if (result.items_converted == 0 && result.items_failed == 0 && result.itemsSkipped() == 0) {
        return tr("No messages in this file");
    }
    // "12 items, 13 not mail" read as 12 items of which 13 were not mail: name what each count is.
    QStringList parts{
        countOf(result.items_converted, tr("message converted"), tr("messages converted"))};
    if (result.items_failed > 0) {
        parts.append(tr("%1 failed").arg(result.items_failed));
    }
    if (result.itemsSkipped() > 0) {
        parts.append(tr("%1 left out (not mail)")
                         .arg(countOf(result.itemsSkipped(), tr("item"), tr("items"))));
    }
    return parts.join(QStringLiteral(", "));
}

QString OstConverterWidget::statusLabel(OstConversionJob::Status status) {
    switch (status) {
    case OstConversionJob::Status::Queued:
        return tr("Queued");
    case OstConversionJob::Status::Parsing:
        return tr("Parsing");
    case OstConversionJob::Status::Converting:
        return tr("Converting");
    case OstConversionJob::Status::Complete:
        return tr("Complete");
    case OstConversionJob::Status::Failed:
        return tr("Failed");
    case OstConversionJob::Status::Cancelled:
        return tr("Cancelled");
    }
    return tr("Unknown");
}

void OstConverterWidget::loadSettings() {
    PortableSettings settings;
    settings.beginGroup(QStringLiteral("OstConverter"));

    m_output_dir_edit->setText(settings.value(QStringLiteral("lastOutputDir")).toString());

    // Removing PST, DBX and IMAP upload renumbered OstOutputFormat, so an int stored under
    // the old "lastFormat" key now names a DIFFERENT format -- a saved DBX (4) would come
    // back as PDF. Read a new key instead and delete the old one, so a stale value restores
    // nothing rather than silently restoring the wrong format. Same for the IMAP fields,
    // which described features that no longer exist. Every key here named a setting for a
    // format the converter no longer produces, so leaving them would restore state for a
    // control that is gone.
    for (const auto& stale : {QStringLiteral("lastFormat"),
                              QStringLiteral("outputFormat"),
                              QStringLiteral("preserveFolders"),
                              QStringLiteral("prefixDate"),
                              QStringLiteral("imapHost"),
                              QStringLiteral("imapPort"),
                              QStringLiteral("imapSsl"),
                              QStringLiteral("imapAuth"),
                              QStringLiteral("imapUser"),
                              QStringLiteral("skipCorrupt")}) {
        settings.remove(stale);
    }

    m_mbox_per_folder_check->setChecked(
        settings.value(QStringLiteral("mboxPerFolder"), true).toBool());
    m_threads_spin->setValue(
        settings.value(QStringLiteral("threads"), ost::kDefaultThreads).toInt());
    m_filters_enabled_check->setChecked(
        settings.value(QStringLiteral("filtersEnabled"), false).toBool());
    m_recover_deleted_check->setChecked(
        settings.value(QStringLiteral("recoverDeleted"), false).toBool());
    m_deep_recovery_check->setChecked(
        settings.value(QStringLiteral("deepRecovery"), false).toBool());

    settings.endGroup();

    // Trigger visibility updates
    updateFilterControlsEnabled();
}

void OstConverterWidget::saveSettings() {
    PortableSettings settings;
    settings.beginGroup(QStringLiteral("OstConverter"));

    settings.setValue(QStringLiteral("lastOutputDir"), m_output_dir_edit->text());
    // "outputFormat", not the old "lastFormat": see loadSettings -- the enum was renumbered,
    // so the old key's stored ints no longer mean what they did.
    settings.setValue(QStringLiteral("mboxPerFolder"), m_mbox_per_folder_check->isChecked());
    settings.setValue(QStringLiteral("threads"), m_threads_spin->value());
    settings.setValue(QStringLiteral("filtersEnabled"), m_filters_enabled_check->isChecked());
    settings.setValue(QStringLiteral("recoverDeleted"), m_recover_deleted_check->isChecked());
    settings.setValue(QStringLiteral("deepRecovery"), m_deep_recovery_check->isChecked());

    settings.endGroup();
}

void OstConverterWidget::onViewReportClicked() {
    const QString report = m_controller->reportPath();
    if (!report.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(report));
    }
}

}  // namespace sak
