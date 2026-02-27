// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/detachable_log_window.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QDateTime>
#include <QMoveEvent>
#include <QShowEvent>
#include <QCloseEvent>
#include <QTimer>
#include <QScreen>
#include <QApplication>
#include <QMainWindow>
#include <QPushButton>

namespace sak {

// ============================================================================
// DetachableLogWindow
// ============================================================================

DetachableLogWindow::DetachableLogWindow(const QString& title, QWidget* parent)
    : QWidget(parent, Qt::Tool)
{
    setWindowTitle(title);
    setMinimumSize(360, 200);
    resize(420, 300);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    // Log text area (uses app theme — no custom dark style)
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setPlaceholderText(tr("Operation log will appear here..."));
    layout->addWidget(m_logEdit);

    // Bottom row: clear button
    auto* bottomRow = new QHBoxLayout();
    auto* clearBtn = new QPushButton(tr("Clear"), this);
    connect(clearBtn, &QPushButton::clicked, this, &DetachableLogWindow::clearLog);
    bottomRow->addWidget(clearBtn);
    bottomRow->addStretch();
    layout->addLayout(bottomRow);

    m_snapTimer = new QTimer(this);
    m_snapTimer->setSingleShot(true);
    m_snapTimer->setInterval(50);
    connect(m_snapTimer, &QTimer::timeout, this, &DetachableLogWindow::snapToMainWindow);
}

DetachableLogWindow::~DetachableLogWindow() = default;

void DetachableLogWindow::appendLog(const QString& message) {
    const QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    m_logEdit->append(timestamp + message);
}

void DetachableLogWindow::clearLog() {
    m_logEdit->clear();
}

void DetachableLogWindow::setLogVisible(bool visible) {
    if (visible) {
        m_anchored = true;   // Always snap on toggle-open
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
    if (!mainWin) return;

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
    if (!mainWin) return;

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

LogToggleSwitch::LogToggleSwitch(const QString& label, QWidget* parent)
    : QWidget(parent)
    , m_label(label)
{
    setFixedSize(80, 28);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Toggle log window"));
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

    const int trackHeight = 22;
    const int trackWidth = 44;
    const int knobSize = 18;
    const int labelX = trackWidth + 6;

    // Track
    QRect trackRect(0, (height() - trackHeight) / 2, trackWidth, trackHeight);
    const QColor trackColor = m_checked ? QColor(59, 130, 246) : QColor(148, 163, 184);
    p.setBrush(trackColor);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(trackRect, trackHeight / 2, trackHeight / 2);

    // Knob
    const int knobX = m_checked ? (trackWidth - knobSize - 2) : 2;
    const int knobY = (height() - knobSize) / 2;
    p.setBrush(Qt::white);
    p.drawEllipse(knobX, knobY, knobSize, knobSize);

    // Label
    p.setPen(palette().color(QPalette::WindowText));
    QFont f = font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    QRect labelRect(labelX, 0, width() - labelX, height());
    p.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, m_label);
}

void LogToggleSwitch::mousePressEvent(QMouseEvent* event) {
    QWidget::mousePressEvent(event);
    setChecked(!m_checked);
}

} // namespace sak
