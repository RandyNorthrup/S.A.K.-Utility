// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/detachable_log_window.h"

#include "sak/follow_scroll_controller.h"
#include "sak/layout_constants.h"
#include "sak/rich_text_safety.h"
#include "sak/style_constants.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <mutex>

namespace sak {

namespace {
constexpr int kToggleTrackHeight = 22;
constexpr int kToggleTrackWidth = 44;
constexpr int kToggleKnobSize = 18;
constexpr int kToggleTrackRadiusDivisor = 2;
constexpr int kToggleKnobInsetPx = 2;
constexpr int kToggleLabelGap = 6;
constexpr int kToggleLabelTrailingPadding = 14;

// Bound log retention: attacker/tool output is untrusted and can be arbitrarily
// large. Keep only the newest kMaxLogBlocks lines.
constexpr int kMaxLogBlocks = 5000;

QFont toggleLabelFont(QFont font) {
    font.setPointSize(ui::kFontSizeNote);
    font.setBold(true);
    return font;
}

int toggleSwitchWidth(const QString& label, const QFont& font) {
    const QFontMetrics metrics(toggleLabelFont(font));
    return (std::max)(sak::kSnapButtonW,
                      kToggleTrackWidth + kToggleLabelGap + metrics.horizontalAdvance(label) +
                          kToggleLabelTrailingPadding);
}

struct LogBlockFormats {
    QTextBlockFormat message_block;
    QTextCharFormat message_text;
    QTextBlockFormat output_block;
    QTextCharFormat output_text;
};

LogBlockFormats logBlockFormats(const QFont& log_font) {
    LogBlockFormats formats;
    // Consolas is on every Windows install, so asking for it never sends Qt through the font
    // alias table the way a missing family does (docs/GUI_STARTUP_FIELD_NOTES.md).
    QFont fixed(QStringLiteral("Consolas"));
    fixed.setPointSizeF(log_font.pointSizeF());
    formats.output_text.setFont(fixed);
    formats.output_block.setNonBreakableLines(true);
    return formats;
}

// A message's first line is the message. The lines under it are what a program wrote: the power
// plan and Check Disk reports drawn with | and =, a dry run's script, a command's output. In the
// log's own proportional font, wrapped to the window, their borders and columns fell apart
// (review 2, F29 and F31). They go in a block of their own in a fixed-width font that does not
// wrap; a line wider than the window scrolls sideways. The message's own line breaks stay line
// breaks inside that one block, so the kMaxLogBlocks cap still counts about one block per line.
void insertLogMessage(QTextCursor* cursor, const QString& message, const LogBlockFormats& formats) {
    const qsizetype newline = message.indexOf(QLatin1Char('\n'));
    if (newline < 0) {
        cursor->insertText(message, formats.message_text);
        return;
    }
    cursor->insertText(message.left(newline), formats.message_text);
    QString output = message.mid(newline + 1);
    while (output.endsWith(QLatin1Char('\n'))) {
        output.chop(1);
    }
    if (output.isEmpty()) {
        return;
    }
    output.replace(QLatin1Char('\n'), QChar::LineSeparator);
    cursor->insertBlock(formats.output_block, formats.output_text);
    cursor->insertText(output, formats.output_text);
}
}  // namespace

// ============================================================================
// DetachableLogWindow
// ============================================================================

DetachableLogWindow::DetachableLogWindow(const QString& title, QWidget* parent)
    : QWidget(parent, Qt::Tool) {
    setWindowTitle(title);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMinimumSize(sak::kDetachLogMinW, sak::kDetachLogMinH);
    resize(sak::kDetachLogInitW, sak::kDetachLogInitH);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        sak::ui::kMarginTight, sak::ui::kMarginTight, sak::ui::kMarginTight, sak::ui::kMarginTight);
    layout->setSpacing(sak::ui::kSpacingTight);

    // Log text area (uses app theme -- no custom dark style)
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    // Cap accumulated lines so a long, high-volume elevated session cannot grow the
    // document without bound (oldest lines drop, newest are kept).
    m_logEdit->document()->setMaximumBlockCount(kMaxLogBlocks);
    m_logEdit->setAccessibleName(tr("Detached operation log"));
    m_logEdit->setPlaceholderText(tr("Operation log will appear here..."));
    layout->addWidget(m_logEdit);
    m_logScrollController = new FollowScrollController(m_logEdit, this);

    auto* bottom_row = new QHBoxLayout();
    auto* clear_btn = new QPushButton(tr("Clear"), this);
    clear_btn->setAccessibleName(tr("Clear detached log"));
    clear_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(clear_btn, &QPushButton::clicked, this, &DetachableLogWindow::clearLog);
    bottom_row->addWidget(clear_btn);
    bottom_row->addStretch();
    m_jumpToNewestButton = new QPushButton(tr("Jump to newest"), this);
    m_jumpToNewestButton->setToolTip(tr("Scroll to the latest log line and resume auto-scroll"));
    m_jumpToNewestButton->setAccessibleName(tr("Jump to newest log entry"));
    m_jumpToNewestButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_jumpToNewestButton->hide();
    m_logScrollController->setJumpToNewestButton(m_jumpToNewestButton);
    connect(m_jumpToNewestButton,
            &QPushButton::clicked,
            m_logScrollController,
            &FollowScrollController::jumpToNewest);
    bottom_row->addWidget(m_jumpToNewestButton);
    layout->addLayout(bottom_row);

    m_snapTimer = new QTimer(this);
    m_snapTimer->setSingleShot(true);
    m_snapTimer->setInterval(sak::kTimerSnapMs);
    connect(m_snapTimer, &QTimer::timeout, this, &DetachableLogWindow::snapToMainWindow);
}

DetachableLogWindow::~DetachableLogWindow() = default;

bool DetachableLogWindow::replaceLog(const QStringList& lines) {
    if (!isVisible()) {
        return false;
    }
    // One edit block and no repaints while the document is rebuilt: a full panel log is up to
    // 5000 lines, and per-line append() relaid the document out each time.
    m_logEdit->setUpdatesEnabled(false);
    m_logEdit->clear();
    QTextCursor cursor(m_logEdit->document());
    cursor.beginEditBlock();
    const LogBlockFormats formats = logBlockFormats(m_logEdit->font());
    for (const QString& line : lines) {
        // Panel log lines carry paths, program names and command output; insert them as
        // literal text so nothing in them is rendered as markup.
        if (!cursor.atStart()) {
            cursor.insertBlock(formats.message_block, formats.message_text);
        }
        insertLogMessage(&cursor, line, formats);
    }
    cursor.endEditBlock();
    m_logEdit->setUpdatesEnabled(true);
    if (m_logScrollController != nullptr) {
        m_logScrollController->scrollToBottomLater();
    }
    return true;
}

void DetachableLogWindow::appendLine(const QString& line) {
    QTextCursor cursor(m_logEdit->document());
    cursor.movePosition(QTextCursor::End);
    const LogBlockFormats formats = logBlockFormats(m_logEdit->font());
    cursor.beginEditBlock();
    if (!m_logEdit->document()->isEmpty()) {
        cursor.insertBlock(formats.message_block, formats.message_text);
    }
    insertLogMessage(&cursor, line, formats);
    cursor.endEditBlock();
}

void DetachableLogWindow::clearLog() {
    if (m_logScrollController != nullptr) {
        m_logScrollController->setAutoScroll(true);
    }
    m_logEdit->clear();
}

void DetachableLogWindow::setLogVisible(bool visible) {
    if (visible) {
        m_anchored = true;  // Always snap on toggle-open
        // Opening the log must not take focus away from the window the user is typing in:
        // WA_ShowWithoutActivating (constructor) shows it on top without activating it, and
        // neither raise() nor activateWindow() is called -- an owned tool window already sits
        // above its owner.
        show();
        makeRoomBesideMainWindow();
        snapToMainWindow();
    } else {
        hide();
    }
    Q_EMIT visibilityChanged(visible);
}

bool DetachableLogWindow::isLogVisible() const {
    return isVisible();
}

bool DetachableLogWindow::eventFilter(QObject* watched, QEvent* event) {
    return QWidget::eventFilter(watched, event);
}

void DetachableLogWindow::repositionIfAnchored() {
    if (m_anchored && isVisible()) {
        snapToMainWindow();
    }
}

void DetachableLogWindow::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_anchored) {
        m_snapTimer->start();
    }
}

void DetachableLogWindow::closeEvent(QCloseEvent* event) {
    event->ignore();
    hide();
    Q_EMIT visibilityChanged(false);
}

void DetachableLogWindow::moveEvent(QMoveEvent* event) {
    QWidget::moveEvent(event);

    if (m_programmaticMove) {
        return;
    }

    // If user drags window, check whether it's still close to main window
    auto* main_win = findMainWindow();
    if (main_win == nullptr) {
        return;
    }

    const QRect main_frame = main_win->frameGeometry();
    const int snap_threshold = 40;
    const int right_edge = main_frame.right() + 1;

    // If within snap threshold of the main window's right edge, re-anchor
    if (std::abs(pos().x() - right_edge) < snap_threshold &&
        std::abs(frameGeometry().top() - main_frame.top()) < snap_threshold) {
        m_anchored = true;
    } else {
        m_anchored = false;
    }
}

void DetachableLogWindow::makeRoomBesideMainWindow() {
    // Only when the person opens the log. Doing this on every snap would push the main window back
    // each time they dragged it toward the screen edge.
    auto* main_win = findMainWindow();
    if (main_win == nullptr) {
        return;
    }
    const QScreen* screen = main_win->screen();
    if (screen == nullptr) {
        return;
    }
    const int shift = mainWindowShiftForLog(main_win->frameGeometry(),
                                            frameGeometry().width(),
                                            screen->availableGeometry());
    if (shift > 0) {
        main_win->move(main_win->pos() - QPoint(shift, 0));
    }
}

int DetachableLogWindow::mainWindowShiftForLog(const QRect& main_frame,
                                               int log_frame_width,
                                               const QRect& available) {
    const int room_on_right = available.right() - main_frame.right();
    const int room_on_left = main_frame.left() - available.left();
    return std::clamp(log_frame_width - room_on_right, 0, (std::max)(room_on_left, 0));
}

void DetachableLogWindow::snapToMainWindow() {
    auto* main_win = findMainWindow();
    if (main_win == nullptr) {
        return;
    }

    m_programmaticMove = true;

    const QScreen* screen = main_win->screen();
    const QRect available = (screen != nullptr) ? screen->availableGeometry() : QRect();
    const QRect own_frame = frameGeometry();
    const QRect own_content = geometry();
    const QMargins log_frame(own_content.left() - own_frame.left(),
                             own_content.top() - own_frame.top(),
                             own_frame.right() - own_content.right(),
                             own_frame.bottom() - own_content.bottom());
    const QRect target = anchoredGeometry(
        main_win->frameGeometry(), main_win->geometry(), width(), log_frame, available);
    move(target.topLeft());
    resize(target.size());

    m_anchored = true;
    m_programmaticMove = false;
}

QRect DetachableLogWindow::anchoredGeometry(const QRect& main_frame,
                                            const QRect& main_geometry,
                                            int log_width,
                                            const QMargins& log_frame,
                                            const QRect& available) {
    // Snap the frame to the right edge and top of the main window, content as tall as its.
    const int frame_w = log_frame.left() + log_frame.right();
    const int frame_h = log_frame.top() + log_frame.bottom();
    QRect frame(main_frame.right() + 1,
                main_frame.top(),
                log_width + frame_w,
                main_geometry.height() + frame_h);
    if (available.isValid()) {
        // Beside the main window whenever it fits, narrowing the log down to its minimum to do so.
        const int room_on_right = available.right() - main_frame.right();
        if (room_on_right >= sak::kDetachLogMinW + frame_w) {
            frame.setWidth((std::min)(frame.width(), room_on_right));
        }
        // Keep the whole frame on the work area. A maximized main window leaves no room on its
        // right, so the log overlaps its right edge rather than opening off-screen.
        frame.setWidth((std::min)(frame.width(), available.width()));
        frame.setHeight((std::min)(frame.height(), available.height()));
        if (frame.right() > available.right()) {
            frame.moveRight(available.right());
        }
        if (frame.left() < available.left()) {
            frame.moveLeft(available.left());
        }
        if (frame.bottom() > available.bottom()) {
            frame.moveBottom(available.bottom());
        }
        if (frame.top() < available.top()) {
            frame.moveTop(available.top());
        }
    }
    return {frame.topLeft(), QSize(frame.width() - frame_w, frame.height() - frame_h)};
}

QWidget* DetachableLogWindow::findMainWindow() const {
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow*>(widget) != nullptr) {
            return widget;
        }
    }
    return nullptr;
}

// ============================================================================
// LogToggleSwitch
// ============================================================================

namespace {

/// @brief True for keys that should activate a toggle (Space / Enter).
[[nodiscard]] bool isToggleActivationKey(int key) {
    return key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter;
}

/// @brief Gives LogToggleSwitch keyboard activation without adding a
///        keyPressEvent override to its shared class header. Parented to the
///        switch, so it shares the widget's lifetime.
class ToggleKeyActivator : public QObject {
public:
    explicit ToggleKeyActivator(LogToggleSwitch* target) : QObject(target), m_target(target) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::KeyPress) {
            return QObject::eventFilter(watched, event);
        }
        const auto* key = static_cast<QKeyEvent*>(event);
        if (!isToggleActivationKey(key->key())) {
            return QObject::eventFilter(watched, event);
        }
        m_target->setChecked(!m_target->isChecked());
        return true;
    }

private:
    LogToggleSwitch* m_target;
};

/// The Log switch as a screen reader and UI Automation see it: a check box named for its label,
/// checked while the log window shows. As a plain widget it was an unnamed Group, and its tooltip
/// was all it said (review 3).
class LogToggleAccessible final : public QAccessibleWidget {
public:
    explicit LogToggleAccessible(QWidget* widget)
        : QAccessibleWidget(widget, QAccessible::CheckBox) {}

    [[nodiscard]] QAccessible::State state() const override {
        QAccessible::State state = QAccessibleWidget::state();
        state.checkable = true;
        state.checked = toggle()->isChecked();
        return state;
    }

    [[nodiscard]] QStringList actionNames() const override {
        return {QAccessibleActionInterface::toggleAction()};
    }

    void doAction(const QString& action_name) override {
        if (action_name == QAccessibleActionInterface::toggleAction() ||
            action_name == QAccessibleActionInterface::pressAction()) {
            toggle()->setChecked(!toggle()->isChecked());
        }
    }

private:
    [[nodiscard]] LogToggleSwitch* toggle() const {
        return static_cast<LogToggleSwitch*>(widget());
    }
};

QAccessibleInterface* logToggleAccessibleFactory(const QString& class_name, QObject* object) {
    if (class_name == QLatin1String(LogToggleSwitch::staticMetaObject.className()) &&
        object != nullptr && object->isWidgetType()) {
        return new LogToggleAccessible(static_cast<QWidget*>(object));
    }
    return nullptr;
}

}  // namespace

LogToggleSwitch::LogToggleSwitch(const QString& label, QWidget* parent)
    : QWidget(parent), m_label(label) {
    static std::once_flag factory_installed;
    std::call_once(factory_installed,
                   []() { QAccessible::installFactory(logToggleAccessibleFactory); });
    setAccessibleName(label);
    setAccessibleDescription(tr("Shows or hides the log window"));
    setMinimumSize(LogToggleSwitch::minimumSizeHint());
    setMaximumHeight(sak::kSnapButtonH);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Toggle log window"));
    // Keyboard accessibility: focusable, and Space/Enter activate the toggle
    // via the parented key filter above (header cannot gain a new override).
    setFocusPolicy(Qt::StrongFocus);
    installEventFilter(new ToggleKeyActivator(this));
}

QHBoxLayout* addLogFooter(QVBoxLayout* page_layout, LogToggleSwitch* toggle) {
    Q_ASSERT(page_layout != nullptr);
    Q_ASSERT(toggle != nullptr);
    const QMargins page = page_layout->contentsMargins();
    auto* row = new QHBoxLayout();
    row->setContentsMargins(std::max(ui::kMarginNone, ui::kMarginMedium - page.left()),
                            ui::kSpacingTight,
                            std::max(ui::kMarginNone, ui::kMarginMedium - page.right()),
                            std::max(ui::kMarginNone, ui::kMarginMedium - page.bottom()));
    row->setSpacing(ui::kSpacingMedium);
    row->addWidget(toggle);
    row->addStretch();
    page_layout->addLayout(row);
    return row;
}

QSize LogToggleSwitch::sizeHint() const {
    return minimumSizeHint();
}

QSize LogToggleSwitch::minimumSizeHint() const {
    return {toggleSwitchWidth(m_label, font()), sak::kSnapButtonH};
}

void LogToggleSwitch::setChecked(bool checked) {
    if (m_checked != checked) {
        m_checked = checked;
        update();
        QAccessible::State changed;
        changed.checked = true;
        QAccessibleStateChangeEvent event(this, changed);
        QAccessible::updateAccessibility(&event);
        Q_EMIT toggled(m_checked);
    }
}

void LogToggleSwitch::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int track_height = kToggleTrackHeight;
    const int track_width = kToggleTrackWidth;
    const int knob_size = kToggleKnobSize;
    const int label_x = track_width + kToggleLabelGap;

    // Track
    const QRect track_rect(
        0, (height() - track_height) / kToggleTrackRadiusDivisor, track_width, track_height);
    const QColor track_color = m_checked ? QColor(QString::fromLatin1(ui::kColorPrimary))
                                         : ui::themedColor(ui::kColorBorderMuted);
    p.setBrush(track_color);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(track_rect,
                      track_height / kToggleTrackRadiusDivisor,
                      track_height / kToggleTrackRadiusDivisor);

    // Knob
    const int knob_x = m_checked ? (track_width - knob_size - kToggleKnobInsetPx)
                                 : kToggleKnobInsetPx;
    const int knob_y = (height() - knob_size) / kToggleTrackRadiusDivisor;
    p.setBrush(QColor(QString::fromLatin1(ui::kColorBgWhite)));
    p.drawEllipse(knob_x, knob_y, knob_size, knob_size);

    // Label
    p.setPen(palette().color(QPalette::WindowText));
    p.setFont(toggleLabelFont(font()));
    const QRect label_rect(label_x, 0, width() - label_x, height());
    p.drawText(label_rect, Qt::AlignVCenter | Qt::AlignLeft, m_label);
}

void LogToggleSwitch::mousePressEvent(QMouseEvent* event) {
    // Only the left button toggles this security-sensitive control; right/middle
    // clicks fall through to default handling instead of flipping state.
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setChecked(!m_checked);
    event->accept();
}

}  // namespace sak
