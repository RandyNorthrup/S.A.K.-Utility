// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/detachable_log_window.h"

#include "sak/follow_scroll_controller.h"
#include "sak/layout_constants.h"
#include "sak/rich_text_safety.h"
#include "sak/style_constants.h"

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
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

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
// large. Keep only the newest kMaxLogBlocks lines, and truncate any single line to
// kMaxLogMessageChars so one pathological message cannot exhaust memory.
constexpr int kMaxLogBlocks = 5000;
constexpr int kMaxLogMessageChars = 16'384;

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
}  // namespace

// ============================================================================
// DetachableLogWindow
// ============================================================================

DetachableLogWindow::DetachableLogWindow(const QString& title, QWidget* parent)
    : QWidget(parent, Qt::Tool) {
    setWindowTitle(title);
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

void DetachableLogWindow::appendLog(const QString& message) {
    // Bound a single line so one pathological message (huge tool/scan/command output)
    // cannot blow up the document; the block cap in the constructor bounds accumulation.
    QString bounded = message;
    if (bounded.size() > kMaxLogMessageChars) {
        bounded = bounded.left(kMaxLogMessageChars) + QStringLiteral(" [truncated]");
    }
    const QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    const int previous_value =
        (m_logScrollController != nullptr) ? m_logScrollController->scrollValue() : 0;
    const bool follow_newest = (m_logScrollController == nullptr) ||
                               m_logScrollController->shouldFollowNewestForAppend();
    // QTextEdit::append() promotes anything that looks like markup to rich text, and log lines
    // carry file paths, program names, command output and AI text; wrap so they read literally.
    m_logEdit->append(sak::ui::asLiteralRichText(timestamp + bounded));
    if (m_logScrollController == nullptr) {
        return;
    }
    if (follow_newest) {
        m_logScrollController->scrollToBottomLater();
    } else {
        m_logScrollController->restoreScrollPositionLater(previous_value);
    }
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
        show();
        snapToMainWindow();
        raise();
        activateWindow();
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

void DetachableLogWindow::snapToMainWindow() {
    auto* main_win = findMainWindow();
    if (main_win == nullptr) {
        return;
    }

    m_programmaticMove = true;

    const QRect main_frame = main_win->frameGeometry();
    const QRect main_geo = main_win->geometry();
    const int log_width = width();

    // Snap to right edge of main window, match content height so frames align
    const int frame_top_offset = main_geo.top() - main_frame.top();
    move(main_frame.right() + 1, main_geo.top() - frame_top_offset);
    resize(log_width, main_geo.height());

    m_anchored = true;
    m_programmaticMove = false;
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

}  // namespace

LogToggleSwitch::LogToggleSwitch(const QString& label, QWidget* parent)
    : QWidget(parent), m_label(label) {
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
                                         : QColor(QString::fromLatin1(ui::kColorBorderMuted));
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
