// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_inspector_panel.cpp
/// @brief UI construction and signal/slot handlers for the Email Tools

#include "sak/email_inspector_panel.h"

#include "sak/detachable_log_window.h"
#include "sak/email_attachments_browser_dialog.h"
#include "sak/email_calendar_dialog.h"
#include "sak/email_constants.h"
#include "sak/email_contacts_dialog.h"
#include "sak/email_file_scanner_dialog.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStorageInfo>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

EmailInspectorPanel::EmailInspectorPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<EmailInspectorController>(this)) {
    setupUi();
    connectController();
}

EmailInspectorPanel::~EmailInspectorPanel() {
    m_controller->cancelOperation();
}

// ============================================================================
// UI Construction
// ============================================================================

void EmailInspectorPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    root_layout->setSpacing(ui::kSpacingSmall);

    // Dynamic panel header
    m_header_widgets =
        createDynamicPanelHeader(this,
                                 QStringLiteral(":/icons/icons/panel_email.svg"),
                                 tr("Email Tools"),
                                 tr("Browse and export emails, contacts, calendar "
                                    "items, tasks, and notes from PST/OST/MBOX files"),
                                 root_layout);

    // Ribbon toolbar
    root_layout->addWidget(createRibbon());

    // Main splitter: folder tree | content area
    m_main_splitter = new QSplitter(Qt::Horizontal, this);
    m_main_splitter->setChildrenCollapsible(false);
    m_main_splitter->addWidget(createFolderTreePanel());
    m_main_splitter->addWidget(createContentArea());
    m_main_splitter->setSizes({email::kFolderTreeDefaultWidth, 700});
    root_layout->addWidget(m_main_splitter, 1);

    // Status bar
    auto* status_row = new QHBoxLayout();
    m_status_label = new QLabel(tr("Ready"), this);
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    status_row->addWidget(m_status_label, 1);

    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setMaximumWidth(200);
    m_progress_bar->setTextVisible(true);
    m_progress_bar->setFormat(QStringLiteral("%p%"));
    m_progress_bar->setVisible(false);
    m_progress_bar->setAccessibleName(QStringLiteral("Email Operation Progress"));
    status_row->addWidget(m_progress_bar);
    root_layout->addLayout(status_row);

    // Log Toggle (left) + HTML/Plain Text Toggle (right)
    m_log_toggle = new LogToggleSwitch(tr("Log"), this);
    m_html_toggle_switch = new LogToggleSwitch(tr("Plain Text"), this);
    m_html_toggle_switch->setToolTip(tr("Toggle between HTML and plain text view"));
    connect(m_html_toggle_switch, &LogToggleSwitch::toggled, this, [this](bool plain_text) {
        m_show_html = !plain_text;
        displayItemDetail(m_current_detail);
    });
    auto* log_toggle_layout = new QHBoxLayout();
    log_toggle_layout->addWidget(m_log_toggle);
    log_toggle_layout->addStretch();
    log_toggle_layout->addWidget(m_html_toggle_switch);
    root_layout->addLayout(log_toggle_layout);
}

// ============================================================================
// Ribbon Toolbar
// ============================================================================

namespace {

constexpr int kRibbonIconSize = 28;
constexpr int kRibbonButtonWidth = 64;
constexpr int kRibbonButtonHeight = 56;

constexpr auto kRibbonStyle =
    "QWidget#ribbonBar {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 #f8fafc, stop:1 #eef2f7);"
    "  border: 1px solid #cbd5e1;"
    "  border-radius: 8px;"
    "}";

constexpr auto kRibbonButtonStyle =
    "QToolButton {"
    "  background: transparent;"
    "  border: 1px solid transparent;"
    "  border-radius: 6px;"
    "  padding: 4px 2px;"
    "  font-size: 9px; font-weight: 500;"
    "  color: #334155;"
    "}"
    "QToolButton:hover {"
    "  background: rgba(59, 130, 246, 0.08);"
    "  border: 1px solid rgba(59, 130, 246, 0.25);"
    "}"
    "QToolButton:pressed {"
    "  background: rgba(59, 130, 246, 0.15);"
    "  border: 1px solid rgba(59, 130, 246, 0.4);"
    "}"
    "QToolButton:disabled {"
    "  color: #94a3b8;"
    "}";

constexpr auto kRibbonSeparatorStyle = "background: #cbd5e1; margin: 6px 0px;";

QToolButton* makeRibbonButton(const QString& text,
                              const QString& icon_path,
                              const QString& tooltip,
                              QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setIcon(QIcon(icon_path));
    button->setIconSize(QSize(kRibbonIconSize, kRibbonIconSize));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setToolTip(tooltip);
    button->setFixedSize(kRibbonButtonWidth, kRibbonButtonHeight);
    button->setStyleSheet(kRibbonButtonStyle);
    return button;
}

QFrame* makeRibbonSeparator(QWidget* parent) {
    auto* sep = new QFrame(parent);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedWidth(1);
    sep->setStyleSheet(QLatin1String(kRibbonSeparatorStyle));
    return sep;
}

}  // anonymous namespace

QWidget* EmailInspectorPanel::createRibbon() {
    auto* ribbon = new QWidget(this);
    ribbon->setObjectName(QStringLiteral("ribbonBar"));
    ribbon->setStyleSheet(QLatin1String(kRibbonStyle));
    auto* layout = new QHBoxLayout(ribbon);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginTight, ui::kMarginSmall, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingTight);

    // -- File Group -------------------------------------------------------
    m_scan_files_button = makeRibbonButton(tr("Scan"),
                                           QStringLiteral(":/icons/icons/icons8-radar.svg"),
                                           tr("Scan common locations for email data files"),
                                           ribbon);
    connect(m_scan_files_button,
            &QToolButton::clicked,
            this,
            &EmailInspectorPanel::onScanForFilesClicked);
    layout->addWidget(m_scan_files_button);

    m_open_button = makeRibbonButton(tr("Open"),
                                     QStringLiteral(":/icons/icons/icons8-opened-folder.svg"),
                                     tr("Open a PST, OST, or MBOX file"),
                                     ribbon);
    connect(m_open_button, &QToolButton::clicked, this, &EmailInspectorPanel::onOpenFileClicked);
    layout->addWidget(m_open_button);

    m_close_button = makeRibbonButton(tr("Close"),
                                      QStringLiteral(":/icons/icons/icons8-close-window.svg"),
                                      tr("Close the current file"),
                                      ribbon);
    m_close_button->setEnabled(false);
    connect(m_close_button, &QToolButton::clicked, this, &EmailInspectorPanel::onCloseFileClicked);
    layout->addWidget(m_close_button);

    layout->addWidget(makeRibbonSeparator(ribbon));

    // -- Search Group -----------------------------------------------------
    m_search_edit = new QLineEdit(ribbon);
    m_search_edit->setPlaceholderText(tr("Search emails..."));
    m_search_edit->setMinimumWidth(180);
    m_search_edit->setMaximumWidth(280);
    m_search_edit->setEnabled(false);
    m_search_edit->setStyleSheet(
        QStringLiteral("QLineEdit { border: 1px solid #cbd5e1;"
                       " border-radius: 6px; padding: 6px 10px;"
                       " background: white; color: #334155; }"));
    m_search_edit->setAccessibleName(QStringLiteral("Email Search"));
    layout->addWidget(m_search_edit);

    m_search_button = new QPushButton(ribbon);
    m_search_button->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-search.svg")));
    m_search_button->setIconSize(QSize(kRibbonIconSize - 6, kRibbonIconSize - 6));
    m_search_button->setFixedSize(34, 34);
    m_search_button->setEnabled(false);
    m_search_button->setToolTip(tr("Search"));
    m_search_button->setStyleSheet(
        QStringLiteral("QPushButton { background: transparent;"
                       " border: 1px solid transparent;"
                       " border-radius: 6px; }"
                       "QPushButton:hover { background:"
                       " rgba(59, 130, 246, 0.1); }"));
    m_search_button->setAccessibleName(QStringLiteral("Search Emails"));
    connect(m_search_button, &QPushButton::clicked, this, &EmailInspectorPanel::onSearchClicked);
    connect(m_search_edit, &QLineEdit::returnPressed, this, &EmailInspectorPanel::onSearchClicked);
    layout->addWidget(m_search_button);

    layout->addWidget(makeRibbonSeparator(ribbon));

    // -- Actions Group ----------------------------------------------------
    m_export_emails_button = makeRibbonButton(tr("Export"),
                                              QStringLiteral(":/icons/icons/icons8-export.svg"),
                                              tr("Export emails as EML or CSV"),
                                              ribbon);
    m_export_emails_button->setEnabled(false);
    connect(
        m_export_emails_button, &QToolButton::clicked, this, &EmailInspectorPanel::onExportClicked);
    layout->addWidget(m_export_emails_button);

    m_attachments_button = makeRibbonButton(tr("Attachments"),
                                            QStringLiteral(":/icons/icons/icons8-attachment.svg"),
                                            tr("Browse all attachments in this mailbox"),
                                            ribbon);
    m_attachments_button->setEnabled(false);
    connect(m_attachments_button,
            &QToolButton::clicked,
            this,
            &EmailInspectorPanel::onExportAttachmentsClicked);
    layout->addWidget(m_attachments_button);

    layout->addWidget(makeRibbonSeparator(ribbon));

    // -- People & Calendar Group ------------------------------------------
    m_contacts_button = makeRibbonButton(tr("Contacts"),
                                         QStringLiteral(":/icons/icons/icons8-address-book.svg"),
                                         tr("Open the address book to view and export contacts"),
                                         ribbon);
    m_contacts_button->setEnabled(false);
    connect(
        m_contacts_button, &QToolButton::clicked, this, &EmailInspectorPanel::onContactsClicked);
    layout->addWidget(m_contacts_button);

    m_calendar_button = makeRibbonButton(tr("Calendar"),
                                         QStringLiteral(":/icons/icons/icons8-calendar.svg"),
                                         tr("Open calendar viewer to view and export events"),
                                         ribbon);
    m_calendar_button->setEnabled(false);
    connect(
        m_calendar_button, &QToolButton::clicked, this, &EmailInspectorPanel::onCalendarClicked);
    layout->addWidget(m_calendar_button);

    layout->addStretch();
    return ribbon;
}

QWidget* EmailInspectorPanel::createFolderTreePanel() {
    auto* group = new QGroupBox(tr("Folders"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingSmall);
    group->setMinimumWidth(email::kFolderTreeMinWidth);

    m_folder_tree = new QTreeWidget(group);
    m_folder_tree->setHeaderHidden(true);
    m_folder_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_folder_tree->setAccessibleName(QStringLiteral("Email Folder Tree"));
    connect(m_folder_tree,
            &QTreeWidget::itemClicked,
            this,
            &EmailInspectorPanel::onFolderTreeItemClicked);
    connect(m_folder_tree,
            &QTreeWidget::customContextMenuRequested,
            this,
            &EmailInspectorPanel::onFolderTreeContextMenu);
    layout->addWidget(m_folder_tree, 1);

    return group;
}

QWidget* EmailInspectorPanel::createContentArea() {
    // Horizontal splitter: item list on left, preview on right
    m_content_splitter = new QSplitter(Qt::Horizontal, this);
    m_content_splitter->setChildrenCollapsible(false);
    m_content_splitter->addWidget(createItemListPanel());
    m_content_splitter->addWidget(createDetailPanel());
    m_content_splitter->setSizes({400, 500});
    m_content_splitter->setStretchFactor(0, 0);
    m_content_splitter->setStretchFactor(1, 1);
    return m_content_splitter;
}

QWidget* EmailInspectorPanel::createItemListPanel() {
    auto* group = new QGroupBox(tr("Items"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    layout->setSpacing(ui::kSpacingTight);

    m_item_count_label = new QLabel(group);
    m_item_count_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));

    // Page size dropdown
    m_page_size_combo = new QComboBox(group);
    m_page_size_combo->addItems({tr("10"), tr("20"), tr("50"), tr("100"), tr("500")});
    m_page_size_combo->setCurrentIndex(4);  // Default 500
    m_page_size_combo->setToolTip(tr("Maximum items to display"));
    m_page_size_combo->setFixedWidth(70);
    connect(m_page_size_combo, &QComboBox::currentIndexChanged, this, [this] { applyPageSize(); });

    auto* filter_row = new QHBoxLayout();
    filter_row->addWidget(m_item_count_label, 1);
    filter_row->addWidget(new QLabel(tr("Show:"), group));
    filter_row->addWidget(m_page_size_combo);
    layout->addLayout(filter_row);

    m_item_list = new QTableWidget(group);
    m_item_list->setColumnCount(ColCount);
    m_item_list->setAccessibleName(QStringLiteral("Email Items List"));
    m_item_list->setHorizontalHeaderLabels(
        {tr(""), tr("Subject"), tr("From / Name"), tr("Date"), tr("Size"), tr("Type")});
    m_item_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_item_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_item_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_item_list->setSortingEnabled(true);
    m_item_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_item_list->horizontalHeader()->setStretchLastSection(true);
    m_item_list->horizontalHeader()->setSectionResizeMode(ColAttachment, QHeaderView::Fixed);
    m_item_list->setColumnWidth(ColAttachment, 42);
    m_item_list->setColumnWidth(ColSubject, 260);
    m_item_list->setColumnWidth(ColFrom, 180);
    m_item_list->setColumnWidth(ColDate, 150);
    m_item_list->setColumnWidth(ColSize, 80);
    m_item_list->setColumnWidth(ColType, 90);
    m_item_list->verticalHeader()->setVisible(false);

    connect(
        m_item_list, &QTableWidget::cellClicked, this, &EmailInspectorPanel::onItemListCellClicked);
    connect(m_item_list,
            &QTableWidget::customContextMenuRequested,
            this,
            &EmailInspectorPanel::onItemListContextMenu);

    layout->addWidget(m_item_list, 1);
    return group;
}

QWidget* EmailInspectorPanel::createDetailPanel() {
    auto* group = new QGroupBox(tr("Preview"), this);
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(
        ui::kMarginTight, ui::kMarginMedium, ui::kMarginTight, ui::kMarginTight);
    group->setMinimumHeight(email::kDetailPanelMinHeight);
    group->setMinimumWidth(email::kDetailPanelMinWidth);

    m_detail_tabs = new QTabWidget(group);
    m_detail_tabs->addTab(createContentTab(), tr("Content"));
    m_detail_tabs->addTab(createHeadersTab(), tr("Headers"));
    m_detail_tabs->addTab(createPropertiesTab(), tr("Properties"));
    m_detail_tabs->addTab(createAttachmentsTab(), tr("Attachments"));
    layout->addWidget(m_detail_tabs, 1);

    return group;
}

QWidget* EmailInspectorPanel::createContentTab() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kSpacingTight);


    m_content_browser = new QTextBrowser(this);
    m_content_browser->setOpenExternalLinks(false);
    m_content_browser->setReadOnly(true);
    m_content_browser->setAccessibleName(QStringLiteral("Email Content"));
    m_content_browser->setStyleSheet(
        QStringLiteral("QTextBrowser { font-family: 'Segoe UI', sans-serif;"
                       " font-size: 13px; padding: 8px;"
                       " background: white; }"));
    layout->addWidget(m_content_browser, 1);

    return container;
}

QWidget* EmailInspectorPanel::createHeadersTab() {
    m_headers_browser = new QTextBrowser(this);
    m_headers_browser->setReadOnly(true);
    m_headers_browser->setAccessibleName(QStringLiteral("Email Headers"));
    QFont mono_font(QStringLiteral("Consolas"), 9);
    m_headers_browser->setFont(mono_font);
    return m_headers_browser;
}

QWidget* EmailInspectorPanel::createPropertiesTab() {
    m_properties_table = new QTableWidget(this);
    m_properties_table->setColumnCount(2);
    m_properties_table->setAccessibleName(QStringLiteral("MAPI Properties Table"));
    m_properties_table->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    m_properties_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_properties_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_properties_table->setSortingEnabled(true);
    m_properties_table->horizontalHeader()->setStretchLastSection(true);
    m_properties_table->verticalHeader()->setVisible(false);
    return m_properties_table;
}

QWidget* EmailInspectorPanel::createAttachmentsTab() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    m_attachments_table = new QTableWidget(container);
    m_attachments_table->setColumnCount(4);
    m_attachments_table->setAccessibleName(QStringLiteral("Email Attachments Table"));
    m_attachments_table->setHorizontalHeaderLabels(
        {tr("Filename"), tr("Size"), tr("MIME Type"), tr("Content ID")});
    m_attachments_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_attachments_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_attachments_table->horizontalHeader()->setStretchLastSection(true);
    m_attachments_table->verticalHeader()->setVisible(false);
    layout->addWidget(m_attachments_table, 1);

    auto* button_row = new QHBoxLayout();
    m_save_attachment_button = new QPushButton(tr("Save Selected"), container);
    m_save_attachment_button->setEnabled(false);
    m_save_attachment_button->setStyleSheet(ui::kSecondaryButtonStyle);
    m_save_attachment_button->setAccessibleName(QStringLiteral("Save Selected Attachment"));
    connect(m_save_attachment_button,
            &QPushButton::clicked,
            this,
            &EmailInspectorPanel::onSaveAttachmentClicked);
    button_row->addWidget(m_save_attachment_button);

    m_save_all_attachments_button = new QPushButton(tr("Save All"), container);
    m_save_all_attachments_button->setEnabled(false);
    m_save_all_attachments_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_save_all_attachments_button->setAccessibleName(QStringLiteral("Save All Attachments"));
    connect(m_save_all_attachments_button,
            &QPushButton::clicked,
            this,
            &EmailInspectorPanel::onSaveAllAttachmentsClicked);
    button_row->addWidget(m_save_all_attachments_button);
    button_row->addStretch();
    layout->addLayout(button_row);

    return container;
}

// ============================================================================
// Dialog Signal Isolation
// ============================================================================

void EmailInspectorPanel::disconnectDialogSignals() {
    disconnect(m_controller.get(),
               &EmailInspectorController::folderItemsLoaded,
               this,
               &EmailInspectorPanel::onFolderItemsLoaded);
    disconnect(m_controller.get(),
               &EmailInspectorController::itemDetailLoaded,
               this,
               &EmailInspectorPanel::onItemDetailLoaded);
    disconnect(m_controller.get(),
               &EmailInspectorController::itemPropertiesLoaded,
               this,
               &EmailInspectorPanel::onItemPropertiesLoaded);
    disconnect(m_controller.get(),
               &EmailInspectorController::attachmentContentReady,
               this,
               &EmailInspectorPanel::onAttachmentContentReady);
    disconnect(m_controller.get(),
               &EmailInspectorController::stateChanged,
               this,
               &EmailInspectorPanel::onStateChanged);
}

void EmailInspectorPanel::reconnectDialogSignals() {
    connect(m_controller.get(),
            &EmailInspectorController::folderItemsLoaded,
            this,
            &EmailInspectorPanel::onFolderItemsLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemDetailLoaded,
            this,
            &EmailInspectorPanel::onItemDetailLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemPropertiesLoaded,
            this,
            &EmailInspectorPanel::onItemPropertiesLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::attachmentContentReady,
            this,
            &EmailInspectorPanel::onAttachmentContentReady);
    connect(m_controller.get(),
            &EmailInspectorController::stateChanged,
            this,
            &EmailInspectorPanel::onStateChanged);
}

// ============================================================================
// Controller Connections
// ============================================================================

void EmailInspectorPanel::connectController() {
    Q_ASSERT(m_controller);

    // State
    connect(m_controller.get(),
            &EmailInspectorController::stateChanged,
            this,
            &EmailInspectorPanel::onStateChanged);

    // File
    connect(m_controller.get(),
            &EmailInspectorController::fileOpened,
            this,
            &EmailInspectorPanel::onFileOpened);
    connect(m_controller.get(),
            &EmailInspectorController::folderTreeLoaded,
            this,
            &EmailInspectorPanel::onFolderTreeLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::fileClosed,
            this,
            &EmailInspectorPanel::onFileClosed);

    // Navigation
    connect(m_controller.get(),
            &EmailInspectorController::folderItemsLoaded,
            this,
            &EmailInspectorPanel::onFolderItemsLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemDetailLoaded,
            this,
            &EmailInspectorPanel::onItemDetailLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::itemPropertiesLoaded,
            this,
            &EmailInspectorPanel::onItemPropertiesLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::attachmentContentReady,
            this,
            &EmailInspectorPanel::onAttachmentContentReady);

    // Search
    connect(m_controller.get(),
            &EmailInspectorController::searchHit,
            this,
            &EmailInspectorPanel::onSearchHit);
    connect(m_controller.get(),
            &EmailInspectorController::searchComplete,
            this,
            &EmailInspectorPanel::onSearchComplete);

    // Export
    connect(m_controller.get(),
            &EmailInspectorController::exportStarted,
            this,
            &EmailInspectorPanel::onExportStarted);
    connect(m_controller.get(),
            &EmailInspectorController::exportProgress,
            this,
            &EmailInspectorPanel::onExportProgress);
    connect(m_controller.get(),
            &EmailInspectorController::exportComplete,
            this,
            &EmailInspectorPanel::onExportComplete);

    // Common
    connect(m_controller.get(),
            &EmailInspectorController::errorOccurred,
            this,
            &EmailInspectorPanel::onErrorOccurred);
    connect(m_controller.get(),
            &EmailInspectorController::logOutput,
            this,
            &EmailInspectorPanel::logOutput);
    connect(m_controller.get(),
            &EmailInspectorController::progressUpdated,
            this,
            [this](int percent, QString status) {
                if (m_dialog_active) {
                    return;
                }
                m_progress_bar->setValue(percent);
                updateStatusBar(status);
            });

    // MBOX-specific
    connect(m_controller.get(),
            &EmailInspectorController::mboxOpened,
            this,
            &EmailInspectorPanel::onMboxOpened);
    connect(m_controller.get(),
            &EmailInspectorController::mboxMessagesLoaded,
            this,
            &EmailInspectorPanel::onMboxMessagesLoaded);
    connect(m_controller.get(),
            &EmailInspectorController::mboxMessageDetailLoaded,
            this,
            &EmailInspectorPanel::onMboxMessageDetailLoaded);
}

// ============================================================================
// File Operation Slots
// ============================================================================

void EmailInspectorPanel::onOpenFileClicked() {
    QString filter = tr(
        "Email Data Files (*.pst *.ost *.mbox);;"
        "PST Files (*.pst);;"
        "OST Files (*.ost);;"
        "MBOX Files (*.mbox);;"
        "All Files (*)");
    QString path =
        QFileDialog::getOpenFileName(this, tr("Open Email Data File"), QString(), filter);
    if (path.isEmpty()) {
        return;
    }
    m_controller->openFile(path);
}

void EmailInspectorPanel::onCloseFileClicked() {
    m_controller->closeFile();
}

// ============================================================================
// Navigation Slots
// ============================================================================

void EmailInspectorPanel::onFolderTreeItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) {
        return;
    }
    bool ok = false;
    uint64_t folder_id = item->data(0, Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_current_folder_id = folder_id;
    m_controller->loadFolderItems(folder_id, 0, email::kMaxItemsPerLoad);
}

void EmailInspectorPanel::onItemListCellClicked(int row, int /*column*/) {
    if (row < 0 || row >= m_item_list->rowCount()) {
        return;
    }
    auto* subject_item = m_item_list->item(row, ColSubject);
    if (!subject_item) {
        return;
    }
    bool ok = false;
    uint64_t item_id = subject_item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_current_item_id = item_id;
    m_controller->loadItemDetail(item_id);
    m_controller->loadItemProperties(item_id);
}

void EmailInspectorPanel::onItemListContextMenu(const QPoint& pos) {
    QMenu menu(this);
    menu.addAction(tr("Open in Detail Panel"), this, [this] {
        int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                uint64_t nid = subject_item->data(Qt::UserRole).toULongLong();
                m_controller->loadItemDetail(nid);
            }
        }
    });
    menu.addSeparator();
    menu.addAction(tr("Export as EML..."), this, [this] { onExportClicked(); });
    menu.addAction(tr("Browse Attachments..."), this, [this] { onExportAttachmentsClicked(); });
    menu.addSeparator();
    menu.addAction(tr("Copy Subject"), this, [this] {
        int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                QApplication::clipboard()->setText(subject_item->text());
            }
        }
    });
    menu.addAction(tr("View MAPI Properties"), this, [this] {
        int row = m_item_list->currentRow();
        if (row >= 0) {
            auto* subject_item = m_item_list->item(row, ColSubject);
            if (subject_item) {
                uint64_t nid = subject_item->data(Qt::UserRole).toULongLong();
                m_controller->loadItemProperties(nid);
                m_detail_tabs->setCurrentIndex(2);
            }
        }
    });
    menu.exec(m_item_list->viewport()->mapToGlobal(pos));
}

void EmailInspectorPanel::onFolderTreeContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_folder_tree->itemAt(pos);
    if (!item) {
        return;
    }
    QMenu menu(this);
    menu.addAction(tr("Export Folder as EML..."), this, [this] { onExportClicked(); });
    menu.addAction(tr("Export Folder as CSV..."), this, [this] { onExportClicked(); });
    menu.addSeparator();
    menu.addAction(tr("Browse Attachments..."), this, [this] { onExportAttachmentsClicked(); });
    menu.addSeparator();
    menu.addAction(tr("Search in This Folder..."), this, [this] { m_search_edit->setFocus(); });
    menu.addSeparator();
    menu.addAction(tr("Expand All Subfolders"), this, [item] {
        item->setExpanded(true);
        for (int idx = 0; idx < item->childCount(); ++idx) {
            item->child(idx)->setExpanded(true);
        }
    });
    menu.addAction(tr("Collapse All"), this, [item] { item->setExpanded(false); });
    menu.exec(m_folder_tree->viewport()->mapToGlobal(pos));
}

// ============================================================================
// Search Slots
// ============================================================================

void EmailInspectorPanel::onSearchClicked() {
    QString query = m_search_edit->text().trimmed();
    if (query.isEmpty()) {
        return;
    }
    sak::EmailSearchCriteria criteria;
    criteria.query_text = query;
    criteria.search_subject = true;
    criteria.search_body = true;
    criteria.search_sender = true;
    m_controller->startSearch(criteria);
}

void EmailInspectorPanel::onSearchTextChanged() {
    // Placeholder for debounced incremental search
}

// ============================================================================
// Export Slots
// ============================================================================

void EmailInspectorPanel::onExportClicked() {
    if (!m_controller->isFileOpen()) {
        return;
    }
    QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Export Directory"));
    if (dir_path.isEmpty()) {
        return;
    }

    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Eml;
    config.output_path = dir_path;
    config.folder_id = m_current_folder_id;
    m_controller->exportItems(config);
}

void EmailInspectorPanel::onExportAttachmentsClicked() {
    if (!m_controller->isFileOpen()) {
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailAttachmentsBrowserDialog dialog(m_controller.get(), m_cached_folder_tree, this);
    const auto result = dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
    if (result == QDialog::Accepted) {
        uint64_t folder_id = dialog.navigateFolderId();
        uint64_t message_id = dialog.navigateMessageId();
        if (folder_id != 0 && message_id != 0) {
            m_current_folder_id = folder_id;
            m_controller->loadFolderItems(folder_id, 0, m_page_size_combo->currentText().toInt());
            m_controller->loadItemDetail(message_id);
            m_controller->loadItemProperties(message_id);
        }
    }
}

// ============================================================================
// Attachment Slots
// ============================================================================

void EmailInspectorPanel::onSaveAttachmentClicked() {
    int row = m_attachments_table->currentRow();
    if (row < 0 || row >= m_current_detail.attachments.size()) {
        return;
    }
    const auto& attachment = m_current_detail.attachments.at(row);
    QString filename = attachment.long_filename.isEmpty() ? attachment.filename
                                                          : attachment.long_filename;
    QString save_path = QFileDialog::getSaveFileName(this, tr("Save Attachment"), filename);
    if (save_path.isEmpty()) {
        return;
    }
    m_controller->loadAttachmentContent(m_current_item_id, attachment.index);
}

void EmailInspectorPanel::onSaveAllAttachmentsClicked() {
    if (m_current_detail.attachments.isEmpty()) {
        return;
    }
    QString dir_path = QFileDialog::getExistingDirectory(this, tr("Save All Attachments"));
    if (dir_path.isEmpty()) {
        return;
    }
    for (const auto& attachment : m_current_detail.attachments) {
        m_controller->loadAttachmentContent(m_current_item_id, attachment.index);
    }
}

// ============================================================================
// Controller Signal Handlers
// ============================================================================

void EmailInspectorPanel::onStateChanged(EmailInspectorController::State state) {
    if (m_dialog_active) {
        return;
    }
    bool idle = (state == EmailInspectorController::State::Idle);
    setOperationRunning(!idle);
}

void EmailInspectorPanel::onFileOpened(sak::PstFileInfo info) {
    m_close_button->setEnabled(true);
    m_search_edit->setEnabled(true);
    m_search_button->setEnabled(true);
    m_export_emails_button->setEnabled(true);
    m_contacts_button->setEnabled(true);
    m_calendar_button->setEnabled(true);
    m_attachments_button->setEnabled(true);
    updateFileInfoBar(info);
}

void EmailInspectorPanel::onFolderTreeLoaded(sak::PstFolderTree tree) {
    m_cached_folder_tree = tree;
    populateFolderTree(tree);
}

void EmailInspectorPanel::onFileClosed() {
    m_folder_tree->clear();
    m_item_list->setRowCount(0);
    m_content_browser->clear();
    m_headers_browser->clear();
    m_properties_table->setRowCount(0);
    m_attachments_table->setRowCount(0);
    m_status_label->clear();
    m_item_count_label->clear();
    m_current_items.clear();
    m_current_folder_id = 0;
    m_current_item_id = 0;
    m_contact_folder_ids.clear();
    m_calendar_folder_ids.clear();
    m_cached_folder_tree.clear();

    m_close_button->setEnabled(false);
    m_search_edit->setEnabled(false);
    m_search_button->setEnabled(false);
    m_export_emails_button->setEnabled(false);
    m_contacts_button->setEnabled(false);
    m_calendar_button->setEnabled(false);
    m_attachments_button->setEnabled(false);
    m_save_attachment_button->setEnabled(false);
    m_save_all_attachments_button->setEnabled(false);

    updateStatusBar(tr("Ready"));
}

void EmailInspectorPanel::onFolderItemsLoaded(uint64_t /*folder_id*/,
                                              QVector<sak::PstItemSummary> items,
                                              int total) {
    if (m_dialog_active) {
        return;
    }
    m_current_items = items;
    populateItemList(items);
    const int visible = m_item_list->rowCount();
    int filtered = items.size() - visible;
    QString label = tr("%1 items (showing %2)").arg(total).arg(visible);
    if (filtered > 0) {
        label += tr("  [%1 blank filtered]").arg(filtered);
    }
    m_item_count_label->setText(label);
}

void EmailInspectorPanel::onItemDetailLoaded(sak::PstItemDetail detail) {
    if (m_dialog_active) {
        return;
    }
    m_current_detail = detail;
    displayItemDetail(detail);
    displayAttachments(detail.attachments);
    m_save_all_attachments_button->setEnabled(!detail.attachments.isEmpty());

    // Request inline image attachments for HTML rendering
    if (m_show_html && !detail.body_html.isEmpty()) {
        for (const auto& attachment : detail.attachments) {
            if (!attachment.content_id.isEmpty()) {
                m_controller->loadAttachmentContent(detail.node_id, attachment.index);
            }
        }
    }
}

void EmailInspectorPanel::onItemPropertiesLoaded(uint64_t /*item_id*/,
                                                 QVector<sak::MapiProperty> properties) {
    if (m_dialog_active) {
        return;
    }
    displayProperties(properties);
}

void EmailInspectorPanel::onAttachmentContentReady(uint64_t /*message_id*/,
                                                   int index,
                                                   QByteArray attachment_data,
                                                   QString filename) {
    if (m_dialog_active) {
        return;
    }

    // Check if this is an inline image (CID) that should be added to the
    // content browser rather than prompting a save dialog.
    for (const auto& att : m_current_detail.attachments) {
        if (att.index == index && !att.content_id.isEmpty()) {
            QUrl cid_url(QStringLiteral("cid:%1").arg(att.content_id));
            QImage image;
            if (image.loadFromData(attachment_data)) {
                m_content_browser->document()->addResource(QTextDocument::ImageResource,
                                                           cid_url,
                                                           image);
                // Re-render the current HTML to show the newly resolved image
                displayItemDetail(m_current_detail);
            }
            return;
        }
    }

    QString save_path = QFileDialog::getSaveFileName(this, tr("Save Attachment"), filename);
    if (save_path.isEmpty()) {
        return;
    }
    QFile file(save_path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(attachment_data);
        file.close();
        updateStatusBar(tr("Saved: %1").arg(filename));
    } else {
        sak::logError("Failed to save attachment: {}", save_path.toStdString());
        Q_EMIT logOutput(tr("Failed to save attachment: %1").arg(filename));
    }
}

void EmailInspectorPanel::onSearchHit(sak::EmailSearchHit /*hit*/) {
    // Incrementally update result count in status bar
}

void EmailInspectorPanel::onSearchComplete(int total_hits) {
    updateStatusBar(tr("Search complete: %1 hits").arg(total_hits));
}

void EmailInspectorPanel::onExportStarted(int total) {
    m_progress_bar->setVisible(true);
    m_progress_bar->setRange(0, total);
    m_progress_bar->setValue(0);
    updateStatusBar(tr("Exporting %1 items...").arg(total));
}

void EmailInspectorPanel::onExportProgress(int done, int total, qint64 /*bytes*/) {
    m_progress_bar->setRange(0, total);
    m_progress_bar->setValue(done);
}

void EmailInspectorPanel::onExportComplete(sak::EmailExportResult result) {
    m_progress_bar->setVisible(false);
    updateStatusBar(
        tr("Export complete: %1 items to %2").arg(result.items_exported).arg(result.export_path));
    Q_EMIT logOutput(tr("Exported %1 items (%2 bytes)")
                         .arg(result.items_exported)
                         .arg(formatBytes(result.total_bytes)));
}

void EmailInspectorPanel::onErrorOccurred(QString message) {
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorError));
    updateStatusBar(tr("Error: %1").arg(message));
    sak::logError("Email Tools: {}", message.toStdString());
    Q_EMIT logOutput(tr("Error: %1").arg(message));
}

// MBOX-specific handlers
void EmailInspectorPanel::onMboxOpened(int message_count) {
    m_close_button->setEnabled(true);
    m_search_edit->setEnabled(true);
    m_search_button->setEnabled(true);
    m_export_emails_button->setEnabled(true);
    m_contacts_button->setEnabled(false);
    m_calendar_button->setEnabled(false);
    m_attachments_button->setEnabled(true);

    // For MBOX, create a single root folder item
    m_folder_tree->clear();
    auto* root = new QTreeWidgetItem(m_folder_tree);
    root->setText(0, tr("Inbox (%1 messages)").arg(message_count));
    root->setData(0, Qt::UserRole, QVariant::fromValue<uint64_t>(0));
    m_folder_tree->expandAll();

    updateStatusBar(tr("Opened MBOX: %1 messages").arg(message_count));
}

void EmailInspectorPanel::onMboxMessagesLoaded(QVector<sak::MboxMessage> messages, int total) {
    m_item_list->setSortingEnabled(false);
    m_item_list->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_item_list);

    // Filter blank messages
    QVector<int> visible_indices;
    visible_indices.reserve(messages.size());
    for (int idx = 0; idx < messages.size(); ++idx) {
        const auto& msg = messages.at(idx);
        if (!msg.subject.trimmed().isEmpty() || !msg.from.trimmed().isEmpty()) {
            visible_indices.append(idx);
        }
    }

    const int count = visible_indices.size();
    m_item_list->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& msg = messages.at(visible_indices.at(row));
        QString att_icon = msg.has_attachments ? QStringLiteral("\xF0\x9F\x93\x8E") : QString();
        m_item_list->setItem(row, ColAttachment, new QTableWidgetItem(att_icon));

        auto* subject_cell = new QTableWidgetItem(msg.subject);
        subject_cell->setData(Qt::UserRole, QVariant::fromValue<uint64_t>(visible_indices.at(row)));
        m_item_list->setItem(row, ColSubject, subject_cell);

        m_item_list->setItem(row, ColFrom, new QTableWidgetItem(msg.from));
        m_item_list->setItem(row, ColDate, new QTableWidgetItem(msg.date.toString(Qt::ISODate)));
        m_item_list->setItem(row, ColSize, new QTableWidgetItem(formatBytes(msg.message_size)));

        auto* type_cell = new QTableWidgetItem(tr("Email"));
        type_cell->setIcon(itemTypeQIcon(sak::EmailItemType::Email));
        m_item_list->setItem(row, ColType, type_cell);
    }
    m_item_list->setUpdatesEnabled(true);
    m_item_list->setSortingEnabled(true);
    int filtered = total - count;
    QString label = tr("%1 messages (showing %2)").arg(total).arg(count);
    if (filtered > 0) {
        label += tr("  [%1 blank filtered]").arg(filtered);
    }
    m_item_count_label->setText(label);
}

void EmailInspectorPanel::onMboxMessageDetailLoaded(sak::MboxMessageDetail detail) {
    // Display in content tab respecting HTML/plain text toggle
    if (m_show_html && !detail.body_html.isEmpty()) {
        QString wrapped = QStringLiteral(
                              "<html><head>"
                              "<meta charset=\"utf-8\">"
                              "<style>body { font-family: 'Segoe UI', sans-serif;"
                              " font-size: 13px; margin: 0; padding: 8px;"
                              " word-wrap: break-word; }"
                              " img { max-width: 100%%; height: auto; }</style>"
                              "</head><body>%1</body></html>")
                              .arg(detail.body_html);
        m_content_browser->setHtml(wrapped);
    } else if (!detail.body_plain.isEmpty()) {
        m_content_browser->setPlainText(detail.body_plain);
    } else if (!detail.body_html.isEmpty()) {
        m_content_browser->setHtml(detail.body_html);
    } else {
        m_content_browser->clear();
    }

    // Display headers
    m_headers_browser->setPlainText(detail.raw_headers);

    // Attachments
    displayAttachments(detail.attachments);
}

// ============================================================================
// Helpers
// ============================================================================

void EmailInspectorPanel::setOperationRunning(bool running) {
    m_open_button->setEnabled(!running);
    m_progress_bar->setVisible(running);
    if (running) {
        m_progress_bar->setRange(0, 0);
    }
}

void EmailInspectorPanel::populateFolderTree(const sak::PstFolderTree& tree) {
    m_folder_tree->clear();
    m_contact_folder_ids.clear();
    m_calendar_folder_ids.clear();

    // Find IPM_SUBTREE to show user-facing folders at the top level.
    const sak::PstFolder* ipm = findIpmSubtree(tree);

    const auto& source = (ipm != nullptr && !ipm->children.isEmpty()) ? ipm->children : tree;

    // Collect contacts/calendar folder IDs before filtering
    for (const auto& folder : source) {
        collectSpecialFolderIds(folder);
    }

    // Sort folders by display priority
    QVector<const sak::PstFolder*> sorted;
    sorted.reserve(source.size());
    for (const auto& folder : source) {
        if (!isHiddenFolder(folder.display_name, folder.container_class)) {
            sorted.append(&folder);
        }
    }
    std::sort(sorted.begin(),
              sorted.end(),
              [](const sak::PstFolder* lhs, const sak::PstFolder* rhs) {
                  int order_lhs = folderSortOrder(lhs->display_name, lhs->container_class);
                  int order_rhs = folderSortOrder(rhs->display_name, rhs->container_class);
                  if (order_lhs != order_rhs) {
                      return order_lhs < order_rhs;
                  }
                  return lhs->display_name.compare(rhs->display_name, Qt::CaseInsensitive) < 0;
              });

    for (const auto* folder : sorted) {
        auto* item = new QTreeWidgetItem(m_folder_tree);
        item->setIcon(0, folderIcon(folder->display_name));
        item->setText(
            0, QStringLiteral("%1 (%2)").arg(folder->display_name).arg(folder->content_count));
        item->setData(0, Qt::UserRole, QVariant::fromValue(folder->node_id));
        for (const auto& child : folder->children) {
            addFolderToTree(item, child);
        }
    }
    m_folder_tree->expandToDepth(0);
}

void EmailInspectorPanel::addFolderToTree(QTreeWidgetItem* parent, const sak::PstFolder& folder) {
    if (isHiddenFolder(folder.display_name, folder.container_class)) {
        return;
    }
    auto* item = new QTreeWidgetItem(parent);
    item->setIcon(0, folderIcon(folder.display_name));
    item->setText(0, QStringLiteral("%1 (%2)").arg(folder.display_name).arg(folder.content_count));
    item->setData(0, Qt::UserRole, QVariant::fromValue(folder.node_id));
    for (const auto& child : folder.children) {
        addFolderToTree(item, child);
    }
}

// static
const sak::PstFolder* EmailInspectorPanel::findIpmSubtree(const QVector<sak::PstFolder>& folders) {
    // OST files have two IPM_SUBTREE nodes — one under Root - Public
    // (empty) and one under Root - Mailbox (with all user folders).
    // We want the one that actually has children.
    const sak::PstFolder* best = nullptr;

    for (const auto& folder : folders) {
        if (folder.display_name == QLatin1String("IPM_SUBTREE") ||
            folder.display_name == QLatin1String("Top of Information Store")) {
            if (!folder.children.isEmpty()) {
                return &folder;
            }
            if (best == nullptr) {
                best = &folder;
            }
        }
        const sak::PstFolder* found = findIpmSubtree(folder.children);
        if (found != nullptr && !found->children.isEmpty()) {
            return found;
        }
        if (found != nullptr && best == nullptr) {
            best = found;
        }
    }
    return best;
}

// static
QIcon EmailInspectorPanel::folderIcon(const QString& name) {
    const QString lower = name.toLower();
    if (lower == QLatin1String("inbox")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-inbox.svg"));
    }
    if (lower == QLatin1String("sent items")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-sent.svg"));
    }
    if (lower == QLatin1String("drafts")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-edit-file.svg"));
    }
    if (lower == QLatin1String("deleted items")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-trash.svg"));
    }
    if (lower == QLatin1String("junk email")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-spam.svg"));
    }
    if (lower == QLatin1String("outbox")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-clock.svg"));
    }
    if (lower == QLatin1String("archive")) {
        return QIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"));
    }
    return QIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"));
}

// static
int EmailInspectorPanel::folderSortOrder(const QString& name, const QString& /*container_class*/) {
    // Priority ordering for common Outlook folders.
    // Lower number = appears higher in the folder tree.
    if (name.compare(QLatin1String("Inbox"), Qt::CaseInsensitive) == 0) {
        return 0;
    }
    if (name.compare(QLatin1String("Drafts"), Qt::CaseInsensitive) == 0) {
        return 1;
    }
    if (name.compare(QLatin1String("Sent Items"), Qt::CaseInsensitive) == 0) {
        return 2;
    }
    if (name.compare(QLatin1String("Deleted Items"), Qt::CaseInsensitive) == 0) {
        return 3;
    }
    if (name.compare(QLatin1String("Archive"), Qt::CaseInsensitive) == 0) {
        return 4;
    }
    if (name.compare(QLatin1String("Junk Email"), Qt::CaseInsensitive) == 0) {
        return 5;
    }
    if (name.compare(QLatin1String("Outbox"), Qt::CaseInsensitive) == 0) {
        return 6;
    }
    return 10;  // All other user folders
}

// static
bool EmailInspectorPanel::isHiddenFolder(const QString& name, const QString& container_class) {
    // Contact and calendar folders are shown in their own modals
    if (container_class.startsWith(QLatin1String("IPF.Contact"))) {
        return true;
    }
    if (container_class.startsWith(QLatin1String("IPF.Appointment"))) {
        return true;
    }

    // Task, Note, and Journal folders are not email — hide them
    if (container_class.startsWith(QLatin1String("IPF.Task"))) {
        return true;
    }
    if (container_class.startsWith(QLatin1String("IPF.StickyNote"))) {
        return true;
    }
    if (container_class.startsWith(QLatin1String("IPF.Journal"))) {
        return true;
    }

    // System/configuration folders that clutter the tree
    if (container_class == QLatin1String("IPF.Configuration")) {
        return true;
    }

    // Known system folder names to hide
    static const QStringList kHiddenNames = {
        QStringLiteral("PersonMetadata"),
        QStringLiteral("MeContact"),
        QStringLiteral("ExternalContacts"),
        QStringLiteral("Quick Step Settings"),
        QStringLiteral("Conversation Action Settings"),
        QStringLiteral("Yammer Root"),
        QStringLiteral("Social Activity Notifications"),
        QStringLiteral("Conversation History"),
        QStringLiteral("Files"),
        QStringLiteral("Sync Issues"),
        QStringLiteral("Conflicts"),
        QStringLiteral("Local Failures"),
        QStringLiteral("Server Failures"),
    };
    for (const auto& hidden : kHiddenNames) {
        if (name.compare(hidden, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

void EmailInspectorPanel::collectSpecialFolderIds(const sak::PstFolder& folder) {
    if (folder.container_class.startsWith(QLatin1String("IPF.Contact"))) {
        m_contact_folder_ids.append(folder.node_id);
    }
    if (folder.container_class.startsWith(QLatin1String("IPF.Appointment"))) {
        m_calendar_folder_ids.append(folder.node_id);
    }
    for (const auto& child : folder.children) {
        collectSpecialFolderIds(child);
    }
}

void EmailInspectorPanel::onScanForFilesClicked() {
    EmailFileScannerDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        QString path = dialog.selectedFilePath();
        if (!path.isEmpty()) {
            m_controller->openFile(path);
        }
    }
}

void EmailInspectorPanel::onContactsClicked() {
    if (m_contact_folder_ids.isEmpty()) {
        QMessageBox::information(this,
                                 tr("No Contacts"),
                                 tr("No contact folders were found in this file."));
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailContactsDialog dialog(m_controller.get(), m_contact_folder_ids, this);
    dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
}

void EmailInspectorPanel::onCalendarClicked() {
    if (m_calendar_folder_ids.isEmpty()) {
        QMessageBox::information(this,
                                 tr("No Calendar"),
                                 tr("No calendar folders were found in this file."));
        return;
    }
    m_dialog_active = true;
    disconnectDialogSignals();
    EmailCalendarDialog dialog(m_controller.get(), m_calendar_folder_ids, this);
    dialog.exec();
    reconnectDialogSignals();
    m_dialog_active = false;
}

void EmailInspectorPanel::populateItemList(const QVector<sak::PstItemSummary>& items) {
    m_item_list->setSortingEnabled(false);
    m_item_list->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_item_list);

    const int page_size = m_page_size_combo->currentText().toInt();

    // Filter out blank/unknown items, limited to page_size
    QVector<const sak::PstItemSummary*> visible;
    visible.reserve(page_size);
    for (const auto& item : items) {
        if (!isBlankItem(item)) {
            visible.append(&item);
            if (visible.size() >= page_size) {
                break;
            }
        }
    }

    const int count = visible.size();
    m_item_list->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& item = *visible.at(row);

        // Column 0 — type icon + attachment indicator
        auto* icon_cell = new QTableWidgetItem();
        icon_cell->setIcon(itemTypeQIcon(item.item_type));
        if (item.has_attachments) {
            icon_cell->setText(QStringLiteral("\xF0\x9F\x93\x8E"));
        }
        m_item_list->setItem(row, ColAttachment, icon_cell);

        auto* subject_cell = new QTableWidgetItem(item.subject);
        subject_cell->setData(Qt::UserRole, QVariant::fromValue(item.node_id));
        m_item_list->setItem(row, ColSubject, subject_cell);

        m_item_list->setItem(row, ColFrom, new QTableWidgetItem(item.sender_name));
        m_item_list->setItem(row, ColDate, new QTableWidgetItem(item.date.toString(Qt::ISODate)));
        m_item_list->setItem(row, ColSize, new QTableWidgetItem(formatBytes(item.size_bytes)));

        auto* type_cell = new QTableWidgetItem(itemTypeLabel(item.item_type));
        m_item_list->setItem(row, ColType, type_cell);
    }
    m_item_list->setUpdatesEnabled(true);
    m_item_list->setSortingEnabled(true);
}

void EmailInspectorPanel::displayTaskDetail(const sak::PstItemDetail& detail) {
    QString html = QStringLiteral(
                       "<div style='font-family: Segoe UI, sans-serif; "
                       "padding: 12px;'>"
                       "<h2 style='color: %1;'>%2</h2>")
                       .arg(ui::kColorTextHeading)
                       .arg(detail.subject.toHtmlEscaped());
    static const char* const kTaskStatus[] = {
        "Not Started", "In Progress", "Complete", "Waiting", "Deferred"};
    constexpr int kTaskStatusCount = 5;
    if (detail.task_status >= 0 && detail.task_status < kTaskStatusCount) {
        html += QStringLiteral("<p><b>Status:</b> %1 (%2%)</p>")
                    .arg(QLatin1String(kTaskStatus[detail.task_status]))
                    .arg(static_cast<int>(detail.task_percent_complete * 100));
    }
    if (detail.task_due_date.isValid()) {
        html += QStringLiteral("<p><b>Due:</b> %1</p>")
                    .arg(detail.task_due_date.toString(Qt::RFC2822Date));
    }
    if (detail.task_start_date.isValid()) {
        html += QStringLiteral("<p><b>Start:</b> %1</p>")
                    .arg(detail.task_start_date.toString(Qt::RFC2822Date));
    }
    html += QStringLiteral("<hr style='border: 1px solid %1;'>").arg(ui::kColorBorderDefault);
    if (!detail.body_html.isEmpty()) {
        html += detail.body_html;
    } else if (!detail.body_plain.isEmpty()) {
        html += QStringLiteral(
                    "<pre style='white-space: pre-wrap;'>"
                    "%1</pre>")
                    .arg(detail.body_plain.toHtmlEscaped());
    }
    html += QStringLiteral("</div>");
    m_content_browser->setHtml(html);
    if (!detail.transport_headers.isEmpty()) {
        m_headers_browser->setPlainText(detail.transport_headers);
    } else {
        m_headers_browser->setPlainText(tr("No transport headers available"));
    }
}

void EmailInspectorPanel::displayNoteDetail(const sak::PstItemDetail& detail) {
    static const char* const kNoteColors[] = {
        "#DBEAFE", "#D1FAE5", "#FCE7F3", "#FEF9C3", "#F3F4F6"};
    constexpr int kNoteColorCount = 5;
    int color_idx =
        (detail.note_color >= 0 && detail.note_color < kNoteColorCount) ? detail.note_color : 3;
    QString html = QStringLiteral(
                       "<div style='font-family: Segoe UI, sans-serif; "
                       "padding: 16px; background: %1; "
                       "border-radius: 8px; min-height: 200px;'>"
                       "<h3 style='color: %2;'>%3</h3>"
                       "<p style='white-space: pre-wrap; "
                       "color: %4;'>%5</p></div>")
                       .arg(QLatin1String(kNoteColors[color_idx]))
                       .arg(ui::kColorTextHeading)
                       .arg(detail.subject.toHtmlEscaped())
                       .arg(ui::kColorTextBody)
                       .arg(detail.body_plain.toHtmlEscaped());
    m_content_browser->setHtml(html);
    m_headers_browser->setPlainText(tr("No transport headers available"));
}

void EmailInspectorPanel::displayItemDetail(const sak::PstItemDetail& detail) {
    if (detail.item_type == sak::EmailItemType::Task) {
        displayTaskDetail(detail);
        return;
    }

    if (detail.item_type == sak::EmailItemType::StickyNote) {
        displayNoteDetail(detail);
        return;
    }

    // Default: email / journal / RSS / conversation history
    if (m_show_html && !detail.body_html.isEmpty()) {
        // Wrap with charset declaration so QTextBrowser decodes correctly
        QString wrapped = QStringLiteral(
                              "<html><head>"
                              "<meta charset=\"utf-8\">"
                              "<style>body { font-family: 'Segoe UI', sans-serif;"
                              " font-size: 13px; margin: 0; padding: 8px;"
                              " word-wrap: break-word; }"
                              " img { max-width: 100%%; height: auto; }</style>"
                              "</head><body>%1</body></html>")
                              .arg(detail.body_html);
        m_content_browser->setHtml(wrapped);
    } else if (!detail.body_plain.isEmpty()) {
        m_content_browser->setPlainText(detail.body_plain);
    } else if (!detail.body_html.isEmpty()) {
        // No plain text available — show HTML even when plain text preferred
        m_content_browser->setHtml(detail.body_html);
    } else {
        m_content_browser->clear();
    }

    if (!detail.transport_headers.isEmpty()) {
        m_headers_browser->setPlainText(detail.transport_headers);
    } else {
        m_headers_browser->setPlainText(tr("No transport headers available"));
    }
}

void EmailInspectorPanel::displayProperties(const QVector<sak::MapiProperty>& props) {
    m_properties_table->setSortingEnabled(false);
    m_properties_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_properties_table);
    const int count = props.size();
    m_properties_table->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& prop = props.at(row);
        m_properties_table->setItem(row, 0, new QTableWidgetItem(prop.property_name));
        QString display = prop.display_value.isEmpty() ? QStringLiteral("<empty>")
                                                       : prop.display_value;
        m_properties_table->setItem(row, 1, new QTableWidgetItem(display));
    }
    m_properties_table->setUpdatesEnabled(true);
    m_properties_table->setSortingEnabled(true);
}

void EmailInspectorPanel::displayAttachments(const QVector<sak::PstAttachmentInfo>& attachments) {
    m_attachments_table->setSortingEnabled(false);
    m_attachments_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_attachments_table);
    const int count = attachments.size();
    m_attachments_table->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& att = attachments.at(row);
        QString name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        m_attachments_table->setItem(row, 0, new QTableWidgetItem(name));
        m_attachments_table->setItem(row, 1, new QTableWidgetItem(formatBytes(att.size_bytes)));
        m_attachments_table->setItem(row, 2, new QTableWidgetItem(att.mime_type));
        m_attachments_table->setItem(row, 3, new QTableWidgetItem(att.content_id));
    }
    m_attachments_table->setUpdatesEnabled(true);
    m_attachments_table->setSortingEnabled(true);
    m_save_attachment_button->setEnabled(!attachments.isEmpty());
    m_save_all_attachments_button->setEnabled(!attachments.isEmpty());
}

void EmailInspectorPanel::updateFileInfoBar(const sak::PstFileInfo& info) {
    updateStatusBar(QStringLiteral("%1 \u2014 %2 \u2014 %3 items")
                        .arg(info.display_name)
                        .arg(formatBytes(info.file_size_bytes))
                        .arg(info.total_items));
}

void EmailInspectorPanel::updateStatusBar(const QString& message) {
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    m_status_label->setText(message);
    Q_EMIT statusMessage(message, kTimerStatusDefaultMs);
}

void EmailInspectorPanel::logMessage(const QString& message) {
    Q_EMIT logOutput(message);
}

QIcon EmailInspectorPanel::itemTypeQIcon(sak::EmailItemType type) {
    switch (type) {
    case sak::EmailItemType::Email:
        return QIcon(QStringLiteral(":/icons/icons/icons8-sent.svg"));
    case sak::EmailItemType::Contact:
        return QIcon(QStringLiteral(":/icons/icons/icons8-address-book.svg"));
    case sak::EmailItemType::Calendar:
        return QIcon(QStringLiteral(":/icons/icons/icons8-calendar.svg"));
    case sak::EmailItemType::Task:
        return QIcon(QStringLiteral(":/icons/icons/icons8-edit-file.svg"));
    case sak::EmailItemType::StickyNote:
        return QIcon(QStringLiteral(":/icons/icons/icons8-star.svg"));
    case sak::EmailItemType::JournalEntry:
        return QIcon(QStringLiteral(":/icons/icons/icons8-source.svg"));
    case sak::EmailItemType::DistList:
        return QIcon(QStringLiteral(":/icons/icons/icons8-address-book.svg"));
    case sak::EmailItemType::MeetingRequest:
        return QIcon(QStringLiteral(":/icons/icons/icons8-calendar.svg"));
    default:
        return QIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"));
    }
}

QString EmailInspectorPanel::itemTypeLabel(sak::EmailItemType type) {
    switch (type) {
    case sak::EmailItemType::Email:
        return tr("Email");
    case sak::EmailItemType::Contact:
        return tr("Contact");
    case sak::EmailItemType::Calendar:
        return tr("Calendar");
    case sak::EmailItemType::Task:
        return tr("Task");
    case sak::EmailItemType::StickyNote:
        return tr("Note");
    case sak::EmailItemType::JournalEntry:
        return tr("Journal");
    case sak::EmailItemType::DistList:
        return tr("Dist List");
    case sak::EmailItemType::MeetingRequest:
        return tr("Meeting");
    default:
        return tr("Unknown");
    }
}

bool EmailInspectorPanel::isBlankItem(const sak::PstItemSummary& item) {
    // Filter FAI (Folder Associated Information) messages — system/hidden
    // items that Outlook never shows. MSGFLAG_ASSOCIATED = 0x40.
    constexpr uint32_t kMsgFlagAssociated = 0x40;
    if ((item.message_flags & kMsgFlagAssociated) != 0) {
        return true;
    }
    // Also filter items with no subject AND no sender
    bool blank_subject = item.subject.trimmed().isEmpty();
    bool blank_sender = item.sender_name.trimmed().isEmpty() &&
                        item.sender_email.trimmed().isEmpty();
    return blank_subject && blank_sender;
}

void EmailInspectorPanel::applyPageSize() {
    if (m_current_folder_id == 0 || m_current_items.isEmpty()) {
        return;
    }
    populateItemList(m_current_items);
    const int visible = m_item_list->rowCount();
    const int total = m_current_items.size();
    m_item_count_label->setText(tr("%1 items (showing %2)").arg(total).arg(visible));
}

QString EmailInspectorPanel::formatBytes(qint64 bytes) {
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

}  // namespace sak
