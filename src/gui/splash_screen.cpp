// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/splash_screen.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

namespace sak::ui {

namespace {

constexpr int kSplashPaddingSides = 2;
constexpr int kSplashShadowRed = 0;
constexpr int kSplashShadowGreen = 0;
constexpr int kSplashShadowBlue = 0;
constexpr int kSplashShadowAlpha = 90;
constexpr int kCenterCropDivisor = 2;

QPixmap createRoundedPixmap(const QPixmap& source, int radius) {
    Q_ASSERT(radius >= 0);
    if (source.isNull()) {
        return {};
    }

    QPixmap rounded(source.size());
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath clip;
    clip.addRoundedRect(source.rect(), radius, radius);
    painter.setClipPath(clip);
    painter.drawPixmap(0, 0, source);
    painter.end();

    return rounded;
}

QPixmap scaledSplashPixmap(const QPixmap& source) {
    Q_ASSERT(!source.isNull());
    if (source.isNull()) {
        return {};
    }

    const QSize target_size(kSplashSizePx, kSplashSizePx);
    if (source.size() == target_size) {
        return source;
    }

    const QPixmap scaled =
        source.scaled(target_size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    if (scaled.size() == target_size) {
        return scaled;
    }

    const QPoint crop_top_left((scaled.width() - target_size.width()) / kCenterCropDivisor,
                               (scaled.height() - target_size.height()) / kCenterCropDivisor);
    return scaled.copy(QRect(crop_top_left, target_size));
}

}  // namespace

SplashScreen::SplashScreen(const QPixmap& pixmap, QWidget* parent) : QWidget(parent) {
    Q_ASSERT(!pixmap.isNull());
    m_pixmap = scaledSplashPixmap(pixmap);

    m_rounded_pixmap = createRoundedPixmap(m_pixmap, m_corner_radius);

    setWindowFlags(Qt::FramelessWindowHint | Qt::SplashScreen | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    const int padding = m_shadow_radius + m_shadow_offset;
    const QSize content_size(kSplashSizePx - padding * kSplashPaddingSides,
                             kSplashSizePx - padding * kSplashPaddingSides);
    resize(content_size.width() + padding * kSplashPaddingSides,
           content_size.height() + padding * kSplashPaddingSides);
    setFixedSize(kSplashSizePx, kSplashSizePx);
}

void SplashScreen::showCentered() {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        show();
        return;
    }

    const QRect screen_geometry = screen->availableGeometry();
    move(screen_geometry.center() - rect().center());
    show();
}

void SplashScreen::finish() {
    close();
}

void SplashScreen::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int padding = m_shadow_radius + m_shadow_offset;
    QRect content_rect = rect().adjusted(padding, padding, -padding, -padding);

    QPainterPath shadow_path;
    shadow_path.addRoundedRect(content_rect, m_corner_radius, m_corner_radius);
    painter.fillPath(shadow_path, QColor(0, 0, 0, 0));

    // Drop shadow
    QPainterPath shadow;
    shadow.addRoundedRect(content_rect, m_corner_radius, m_corner_radius);
    QColor shadow_color(
        kSplashShadowRed, kSplashShadowGreen, kSplashShadowBlue, kSplashShadowAlpha);
    for (int i = 0; i < m_shadow_radius; ++i) {
        QColor c = shadow_color;
        c.setAlpha(shadow_color.alpha() * (m_shadow_radius - i) / m_shadow_radius);
        painter.setPen(c);
        painter.drawRoundedRect(content_rect.adjusted(-i, -i, i, i),
                                m_corner_radius + i,
                                m_corner_radius + i);
    }

    if (!m_rounded_pixmap.isNull()) {
        painter.drawPixmap(content_rect.topLeft(),
                           m_rounded_pixmap.scaled(
                               content_rect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

}  // namespace sak::ui
