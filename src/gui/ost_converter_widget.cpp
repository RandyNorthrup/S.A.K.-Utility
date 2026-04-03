// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_widget.cpp
/// @brief UI widget for the OST/PST Converter tab

#include "sak/ost_converter_widget.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/ost_converter_constants.h"
#include "sak/ost_converter_controller.h"
#include "sak/style_constants.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDesktopServices>
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
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace sak {

namespace {

constexpr int kQueueTableMinHeight = 120;

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
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(ui::kSpacingMedium);

    content_layout->addWidget(createFileQueueSection());
    content_layout->addWidget(createOutputSettingsSection());
    content_layout->addWidget(createFilterSection());
    content_layout->addWidget(createRecoverySection());
    content_layout->addWidget(createImapSection());
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
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);

    // Button bar
    auto* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(ui::kSpacingSmall);

    m_add_files_button = new QPushButton(tr("+ Add Files"), group);
    m_add_files_button->setToolTip(tr("Add OST or PST files to the conversion queue"));
    m_remove_button = new QPushButton(tr("Remove"), group);
    m_remove_button->setToolTip(tr("Remove selected file from the queue"));
    m_clear_button = new QPushButton(tr("Clear All"), group);
    m_clear_button->setToolTip(tr("Remove all files from the queue"));

    btn_layout->addWidget(m_add_files_button);
    btn_layout->addWidget(m_remove_button);
    btn_layout->addWidget(m_clear_button);
    btn_layout->addStretch(1);
    layout->addLayout(btn_layout);

    connect(
        m_add_files_button, &QPushButton::clicked, this, &OstConverterWidget::onAddFilesClicked);
    connect(m_remove_button, &QPushButton::clicked, this, &OstConverterWidget::onRemoveFileClicked);
    connect(m_clear_button, &QPushButton::clicked, this, &OstConverterWidget::onClearQueueClicked);

    // Queue table
    m_queue_table = new QTableWidget(0, ost::ColCount, group);
    m_queue_table->setHorizontalHeaderLabels(
        {tr("File"), tr("Size"), tr("Items"), tr("Status"), tr("Progress")});
    m_queue_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_queue_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_queue_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_queue_table->horizontalHeader()->setStretchLastSection(true);
    m_queue_table->verticalHeader()->setVisible(false);
    m_queue_table->setMinimumHeight(kQueueTableMinHeight);

    // Column widths
    m_queue_table->setColumnWidth(ost::ColFile, 300);
    m_queue_table->setColumnWidth(ost::ColSize, 80);
    m_queue_table->setColumnWidth(ost::ColItems, 60);
    m_queue_table->setColumnWidth(ost::ColStatus, 100);

    layout->addWidget(m_queue_table, 1);

    return group;
}

QWidget* OstConverterWidget::createOutputSettingsSection() {
    auto* group = new QGroupBox(tr("Output Settings"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);

    // Format row
    auto* format_row = new QHBoxLayout();
    format_row->setSpacing(ui::kSpacingSmall);
    format_row->addWidget(new QLabel(tr("Format:"), group));

    m_format_combo = new QComboBox(group);
    m_format_combo->addItem(tr("PST (Microsoft Outlook)"), static_cast<int>(OstOutputFormat::Pst));
    m_format_combo->addItem(tr("EML (RFC 5322)"), static_cast<int>(OstOutputFormat::Eml));
    m_format_combo->addItem(tr("MSG (MAPI Properties)"), static_cast<int>(OstOutputFormat::Msg));
    m_format_combo->addItem(tr("MBOX (Unix Mailbox)"), static_cast<int>(OstOutputFormat::Mbox));
    m_format_combo->addItem(tr("DBX (Outlook Express)"), static_cast<int>(OstOutputFormat::Dbx));
    m_format_combo->addItem(tr("HTML (Web Pages)"), static_cast<int>(OstOutputFormat::Html));
    m_format_combo->addItem(tr("PDF (Documents)"), static_cast<int>(OstOutputFormat::Pdf));
    m_format_combo->addItem(tr("IMAP Upload"), static_cast<int>(OstOutputFormat::ImapUpload));
    m_format_combo->setCurrentIndex(0);
    m_format_combo->setToolTip(tr("Output format for converted emails"));
    format_row->addWidget(m_format_combo);
    format_row->addStretch(1);

    // Destination row
    format_row->addWidget(new QLabel(tr("Destination:"), group));
    m_output_dir_edit = new QLineEdit(group);
    m_output_dir_edit->setPlaceholderText(tr("Select output directory..."));
    m_output_dir_edit->setReadOnly(true);
    format_row->addWidget(m_output_dir_edit, 1);

    m_browse_button = new QPushButton(tr("Browse"), group);
    connect(m_browse_button, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this,
                                                        tr("Select Output Directory"),
                                                        m_output_dir_edit->text());
        if (!dir.isEmpty()) {
            m_output_dir_edit->setText(dir);
        }
    });
    format_row->addWidget(m_browse_button);

    layout->addLayout(format_row);

    connect(m_format_combo,
            &QComboBox::currentIndexChanged,
            this,
            &OstConverterWidget::onFormatChanged);

    // Options row
    auto* options_row = new QHBoxLayout();
    options_row->setSpacing(ui::kSpacingMedium);

    m_preserve_folders_check = new QCheckBox(tr("Preserve folder structure"), group);
    m_preserve_folders_check->setChecked(true);
    options_row->addWidget(m_preserve_folders_check);

    m_prefix_date_check = new QCheckBox(tr("Prefix filenames with date"), group);
    m_prefix_date_check->setChecked(true);
    options_row->addWidget(m_prefix_date_check);

    options_row->addStretch(1);

    options_row->addWidget(new QLabel(tr("Threads:"), group));
    m_threads_spin = new QSpinBox(group);
    m_threads_spin->setRange(ost::kMinThreads, ost::kMaxThreads);
    m_threads_spin->setValue(ost::kDefaultThreads);
    m_threads_spin->setToolTip(
        tr("Number of concurrent file conversions (1-%1)").arg(ost::kMaxThreads));
    options_row->addWidget(m_threads_spin);

    layout->addLayout(options_row);

    // PST split options (hidden initially since EML is default)
    auto* split_row = new QHBoxLayout();
    m_split_check = new QCheckBox(tr("Split PST files:"), group);
    m_split_check->setVisible(false);
    split_row->addWidget(m_split_check);

    m_split_size_combo = new QComboBox(group);
    m_split_size_combo->addItem(tr("2 GB"), static_cast<int>(PstSplitSize::Split2Gb));
    m_split_size_combo->addItem(tr("5 GB"), static_cast<int>(PstSplitSize::Split5Gb));
    m_split_size_combo->addItem(tr("10 GB"), static_cast<int>(PstSplitSize::Split10Gb));
    m_split_size_combo->setCurrentIndex(1);
    m_split_size_combo->setVisible(false);
    split_row->addWidget(m_split_size_combo);
    split_row->addStretch(1);
    layout->addLayout(split_row);

    return group;
}

QWidget* OstConverterWidget::createFilterSection() {
    m_filter_group = new QGroupBox(tr("Filters (optional)"), this);
    m_filter_group->setCheckable(true);
    m_filter_group->setChecked(false);

    auto* layout = new QVBoxLayout(m_filter_group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);

    // Date range row
    auto* date_row = new QHBoxLayout();
    date_row->setSpacing(ui::kSpacingSmall);

    m_date_filter_check = new QCheckBox(tr("Date range:"), m_filter_group);
    date_row->addWidget(m_date_filter_check);

    m_date_from_edit = new QDateEdit(m_filter_group);
    m_date_from_edit->setCalendarPopup(true);
    m_date_from_edit->setDate(QDate::currentDate().addYears(-1));
    m_date_from_edit->setEnabled(false);
    date_row->addWidget(m_date_from_edit);

    date_row->addWidget(new QLabel(tr("to"), m_filter_group));

    m_date_to_edit = new QDateEdit(m_filter_group);
    m_date_to_edit->setCalendarPopup(true);
    m_date_to_edit->setDate(QDate::currentDate());
    m_date_to_edit->setEnabled(false);
    date_row->addWidget(m_date_to_edit);

    date_row->addStretch(1);
    layout->addLayout(date_row);

    connect(m_date_filter_check, &QCheckBox::toggled, this, [this](bool checked) {
        m_date_from_edit->setEnabled(checked);
        m_date_to_edit->setEnabled(checked);
    });

    // Sender filter row
    auto* sender_row = new QHBoxLayout();
    sender_row->setSpacing(ui::kSpacingSmall);
    sender_row->addWidget(new QLabel(tr("Sender:"), m_filter_group));
    m_sender_filter_edit = new QLineEdit(m_filter_group);
    m_sender_filter_edit->setPlaceholderText(tr("Filter by sender email (contains)"));
    sender_row->addWidget(m_sender_filter_edit, 1);
    layout->addLayout(sender_row);

    // Recipient filter row
    auto* recip_row = new QHBoxLayout();
    recip_row->setSpacing(ui::kSpacingSmall);
    recip_row->addWidget(new QLabel(tr("Recipient:"), m_filter_group));
    m_recipient_filter_edit = new QLineEdit(m_filter_group);
    m_recipient_filter_edit->setPlaceholderText(tr("Filter by recipient email (contains)"));
    recip_row->addWidget(m_recipient_filter_edit, 1);
    layout->addLayout(recip_row);

    return m_filter_group;
}

QWidget* OstConverterWidget::createRecoverySection() {
    m_recovery_group = new QGroupBox(tr("Recovery Options"), this);

    auto* layout = new QVBoxLayout(m_recovery_group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);

    m_recover_deleted_check = new QCheckBox(
        tr("Recover deleted items (scan Recoverable Items folder)"), m_recovery_group);
    layout->addWidget(m_recover_deleted_check);

    m_deep_recovery_check =
        new QCheckBox(tr("Deep recovery (scan orphaned nodes — slow, thorough)"), m_recovery_group);
    m_deep_recovery_check->setToolTip(
        tr("Walk all NBT nodes to find hard-deleted messages not in any folder"));
    layout->addWidget(m_deep_recovery_check);

    m_skip_corrupt_check = new QCheckBox(
        tr("Skip corrupt blocks (continue on errors, log skipped items)"), m_recovery_group);
    layout->addWidget(m_skip_corrupt_check);

    return m_recovery_group;
}

QWidget* OstConverterWidget::createImapSection() {
    m_imap_group = new QGroupBox(tr("IMAP Server Settings"), this);
    m_imap_group->setVisible(false);

    auto* layout = new QVBoxLayout(m_imap_group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);

    // Host / Port / SSL row
    auto* server_row = new QHBoxLayout();
    server_row->setSpacing(ui::kSpacingSmall);

    server_row->addWidget(new QLabel(tr("Host:"), m_imap_group));
    m_imap_host_edit = new QLineEdit(m_imap_group);
    m_imap_host_edit->setPlaceholderText(tr("imap.example.com"));
    server_row->addWidget(m_imap_host_edit, 1);

    server_row->addWidget(new QLabel(tr("Port:"), m_imap_group));
    m_imap_port_spin = new QSpinBox(m_imap_group);
    m_imap_port_spin->setRange(1, 65'535);
    m_imap_port_spin->setValue(993);
    server_row->addWidget(m_imap_port_spin);

    m_imap_ssl_check = new QCheckBox(tr("SSL/TLS"), m_imap_group);
    m_imap_ssl_check->setChecked(true);
    server_row->addWidget(m_imap_ssl_check);

    layout->addLayout(server_row);

    // Auth / User / Password row
    auto* auth_row = new QHBoxLayout();
    auth_row->setSpacing(ui::kSpacingSmall);

    auth_row->addWidget(new QLabel(tr("Auth:"), m_imap_group));
    m_imap_auth_combo = new QComboBox(m_imap_group);
    m_imap_auth_combo->addItem(tr("PLAIN"), static_cast<int>(ImapAuthMethod::Plain));
    m_imap_auth_combo->addItem(tr("LOGIN"), static_cast<int>(ImapAuthMethod::Login));
    m_imap_auth_combo->addItem(tr("XOAUTH2"), static_cast<int>(ImapAuthMethod::XOAuth2));
    auth_row->addWidget(m_imap_auth_combo);

    auth_row->addWidget(new QLabel(tr("User:"), m_imap_group));
    m_imap_user_edit = new QLineEdit(m_imap_group);
    m_imap_user_edit->setPlaceholderText(tr("user@example.com"));
    auth_row->addWidget(m_imap_user_edit, 1);

    auth_row->addWidget(new QLabel(tr("Password:"), m_imap_group));
    m_imap_password_edit = new QLineEdit(m_imap_group);
    m_imap_password_edit->setEchoMode(QLineEdit::Password);
    auth_row->addWidget(m_imap_password_edit, 1);

    layout->addLayout(auth_row);

    return m_imap_group;
}

QWidget* OstConverterWidget::createButtonBar() {
    auto* bar = new QWidget(this);
    auto* layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kSpacingMedium);

    m_view_report_button = new QPushButton(tr("View Report"), bar);
    m_view_report_button->setEnabled(false);
    m_view_report_button->setToolTip(tr("Open the batch conversion report in your browser"));
    connect(m_view_report_button,
            &QPushButton::clicked,
            this,
            &OstConverterWidget::onViewReportClicked);
    layout->addWidget(m_view_report_button);

    layout->addStretch(1);

    m_convert_button = new QPushButton(tr("Convert All"), bar);
    m_convert_button->setToolTip(tr("Start converting all queued files"));
    m_convert_button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: white; padding: 8px 24px; "
                       "border-radius: 4px; font-weight: bold; }"
                       "QPushButton:hover { background: %2; }"
                       "QPushButton:pressed { background: %3; }"
                       "QPushButton:disabled { background: %4; }")
            .arg(ui::kColorPrimary,
                 ui::kColorPrimaryHover,
                 ui::kColorPrimaryPressed,
                 ui::kColorTextDisabled));
    layout->addWidget(m_convert_button);

    m_cancel_button = new QPushButton(tr("Cancel"), bar);
    m_cancel_button->setEnabled(false);
    m_cancel_button->setToolTip(tr("Cancel all in-progress conversions"));
    m_cancel_button->setStyleSheet(
        QStringLiteral("QPushButton { background: %1; color: white; padding: 8px 24px; "
                       "border-radius: 4px; font-weight: bold; }"
                       "QPushButton:hover { background: %2; }"
                       "QPushButton:pressed { background: %3; }"
                       "QPushButton:disabled { background: %4; }")
            .arg(ui::kColorDangerBtnNormal,
                 ui::kColorDangerBtnHover,
                 ui::kColorDangerBtnPressed,
                 ui::kColorTextDisabled));
    layout->addWidget(m_cancel_button);

    connect(m_convert_button, &QPushButton::clicked, this, &OstConverterWidget::onConvertClicked);
    connect(m_cancel_button, &QPushButton::clicked, this, &OstConverterWidget::onCancelClicked);

    return bar;
}

// ============================================================================
// Slot Implementations — File Queue
// ============================================================================

void OstConverterWidget::onAddFilesClicked() {
    QStringList files =
        QFileDialog::getOpenFileNames(this,
                                      tr("Select OST/PST Files"),
                                      QString(),
                                      tr("Outlook Data Files (*.ost *.pst);;All Files (*.*)"));

    for (const auto& file : files) {
        m_controller->addFile(file);
    }
}

void OstConverterWidget::onRemoveFileClicked() {
    int row = m_queue_table->currentRow();
    if (row >= 0) {
        m_controller->removeFile(row);
    }
}

void OstConverterWidget::onClearQueueClicked() {
    m_controller->clearQueue();
}

// ============================================================================
// Slot Implementations — Conversion Control
// ============================================================================

void OstConverterWidget::onConvertClicked() {
    if (m_output_dir_edit->text().isEmpty()) {
        QMessageBox::warning(this,
                             tr("Missing Output Directory"),
                             tr("Please select an output directory before converting."));
        sak::logWarning("OST Converter: conversion started without output directory");
        return;
    }

    if (m_controller->queue().isEmpty()) {
        QMessageBox::warning(this,
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

void OstConverterWidget::onFormatChanged(int /*index*/) {
    int format_val = m_format_combo->currentData().toInt();
    auto format = static_cast<OstOutputFormat>(format_val);

    bool is_pst = (format == OstOutputFormat::Pst);
    m_split_check->setVisible(is_pst);
    m_split_size_combo->setVisible(is_pst);

    bool is_per_file = (format == OstOutputFormat::Eml || format == OstOutputFormat::Msg);
    m_prefix_date_check->setVisible(is_per_file);
    m_preserve_folders_check->setVisible(format != OstOutputFormat::ImapUpload);

    bool is_imap = (format == OstOutputFormat::ImapUpload);
    m_imap_group->setVisible(is_imap);
    m_output_dir_edit->setEnabled(!is_imap);
    m_browse_button->setEnabled(!is_imap);
}

// ============================================================================
// Slot Implementations — Controller Signals
// ============================================================================

void OstConverterWidget::onFileAdded(int /*index*/, OstConversionJob job) {
    int row = m_queue_table->rowCount();
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
    Q_EMIT statusMessage(tr("Starting conversion of %1 file(s)...").arg(total_files),
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
        if (item) {
            item->setText(progress_text);
        }
        auto* status_item = m_queue_table->item(file_index, ost::ColStatus);
        if (status_item) {
            status_item->setText(tr("Converting"));
        }
    }

    Q_EMIT statusMessage(tr("Converting: %1 — %2")
                             .arg(current_folder)
                             .arg(tr("%1 / %2 items").arg(items_done).arg(items_total)),
                         0);
}

void OstConverterWidget::onFileConversionComplete(int file_index, OstConversionResult result) {
    ++m_files_done;

    Q_EMIT progressUpdate(m_files_done, m_total_files);

    if (file_index >= 0 && file_index < m_queue_table->rowCount()) {
        auto* status_item = m_queue_table->item(file_index, ost::ColStatus);
        if (status_item) {
            bool failed = (result.items_converted == 0 && result.items_failed > 0);
            status_item->setText(failed ? tr("Failed") : tr("Complete"));
        }
        auto* progress_item = m_queue_table->item(file_index, ost::ColProgress);
        if (progress_item) {
            progress_item->setText(tr("%1 items").arg(result.items_converted));
        }
    }
}

void OstConverterWidget::onAllConversionsComplete(OstConversionBatchResult result) {
    setConvertingState(false);

    // Reset progress bar (0/0 hides it in MainWindow)
    Q_EMIT progressUpdate(0, 0);

    // Enable report button if a report was generated
    QString report = m_controller->reportPath();
    m_view_report_button->setEnabled(!report.isEmpty());

    Q_EMIT statusMessage(tr("Conversion complete: %1/%2 files, %3 items")
                             .arg(result.files_succeeded)
                             .arg(result.files_total)
                             .arg(result.total_items_converted),
                         kTimerStatusLongMs);
}

void OstConverterWidget::onErrorOccurred(int file_index, QString message) {
    Q_EMIT statusMessage(tr("Error: %1").arg(message), kTimerStatusWarnMs);

    if (file_index >= 0 && file_index < m_queue_table->rowCount()) {
        auto* status_item = m_queue_table->item(file_index, ost::ColStatus);
        if (status_item) {
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
    m_format_combo->setEnabled(!converting);
    m_browse_button->setEnabled(!converting);
    m_threads_spin->setEnabled(!converting);
    m_filter_group->setEnabled(!converting);
    m_recovery_group->setEnabled(!converting);
    m_imap_group->setEnabled(!converting);
    m_view_report_button->setEnabled(false);
}

OstConversionConfig OstConverterWidget::buildConfig() const {
    OstConversionConfig config;

    config.format = static_cast<OstOutputFormat>(m_format_combo->currentData().toInt());
    config.output_directory = m_output_dir_edit->text();
    config.max_threads = m_threads_spin->value();
    config.preserve_folder_structure = m_preserve_folders_check->isChecked();
    config.prefix_filename_with_date = m_prefix_date_check->isChecked();
    config.recover_deleted_items = m_recover_deleted_check->isChecked();

    // Recovery mode
    if (m_deep_recovery_check->isChecked()) {
        config.recovery_mode = RecoveryMode::DeepRecovery;
    } else if (m_skip_corrupt_check->isChecked()) {
        config.recovery_mode = RecoveryMode::SkipCorrupt;
    }

    // PST split
    if (m_split_check->isChecked() && m_split_check->isVisible()) {
        config.split_size = static_cast<PstSplitSize>(m_split_size_combo->currentData().toInt());
    }

    // Date filter
    if (m_filter_group->isChecked() && m_date_filter_check->isChecked()) {
        config.date_from = m_date_from_edit->dateTime();
        config.date_to = m_date_to_edit->dateTime();
    }

    // Sender / recipient filters
    if (m_filter_group->isChecked()) {
        config.sender_filter = m_sender_filter_edit->text().trimmed();
        config.recipient_filter = m_recipient_filter_edit->text().trimmed();
    }

    // IMAP config
    if (config.format == OstOutputFormat::ImapUpload) {
        config.imap_config.host = m_imap_host_edit->text().trimmed();
        config.imap_config.port = static_cast<uint16_t>(m_imap_port_spin->value());
        config.imap_config.use_ssl = m_imap_ssl_check->isChecked();
        config.imap_config.auth_method =
            static_cast<ImapAuthMethod>(m_imap_auth_combo->currentData().toInt());
        config.imap_config.username = m_imap_user_edit->text().trimmed();
        config.imap_config.password = m_imap_password_edit->text();
    }

    return config;
}

void OstConverterWidget::updateQueueRow(int row, const OstConversionJob& job) {
    m_queue_table->setItem(row, ost::ColFile, new QTableWidgetItem(job.display_name));
    m_queue_table->setItem(row,
                           ost::ColSize,
                           new QTableWidgetItem(formatBytes(job.file_size_bytes)));
    m_queue_table->setItem(row,
                           ost::ColItems,
                           new QTableWidgetItem(job.estimated_items > 0
                                                    ? QStringLiteral("~%1").arg(job.estimated_items)
                                                    : tr("—")));
    m_queue_table->setItem(row, ost::ColStatus, new QTableWidgetItem(statusLabel(job.status)));
    m_queue_table->setItem(row, ost::ColProgress, new QTableWidgetItem(QString()));
}

QString OstConverterWidget::formatBytes(qint64 bytes) {
    if (bytes < kBytesPerKB) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < kBytesPerMB) {
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / kBytesPerKBf, 0, 'f', 1);
    }
    if (bytes < kBytesPerGB) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / kBytesPerMBf, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / kBytesPerGBf, 0, 'f', 2);
}

QString OstConverterWidget::statusLabel(OstConversionJob::Status status) {
    switch (status) {
    case OstConversionJob::Status::Queued:
        return tr("Queued");
    case OstConversionJob::Status::Parsing:
        return tr("Parsing");
    case OstConversionJob::Status::Converting:
        return tr("Converting");
    case OstConversionJob::Status::Uploading:
        return tr("Uploading");
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
    QSettings settings;
    settings.beginGroup(QStringLiteral("OstConverter"));

    m_output_dir_edit->setText(settings.value(QStringLiteral("lastOutputDir")).toString());
    m_format_combo->setCurrentIndex(settings.value(QStringLiteral("lastFormat"), 0).toInt());
    m_threads_spin->setValue(
        settings.value(QStringLiteral("threads"), ost::kDefaultThreads).toInt());
    m_preserve_folders_check->setChecked(
        settings.value(QStringLiteral("preserveFolders"), true).toBool());
    m_prefix_date_check->setChecked(settings.value(QStringLiteral("prefixDate"), true).toBool());
    m_recover_deleted_check->setChecked(
        settings.value(QStringLiteral("recoverDeleted"), false).toBool());
    m_deep_recovery_check->setChecked(
        settings.value(QStringLiteral("deepRecovery"), false).toBool());
    m_skip_corrupt_check->setChecked(settings.value(QStringLiteral("skipCorrupt"), false).toBool());

    // IMAP settings (password intentionally not persisted)
    m_imap_host_edit->setText(settings.value(QStringLiteral("imapHost")).toString());
    m_imap_port_spin->setValue(settings.value(QStringLiteral("imapPort"), 993).toInt());
    m_imap_ssl_check->setChecked(settings.value(QStringLiteral("imapSsl"), true).toBool());
    m_imap_auth_combo->setCurrentIndex(settings.value(QStringLiteral("imapAuth"), 0).toInt());
    m_imap_user_edit->setText(settings.value(QStringLiteral("imapUser")).toString());

    settings.endGroup();

    // Trigger visibility updates
    onFormatChanged(m_format_combo->currentIndex());
}

void OstConverterWidget::saveSettings() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("OstConverter"));

    settings.setValue(QStringLiteral("lastOutputDir"), m_output_dir_edit->text());
    settings.setValue(QStringLiteral("lastFormat"), m_format_combo->currentIndex());
    settings.setValue(QStringLiteral("threads"), m_threads_spin->value());
    settings.setValue(QStringLiteral("preserveFolders"), m_preserve_folders_check->isChecked());
    settings.setValue(QStringLiteral("prefixDate"), m_prefix_date_check->isChecked());
    settings.setValue(QStringLiteral("recoverDeleted"), m_recover_deleted_check->isChecked());
    settings.setValue(QStringLiteral("deepRecovery"), m_deep_recovery_check->isChecked());
    settings.setValue(QStringLiteral("skipCorrupt"), m_skip_corrupt_check->isChecked());

    // IMAP settings (password intentionally not persisted)
    settings.setValue(QStringLiteral("imapHost"), m_imap_host_edit->text());
    settings.setValue(QStringLiteral("imapPort"), m_imap_port_spin->value());
    settings.setValue(QStringLiteral("imapSsl"), m_imap_ssl_check->isChecked());
    settings.setValue(QStringLiteral("imapAuth"), m_imap_auth_combo->currentIndex());
    settings.setValue(QStringLiteral("imapUser"), m_imap_user_edit->text());

    settings.endGroup();
}

void OstConverterWidget::onViewReportClicked() {
    QString report = m_controller->reportPath();
    if (!report.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(report));
    }
}

}  // namespace sak
