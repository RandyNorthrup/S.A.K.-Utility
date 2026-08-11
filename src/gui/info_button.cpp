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
    const int bar_w = qMax(kInfoGlyphMinBarWidthPx, size / kInfoGlyphBarWidthDivisor);
    const int bar_top = size * kInfoGlyphBarTopPercent / kPercentMax;
    const int bar_bot = size * kInfoGlyphBarBottomPercent / kPercentMax;
    p.drawRoundedRect(cx - bar_w,
                      bar_top,
                      bar_w * kInfoIconInsetTotalPx,
                      bar_bot - bar_top,
                      kInfoGlyphCornerRadiusPx,
                      kInfoGlyphCornerRadiusPx);

    // White "i" dot
    const int dot_r = qMax(kInfoIconInsetPx, size / kInfoGlyphDotRadiusDivisor);
    p.drawEllipse(QPoint(cx, size * kInfoGlyphDotTopPercent / kPercentMax), dot_r, dot_r);

    p.end();
    return QIcon(pixmap);
}

// ============================================================================
// InfoButton
// ============================================================================

InfoButton::InfoButton(const QString& info_text, QWidget* parent)
    : QToolButton(parent), m_infoText(info_text) {
    static const QIcon kSIcon(QStringLiteral(":/icons/icons/icons8-settings_help.svg"));
    setIcon(kSIcon);
    setIconSize(QSize(ui::kUiIconSmall, ui::kUiIconSmall));
    setCursor(Qt::PointingHandCursor);
    setAutoRaise(true);
    setFixedSize(sak::kInfoButtonSize, sak::kInfoButtonSize);
    setFocusPolicy(Qt::TabFocus);
    setAccessibleName(QStringLiteral("Info"));
    setAccessibleDescription(info_text);
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
    QPoint global_pos = mapToGlobal(QPoint(0, height() + kInfoPopupScreenPaddingPx));
    const QRect screen_rect = screen()->availableGeometry();

    // Ensure popup stays on screen
    if (global_pos.y() + popup->height() > screen_rect.bottom()) {
        global_pos.setY(mapToGlobal(QPoint(0, 0)).y() - popup->height() -
                        kInfoPopupScreenPaddingPx);
    }
    if (global_pos.x() + popup->width() > screen_rect.right()) {
        global_pos.setX(screen_rect.right() - popup->width() - kInfoPopupScreenPaddingPx);
    }

    popup->move(global_pos);
    popup->show();
    m_popup = popup;
}

// ============================================================================
// Factory helper
// ============================================================================

QWidget* InfoButton::createInfoLabel(const QString& label_text,
                                     const QString& info_text,
                                     QWidget* parent) {
    auto* container = new QWidget(parent);
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    layout->setSpacing(sak::ui::kSpacingTight);

    auto* label = new QLabel(label_text, container);
    layout->addWidget(label);

    auto* btn = new InfoButton(info_text, container);
    layout->addWidget(btn);

    layout->addStretch();
    return container;
}

}  // namespace sak
