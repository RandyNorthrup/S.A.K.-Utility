// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_calendar_dialog.cpp
/// @brief Calendar viewer modal implementation

#include "sak/email_calendar_dialog.h"

#include "sak/email_inspector_controller.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QCalendarWidget>
#include <QDate>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QVBoxLayout>

namespace sak {

// ============================================================================
// Column indices for the event table
// ============================================================================

enum EventColumn {
    ColTime = 0,
    ColSubject,
    ColLocation,
    ColDuration,
    ColEventCount
};

// ============================================================================
// Construction
// ============================================================================

EmailCalendarDialog::EmailCalendarDialog(::EmailInspectorController* controller,
                                         const QVector<uint64_t>& calendar_folder_ids,
                                         QWidget* parent)
    : QDialog(parent), m_controller(controller), m_folder_ids(calendar_folder_ids) {
    setWindowTitle(tr("Calendar Viewer"));
    setModal(true);
    resize(kWizardLargeWidth, kWizardLargeHeight);
    setupUi();
    loadCalendarItems();
}

EmailCalendarDialog::~EmailCalendarDialog() = default;

// ============================================================================
// UI Setup
// ============================================================================

void EmailCalendarDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge);
    layout->setSpacing(ui::kSpacingDefault);

    // Title
    auto* heading = new QLabel(tr("Calendar"), this);
    heading->setStyleSheet(QStringLiteral("font-size: %1pt; font-weight: 600; "
                                          "color: %2;")
                               .arg(ui::kFontSizeTitle)
                               .arg(ui::kColorTextHeading));
    layout->addWidget(heading);

    // Main layout: calendar (left) | events + detail (right)
    m_main_splitter = new QSplitter(Qt::Horizontal, this);
    m_main_splitter->setChildrenCollapsible(false);

    // Calendar widget
    m_calendar = new QCalendarWidget(this);
    m_calendar->setGridVisible(true);
    m_calendar->setMinimumWidth(320);
    m_calendar->setStyleSheet(QStringLiteral("QCalendarWidget QToolButton {"
                                             "  color: %1; font-weight: 600;"
                                             "}"
                                             "QCalendarWidget QWidget#qt_calendar_navigationbar {"
                                             "  background: %2; border-radius: 6px;"
                                             "}"
                                             "QCalendarWidget QAbstractItemView:enabled {"
                                             "  color: %3; selection-background-color: %4;"
                                             "  selection-color: white;"
                                             "}")
                                  .arg(ui::kColorTextHeading)
                                  .arg(ui::kColorBgSurface)
                                  .arg(ui::kColorTextBody)
                                  .arg(ui::kColorPrimary));
    connect(m_calendar, &QCalendarWidget::clicked, this, &EmailCalendarDialog::onDateClicked);
    m_main_splitter->addWidget(m_calendar);

    // Right: events table + detail
    m_right_splitter = new QSplitter(Qt::Vertical, this);
    m_right_splitter->setChildrenCollapsible(false);

    // Event table
    m_event_table = new QTableWidget(this);
    m_event_table->setColumnCount(ColEventCount);
    m_event_table->setHorizontalHeaderLabels(
        {tr("Time"), tr("Subject"), tr("Location"), tr("Duration")});
    m_event_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_event_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_event_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_event_table->setSortingEnabled(true);
    m_event_table->horizontalHeader()->setStretchLastSection(true);
    m_event_table->setColumnWidth(ColTime, 100);
    m_event_table->setColumnWidth(ColSubject, 250);
    m_event_table->setColumnWidth(ColLocation, 150);
    m_event_table->verticalHeader()->setVisible(false);
    connect(m_event_table, &QTableWidget::cellClicked, this, &EmailCalendarDialog::onEventSelected);
    m_right_splitter->addWidget(m_event_table);

    // Detail panel
    m_detail_browser = new QTextBrowser(this);
    m_detail_browser->setOpenExternalLinks(false);
    m_detail_browser->setPlaceholderText(tr("Select an event to view details"));
    m_right_splitter->addWidget(m_detail_browser);
    m_right_splitter->setSizes({300, 300});
    m_main_splitter->addWidget(m_right_splitter);
    m_main_splitter->setSizes({360, 540});
    layout->addWidget(m_main_splitter, 1);

    // Button row
    auto* button_row = new QHBoxLayout();

    m_export_ics_button = new QPushButton(tr("Export ICS"), this);
    m_export_ics_button->setToolTip(tr("Export all events as iCalendar (.ics) files"));
    m_export_ics_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(
        m_export_ics_button, &QPushButton::clicked, this, &EmailCalendarDialog::onExportIcsClicked);
    button_row->addWidget(m_export_ics_button);

    m_export_csv_button = new QPushButton(tr("Export CSV"), this);
    m_export_csv_button->setToolTip(tr("Export all events as a CSV spreadsheet"));
    m_export_csv_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(
        m_export_csv_button, &QPushButton::clicked, this, &EmailCalendarDialog::onExportCsvClicked);
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

void EmailCalendarDialog::loadCalendarItems() {
    if (m_folder_ids.isEmpty()) {
        m_status_label->setText(tr("No calendar folders found"));
        return;
    }

    m_status_label->setText(tr("Loading calendar events..."));
    m_folders_loaded = 0;
    m_all_events.clear();

    connect(m_controller,
            &::EmailInspectorController::folderItemsLoaded,
            this,
            &EmailCalendarDialog::onItemsLoaded);
    connect(m_controller,
            &::EmailInspectorController::itemDetailLoaded,
            this,
            &EmailCalendarDialog::onDetailLoaded);

    constexpr int kMaxEventsPerFolder = 5000;
    for (uint64_t folder_id : m_folder_ids) {
        m_controller->loadFolderItems(folder_id, 0, kMaxEventsPerFolder);
    }
}

// ============================================================================
// Slots
// ============================================================================

void EmailCalendarDialog::onDateClicked(const QDate& date) {
    populateEventList(date);
}

void EmailCalendarDialog::onEventSelected(int row, int /*column*/) {
    if (row < 0 || row >= m_event_table->rowCount()) {
        return;
    }
    auto* item = m_event_table->item(row, ColSubject);
    if (item == nullptr) {
        return;
    }
    bool ok = false;
    uint64_t node_id = item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    m_controller->loadItemDetail(node_id);
}

void EmailCalendarDialog::onExportIcsClicked() {
    QString dir_path =
        QFileDialog::getExistingDirectory(this, tr("Select Export Directory for ICS Files"));
    if (dir_path.isEmpty()) {
        return;
    }
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::Ics;
    config.output_path = dir_path;
    for (uint64_t folder_id : m_folder_ids) {
        config.folder_id = folder_id;
        m_controller->exportItems(config);
    }
}

void EmailCalendarDialog::onExportCsvClicked() {
    QString file_path = QFileDialog::getSaveFileName(this,
                                                     tr("Export Calendar as CSV"),
                                                     QStringLiteral("calendar.csv"),
                                                     tr("CSV Files (*.csv)"));
    if (file_path.isEmpty()) {
        return;
    }
    sak::EmailExportConfig config;
    config.format = sak::ExportFormat::CsvCalendar;
    config.output_path = file_path;
    for (uint64_t folder_id : m_folder_ids) {
        config.folder_id = folder_id;
        m_controller->exportItems(config);
    }
}

void EmailCalendarDialog::onItemsLoaded(uint64_t /*folder_id*/,
                                        QVector<sak::PstItemSummary> items,
                                        int /*total*/) {
    for (const auto& item : items) {
        if (item.item_type == EmailItemType::Calendar ||
            item.item_type == EmailItemType::MeetingRequest) {
            m_all_events.append(item);
        }
    }
    ++m_folders_loaded;
    if (m_folders_loaded >= m_folder_ids.size()) {
        highlightDatesWithEvents();
        m_status_label->setText(tr("%1 events").arg(m_all_events.size()));
        // Select today
        m_calendar->setSelectedDate(QDate::currentDate());
        populateEventList(QDate::currentDate());
    }
}

void EmailCalendarDialog::onDetailLoaded(sak::PstItemDetail detail) {
    if (detail.item_type == EmailItemType::Calendar ||
        detail.item_type == EmailItemType::MeetingRequest) {
        displayEventDetail(detail);
    }
}

// ============================================================================
// Helpers
// ============================================================================

void EmailCalendarDialog::populateEventList(const QDate& date) {
    m_event_table->setSortingEnabled(false);
    m_event_table->setRowCount(0);

    for (const auto& event : m_all_events) {
        if (event.date.date() != date) {
            continue;
        }
        int row = m_event_table->rowCount();
        m_event_table->insertRow(row);

        m_event_table->setItem(row,
                               ColTime,
                               new QTableWidgetItem(
                                   event.date.time().toString(QStringLiteral("hh:mm AP"))));

        auto* subject_item = new QTableWidgetItem(event.subject);
        subject_item->setData(Qt::UserRole, QVariant::fromValue(event.node_id));
        m_event_table->setItem(row, ColSubject, subject_item);

        // Location/duration not available in summary
        m_event_table->setItem(row, ColLocation, new QTableWidgetItem());
        m_event_table->setItem(row, ColDuration, new QTableWidgetItem());
    }
    m_event_table->setSortingEnabled(true);
}

void EmailCalendarDialog::appendOptionalHtmlField(QString& html,
                                                  const QString& label,
                                                  const QString& value) {
    if (!value.isEmpty()) {
        html += QStringLiteral("<p><b>%1:</b> %2</p>").arg(label, value.toHtmlEscaped());
    }
}

void EmailCalendarDialog::displayEventDetail(const sak::PstItemDetail& detail) {
    QString html = QStringLiteral(
                       "<div style='font-family: Segoe UI, sans-serif; "
                       "padding: 12px;'>"
                       "<h2 style='color: %1; margin-bottom: 8px;'>"
                       "%2</h2>")
                       .arg(ui::kColorTextHeading)
                       .arg(detail.subject.toHtmlEscaped());

    if (detail.start_time.isValid()) {
        appendOptionalHtmlField(html,
                                QStringLiteral("Start"),
                                detail.start_time.toString(Qt::RFC2822Date));
    }
    if (detail.end_time.isValid()) {
        appendOptionalHtmlField(html,
                                QStringLiteral("End"),
                                detail.end_time.toString(Qt::RFC2822Date));
    }
    if (detail.is_all_day) {
        html += QStringLiteral("<p><b>All Day Event</b></p>");
    }

    appendOptionalHtmlField(html, QStringLiteral("Location"), detail.location);

    static const char* const kBusyStatus[] = {"Free", "Tentative", "Busy", "Out of Office"};
    constexpr int kBusyStatusCount = 4;
    if (detail.busy_status >= 0 && detail.busy_status < kBusyStatusCount) {
        html += QStringLiteral("<p><b>Status:</b> %1</p>")
                    .arg(QLatin1String(kBusyStatus[detail.busy_status]));
    }

    appendOptionalHtmlField(html, QStringLiteral("Organizer"), detail.sender_name);

    if (!detail.attendees.isEmpty()) {
        html += QStringLiteral("<p><b>Attendees:</b></p><ul>");
        for (const auto& attendee : detail.attendees) {
            html += QStringLiteral("<li>%1</li>").arg(attendee.toHtmlEscaped());
        }
        html += QStringLiteral("</ul>");
    }

    appendOptionalHtmlField(html, QStringLiteral("Recurrence"), detail.recurrence_description);

    html += QStringLiteral("<hr style='border: 1px solid %1;'>").arg(ui::kColorBorderDefault);

    if (!detail.body_html.isEmpty()) {
        html += detail.body_html;
    } else if (!detail.body_plain.isEmpty()) {
        html += QStringLiteral("<p style='white-space: pre-wrap;'>%1</p>")
                    .arg(detail.body_plain.toHtmlEscaped());
    }

    html += QStringLiteral("</div>");
    m_detail_browser->setHtml(html);
}

void EmailCalendarDialog::highlightDatesWithEvents() {
    QTextCharFormat highlight;
    highlight.setBackground(QColor(ui::kColorPrimary).lighter(170));
    highlight.setForeground(QColor(ui::kColorPrimaryDark));

    for (const auto& event : m_all_events) {
        if (event.date.isValid()) {
            m_calendar->setDateTextFormat(event.date.date(), highlight);
        }
    }
}

}  // namespace sak
