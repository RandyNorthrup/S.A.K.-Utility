// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_calendar_dialog.cpp
/// @brief Full-featured calendar viewer with month/week/day views

#include "sak/email_calendar_dialog.h"

#include "sak/email_constants.h"
#include "sak/email_inspector_controller.h"
#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDate>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextStream>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace sak {

// ============================================================================
// Day-of-week column headers
// ============================================================================

static const QString kDayHeaders[] = {QStringLiteral("Sun"),
                                      QStringLiteral("Mon"),
                                      QStringLiteral("Tue"),
                                      QStringLiteral("Wed"),
                                      QStringLiteral("Thu"),
                                      QStringLiteral("Fri"),
                                      QStringLiteral("Sat")};

constexpr int kDaysPerWeek = 7;
constexpr int kMonthGridRows = 6;

// ============================================================================
// Event table columns for sidebar event list
// ============================================================================

enum DayListColumn {
    DayColTime = 0,
    DayColSubject,
    DayColCount
};

// ============================================================================
// Busy-status label strings
// ============================================================================

static const char* const kBusyLabels[] = {"Free", "Tentative", "Busy", "Out of Office"};

// ============================================================================
// CalendarMonthView — custom-painted month grid
// ============================================================================

class CalendarMonthView : public QWidget {
    Q_OBJECT

public:
    explicit CalendarMonthView(QWidget* parent = nullptr) : QWidget(parent) {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setDate(const QDate& date) {
        m_date = date;
        update();
    }

    void setEvents(const QMap<QDate, QVector<const CalendarEvent*>>& events) {
        m_events = events;
        update();
    }

    void setSelectedDate(const QDate& date) {
        m_selected_date = date;
        update();
    }

Q_SIGNALS:
    void dayClicked(const QDate& date);
    void eventClicked(uint64_t node_id);
    void eventContextMenuRequested(const QPoint& global_pos, uint64_t node_id);

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        m_event_rects.clear();
        drawHeader(painter);
        drawGrid(painter);
    }

    void mousePressEvent(QMouseEvent* event) override { handleClick(event->pos()); }

    void contextMenuEvent(QContextMenuEvent* event) override {
        for (const auto& hit : m_event_rects) {
            if (hit.rect.contains(event->pos())) {
                Q_EMIT eventContextMenuRequested(event->globalPos(), hit.node_id);
                return;
            }
        }
    }

private:
    void drawHeader(QPainter& painter) {
        int col_width = width() / kDaysPerWeek;
        painter.setPen(QColor(ui::kColorTextSecondary));
        QFont header_font = font();
        header_font.setWeight(QFont::DemiBold);
        header_font.setPointSize(ui::kFontSizeBody);
        painter.setFont(header_font);
        for (int col = 0; col < kDaysPerWeek; ++col) {
            QRect cell(col * col_width, 0, col_width, email::kCalendarDayHeaderHeight);
            painter.drawText(cell, Qt::AlignCenter, kDayHeaders[col]);
        }
    }

    void drawGrid(QPainter& painter) {
        QDate first_of_month(m_date.year(), m_date.month(), 1);
        int start_dow = first_of_month.dayOfWeek() % kDaysPerWeek;
        QDate grid_start = first_of_month.addDays(-start_dow);

        int col_width = width() / kDaysPerWeek;
        int avail_height = height() - email::kCalendarDayHeaderHeight;
        int row_height = avail_height / kMonthGridRows;

        for (int row = 0; row < kMonthGridRows; ++row) {
            for (int col = 0; col < kDaysPerWeek; ++col) {
                QDate day = grid_start.addDays(row * kDaysPerWeek + col);
                int left = col * col_width;
                int top = email::kCalendarDayHeaderHeight + row * row_height;
                QRect cell_rect(left, top, col_width, row_height);
                drawDayCell(painter, cell_rect, day);
            }
        }
    }

    void drawDayCell(QPainter& painter, const QRect& rect, const QDate& day) {
        bool is_current_month = (day.month() == m_date.month());
        bool is_today = (day == QDate::currentDate());
        bool is_selected = (day == m_selected_date);
        bool is_weekend = (day.dayOfWeek() == Qt::Saturday || day.dayOfWeek() == Qt::Sunday);

        QColor bg_color(ui::kColorBgWhite);
        if (!is_current_month) {
            bg_color = QColor(email::kCalColorOutsideMonthBg);
        } else if (is_today) {
            bg_color = QColor(email::kCalColorTodayBg);
        } else if (is_weekend) {
            bg_color = QColor(email::kCalColorWeekendBg);
        }
        painter.fillRect(rect, bg_color);

        if (is_selected) {
            painter.setPen(QPen(QColor(ui::kColorPrimary), 2));
        } else {
            painter.setPen(QColor(ui::kColorBorderDefault));
        }
        painter.drawRect(rect);

        drawDayNumber(painter, rect, day, is_current_month, is_today);
        drawEventBars(painter, rect, day);
    }

    void drawDayNumber(
        QPainter& painter, const QRect& rect, const QDate& day, bool is_month, bool is_today) {
        QFont num_font = font();
        num_font.setPointSize(ui::kFontSizeBody);
        num_font.setWeight(is_today ? QFont::Bold : QFont::Normal);
        painter.setFont(num_font);

        QColor text_color(ui::kColorTextBody);
        if (!is_month) {
            text_color = QColor(ui::kColorTextDisabled);
        }
        if (is_today) {
            int circle_diam = email::kCalendarDayLabelHeight - 2;
            int cx = rect.left() + email::kCalendarCellPadding + circle_diam / 2 + 2;
            int cy = rect.top() + email::kCalendarCellPadding + circle_diam / 2;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(ui::kColorPrimary));
            painter.drawEllipse(QPoint(cx, cy), circle_diam / 2, circle_diam / 2);
            text_color = QColor(ui::kColorBgWhite);
        }
        painter.setPen(text_color);
        QRect label_rect(rect.left() + email::kCalendarCellPadding,
                         rect.top() + email::kCalendarCellPadding,
                         email::kCalendarDayLabelHeight + 4,
                         email::kCalendarDayLabelHeight);
        painter.drawText(label_rect, Qt::AlignCenter, QString::number(day.day()));
    }

    void drawEventBars(QPainter& painter, const QRect& rect, const QDate& day) {
        auto iter = m_events.find(day);
        if (iter == m_events.end() || iter->isEmpty()) {
            return;
        }
        const auto& events = *iter;
        int bar_top = rect.top() + email::kCalendarDayLabelHeight + email::kCalendarCellPadding;
        int bar_left = rect.left() + email::kCalendarCellPadding;
        int bar_width = rect.width() - 2 * email::kCalendarCellPadding;
        int max_bars = std::min(static_cast<int>(events.size()), email::kCalendarMaxVisibleBars);
        int space_per_bar = email::kCalendarEventBarHeight + email::kCalendarBarGap;

        bool need_more = (events.size() > email::kCalendarMaxVisibleBars);
        if (need_more && max_bars > 1) {
            --max_bars;
        }

        for (int idx = 0; idx < max_bars; ++idx) {
            int top = bar_top + idx * space_per_bar;
            if (top + email::kCalendarEventBarHeight > rect.bottom()) {
                break;
            }
            drawSingleBar(painter,
                          events[idx],
                          QRect(bar_left, top, bar_width, email::kCalendarEventBarHeight));
        }

        if (need_more) {
            int more_count = events.size() - max_bars;
            drawMoreLabel(
                painter, more_count, bar_left, bar_top + max_bars * space_per_bar, bar_width);
        }
    }

    void drawSingleBar(QPainter& painter, const CalendarEvent* evt, const QRect& bar_rect) {
        QColor fill = EmailCalendarDialog::fillColorForStatus(evt->busy_status, evt->item_type);
        QColor border = EmailCalendarDialog::borderColorForStatus(evt->busy_status, evt->item_type);

        QPainterPath path;
        path.addRoundedRect(QRectF(bar_rect),
                            email::kCalendarBarCornerRadius,
                            email::kCalendarBarCornerRadius);
        painter.fillPath(path, fill);

        painter.fillRect(QRect(bar_rect.left(),
                               bar_rect.top(),
                               email::kCalendarBarBorderWidth,
                               bar_rect.height()),
                         border);

        painter.setPen(QColor(ui::kColorTextBody));
        QFont bar_font = font();
        bar_font.setPointSize(ui::kFontSizeSmall);
        painter.setFont(bar_font);

        QRect text_rect = bar_rect.adjusted(email::kCalendarBarBorderWidth + 3, 0, -2, 0);
        QString label = buildBarLabel(evt);
        QString elided = painter.fontMetrics().elidedText(label, Qt::ElideRight, text_rect.width());
        painter.drawText(text_rect, Qt::AlignVCenter, elided);

        // Store hit rect for click detection
        m_event_rects.append({bar_rect, evt->node_id});
    }

    static QString buildBarLabel(const CalendarEvent* evt) {
        QString label;
        if (evt->start_time.isValid() && !evt->is_all_day) {
            label = evt->start_time.time().toString(QStringLiteral("h:mm AP")) +
                    QStringLiteral(" ");
        }
        label += evt->subject;
        return label;
    }

    void drawMoreLabel(QPainter& painter, int count, int left, int top, int width) {
        painter.setPen(QColor(ui::kColorTextMuted));
        QFont more_font = font();
        more_font.setPointSize(ui::kFontSizeSmall);
        more_font.setItalic(true);
        painter.setFont(more_font);
        QRect label_rect(left, top, width, email::kCalendarEventBarHeight);
        painter.drawText(label_rect, Qt::AlignVCenter, QStringLiteral("+%1 more...").arg(count));
    }

    void handleClick(const QPoint& pos) {
        // Check event bars first
        for (const auto& hit : m_event_rects) {
            if (hit.rect.contains(pos)) {
                Q_EMIT eventClicked(hit.node_id);
                return;
            }
        }
        // Fall back to day click
        QDate clicked_day = dayAtPosition(pos);
        if (clicked_day.isValid()) {
            Q_EMIT dayClicked(clicked_day);
        }
    }

    [[nodiscard]] QDate dayAtPosition(const QPoint& pos) const {
        if (pos.y() < email::kCalendarDayHeaderHeight) {
            return {};
        }
        int col_width = width() / kDaysPerWeek;
        int avail_h = height() - email::kCalendarDayHeaderHeight;
        int row_height = avail_h / kMonthGridRows;
        if (col_width <= 0 || row_height <= 0) {
            return {};
        }

        int col = pos.x() / col_width;
        int row = (pos.y() - email::kCalendarDayHeaderHeight) / row_height;
        if (col < 0 || col >= kDaysPerWeek || row < 0 || row >= kMonthGridRows) {
            return {};
        }

        QDate first_of_month(m_date.year(), m_date.month(), 1);
        int start_dow = first_of_month.dayOfWeek() % kDaysPerWeek;
        QDate grid_start = first_of_month.addDays(-start_dow);
        return grid_start.addDays(row * kDaysPerWeek + col);
    }

    struct HitRect {
        QRect rect;
        uint64_t node_id;
    };

    QDate m_date = QDate::currentDate();
    QDate m_selected_date;
    QMap<QDate, QVector<const CalendarEvent*>> m_events;
    QVector<HitRect> m_event_rects;
};

// ============================================================================
// CalendarWeekDayView — custom-painted week or day time grid
// ============================================================================

class CalendarWeekDayView : public QWidget {
    Q_OBJECT

public:
    explicit CalendarWeekDayView(bool week_mode, QWidget* parent = nullptr)
        : QWidget(parent), m_week_mode(week_mode) {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setDate(const QDate& date) {
        m_date = date;
        update();
    }

    void setEvents(const QMap<QDate, QVector<const CalendarEvent*>>& events) {
        m_events = events;
        update();
    }

    [[nodiscard]] int totalHeight() const {
        int hours = email::kCalendarDayEndHour - email::kCalendarDayStartHour;
        return email::kCalendarDayHeaderHeight + email::kCalendarAllDayRowHeight +
               hours * email::kCalendarHourHeight;
    }

Q_SIGNALS:
    void dayClicked(const QDate& date);
    void eventClicked(uint64_t node_id);
    void eventContextMenuRequested(const QPoint& global_pos, uint64_t node_id);

protected:
    void paintEvent(QPaintEvent* /*event*/) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        m_event_rects.clear();
        drawColumnHeaders(painter);
        drawTimeGrid(painter);
        drawAllDayEvents(painter);
        drawTimedEvents(painter);
        drawCurrentTimeLine(painter);
    }

    void mousePressEvent(QMouseEvent* event) override {
        for (const auto& hit : m_event_rects) {
            if (hit.rect.contains(event->pos())) {
                Q_EMIT eventClicked(hit.node_id);
                return;
            }
        }
        QDate clicked = dayAtColumn(event->pos().x());
        if (clicked.isValid()) {
            Q_EMIT dayClicked(clicked);
        }
    }

    void contextMenuEvent(QContextMenuEvent* event) override {
        for (const auto& hit : m_event_rects) {
            if (hit.rect.contains(event->pos())) {
                Q_EMIT eventContextMenuRequested(event->globalPos(), hit.node_id);
                return;
            }
        }
    }

    QSize sizeHint() const override { return {600, totalHeight()}; }

    QSize minimumSizeHint() const override { return {400, totalHeight()}; }

private:
    [[nodiscard]] int columnCount() const { return m_week_mode ? kDaysPerWeek : 1; }

    [[nodiscard]] QDate startDate() const {
        if (!m_week_mode) {
            return m_date;
        }
        int dow = m_date.dayOfWeek() % kDaysPerWeek;
        return m_date.addDays(-dow);
    }

    [[nodiscard]] int columnWidth() const {
        int avail = width() - email::kCalendarTimeColumnWidth;
        return avail / columnCount();
    }

    [[nodiscard]] int columnLeft(int col) const {
        return email::kCalendarTimeColumnWidth + col * columnWidth();
    }

    [[nodiscard]] int timeToY(const QTime& time) const {
        int top_offset = email::kCalendarDayHeaderHeight + email::kCalendarAllDayRowHeight;
        int minutes = time.hour() * 60 + time.minute() - email::kCalendarDayStartHour * 60;
        return top_offset + (minutes * email::kCalendarHourHeight) / 60;
    }

    [[nodiscard]] QDate dayAtColumn(int px) const {
        if (px < email::kCalendarTimeColumnWidth) {
            return {};
        }
        int col = (px - email::kCalendarTimeColumnWidth) / columnWidth();
        if (col < 0 || col >= columnCount()) {
            return {};
        }
        return startDate().addDays(col);
    }

    void drawColumnHeaders(QPainter& painter) {
        QFont header_font = font();
        header_font.setWeight(QFont::DemiBold);
        header_font.setPointSize(ui::kFontSizeBody);
        painter.setFont(header_font);
        QDate base = startDate();

        for (int col = 0; col < columnCount(); ++col) {
            QDate day = base.addDays(col);
            int left = columnLeft(col);
            QRect header_rect(left, 0, columnWidth(), email::kCalendarDayHeaderHeight);
            bool is_today = (day == QDate::currentDate());
            if (is_today) {
                painter.fillRect(header_rect, QColor(email::kCalColorTodayBg));
            }
            painter.setPen(QColor(is_today ? ui::kColorPrimary : ui::kColorTextBody));
            QString label = day.toString(QStringLiteral("ddd M/d"));
            painter.drawText(header_rect, Qt::AlignCenter, label);

            // Vertical separator between columns
            if (col > 0) {
                painter.setPen(QColor(ui::kColorBorderDefault));
                painter.drawLine(left, 0, left, height());
            }
        }
        // Bottom border of header row
        painter.setPen(QColor(ui::kColorBorderDefault));
        int header_bottom = email::kCalendarDayHeaderHeight;
        painter.drawLine(0, header_bottom, width(), header_bottom);
    }

    void drawTimeGrid(QPainter& painter) {
        int top_base = email::kCalendarDayHeaderHeight + email::kCalendarAllDayRowHeight;
        int hours = email::kCalendarDayEndHour - email::kCalendarDayStartHour;
        QFont time_font = font();
        time_font.setPointSize(ui::kFontSizeSmall);
        painter.setFont(time_font);

        for (int hour = 0; hour <= hours; ++hour) {
            int display_hour = email::kCalendarDayStartHour + hour;
            int row_y = top_base + hour * email::kCalendarHourHeight;

            drawHourRow(painter, display_hour, row_y);
        }
        drawWorkHoursBackground(painter, top_base);
    }

    void drawHourRow(QPainter& painter, int hour, int row_y) {
        // Full hour solid line
        painter.setPen(QPen(QColor(ui::kColorBorderDefault), 1, Qt::SolidLine));
        painter.drawLine(email::kCalendarTimeColumnWidth, row_y, width(), row_y);

        // Half-hour dashed line
        if (hour < email::kCalendarDayEndHour) {
            int half_y = row_y + email::kCalendarHourHeight / 2;
            QColor half_color = QColor(ui::kColorBorderDefault).lighter(130);
            painter.setPen(QPen(half_color, 1, Qt::DotLine));
            painter.drawLine(email::kCalendarTimeColumnWidth, half_y, width(), half_y);
        }

        if (hour < email::kCalendarDayEndHour) {
            painter.setPen(QColor(ui::kColorTextMuted));
            QString time_str;
            if (hour == 0) {
                time_str = QStringLiteral("12 AM");
            } else if (hour < 12) {
                time_str = QStringLiteral("%1 AM").arg(hour);
            } else if (hour == 12) {
                time_str = QStringLiteral("12 PM");
            } else {
                time_str = QStringLiteral("%1 PM").arg(hour - 12);
            }
            QRect time_rect(
                0, row_y, email::kCalendarTimeColumnWidth - 4, email::kCalendarHourHeight);
            painter.drawText(time_rect, Qt::AlignRight | Qt::AlignTop, time_str);
        }
    }

    void drawWorkHoursBackground(QPainter& painter, int top_base) {
        int work_top = top_base + (email::kCalendarWorkStartHour - email::kCalendarDayStartHour) *
                                      email::kCalendarHourHeight;
        int work_bot = top_base + (email::kCalendarWorkEndHour - email::kCalendarDayStartHour) *
                                      email::kCalendarHourHeight;

        for (int col = 0; col < columnCount(); ++col) {
            int left = columnLeft(col);
            QRect work_rect(left, work_top, columnWidth(), work_bot - work_top);
            painter.fillRect(work_rect, QColor(email::kCalColorWorkHoursBg));
        }
    }

    void drawAllDayEvents(QPainter& painter) {
        int header_y = email::kCalendarDayHeaderHeight;
        painter.fillRect(QRect(0, header_y, width(), email::kCalendarAllDayRowHeight),
                         QColor(ui::kColorBgSurface));
        painter.setPen(QColor(ui::kColorBorderDefault));
        painter.drawLine(0,
                         header_y + email::kCalendarAllDayRowHeight,
                         width(),
                         header_y + email::kCalendarAllDayRowHeight);

        QDate base = startDate();
        for (int col = 0; col < columnCount(); ++col) {
            QDate day = base.addDays(col);
            auto iter = m_events.find(day);
            if (iter == m_events.end()) {
                continue;
            }
            drawAllDayForColumn(painter, *iter, col, header_y);
        }
    }

    void drawAllDayForColumn(QPainter& painter,
                             const QVector<const CalendarEvent*>& events,
                             int col,
                             int header_y) {
        int left = columnLeft(col) + 2;
        int bar_width = columnWidth() - 4;

        for (const auto* evt : events) {
            if (!evt->is_all_day) {
                continue;
            }
            QColor fill = EmailCalendarDialog::fillColorForStatus(evt->busy_status, evt->item_type);
            QColor border = EmailCalendarDialog::borderColorForStatus(evt->busy_status,
                                                                      evt->item_type);

            QRect bar(left, header_y + 2, bar_width, email::kCalendarAllDayRowHeight - 4);
            QPainterPath path;
            path.addRoundedRect(QRectF(bar),
                                email::kCalendarBarCornerRadius,
                                email::kCalendarBarCornerRadius);
            painter.fillPath(path, fill);
            painter.fillRect(
                QRect(bar.left(), bar.top(), email::kCalendarBarBorderWidth, bar.height()), border);
            drawBarText(painter, bar, evt);
            m_event_rects.append({bar, evt->node_id});
            break;  // Only one all-day bar per column in header
        }
    }

    void drawTimedEvents(QPainter& painter) {
        QDate base = startDate();
        for (int col = 0; col < columnCount(); ++col) {
            QDate day = base.addDays(col);
            auto iter = m_events.find(day);
            if (iter == m_events.end()) {
                continue;
            }
            drawTimedEventsForColumn(painter, *iter, col);
        }
    }

    void drawTimedEventsForColumn(QPainter& painter,
                                  const QVector<const CalendarEvent*>& events,
                                  int col) {
        int left = columnLeft(col) + 2;
        int bar_width = columnWidth() - 4;

        for (const auto* evt : events) {
            if (evt->is_all_day || !evt->start_time.isValid()) {
                continue;
            }
            drawTimedEventBar(painter, evt, left, bar_width);
        }
    }

    void drawTimedEventBar(QPainter& painter, const CalendarEvent* evt, int left, int bar_width) {
        QTime start = evt->start_time.time();
        QTime end = evt->end_time.isValid() ? evt->end_time.time() : start.addSecs(3600);

        int top = timeToY(start);
        int bottom = timeToY(end);
        constexpr int kMinBarHeight = 20;
        if (bottom - top < kMinBarHeight) {
            bottom = top + kMinBarHeight;
        }

        QColor fill = EmailCalendarDialog::fillColorForStatus(evt->busy_status, evt->item_type);
        QColor border = EmailCalendarDialog::borderColorForStatus(evt->busy_status, evt->item_type);

        QRect bar(left, top, bar_width, bottom - top);
        QPainterPath path;
        path.addRoundedRect(QRectF(bar),
                            email::kCalendarBarCornerRadius,
                            email::kCalendarBarCornerRadius);
        painter.fillPath(path, fill);
        painter.fillRect(QRect(bar.left(), bar.top(), email::kCalendarBarBorderWidth, bar.height()),
                         border);
        drawBarText(painter, bar, evt);
        m_event_rects.append({bar, evt->node_id});
    }

    void drawBarText(QPainter& painter, const QRect& bar, const CalendarEvent* evt) {
        painter.setPen(QColor(ui::kColorTextBody));
        QFont bar_font = font();
        bar_font.setPointSize(ui::kFontSizeSmall);
        painter.setFont(bar_font);

        QRect text_rect = bar.adjusted(email::kCalendarBarBorderWidth + 3, 1, -2, -1);
        QString label;
        if (!evt->is_all_day && evt->start_time.isValid()) {
            label = evt->start_time.time().toString(QStringLiteral("h:mm AP")) +
                    QStringLiteral(" ");
        }
        label += evt->subject;
        QString elided = painter.fontMetrics().elidedText(label, Qt::ElideRight, text_rect.width());
        painter.drawText(text_rect, Qt::AlignVCenter, elided);
    }

    void drawCurrentTimeLine(QPainter& painter) {
        QDate today = QDate::currentDate();
        QDate base = startDate();
        int col = -1;
        for (int idx = 0; idx < columnCount(); ++idx) {
            if (base.addDays(idx) == today) {
                col = idx;
                break;
            }
        }
        if (col < 0) {
            return;
        }
        QTime now = QTime::currentTime();
        int line_y = timeToY(now);
        int left = columnLeft(col);
        constexpr int kTimeDotRadius = 4;

        painter.setPen(QPen(QColor(ui::kColorError), 2));
        painter.setBrush(QColor(ui::kColorError));
        painter.drawEllipse(QPoint(left, line_y), kTimeDotRadius, kTimeDotRadius);
        painter.drawLine(left + kTimeDotRadius, line_y, left + columnWidth(), line_y);
        painter.setBrush(Qt::NoBrush);
    }

    struct HitRect {
        QRect rect;
        uint64_t node_id;
    };

    bool m_week_mode{true};
    QDate m_date = QDate::currentDate();
    QMap<QDate, QVector<const CalendarEvent*>> m_events;
    QVector<HitRect> m_event_rects;
};

// ============================================================================
// EmailCalendarDialog — Construction
// ============================================================================

EmailCalendarDialog::EmailCalendarDialog(::EmailInspectorController* controller,
                                         const QVector<uint64_t>& calendar_folder_ids,
                                         QWidget* parent)
    : QDialog(parent)
    , m_controller(controller)
    , m_folder_ids(calendar_folder_ids)
    , m_current_date(QDate::currentDate()) {
    setWindowTitle(tr("Calendar"));
    setModal(true);
    resize(kWizardLargeWidth + 200, kWizardLargeHeight + 100);
    setupUi();
    loadCalendarItems();
}

EmailCalendarDialog::~EmailCalendarDialog() = default;

bool EmailCalendarDialog::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonPress) {
        if (watched == m_month_label) {
            onMonthLabelClicked();
            return true;
        }
        if (watched == m_year_label) {
            onYearLabelClicked();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

// ============================================================================
// UI Setup — main layout
// ============================================================================

void EmailCalendarDialog::setupUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    setupToolbar(root_layout);
    setupMainContent(root_layout);
    setupFooter(root_layout);
}

// ============================================================================
// UI Setup — toolbar (navigation, view switch, search)
// ============================================================================

void EmailCalendarDialog::setupToolbar(QVBoxLayout* parent) {
    auto* toolbar = new QWidget(this);
    toolbar->setStyleSheet(QStringLiteral("background: %1; border-bottom: 1px solid %2;")
                               .arg(ui::kColorBgSurface, ui::kColorBorderDefault));
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginSmall, ui::kMarginMedium, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingMedium);

    setupNavButtons(layout);
    layout->addStretch();
    setupViewButtons(layout);
    layout->addStretch();
    setupSearchBar(layout);

    parent->addWidget(toolbar);
    updateNavigationLabel();
}

static constexpr auto kNavButtonStyle =
    "QToolButton {"
    "  background: transparent; border: 1px solid %1;"
    "  border-radius: 4px; padding: 4px 8px;"
    "  color: %2; font-size: 11pt;"
    "}"
    "QToolButton:hover { background: %3; }"
    "QToolButton:pressed { background: %4; }";

static constexpr auto kViewToggleStyle =
    "QPushButton {"
    "  background: transparent; border: 1px solid %1;"
    "  border-radius: 4px; padding: 4px 12px;"
    "  color: %2; font-weight: 500;"
    "}"
    "QPushButton:hover { background: %3; }"
    "QPushButton:checked {"
    "  background: %4; color: white;"
    "  border: 1px solid %5;"
    "}";

static constexpr auto kSearchBarStyle =
    "QLineEdit {"
    "  border: 1px solid %1; border-radius: 4px;"
    "  padding: 4px 8px; background: %2; color: %3;"
    "}"
    "QLineEdit:focus { border: 1px solid %4; }";

void EmailCalendarDialog::setupNavButtons(QHBoxLayout* layout) {
    QString nav_style = QString(kNavButtonStyle)
                            .arg(ui::kColorBorderDefault,
                                 ui::kColorTextSecondary,
                                 ui::kColorBgPageHover,
                                 ui::kColorBgSurface);

    m_prev_button = new QToolButton(this);
    m_prev_button->setText(QStringLiteral("\u25C0"));
    m_prev_button->setToolTip(tr("Previous"));
    m_prev_button->setStyleSheet(nav_style);
    connect(m_prev_button, &QToolButton::clicked, this, &EmailCalendarDialog::onNavigatePrevious);
    layout->addWidget(m_prev_button);

    m_today_button = new QPushButton(tr("Today"), this);
    m_today_button->setToolTip(tr("Go to today"));
    m_today_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(m_today_button, &QPushButton::clicked, this, &EmailCalendarDialog::onNavigateToday);
    layout->addWidget(m_today_button);

    m_next_button = new QToolButton(this);
    m_next_button->setText(QStringLiteral("\u25B6"));
    m_next_button->setToolTip(tr("Next"));
    m_next_button->setStyleSheet(nav_style);
    connect(m_next_button, &QToolButton::clicked, this, &EmailCalendarDialog::onNavigateNext);
    layout->addWidget(m_next_button);

    static constexpr auto kClickableLabelStyle =
        "QLabel {"
        "  font-size: %1pt; font-weight: 600; color: %2;"
        "  padding: 2px 6px; border-radius: 4px;"
        "}"
        "QLabel:hover {"
        "  background: %3; cursor: pointer;"
        "}";

    QString label_style = QString(kClickableLabelStyle)
                              .arg(ui::kFontSizeSection)
                              .arg(ui::kColorTextHeading, ui::kColorBgPageHover);

    m_month_label = new QLabel(this);
    m_month_label->setStyleSheet(label_style);
    m_month_label->setCursor(Qt::PointingHandCursor);
    m_month_label->setToolTip(tr("Click to select month"));
    m_month_label->installEventFilter(this);
    layout->addWidget(m_month_label);

    m_year_label = new QLabel(this);
    m_year_label->setStyleSheet(label_style);
    m_year_label->setCursor(Qt::PointingHandCursor);
    m_year_label->setToolTip(tr("Click to select year"));
    m_year_label->installEventFilter(this);
    layout->addWidget(m_year_label);
}

void EmailCalendarDialog::setupViewButtons(QHBoxLayout* layout) {
    m_view_group = new QButtonGroup(this);
    m_view_group->setExclusive(true);

    QString toggle_style = QString(kViewToggleStyle)
                               .arg(ui::kColorBorderDefault,
                                    ui::kColorTextBody,
                                    ui::kColorBgPageHover,
                                    ui::kColorPrimary,
                                    ui::kColorPrimaryDark);

    m_month_button = new QPushButton(tr("Month"), this);
    m_month_button->setCheckable(true);
    m_month_button->setChecked(true);
    m_month_button->setStyleSheet(toggle_style);
    m_view_group->addButton(m_month_button, 0);
    layout->addWidget(m_month_button);

    m_week_button = new QPushButton(tr("Week"), this);
    m_week_button->setCheckable(true);
    m_week_button->setStyleSheet(toggle_style);
    m_view_group->addButton(m_week_button, 1);
    layout->addWidget(m_week_button);

    m_day_button = new QPushButton(tr("Day"), this);
    m_day_button->setCheckable(true);
    m_day_button->setStyleSheet(toggle_style);
    m_view_group->addButton(m_day_button, 2);
    layout->addWidget(m_day_button);

    connect(m_view_group, &QButtonGroup::idClicked, this, &EmailCalendarDialog::onViewModeChanged);
}

void EmailCalendarDialog::setupSearchBar(QHBoxLayout* layout) {
    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText(tr("\xF0\x9F\x94\x8D Search events..."));
    m_search_edit->setMaximumWidth(250);
    m_search_edit->setClearButtonEnabled(true);
    m_search_edit->setStyleSheet(QString(kSearchBarStyle)
                                     .arg(ui::kColorBorderDefault,
                                          ui::kColorBgWhite,
                                          ui::kColorTextBody,
                                          ui::kColorPrimary));
    connect(
        m_search_edit, &QLineEdit::textChanged, this, &EmailCalendarDialog::onSearchTextChanged);
    layout->addWidget(m_search_edit);

    m_search_timer = new QTimer(this);
    m_search_timer->setSingleShot(true);
    m_search_timer->setInterval(email::kCalendarSearchDebounceMs);
    connect(m_search_timer, &QTimer::timeout, this, &EmailCalendarDialog::onSearchTimerFired);
}

// ============================================================================
// UI Setup — main content (sidebar + views + detail)
// ============================================================================

void EmailCalendarDialog::setupMainContent(QVBoxLayout* parent) {
    auto* outer_splitter = new QSplitter(Qt::Horizontal, this);
    outer_splitter->setChildrenCollapsible(false);

    setupSidebar(outer_splitter);

    auto* right_splitter = new QSplitter(Qt::Horizontal, this);
    right_splitter->setChildrenCollapsible(false);

    setupCalendarViews(right_splitter);
    setupDetailPanel(right_splitter);

    right_splitter->setSizes({600, 300});
    outer_splitter->addWidget(right_splitter);
    outer_splitter->setSizes({220, 880});
    parent->addWidget(outer_splitter, 1);
}

// ============================================================================
// UI Setup — sidebar (filters, day events)
// ============================================================================

void EmailCalendarDialog::setupSidebar(QSplitter* splitter) {
    auto* sidebar = new QWidget(this);
    sidebar->setMaximumWidth(260);
    sidebar->setMinimumWidth(200);
    auto* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(
        ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingMedium);

    setupFilterSection(layout);
    setupDayEventList(layout);

    splitter->addWidget(sidebar);
}

void EmailCalendarDialog::setupFilterSection(QVBoxLayout* layout) {
    auto* filter_label = new QLabel(tr("Filter by Status"), this);
    filter_label->setStyleSheet(
        QStringLiteral("font-weight: 600; color: %1;").arg(ui::kColorTextHeading));
    layout->addWidget(filter_label);

    m_filter_free = new QCheckBox(tr("\u2B24 Free"), this);
    m_filter_free->setChecked(true);
    m_filter_free->setStyleSheet(
        QStringLiteral("QCheckBox { color: %1; }").arg(email::kCalColorFreeBorder));
    m_filter_tentative = new QCheckBox(tr("\u2B24 Tentative"), this);
    m_filter_tentative->setChecked(true);
    m_filter_tentative->setStyleSheet(
        QStringLiteral("QCheckBox { color: %1; }").arg(email::kCalColorTentativeBorder));
    m_filter_busy = new QCheckBox(tr("\u2B24 Busy"), this);
    m_filter_busy->setChecked(true);
    m_filter_busy->setStyleSheet(
        QStringLiteral("QCheckBox { color: %1; }").arg(email::kCalColorBusyBorder));
    m_filter_oof = new QCheckBox(tr("\u2B24 Out of Office"), this);
    m_filter_oof->setChecked(true);
    m_filter_oof->setStyleSheet(
        QStringLiteral("QCheckBox { color: %1; }").arg(email::kCalColorOofBorder));

    for (auto* cb : {m_filter_free, m_filter_tentative, m_filter_busy, m_filter_oof}) {
        layout->addWidget(cb);
        connect(cb, &QCheckBox::toggled, this, &EmailCalendarDialog::onBusyFilterChanged);
    }

    auto* org_label = new QLabel(tr("Organizer"), this);
    org_label->setStyleSheet(
        QStringLiteral("font-weight: 600; color: %1; margin-top: 6px;").arg(ui::kColorTextHeading));
    layout->addWidget(org_label);

    m_organizer_filter = new QComboBox(this);
    m_organizer_filter->addItem(tr("All Organizers"));
    connect(m_organizer_filter,
            &QComboBox::currentIndexChanged,
            this,
            &EmailCalendarDialog::onOrganizerFilterChanged);
    layout->addWidget(m_organizer_filter);
}

void EmailCalendarDialog::setupDayEventList(QVBoxLayout* layout) {
    auto* events_label = new QLabel(tr("Events for Selected Date"), this);
    events_label->setStyleSheet(
        QStringLiteral("font-weight: 600; color: %1; margin-top: 8px;").arg(ui::kColorTextHeading));
    layout->addWidget(events_label);

    m_day_event_list = new QTableWidget(this);
    m_day_event_list->setColumnCount(DayColCount);
    m_day_event_list->setHorizontalHeaderLabels({tr("Time"), tr("Subject")});
    m_day_event_list->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_day_event_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_day_event_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_day_event_list->horizontalHeader()->setStretchLastSection(true);
    m_day_event_list->setColumnWidth(DayColTime, 70);
    m_day_event_list->verticalHeader()->setVisible(false);
    m_day_event_list->setMaximumHeight(250);
    m_day_event_list->setAlternatingRowColors(true);
    m_day_event_list->setStyleSheet(
        QStringLiteral("QTableWidget { border: 1px solid %1; font-size: %2pt; }"
                       "QTableWidget::item:selected { background: %3; color: white; }")
            .arg(ui::kColorBorderDefault)
            .arg(ui::kFontSizeSmall)
            .arg(ui::kColorPrimary));
    connect(m_day_event_list, &QTableWidget::cellClicked, this, [this](int row, int /*col*/) {
        auto* item = m_day_event_list->item(row, DayColSubject);
        if (item == nullptr) {
            return;
        }
        bool ok = false;
        uint64_t nid = item->data(Qt::UserRole).toULongLong(&ok);
        if (ok) {
            onEventClicked(nid);
        }
    });
    m_day_event_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_day_event_list,
            &QTableWidget::customContextMenuRequested,
            this,
            &EmailCalendarDialog::onDayEventListContextMenu);
    layout->addWidget(m_day_event_list, 1);
}

// ============================================================================
// UI Setup — calendar views (stacked month/week/day)
// ============================================================================

void EmailCalendarDialog::setupCalendarViews(QSplitter* splitter) {
    m_view_stack = new QStackedWidget(this);

    // Month view
    m_month_view = new CalendarMonthView(this);
    connect(m_month_view, &CalendarMonthView::dayClicked, this, &EmailCalendarDialog::onDayClicked);
    connect(
        m_month_view, &CalendarMonthView::eventClicked, this, &EmailCalendarDialog::onEventClicked);
    connect(m_month_view,
            &CalendarMonthView::eventContextMenuRequested,
            this,
            &EmailCalendarDialog::showEventContextMenu);
    m_view_stack->addWidget(m_month_view);

    // Week view (inside scroll area)
    auto* week_scroll = new QScrollArea(this);
    week_scroll->setWidgetResizable(true);
    m_week_view = new CalendarWeekDayView(true, this);
    connect(
        m_week_view, &CalendarWeekDayView::dayClicked, this, &EmailCalendarDialog::onDayClicked);
    connect(m_week_view,
            &CalendarWeekDayView::eventClicked,
            this,
            &EmailCalendarDialog::onEventClicked);
    connect(m_week_view,
            &CalendarWeekDayView::eventContextMenuRequested,
            this,
            &EmailCalendarDialog::showEventContextMenu);
    week_scroll->setWidget(m_week_view);
    m_view_stack->addWidget(week_scroll);

    // Day view (inside scroll area)
    auto* day_scroll = new QScrollArea(this);
    day_scroll->setWidgetResizable(true);
    m_day_view = new CalendarWeekDayView(false, this);
    connect(m_day_view, &CalendarWeekDayView::dayClicked, this, &EmailCalendarDialog::onDayClicked);
    connect(
        m_day_view, &CalendarWeekDayView::eventClicked, this, &EmailCalendarDialog::onEventClicked);
    connect(m_day_view,
            &CalendarWeekDayView::eventContextMenuRequested,
            this,
            &EmailCalendarDialog::showEventContextMenu);
    day_scroll->setWidget(m_day_view);
    m_view_stack->addWidget(day_scroll);

    m_view_stack->setCurrentIndex(0);
    splitter->addWidget(m_view_stack);
}

// ============================================================================
// UI Setup — detail panel
// ============================================================================

void EmailCalendarDialog::setupDetailPanel(QSplitter* splitter) {
    m_detail_browser = new QTextBrowser(this);
    m_detail_browser->setOpenExternalLinks(false);
    m_detail_browser->setMinimumWidth(250);
    m_detail_browser->setPlaceholderText(tr("Select an event to view details"));
    m_detail_browser->setStyleSheet(
        QStringLiteral("QTextBrowser { border-left: 1px solid %1; }").arg(ui::kColorBorderDefault));
    splitter->addWidget(m_detail_browser);
}

// ============================================================================
// UI Setup — footer (status, export, close)
// ============================================================================

void EmailCalendarDialog::setupFooter(QVBoxLayout* parent) {
    auto* footer = new QWidget(this);
    footer->setStyleSheet(QStringLiteral("background: %1; border-top: 1px solid %2;")
                              .arg(ui::kColorBgSurface, ui::kColorBorderDefault));
    auto* layout = new QHBoxLayout(footer);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginSmall, ui::kMarginMedium, ui::kMarginSmall);
    layout->setSpacing(ui::kSpacingMedium);

    m_status_label = new QLabel(this);
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    layout->addWidget(m_status_label);

    layout->addStretch();

    m_export_ics_button = new QPushButton(tr("Export ICS"), this);
    m_export_ics_button->setToolTip(tr("Export all events as iCalendar (.ics) files"));
    m_export_ics_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(
        m_export_ics_button, &QPushButton::clicked, this, &EmailCalendarDialog::onExportIcsClicked);
    layout->addWidget(m_export_ics_button);

    m_export_csv_button = new QPushButton(tr("Export CSV"), this);
    m_export_csv_button->setToolTip(tr("Export all events as a CSV spreadsheet"));
    m_export_csv_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(
        m_export_csv_button, &QPushButton::clicked, this, &EmailCalendarDialog::onExportCsvClicked);
    layout->addWidget(m_export_csv_button);

    m_close_button = new QPushButton(tr("Close"), this);
    m_close_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(m_close_button, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(m_close_button);

    parent->addWidget(footer);
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
    m_date_index.clear();
    m_node_index.clear();

    connect(m_controller,
            &::EmailInspectorController::folderItemsLoaded,
            this,
            &EmailCalendarDialog::onItemsLoaded);
    connect(m_controller,
            &::EmailInspectorController::itemDetailLoaded,
            this,
            &EmailCalendarDialog::onDetailLoaded);

    for (uint64_t folder_id : m_folder_ids) {
        m_controller->loadFolderItems(folder_id, 0, email::kCalendarMaxEventsPerFolder);
    }
}

// ============================================================================
// Index rebuilding
// ============================================================================

void EmailCalendarDialog::rebuildDateIndex() {
    m_date_index.clear();
    m_node_index.clear();

    for (int idx = 0; idx < m_all_events.size(); ++idx) {
        const auto& evt = m_all_events[idx];
        m_node_index[evt.node_id] = idx;
        if (!evt.start_time.isValid()) {
            continue;
        }
        if (!matchesSearch(evt) || !matchesFilter(evt)) {
            continue;
        }
        m_date_index[evt.start_time.date()].append(idx);
    }

    updateStatusLabel();
}

void EmailCalendarDialog::rebuildOrganizerFilter() {
    QString current = m_organizer_filter->currentText();
    m_organizer_filter->blockSignals(true);
    m_organizer_filter->clear();
    m_organizer_filter->addItem(tr("All Organizers"));

    QSet<QString> organizers;
    for (const auto& evt : m_all_events) {
        if (!evt.sender_name.isEmpty()) {
            organizers.insert(evt.sender_name);
        }
    }
    QStringList sorted(organizers.begin(), organizers.end());
    sorted.sort(Qt::CaseInsensitive);
    m_organizer_filter->addItems(sorted);

    int restore_idx = m_organizer_filter->findText(current);
    m_organizer_filter->setCurrentIndex(restore_idx > 0 ? restore_idx : 0);
    m_organizer_filter->blockSignals(false);
}

// ============================================================================
// Detail requests
// ============================================================================

void EmailCalendarDialog::requestDetailsForDate(const QDate& date) {
    for (const auto& evt : m_all_events) {
        if (evt.start_time.date() != date || evt.detail_loaded) {
            continue;
        }
        m_controller->loadItemDetail(evt.node_id);
    }
}

// ============================================================================
// Navigation slots
// ============================================================================

void EmailCalendarDialog::onNavigatePrevious() {
    switch (m_view_mode) {
    case ViewMode::Month:
        setCurrentDate(m_current_date.addMonths(-1));
        break;
    case ViewMode::Week:
        setCurrentDate(m_current_date.addDays(-kDaysPerWeek));
        break;
    case ViewMode::Day:
        setCurrentDate(m_current_date.addDays(-1));
        break;
    }
}

void EmailCalendarDialog::onNavigateNext() {
    switch (m_view_mode) {
    case ViewMode::Month:
        setCurrentDate(m_current_date.addMonths(1));
        break;
    case ViewMode::Week:
        setCurrentDate(m_current_date.addDays(kDaysPerWeek));
        break;
    case ViewMode::Day:
        setCurrentDate(m_current_date.addDays(1));
        break;
    }
}

void EmailCalendarDialog::onNavigateToday() {
    setCurrentDate(QDate::currentDate());
}

void EmailCalendarDialog::onViewModeChanged(int mode_index) {
    auto mode = static_cast<ViewMode>(mode_index);
    setViewMode(mode);
}

void EmailCalendarDialog::onMonthLabelClicked() {
    QMenu menu(this);
    for (int month = 1; month <= 12; ++month) {
        QDate sample(m_current_date.year(), month, 1);
        auto* action = menu.addAction(sample.toString(QStringLiteral("MMMM")));
        action->setCheckable(true);
        action->setChecked(month == m_current_date.month());
        connect(action, &QAction::triggered, this, [this, month]() {
            int day = qMin(m_current_date.day(),
                           QDate(m_current_date.year(), month, 1).daysInMonth());
            setCurrentDate(QDate(m_current_date.year(), month, day));
        });
    }
    menu.exec(m_month_label->mapToGlobal(QPoint(0, m_month_label->height())));
}

void EmailCalendarDialog::onYearLabelClicked() {
    constexpr int kYearRange = 5;
    int current_year = m_current_date.year();
    QMenu menu(this);
    for (int year = current_year - kYearRange; year <= current_year + kYearRange; ++year) {
        auto* action = menu.addAction(QString::number(year));
        action->setCheckable(true);
        action->setChecked(year == current_year);
        connect(action, &QAction::triggered, this, [this, year]() {
            int day = qMin(m_current_date.day(),
                           QDate(year, m_current_date.month(), 1).daysInMonth());
            setCurrentDate(QDate(year, m_current_date.month(), day));
        });
    }
    menu.exec(m_year_label->mapToGlobal(QPoint(0, m_year_label->height())));
}

// ============================================================================
// Event interaction slots
// ============================================================================

void EmailCalendarDialog::onEventClicked(uint64_t node_id) {
    m_selected_event_id = node_id;
    auto iter = m_node_index.find(node_id);
    if (iter == m_node_index.end()) {
        return;
    }
    const auto& evt = m_all_events[*iter];
    if (evt.detail_loaded) {
        displayEventSummary(evt);
    } else {
        m_controller->loadItemDetail(node_id);
    }
}

void EmailCalendarDialog::onDayClicked(const QDate& date) {
    setCurrentDate(date);
    populateDayEventList(date);
    requestDetailsForDate(date);
}

void EmailCalendarDialog::onEventDoubleClicked(uint64_t node_id) {
    onEventClicked(node_id);
    setViewMode(ViewMode::Day);
}

// ============================================================================
// Search and filter slots
// ============================================================================

void EmailCalendarDialog::onSearchTextChanged(const QString& text) {
    m_search_text = text;
    m_search_timer->start();
}

void EmailCalendarDialog::onSearchTimerFired() {
    rebuildDateIndex();
    refreshAllViews();
    populateDayEventList(m_current_date);
}

void EmailCalendarDialog::onBusyFilterChanged() {
    rebuildDateIndex();
    refreshAllViews();
    populateDayEventList(m_current_date);
}

void EmailCalendarDialog::onOrganizerFilterChanged() {
    rebuildDateIndex();
    refreshAllViews();
    populateDayEventList(m_current_date);
}

// ============================================================================
// Export slots
// ============================================================================

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

// ============================================================================
// Context menus
// ============================================================================

void EmailCalendarDialog::showEventContextMenu(const QPoint& global_pos, uint64_t node_id) {
    auto iter = m_node_index.find(node_id);
    if (iter == m_node_index.end()) {
        return;
    }
    const auto& evt = m_all_events[*iter];

    QMenu menu(this);
    menu.addAction(tr("View Details"), this, [this, node_id]() { onEventClicked(node_id); });
    menu.addAction(tr("View in Day"), this, [this, node_id, &evt]() {
        if (evt.start_time.isValid()) {
            setCurrentDate(evt.start_time.date());
        }
        setViewMode(ViewMode::Day);
        onEventClicked(node_id);
    });
    menu.addSeparator();
    menu.addAction(tr("Export Event as ICS"), this, [this, &evt]() { exportSingleEventIcs(evt); });
    menu.addAction(tr("Copy Details"), this, [this, &evt]() { copyEventDetailsToClipboard(evt); });
    menu.exec(global_pos);
}

void EmailCalendarDialog::onDayEventListContextMenu(const QPoint& pos) {
    auto* item = m_day_event_list->itemAt(pos);
    if (item == nullptr) {
        return;
    }
    int row = item->row();
    auto* subj_item = m_day_event_list->item(row, DayColSubject);
    if (subj_item == nullptr) {
        return;
    }
    bool ok = false;
    uint64_t node_id = subj_item->data(Qt::UserRole).toULongLong(&ok);
    if (!ok) {
        return;
    }
    showEventContextMenu(m_day_event_list->viewport()->mapToGlobal(pos), node_id);
}

void EmailCalendarDialog::exportSingleEventIcs(const CalendarEvent& evt) {
    QString file_path =
        QFileDialog::getSaveFileName(this,
                                     tr("Export Event as ICS"),
                                     evt.subject.simplified().left(50) + QStringLiteral(".ics"),
                                     tr("iCalendar Files (*.ics)"));
    if (file_path.isEmpty()) {
        return;
    }

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
    out << "BEGIN:VCALENDAR\r\n"
        << "VERSION:2.0\r\n"
        << "PRODID:-//SAK Utility//EN\r\n"
        << "BEGIN:VEVENT\r\n";
    if (evt.start_time.isValid()) {
        out << "DTSTART:" << evt.start_time.toUTC().toString("yyyyMMdd'T'HHmmss'Z'") << "\r\n";
    }
    if (evt.end_time.isValid()) {
        out << "DTEND:" << evt.end_time.toUTC().toString("yyyyMMdd'T'HHmmss'Z'") << "\r\n";
    }
    out << "SUMMARY:" << evt.subject << "\r\n";
    if (!evt.location.isEmpty()) {
        out << "LOCATION:" << evt.location << "\r\n";
    }
    if (!evt.body_plain.isEmpty()) {
        out << "DESCRIPTION:" << evt.body_plain.left(500) << "\r\n";
    }
    out << "END:VEVENT\r\n"
        << "END:VCALENDAR\r\n";
}

void EmailCalendarDialog::copyEventDetailsToClipboard(const CalendarEvent& evt) {
    QString text = evt.subject + QStringLiteral("\n");
    if (evt.start_time.isValid()) {
        text += tr("Start: %1\n")
                    .arg(evt.start_time.toString(QStringLiteral("ddd, MMM d yyyy h:mm AP")));
    }
    if (evt.end_time.isValid()) {
        text +=
            tr("End: %1\n").arg(evt.end_time.toString(QStringLiteral("ddd, MMM d yyyy h:mm AP")));
    }
    if (!evt.location.isEmpty()) {
        text += tr("Location: %1\n").arg(evt.location);
    }
    if (!evt.sender_name.isEmpty()) {
        text += tr("Organizer: %1\n").arg(evt.sender_name);
    }
    QApplication::clipboard()->setText(text);
}

// ============================================================================
// Data loading slots
// ============================================================================

void EmailCalendarDialog::onItemsLoaded(uint64_t /*folder_id*/,
                                        QVector<sak::PstItemSummary> items,
                                        int /*total*/) {
    for (const auto& item : items) {
        if (item.item_type != EmailItemType::Calendar &&
            item.item_type != EmailItemType::MeetingRequest) {
            continue;
        }
        CalendarEvent evt;
        evt.node_id = item.node_id;
        evt.item_type = item.item_type;
        evt.subject = item.subject;
        evt.sender_name = item.sender_name;
        evt.start_time = item.date;
        m_all_events.append(std::move(evt));
    }

    ++m_folders_loaded;
    if (m_folders_loaded < m_folder_ids.size()) {
        return;
    }

    rebuildDateIndex();
    rebuildOrganizerFilter();
    refreshAllViews();
    populateDayEventList(m_current_date);
}

void EmailCalendarDialog::onDetailLoaded(sak::PstItemDetail detail) {
    if (detail.item_type != EmailItemType::Calendar &&
        detail.item_type != EmailItemType::MeetingRequest) {
        return;
    }

    auto iter = m_node_index.find(detail.node_id);
    if (iter == m_node_index.end()) {
        return;
    }

    auto& evt = m_all_events[*iter];
    evt.detail_loaded = true;
    evt.end_time = detail.end_time;
    evt.location = detail.location;
    evt.is_all_day = detail.is_all_day;
    evt.busy_status = detail.busy_status;
    evt.importance = detail.importance;
    evt.attendees = detail.attendees;
    evt.recurrence_description = detail.recurrence_description;
    evt.body_plain = detail.body_plain;
    evt.body_html = detail.body_html;
    evt.sender_email = detail.sender_email;
    evt.attachment_count = detail.attachments.size();

    if (detail.start_time.isValid()) {
        evt.start_time = detail.start_time;
    }

    rebuildDateIndex();
    refreshAllViews();

    if (m_selected_event_id == detail.node_id) {
        displayEventSummary(evt);
    }

    populateDayEventList(m_current_date);
}

// ============================================================================
// View management
// ============================================================================

void EmailCalendarDialog::setViewMode(ViewMode mode) {
    m_view_mode = mode;
    m_view_stack->setCurrentIndex(static_cast<int>(mode));
    updateNavigationLabel();
    refreshAllViews();

    m_month_button->setChecked(mode == ViewMode::Month);
    m_week_button->setChecked(mode == ViewMode::Week);
    m_day_button->setChecked(mode == ViewMode::Day);
}

void EmailCalendarDialog::setCurrentDate(const QDate& date) {
    m_current_date = date;
    updateNavigationLabel();
    refreshAllViews();
    populateDayEventList(date);
}

void EmailCalendarDialog::refreshAllViews() {
    QMap<QDate, QVector<const CalendarEvent*>> event_map;
    for (auto it = m_date_index.constBegin(); it != m_date_index.constEnd(); ++it) {
        QVector<const CalendarEvent*> ptrs;
        ptrs.reserve(it->size());
        for (int idx : *it) {
            ptrs.append(&m_all_events[idx]);
        }
        event_map[it.key()] = std::move(ptrs);
    }

    m_month_view->setDate(m_current_date);
    m_month_view->setSelectedDate(m_current_date);
    m_month_view->setEvents(event_map);

    m_week_view->setDate(m_current_date);
    m_week_view->setEvents(event_map);
    m_week_view->setMinimumHeight(m_week_view->totalHeight());

    m_day_view->setDate(m_current_date);
    m_day_view->setEvents(event_map);
    m_day_view->setMinimumHeight(m_day_view->totalHeight());
}

void EmailCalendarDialog::updateNavigationLabel() {
    m_month_label->setText(m_current_date.toString(QStringLiteral("MMMM")));
    m_year_label->setText(m_current_date.toString(QStringLiteral("yyyy")));
}

void EmailCalendarDialog::updateStatusLabel() {
    int total = m_all_events.size();
    int visible = 0;
    for (const auto& evt : m_all_events) {
        if (matchesSearch(evt) && matchesFilter(evt)) {
            ++visible;
        }
    }
    if (visible == total) {
        m_status_label->setText(tr("%1 events").arg(total));
    } else {
        m_status_label->setText(tr("%1 of %2 events (filtered)").arg(visible).arg(total));
    }
}

// ============================================================================
// Event display — detail panel
// ============================================================================

void EmailCalendarDialog::displayEventSummary(const CalendarEvent& evt) {
    QColor fill = fillColorForStatus(evt.busy_status, evt.item_type);
    QColor border = borderColorForStatus(evt.busy_status, evt.item_type);

    QString html =
        QStringLiteral(
            "<div style='font-family: Segoe UI, sans-serif; "
            "padding: 12px;'>"
            "<div style='border-left: 4px solid %1; "
            "background: %2; padding: 8px 12px; "
            "border-radius: 4px; margin-bottom: 12px;'>"
            "<h3 style='color: %3; margin: 0 0 4px 0;'>"
            "%4</h3>")
            .arg(border.name(), fill.name(), ui::kColorTextHeading, evt.subject.toHtmlEscaped());

    html += buildDateHtml(evt);
    html += QStringLiteral("</div>");

    if (evt.is_all_day) {
        html += QStringLiteral("<p><b>All Day Event</b></p>");
    }

    appendHtmlField(html, QStringLiteral("Location"), evt.location);

    if (evt.busy_status >= 0 && evt.busy_status < email::kCalBusyStatusCount) {
        html += QStringLiteral(
                    "<p><b>Status:</b> "
                    "<span style='color: %1; font-weight: 600;'>"
                    "%2</span></p>")
                    .arg(border.name(), QLatin1String(kBusyLabels[evt.busy_status]));
    }

    appendHtmlField(html, QStringLiteral("Organizer"), evt.sender_name);
    if (!evt.sender_email.isEmpty()) {
        appendHtmlField(html, QStringLiteral("Email"), evt.sender_email);
    }
    html += buildAttendeesHtml(evt);
    appendHtmlField(html, QStringLiteral("Recurrence"), evt.recurrence_description);
    if (evt.importance != 1) {
        QString importance_str = (evt.importance == 2) ? tr("High") : tr("Low");
        appendHtmlField(html, QStringLiteral("Importance"), importance_str);
    }
    if (evt.attachment_count > 0) {
        appendHtmlField(html, QStringLiteral("Attachments"), QString::number(evt.attachment_count));
    }
    html += buildBodyHtml(evt);

    html += QStringLiteral("</div>");
    m_detail_browser->setHtml(html);
}

QString EmailCalendarDialog::buildDateHtml(const CalendarEvent& evt) {
    if (!evt.start_time.isValid()) {
        return {};
    }
    QString date_str = evt.start_time.toString(QStringLiteral("ddd, MMM d yyyy"));
    if (!evt.is_all_day) {
        date_str += QStringLiteral(" at ") +
                    evt.start_time.time().toString(QStringLiteral("h:mm AP"));
    }
    if (evt.end_time.isValid() && !evt.is_all_day) {
        date_str += QStringLiteral(" \u2013 ") +
                    evt.end_time.time().toString(QStringLiteral("h:mm AP"));
    }
    return QStringLiteral("<p style='color: %1; margin: 2px 0;'>%2</p>")
        .arg(ui::kColorTextSecondary, date_str);
}

QString EmailCalendarDialog::buildAttendeesHtml(const CalendarEvent& evt) {
    if (evt.attendees.isEmpty()) {
        return {};
    }
    QString html = QStringLiteral("<p><b>Attendees:</b></p><ul>");
    for (const auto& attendee : evt.attendees) {
        html += QStringLiteral("<li>%1</li>").arg(attendee.toHtmlEscaped());
    }
    html += QStringLiteral("</ul>");
    return html;
}

QString EmailCalendarDialog::buildBodyHtml(const CalendarEvent& evt) {
    if (!evt.body_html.isEmpty()) {
        return QStringLiteral("<hr style='border: 1px solid %1;'>").arg(ui::kColorBorderDefault) +
               evt.body_html;
    }
    if (!evt.body_plain.isEmpty()) {
        return QStringLiteral(
                   "<hr style='border: 1px solid %1;'>"
                   "<p style='white-space: pre-wrap;'>%2</p>")
            .arg(ui::kColorBorderDefault, evt.body_plain.toHtmlEscaped());
    }
    return {};
}

// ============================================================================
// Day event list (sidebar)
// ============================================================================

void EmailCalendarDialog::populateDayEventList(const QDate& date) {
    m_day_event_list->setRowCount(0);
    auto events = eventsForDate(date);

    std::sort(events.begin(), events.end(), [](const CalendarEvent* lhs, const CalendarEvent* rhs) {
        if (lhs->is_all_day != rhs->is_all_day) {
            return lhs->is_all_day;
        }
        return lhs->start_time < rhs->start_time;
    });

    for (const auto* evt : events) {
        int row = m_day_event_list->rowCount();
        m_day_event_list->insertRow(row);

        QString time_str;
        if (evt->is_all_day) {
            time_str = tr("All day");
        } else if (evt->start_time.isValid()) {
            time_str = evt->start_time.time().toString(QStringLiteral("h:mm AP"));
        }

        auto* time_item = new QTableWidgetItem(time_str);
        QColor fill = fillColorForStatus(evt->busy_status, evt->item_type);
        time_item->setBackground(fill);
        m_day_event_list->setItem(row, DayColTime, time_item);

        auto* subj_item = new QTableWidgetItem(evt->subject);
        subj_item->setData(Qt::UserRole, QVariant::fromValue(evt->node_id));
        subj_item->setBackground(fill);
        m_day_event_list->setItem(row, DayColSubject, subj_item);
    }
}

// ============================================================================
// Data query helpers
// ============================================================================

QVector<const CalendarEvent*> EmailCalendarDialog::eventsForDate(const QDate& date) const {
    QVector<const CalendarEvent*> result;
    auto it = m_date_index.find(date);
    if (it == m_date_index.end()) {
        return result;
    }
    result.reserve(it->size());
    for (int idx : *it) {
        result.append(&m_all_events[idx]);
    }
    return result;
}

QVector<const CalendarEvent*> EmailCalendarDialog::eventsInRange(const QDate& from,
                                                                 const QDate& to) const {
    QVector<const CalendarEvent*> result;
    auto it_begin = m_date_index.lowerBound(from);
    auto it_end = m_date_index.upperBound(to);
    for (auto iter = it_begin; iter != it_end; ++iter) {
        for (int idx : *iter) {
            result.append(&m_all_events[idx]);
        }
    }
    return result;
}

QVector<const CalendarEvent*> EmailCalendarDialog::filteredEvents() const {
    QVector<const CalendarEvent*> result;
    for (const auto& evt : m_all_events) {
        if (matchesSearch(evt) && matchesFilter(evt)) {
            result.append(&evt);
        }
    }
    return result;
}

bool EmailCalendarDialog::matchesSearch(const CalendarEvent& evt) const {
    if (m_search_text.isEmpty()) {
        return true;
    }
    if (evt.subject.contains(m_search_text, Qt::CaseInsensitive)) {
        return true;
    }
    if (evt.location.contains(m_search_text, Qt::CaseInsensitive)) {
        return true;
    }
    if (evt.sender_name.contains(m_search_text, Qt::CaseInsensitive)) {
        return true;
    }
    for (const auto& attendee : evt.attendees) {
        if (attendee.contains(m_search_text, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool EmailCalendarDialog::matchesFilter(const CalendarEvent& evt) const {
    // Organizer filter
    if (m_organizer_filter->currentIndex() > 0) {
        if (evt.sender_name != m_organizer_filter->currentText()) {
            return false;
        }
    }

    // Busy-status filter
    int status = evt.busy_status;
    if (status < 0) {
        return true;  // Unknown status always shown
    }
    switch (status) {
    case 0:
        return m_filter_free->isChecked();
    case 1:
        return m_filter_tentative->isChecked();
    case 2:
        return m_filter_busy->isChecked();
    case 3:
        return m_filter_oof->isChecked();
    default:
        return true;
    }
}

// ============================================================================
// Color helpers
// ============================================================================

QColor EmailCalendarDialog::fillColorForStatus(int busy_status, EmailItemType type) {
    if (type == EmailItemType::MeetingRequest) {
        return QColor(email::kCalColorMeetingFill);
    }
    switch (busy_status) {
    case 0:
        return QColor(email::kCalColorFreeFill);
    case 1:
        return QColor(email::kCalColorTentativeFill);
    case 2:
        return QColor(email::kCalColorBusyFill);
    case 3:
        return QColor(email::kCalColorOofFill);
    default:
        return QColor(email::kCalColorDefaultFill);
    }
}

QColor EmailCalendarDialog::borderColorForStatus(int busy_status, EmailItemType type) {
    if (type == EmailItemType::MeetingRequest) {
        return QColor(email::kCalColorMeetingBorder);
    }
    switch (busy_status) {
    case 0:
        return QColor(email::kCalColorFreeBorder);
    case 1:
        return QColor(email::kCalColorTentativeBorder);
    case 2:
        return QColor(email::kCalColorBusyBorder);
    case 3:
        return QColor(email::kCalColorOofBorder);
    default:
        return QColor(email::kCalColorDefaultBorder);
    }
}

void EmailCalendarDialog::appendHtmlField(QString& html,
                                          const QString& label,
                                          const QString& value) {
    if (!value.isEmpty()) {
        html += QStringLiteral("<p><b>%1:</b> %2</p>").arg(label, value.toHtmlEscaped());
    }
}

}  // namespace sak

#include "email_calendar_dialog.moc"
