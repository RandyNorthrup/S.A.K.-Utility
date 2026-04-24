// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_contacts_dialog.cpp
/// @brief Address book modal implementation

#include "sak/email_contacts_dialog.h"

#include "sak/email_inspector_controller.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace sak {

// ============================================================================
// Column indices for the contact table
// ============================================================================

enum ContactColumn {
    ColName = 0,
    ColEmail,
    ColCompany,
    ColPhone,
    ColCount
};

// ============================================================================
// Construction
// ============================================================================

EmailContactsDialog::EmailContactsDialog(::EmailInspectorController* controller,
                                         const QVector<uint64_t>& contact_folder_ids,
                                         QWidget* parent)
    : QDialog(parent), m_controller(controller), m_folder_ids(contact_folder_ids) {
    setWindowTitle(tr("Address Book — Contacts"));
    setModal(true);
    resize(kWizardLargeWidth, kWizardLargeHeight);

    // Debounce search input — refilter at most once every 150 ms while
    // the user is typing to avoid O(N) scans on every keystroke.
    constexpr int kSearchDebounceMs = 150;
    m_search_timer = new QTimer(this);
    m_search_timer->setSingleShot(true);
    m_search_timer->setInterval(kSearchDebounceMs);
    connect(m_search_timer, &QTimer::timeout, this, [this] {
        filterContacts(m_pending_search_text);
    });

    setupUi();
    loadContacts();
}

EmailContactsDialog::~EmailContactsDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void EmailContactsDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge);
    layout->setSpacing(ui::kSpacingDefault);

    // Title
    auto* heading = new QLabel(tr("Address Book"), this);
    heading->setStyleSheet(QStringLiteral("font-size: %1pt; font-weight: 600; "
                                          "color: %2;")
                               .arg(ui::kFontSizeTitle)
                               .arg(ui::kColorTextHeading));
    layout->addWidget(heading);

    // Search bar
    auto* search_row = new QHBoxLayout();
    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText(tr("Search contacts by name, email, or company..."));
    m_search_edit->setClearButtonEnabled(true);
    connect(
        m_search_edit, &QLineEdit::textChanged, this, &EmailContactsDialog::onSearchTextChanged);
    search_row->addWidget(m_search_edit);
    layout->addLayout(search_row);

    // Splitter: contact table | detail panel
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    // Contact table
    m_contact_table = new QTableWidget(this);
    m_contact_table->setColumnCount(ColCount);
    m_contact_table->setHorizontalHeaderLabels(
        {tr("Name"), tr("Email"), tr("Company"), tr("Phone")});
    m_contact_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_contact_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_contact_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_contact_table->setSortingEnabled(true);
    m_contact_table->horizontalHeader()->setStretchLastSection(true);
    m_contact_table->setColumnWidth(ColName, 200);
    m_contact_table->setColumnWidth(ColEmail, 250);
    m_contact_table->setColumnWidth(ColCompany, 180);
    m_contact_table->verticalHeader()->setVisible(false);
    connect(
        m_contact_table, &QTableWidget::cellClicked, this, &EmailContactsDialog::onContactSelected);
    m_splitter->addWidget(m_contact_table);

    // Detail panel
    m_detail_browser = new QTextBrowser(this);
    m_detail_browser->setOpenExternalLinks(false);
    m_detail_browser->setMinimumWidth(280);
    m_detail_browser->setPlaceholderText(tr("Select a contact to view details"));
    m_splitter->addWidget(m_detail_browser);
    m_splitter->setSizes({550, 350});
    layout->addWidget(m_splitter, 1);

    // Button row
    auto* button_row = new QHBoxLayout();

    m_export_vcf_button = new QPushButton(tr("Export VCF"), this);
    m_export_vcf_button->setToolTip(tr("Export all contacts as vCard (.vcf) files"));
    m_export_vcf_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(
        m_export_vcf_button, &QPushButton::clicked, this, &EmailContactsDialog::onExportVcfClicked);
    button_row->addWidget(m_export_vcf_button);

    m_export_csv_button = new QPushButton(tr("Export CSV"), this);
    m_export_csv_button->setToolTip(tr("Export all contacts as a CSV spreadsheet"));
    m_export_csv_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(
        m_export_csv_button, &QPushButton::clicked, this, &EmailContactsDialog::onExportCsvClicked);
    button_row->addWidget(m_export_csv_button);

    button_row->addStretch();

    m_status_label = new QLabel(this);
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    button_row->addWidget(m_status_label);

    button_row->addStretch();

    m_close_button = new QPushButton(tr("Close"), this);
    m_close_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(m_close_button, &QPushButton::clicked, this, &QDialog::accept);
    button_row->addWidget(m_close_button);

    layout->addLayout(button_row);
}

// ============================================================================
// Data loading
// ============================================================================

void EmailContactsDialog::loadContacts() {
    if (m_folder_ids.isEmpty()) {
        m_status_label->setText(tr("No contact folders found"));
        return;
    }

    m_status_label->setText(tr("Loading contacts..."));
    m_folders_loaded = 0;
    m_all_contacts.clear();

    // Connect to controller to receive folder items
    connect(m_controller,
            &::EmailInspectorController::folderItemsLoaded,
            this,
            &EmailContactsDialog::onItemsLoaded);
    connect(m_controller,
            &::EmailInspectorController::itemDetailLoaded,
            this,
            &EmailContactsDialog::onDetailLoaded);

    constexpr int kMaxContactsPerFolder = 2000;
    for (uint64_t folder_id : m_folder_ids) {
        m_controller->loadFolderItems(folder_id, 0, kMaxContactsPerFolder);
    }
}

// ============================================================================
// Slots
// ============================================================================

void EmailContactsDialog::onSearchTextChanged(const QString& text) {
    m_pending_search_text = text;
    m_search_timer->start();
}

void EmailContactsDialog::onContactSelected(int row, int /*column*/) {
    if (row < 0 || row >= m_contact_table->rowCount()) {
        return;
    }
    auto* hidden_item = m_contact_table->item(row, 0);
    if (hidden_item == nullptr) {
        return;
    }
    bool ok = false;
    uint64_t node_id = hidden_item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_controller->loadItemDetail(node_id);
}

void EmailContactsDialog::onExportVcfClicked() {
    QString dir_path =
        QFileDialog::getExistingDirectory(this, tr("Select Export Directory for VCF Files"));
    if (dir_path.isEmpty()) {
        return;
    }
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Vcf;
    config.output_path = dir_path;
    for (uint64_t folder_id : m_folder_ids) {
        config.folder_id = folder_id;
        m_controller->exportItems(config);
    }
}

void EmailContactsDialog::onExportCsvClicked() {
    QString file_path = QFileDialog::getSaveFileName(this,
                                                     tr("Export Contacts as CSV"),
                                                     QStringLiteral("contacts.csv"),
                                                     tr("CSV Files (*.csv)"));
    if (file_path.isEmpty()) {
        return;
    }
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::CsvContacts;
    config.output_path = file_path;
    for (uint64_t folder_id : m_folder_ids) {
        config.folder_id = folder_id;
        m_controller->exportItems(config);
    }
}

void EmailContactsDialog::onItemsLoaded(uint64_t /*folder_id*/,
                                        QVector<sak::PstItemSummary> items,
                                        int /*total*/) {
    for (const auto& item : items) {
        if (item.item_type == EmailItemType::Contact || item.item_type == EmailItemType::DistList) {
            m_all_contacts.append(item);
        }
    }
    ++m_folders_loaded;
    if (m_folders_loaded >= m_folder_ids.size()) {
        filterContacts(m_search_edit->text());
        m_status_label->setText(tr("%1 contacts").arg(m_all_contacts.size()));
    }
}

void EmailContactsDialog::onDetailLoaded(sak::PstItemDetail detail) {
    if (detail.item_type == EmailItemType::Contact || detail.item_type == EmailItemType::DistList) {
        displayContactDetail(detail);
    }
}

// ============================================================================
// Helpers
// ============================================================================

void EmailContactsDialog::filterContacts(const QString& text) {
    m_contact_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_contact_table);
    m_contact_table->setSortingEnabled(false);
    m_contact_table->setRowCount(0);
    QString filter = text.trimmed().toLower();

    // Pre-filter into indices so the table can be sized in a single call
    // instead of paying per-row `insertRow` cost.
    QVector<int> matching;
    matching.reserve(m_all_contacts.size());
    for (int idx = 0; idx < m_all_contacts.size(); ++idx) {
        const auto& contact = m_all_contacts.at(idx);
        if (!filter.isEmpty()) {
            bool match = contact.subject.toLower().contains(filter) ||
                         contact.sender_name.toLower().contains(filter) ||
                         contact.sender_email.toLower().contains(filter);
            if (!match) {
                continue;
            }
        }
        matching.append(idx);
    }
    m_contact_table->setRowCount(matching.size());
    for (int row = 0; row < matching.size(); ++row) {
        const auto& contact = m_all_contacts.at(matching.at(row));

        auto* name_item = new QTableWidgetItem(contact.subject);
        name_item->setData(Qt::UserRole, QVariant::fromValue(contact.node_id));
        m_contact_table->setItem(row, ColName, name_item);
        m_contact_table->setItem(row, ColEmail, new QTableWidgetItem(contact.sender_email));
        m_contact_table->setItem(row, ColCompany, new QTableWidgetItem(contact.sender_name));
        m_contact_table->setItem(row, ColPhone, new QTableWidgetItem());
    }
    m_contact_table->setSortingEnabled(true);
    m_contact_table->setUpdatesEnabled(true);
}

void EmailContactsDialog::displayContactDetail(const sak::PstItemDetail& detail) {
    QString html = QStringLiteral(
                       "<div style='font-family: Segoe UI, sans-serif; "
                       "padding: 12px;'>"
                       "<h2 style='color: %1; margin-bottom: 8px;'>%2 %3</h2>")
                       .arg(ui::kColorTextHeading)
                       .arg(detail.given_name.toHtmlEscaped())
                       .arg(detail.surname.toHtmlEscaped());

    if (!detail.job_title.isEmpty() || !detail.company_name.isEmpty()) {
        html += QStringLiteral("<p style='color: %1; margin: 2px 0;'>%2")
                    .arg(ui::kColorTextSecondary)
                    .arg(detail.job_title.toHtmlEscaped());
        if (!detail.company_name.isEmpty()) {
            html += QStringLiteral(" at <b>%1</b>").arg(detail.company_name.toHtmlEscaped());
        }
        html += QStringLiteral("</p>");
    }

    html += QStringLiteral("<hr style='border: 1px solid %1;'>").arg(ui::kColorBorderDefault);

    // Email
    if (!detail.email_address.isEmpty()) {
        html += QStringLiteral("<p><b>Email:</b> %1</p>").arg(detail.email_address.toHtmlEscaped());
    }

    // Phones
    if (!detail.business_phone.isEmpty()) {
        html +=
            QStringLiteral("<p><b>Business:</b> %1</p>").arg(detail.business_phone.toHtmlEscaped());
    }
    if (!detail.mobile_phone.isEmpty()) {
        html += QStringLiteral("<p><b>Mobile:</b> %1</p>").arg(detail.mobile_phone.toHtmlEscaped());
    }
    if (!detail.home_phone.isEmpty()) {
        html += QStringLiteral("<p><b>Home:</b> %1</p>").arg(detail.home_phone.toHtmlEscaped());
    }

    // Subject (often the full display name)
    if (!detail.subject.isEmpty()) {
        html +=
            QStringLiteral("<p><b>Display Name:</b> %1</p>").arg(detail.subject.toHtmlEscaped());
    }

    // Notes / body
    if (!detail.body_plain.isEmpty()) {
        html += QStringLiteral(
                    "<hr style='border: 1px solid %1;'>"
                    "<p style='color: %2; white-space: pre-wrap;'>%3</p>")
                    .arg(ui::kColorBorderDefault)
                    .arg(ui::kColorTextBody)
                    .arg(detail.body_plain.toHtmlEscaped());
    }

    html += QStringLiteral("</div>");
    m_detail_browser->setHtml(html);
}

}  // namespace sak
