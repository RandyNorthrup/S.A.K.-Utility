// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/info_button.h"

#include "sak/layout_constants.h"
#include "sak/style_constants.h"

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

namespace {

constexpr int kInfoIconInsetPx = 1;
constexpr int kInfoIconInsetTotalPx = 2;
constexpr int kInfoGlyphMinBarWidthPx = 2;
constexpr int kInfoGlyphBarWidthDivisor = 6;
constexpr int kInfoGlyphBarTopPercent = 40;
constexpr int kInfoGlyphBarBottomPercent = 78;
constexpr int kInfoGlyphDotTopPercent = 28;
constexpr int kInfoGlyphDotRadiusDivisor = 8;
constexpr int kInfoGlyphCornerRadiusPx = 1;
constexpr qreal kInfoPopupShadowBlurRadius = 16;
constexpr int kInfoPopupShadowAlpha = 40;
constexpr qreal kInfoPopupShadowYOffset = 2;
constexpr int kInfoPopupScreenPaddingPx = 4;

}  // namespace

// ============================================================================
// Icon rendering
// ============================================================================

QIcon InfoButton::createInfoIcon(int size) {
    Q_ASSERT(size >= 0);
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing, true);

    // Filled circle -- Windows 11 accent blue
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QString::fromLatin1(ui::kColorAccentWindows)));
    p.drawEllipse(kInfoIconInsetPx,
                  kInfoIconInsetPx,
                  size - kInfoIconInsetTotalPx,
                  size - kInfoIconInsetTotalPx);

    // White "i" body (vertical bar)
    p.setPen(Qt::NoPen);
    p.setBrush(Qt::white);
    const int cx = size / kInfoIconInsetTotalPx;
    const int barW = qMax(kInfoGlyphMinBarWidthPx, size / kInfoGlyphBarWidthDivisor);
    const int barTop = size * kInfoGlyphBarTopPercent / kPercentMax;
    const int barBot = size * kInfoGlyphBarBottomPercent / kPercentMax;
    p.drawRoundedRect(cx - barW,
                      barTop,
                      barW * kInfoIconInsetTotalPx,
                      barBot - barTop,
                      kInfoGlyphCornerRadiusPx,
                      kInfoGlyphCornerRadiusPx);

    // White "i" dot
    const int dotR = qMax(kInfoIconInsetPx, size / kInfoGlyphDotRadiusDivisor);
    p.drawEllipse(QPoint(cx, size * kInfoGlyphDotTopPercent / kPercentMax), dotR, dotR);

    p.end();
    return QIcon(pixmap);
}

// ============================================================================
// InfoButton
// ============================================================================

InfoButton::InfoButton(const QString& infoText, QWidget* parent)
    : QToolButton(parent), m_infoText(infoText) {
    static const QIcon s_icon(QStringLiteral(":/icons/icons/icons8-settings_help.svg"));
    setIcon(s_icon);
    setIconSize(QSize(ui::kUiIconSmall, ui::kUiIconSmall));
    setCursor(Qt::PointingHandCursor);
    setAutoRaise(true);
    setFixedSize(sak::kInfoButtonSize, sak::kInfoButtonSize);
    setFocusPolicy(Qt::TabFocus);
    setAccessibleName(QStringLiteral("Info"));
    setAccessibleDescription(infoText);
    setToolTip(QStringLiteral("Show more info"));

    // Transparent background -- the icon alone is the visual
    setStyleSheet(ui::infoButtonStyle());

    connect(this, &QToolButton::clicked, this, &InfoButton::togglePopup);
}

// ============================================================================
// Popup
// ============================================================================

void InfoButton::togglePopup() {
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
    popup->setStyleSheet(sak::ui::infoPopupFrameStyle());

    auto* layout = new QVBoxLayout(popup);
    layout->setContentsMargins(sak::ui::kUiIconCompact,
                               sak::ui::kSpacingDefault,
                               sak::ui::kUiIconCompact,
                               sak::ui::kSpacingDefault);

    auto* label = new QLabel(m_infoText, popup);
    label->setWordWrap(true);
    label->setMaximumWidth(sak::kTooltipMaxW);
    label->setStyleSheet(sak::ui::infoPopupLabelStyle());
    layout->addWidget(label);

    // Drop shadow
    auto* shadow = new QGraphicsDropShadowEffect(popup);
    shadow->setBlurRadius(kInfoPopupShadowBlurRadius);
    QColor shadow_color(Qt::black);
    shadow_color.setAlpha(kInfoPopupShadowAlpha);
    shadow->setColor(shadow_color);
    shadow->setOffset(0, kInfoPopupShadowYOffset);
    popup->setGraphicsEffect(shadow);

    popup->adjustSize();

    // Position: below the button, left-aligned
    QPoint globalPos = mapToGlobal(QPoint(0, height() + kInfoPopupScreenPaddingPx));
    QRect screenRect = screen()->availableGeometry();

    // Ensure popup stays on screen
    if (globalPos.y() + popup->height() > screenRect.bottom()) {
        globalPos.setY(mapToGlobal(QPoint(0, 0)).y() - popup->height() - kInfoPopupScreenPaddingPx);
    }
    if (globalPos.x() + popup->width() > screenRect.right()) {
        globalPos.setX(screenRect.right() - popup->width() - kInfoPopupScreenPaddingPx);
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
                                     QWidget* parent) {
    auto* container = new QWidget(parent);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    layout->setSpacing(sak::ui::kSpacingTight);

    auto* label = new QLabel(labelText, container);
    layout->addWidget(label);

    auto* btn = new InfoButton(infoText, container);
    layout->addWidget(btn);

    layout->addStretch();
    return container;
}

}  // namespace sak
