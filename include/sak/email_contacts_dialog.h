// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_contacts_dialog.h
/// @brief Address book modal for viewing and exporting contacts

#pragma once

#include "sak/email_inspector_controller.h"
#include "sak/email_types.h"

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTextBrowser;
class QVBoxLayout;

namespace sak {

/// @brief Modal dialog displaying a searchable address book of contacts.
///
/// Contacts and distribution lists from the PST/OST file are displayed
/// in a sortable table with search. Selecting a contact shows full
/// detail in a side panel. Export to VCF or CSV is built in.
class EmailContactsDialog : public QDialog {
    Q_OBJECT

public:
    explicit EmailContactsDialog(::EmailInspectorController* controller,
                                 const QVector<uint64_t>& contact_folder_ids,
                                 QWidget* parent = nullptr);
    ~EmailContactsDialog() override;

private Q_SLOTS:
    void onSearchTextChanged(const QString& text);
    void onContactSelected(int row, int column);
    void onExportVcfClicked();
    void onExportCsvClicked();
    void onItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total);
    void onDetailLoaded(sak::PstItemDetail detail);

private:
    void setupUi();
    void loadContacts();
    void filterContacts(const QString& text);
    void displayContactDetail(const sak::PstItemDetail& detail);

    ::EmailInspectorController* m_controller{nullptr};
    QVector<uint64_t> m_folder_ids;

    // Widgets
    QLineEdit* m_search_edit{nullptr};
    QTableWidget* m_contact_table{nullptr};
    QTextBrowser* m_detail_browser{nullptr};
    QSplitter* m_splitter{nullptr};
    QPushButton* m_export_vcf_button{nullptr};
    QPushButton* m_export_csv_button{nullptr};
    QPushButton* m_close_button{nullptr};
    QLabel* m_status_label{nullptr};

    // Data
    QVector<sak::PstItemSummary> m_all_contacts;
    int m_folders_loaded{0};
};

}  // namespace sak
