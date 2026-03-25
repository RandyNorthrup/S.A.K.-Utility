// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_calendar_dialog.h
/// @brief Full-featured calendar viewer with month/week/day views,
///        color-coded events, search, filtering, and export.

#pragma once

#include "sak/email_inspector_controller.h"
#include "sak/email_types.h"

#include <QDate>
#include <QDialog>
#include <QHash>
#include <QMap>
#include <QVector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QTextBrowser;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace sak {

// ============================================================================
// CalendarEvent — enriched event for display
// ============================================================================

/// @brief Enriched calendar event combining summary and detail data
struct CalendarEvent {
    uint64_t node_id = 0;
    EmailItemType item_type = EmailItemType::Unknown;
    QString subject;
    QString sender_name;
    QDateTime start_time;

    // Detail fields — populated when detail is loaded
    bool detail_loaded = false;
    QDateTime end_time;
    QString location;
    bool is_all_day = false;
    int busy_status = -1;  ///< -1=unknown, 0=Free, 1=Tentative, 2=Busy, 3=OOF
    int importance = 1;    ///< 0=Low, 1=Normal, 2=High
    QStringList attendees;
    QString recurrence_description;
    QString body_plain;
    QString body_html;
    QString sender_email;
    int attachment_count = 0;
};

// ============================================================================
// Forward declarations for view widgets (defined in .cpp)
// ============================================================================

class CalendarMonthView;
class CalendarWeekDayView;

// ============================================================================
// EmailCalendarDialog
// ============================================================================

/// @brief Full-featured calendar viewer modal with month, week, and day views.
///
/// Loads calendar items from PST/OST file and presents them in a modern
/// calendar interface with color-coded event bars, search, filtering by
/// busy status, view switching, mini-calendar navigation, and ICS/CSV export.
class EmailCalendarDialog : public QDialog {
    Q_OBJECT

public:
    /// Calendar view mode
    enum class ViewMode {
        Month,
        Week,
        Day
    };

    explicit EmailCalendarDialog(::EmailInspectorController* controller,
                                 const QVector<uint64_t>& calendar_folder_ids,
                                 QWidget* parent = nullptr);
    ~EmailCalendarDialog() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private Q_SLOTS:
    // Navigation
    void onNavigatePrevious();
    void onNavigateNext();
    void onNavigateToday();
    void onViewModeChanged(int mode_index);
    void onMonthLabelClicked();
    void onYearLabelClicked();

    // Event interaction
    void onEventClicked(uint64_t node_id);
    void onDayClicked(const QDate& date);
    void onEventDoubleClicked(uint64_t node_id);

    // Search and filter
    void onSearchTextChanged(const QString& text);
    void onSearchTimerFired();
    void onBusyFilterChanged();
    void onOrganizerFilterChanged();

    // Export
    void onExportIcsClicked();
    void onExportCsvClicked();

    // Context menus
    void onDayEventListContextMenu(const QPoint& pos);

    // Data loading
    void onItemsLoaded(uint64_t folder_id, QVector<sak::PstItemSummary> items, int total);
    void onDetailLoaded(sak::PstItemDetail detail);

private:
    // UI setup helpers — each builds one section of the dialog
    void setupUi();
    void setupToolbar(QVBoxLayout* parent_layout);
    void setupNavButtons(QHBoxLayout* layout);
    void setupViewButtons(QHBoxLayout* layout);
    void setupSearchBar(QHBoxLayout* layout);
    void setupMainContent(QVBoxLayout* parent_layout);
    void setupSidebar(QSplitter* splitter);
    void setupFilterSection(QVBoxLayout* layout);
    void setupDayEventList(QVBoxLayout* layout);
    void setupCalendarViews(QSplitter* splitter);
    void setupDetailPanel(QSplitter* splitter);
    void setupFooter(QVBoxLayout* parent_layout);

    // Data management
    void loadCalendarItems();
    void rebuildDateIndex();
    void rebuildOrganizerFilter();
    void requestDetailsForDate(const QDate& date);

    // View management
    void setViewMode(ViewMode mode);
    void setCurrentDate(const QDate& date);
    void refreshAllViews();
    void updateNavigationLabel();
    void updateStatusLabel();

    // Event display
    void displayEventSummary(const CalendarEvent& event);
    void populateDayEventList(const QDate& date);

    // Context menu helpers
    void showEventContextMenu(const QPoint& global_pos, uint64_t node_id);
    void exportSingleEventIcs(const CalendarEvent& evt);
    void copyEventDetailsToClipboard(const CalendarEvent& evt);

    // Data queries
    [[nodiscard]] QVector<const CalendarEvent*> eventsForDate(const QDate& date) const;
    [[nodiscard]] QVector<const CalendarEvent*> eventsInRange(const QDate& from,
                                                              const QDate& to) const;
    [[nodiscard]] QVector<const CalendarEvent*> filteredEvents() const;
    [[nodiscard]] bool matchesSearch(const CalendarEvent& evt) const;
    [[nodiscard]] bool matchesFilter(const CalendarEvent& evt) const;

public:
    // Color helpers (public for use by view widgets)
    [[nodiscard]] static QColor fillColorForStatus(int busy_status, EmailItemType type);
    [[nodiscard]] static QColor borderColorForStatus(int busy_status, EmailItemType type);

private:
    static void appendHtmlField(QString& html, const QString& label, const QString& value);
    [[nodiscard]] static QString buildDateHtml(const CalendarEvent& evt);
    [[nodiscard]] static QString buildAttendeesHtml(const CalendarEvent& evt);
    [[nodiscard]] static QString buildBodyHtml(const CalendarEvent& evt);

    // ================================================================
    // Member data
    // ================================================================

    ::EmailInspectorController* m_controller{nullptr};
    QVector<uint64_t> m_folder_ids;

    // Current state
    ViewMode m_view_mode{ViewMode::Month};
    QDate m_current_date;
    QString m_search_text;
    uint64_t m_selected_event_id{0};

    // Toolbar widgets
    QToolButton* m_prev_button{nullptr};
    QToolButton* m_next_button{nullptr};
    QPushButton* m_today_button{nullptr};
    QLabel* m_month_label{nullptr};
    QLabel* m_year_label{nullptr};
    QButtonGroup* m_view_group{nullptr};
    QPushButton* m_month_button{nullptr};
    QPushButton* m_week_button{nullptr};
    QPushButton* m_day_button{nullptr};
    QLineEdit* m_search_edit{nullptr};
    QTimer* m_search_timer{nullptr};

    // Sidebar widgets
    QCheckBox* m_filter_free{nullptr};
    QCheckBox* m_filter_tentative{nullptr};
    QCheckBox* m_filter_busy{nullptr};
    QCheckBox* m_filter_oof{nullptr};
    QComboBox* m_organizer_filter{nullptr};
    QTableWidget* m_day_event_list{nullptr};

    // Main view area
    QStackedWidget* m_view_stack{nullptr};
    CalendarMonthView* m_month_view{nullptr};
    CalendarWeekDayView* m_week_view{nullptr};
    CalendarWeekDayView* m_day_view{nullptr};

    // Detail panel
    QTextBrowser* m_detail_browser{nullptr};

    // Footer
    QPushButton* m_export_ics_button{nullptr};
    QPushButton* m_export_csv_button{nullptr};
    QPushButton* m_close_button{nullptr};
    QLabel* m_status_label{nullptr};

    // Event data
    QVector<CalendarEvent> m_all_events;
    QMap<QDate, QVector<int>> m_date_index;  ///< date → indices into m_all_events
    QHash<uint64_t, int> m_node_index;       ///< node_id → index into m_all_events
    int m_folders_loaded{0};
    int m_details_pending{0};
};

}  // namespace sak
