// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_calendar_dialog.h
/// @brief Calendar viewer modal for viewing and exporting calendar items

#pragma once

#include "sak/email_inspector_controller.h"
#include "sak/email_types.h"

#include <QDialog>

class QCalendarWidget;
class QLabel;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTextBrowser;
class QVBoxLayout;

namespace sak {

/// @brief Modal dialog displaying a calendar with event list and detail.
///
/// Calendar items (appointments, meetings) from the PST/OST file are shown
/// in a calendar view with an event list. Selecting a date highlights events
/// for that day. Export to ICS or CSV is built in.
class EmailCalendarDialog : public QDialog {
    Q_OBJECT

public:
    explicit EmailCalendarDialog(::EmailInspectorController* controller,
                                 const QVector<uint64_t>& calendar_folder_ids,
                                 QWidget* parent = nullptr);
    ~EmailCalendarDialog() override;

private Q_SLOTS:
    void onDateClicked(const QDate& date);
    void onEventSelected(int row, int column);
    void onExportIcsClicked();
    void onExportCsvClicked();
    void onItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total);
    void onDetailLoaded(sak::PstItemDetail detail);

private:
    void setupUi();
    void loadCalendarItems();
    void populateEventList(const QDate& date);
    void displayEventDetail(const sak::PstItemDetail& detail);
    void highlightDatesWithEvents();
    static void appendOptionalHtmlField(QString& html, const QString& label, const QString& value);

    ::EmailInspectorController* m_controller{nullptr};
    QVector<uint64_t> m_folder_ids;

    // Widgets
    QCalendarWidget* m_calendar{nullptr};
    QTableWidget* m_event_table{nullptr};
    QTextBrowser* m_detail_browser{nullptr};
    QSplitter* m_main_splitter{nullptr};
    QSplitter* m_right_splitter{nullptr};
    QPushButton* m_export_ics_button{nullptr};
    QPushButton* m_export_csv_button{nullptr};
    QPushButton* m_close_button{nullptr};
    QLabel* m_status_label{nullptr};

    // Data
    QVector<sak::PstItemSummary> m_all_events;
    int m_folders_loaded{0};
};

}  // namespace sak
