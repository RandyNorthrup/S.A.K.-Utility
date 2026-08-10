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

    auto* bottomRow = new QHBoxLayout();
    auto* clearBtn = new QPushButton(tr("Clear"), this);
    clearBtn->setAccessibleName(tr("Clear detached log"));
    clearBtn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(clearBtn, &QPushButton::clicked, this, &DetachableLogWindow::clearLog);
    bottomRow->addWidget(clearBtn);
    bottomRow->addStretch();
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
    bottomRow->addWidget(m_jumpToNewestButton);
    layout->addLayout(bottomRow);

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
    const int previous_value = m_logScrollController ? m_logScrollController->scrollValue() : 0;
    const bool follow_newest = !m_logScrollController ||
                               m_logScrollController->shouldFollowNewestForAppend();
    // QTextEdit::append() promotes anything that looks like markup to rich text, and log lines
    // carry file paths, program names, command output and AI text; wrap so they read literally.
    m_logEdit->append(sak::ui::asLiteralRichText(timestamp + bounded));
    if (!m_logScrollController) {
        return;
    }
    if (follow_newest) {
        m_logScrollController->scrollToBottomLater();
    } else {
        m_logScrollController->restoreScrollPositionLater(previous_value);
    }
}

void DetachableLogWindow::clearLog() {
    if (m_logScrollController) {
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
    auto* mainWin = findMainWindow();
    if (!mainWin) {
        return;
    }

    const QRect mainFrame = mainWin->frameGeometry();
    const int snapThreshold = 40;
    const int rightEdge = mainFrame.right() + 1;

    // If within snap threshold of the main window's right edge, re-anchor
    if (std::abs(pos().x() - rightEdge) < snapThreshold &&
        std::abs(frameGeometry().top() - mainFrame.top()) < snapThreshold) {
        m_anchored = true;
    } else {
        m_anchored = false;
    }
}

void DetachableLogWindow::snapToMainWindow() {
    auto* mainWin = findMainWindow();
    if (!mainWin) {
        return;
    }

    m_programmaticMove = true;

    const QRect mainFrame = mainWin->frameGeometry();
    const QRect mainGeo = mainWin->geometry();
    const int logWidth = width();

    // Snap to right edge of main window, match content height so frames align
    const int frameTopOffset = mainGeo.top() - mainFrame.top();
    move(mainFrame.right() + 1, mainGeo.top() - frameTopOffset);
    resize(logWidth, mainGeo.height());

    m_anchored = true;
    m_programmaticMove = false;
}

QWidget* DetachableLogWindow::findMainWindow() const {
    for (auto* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QMainWindow*>(widget)) {
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
    setMinimumSize(minimumSizeHint());
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

    const int trackHeight = kToggleTrackHeight;
    const int trackWidth = kToggleTrackWidth;
    const int knobSize = kToggleKnobSize;
    const int labelX = trackWidth + kToggleLabelGap;

    // Track
    QRect trackRect(
        0, (height() - trackHeight) / kToggleTrackRadiusDivisor, trackWidth, trackHeight);
    const QColor trackColor = m_checked ? QColor(QString::fromLatin1(ui::kColorPrimary))
                                        : QColor(QString::fromLatin1(ui::kColorBorderMuted));
    p.setBrush(trackColor);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(trackRect,
                      trackHeight / kToggleTrackRadiusDivisor,
                      trackHeight / kToggleTrackRadiusDivisor);

    // Knob
    const int knobX = m_checked ? (trackWidth - knobSize - kToggleKnobInsetPx) : kToggleKnobInsetPx;
    const int knobY = (height() - knobSize) / kToggleTrackRadiusDivisor;
    p.setBrush(QColor(QString::fromLatin1(ui::kColorBgWhite)));
    p.drawEllipse(knobX, knobY, knobSize, knobSize);

    // Label
    p.setPen(palette().color(QPalette::WindowText));
    p.setFont(toggleLabelFont(font()));
    QRect labelRect(labelX, 0, width() - labelX, height());
    p.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, m_label);
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
