// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/info_button.h"

#include <QApplication>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QStyle>
#include <QVBoxLayout>

namespace sak {

// ============================================================================
// Icon rendering
// ============================================================================

QIcon InfoButton::createInfoIcon(int size)
{
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Filled circle — Windows 11 accent blue
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 120, 212));  // #0078D4
    p.drawEllipse(1, 1, size - 2, size - 2);

    // White "i" body (vertical bar)
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    const int cx = size / 2;
    const int barW = qMax(2, size / 6);
    const int barTop = size * 40 / 100;
    const int barBot = size * 78 / 100;
    p.drawRoundedRect(cx - barW, barTop, barW * 2, barBot - barTop, 1, 1);

    // White "i" dot
    const int dotR = qMax(1, size / 8);
    p.drawEllipse(QPoint(cx, size * 28 / 100), dotR, dotR);

    p.end();
    return QIcon(pixmap);
}

// ============================================================================
// InfoButton
// ============================================================================

InfoButton::InfoButton(const QString& infoText, QWidget* parent)
    : QToolButton(parent)
    , m_infoText(infoText)
{
    static const QIcon s_icon = createInfoIcon(32);
    setIcon(s_icon);
    setIconSize(QSize(16, 16));
    setCursor(Qt::PointingHandCursor);
    setAutoRaise(true);
    setFixedSize(20, 20);
    setFocusPolicy(Qt::NoFocus);

    // Transparent background — the icon alone is the visual
    setStyleSheet(
        "QToolButton { background: transparent; border: none; padding: 0; }"
        "QToolButton:hover { background: rgba(0, 120, 212, 0.08); border-radius: 10px; }"
        "QToolButton:pressed { background: rgba(0, 120, 212, 0.15); border-radius: 10px; }"
    );

    connect(this, &QToolButton::clicked, this, &InfoButton::togglePopup);
}

// ============================================================================
// Popup
// ============================================================================

void InfoButton::togglePopup()
{
    // If popup already visible, close it
    if (m_popup) {
        m_popup->close();
        m_popup = nullptr;
        return;
    }

    // Create popup frame
    auto* popup = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setObjectName("sakInfoPopup");
    popup->setStyleSheet(
        "#sakInfoPopup {"
        "  background-color: rgba(255, 255, 255, 0.97);"
        "  border: 1px solid #cbd5e1;"
        "  border-radius: 8px;"
        "  padding: 0px;"
        "}"
    );

    auto* layout = new QVBoxLayout(popup);
    layout->setContentsMargins(14, 10, 14, 10);

    auto* label = new QLabel(m_infoText, popup);
    label->setWordWrap(true);
    label->setMaximumWidth(280);
    label->setStyleSheet(
        "QLabel {"
        "  color: #0f172a;"
        "  font-size: 9pt;"
        "  background: transparent;"
        "  border: none;"
        "  padding: 0px;"
        "}"
    );
    layout->addWidget(label);

    // Drop shadow
    auto* shadow = new QGraphicsDropShadowEffect(popup);
    shadow->setBlurRadius(16);
    shadow->setColor(QColor(0, 0, 0, 40));
    shadow->setOffset(0, 2);
    popup->setGraphicsEffect(shadow);

    popup->adjustSize();

    // Position: below the button, left-aligned
    QPoint globalPos = mapToGlobal(QPoint(0, height() + 4));
    QRect screenRect = screen()->availableGeometry();

    // Ensure popup stays on screen
    if (globalPos.y() + popup->height() > screenRect.bottom()) {
        globalPos.setY(mapToGlobal(QPoint(0, 0)).y() - popup->height() - 4);
    }
    if (globalPos.x() + popup->width() > screenRect.right()) {
        globalPos.setX(screenRect.right() - popup->width() - 4);
    }

    popup->move(globalPos);
    popup->show();
    m_popup = popup;
}

// ============================================================================
// Factory helper
// ============================================================================

QWidget* InfoButton::createInfoLabel(const QString& labelText,
                                     const QString& infoText,
                                     QWidget* parent)
{
    auto* container = new QWidget(parent);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto* label = new QLabel(labelText, container);
    layout->addWidget(label);

    auto* btn = new InfoButton(infoText, container);
    layout->addWidget(btn);

    layout->addStretch();
    return container;
}

} // namespace sak
