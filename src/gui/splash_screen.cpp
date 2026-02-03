#include "gui/splash_screen.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

namespace sak::ui {

namespace {

QPixmap createRoundedPixmap(const QPixmap& source, int radius) {
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

} // namespace

SplashScreen::SplashScreen(const QPixmap& pixmap, QWidget* parent)
    : QWidget(parent)
{
    const QSize max_size(640, 640);
    if (!pixmap.isNull() && (pixmap.width() > max_size.width() || pixmap.height() > max_size.height())) {
        m_pixmap = pixmap.scaled(max_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    } else {
        m_pixmap = pixmap;
    }

    m_rounded_pixmap = createRoundedPixmap(m_pixmap, m_corner_radius);

    setWindowFlags(Qt::FramelessWindowHint | Qt::SplashScreen | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);

    const QSize content_size = m_pixmap.isNull() ? QSize(600, 600) : m_pixmap.size();
    const int padding = m_shadow_radius + m_shadow_offset;
    resize(content_size.width() + padding * 2, content_size.height() + padding * 2);
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
    QColor shadow_color(0, 0, 0, 90);
    for (int i = 0; i < m_shadow_radius; ++i) {
        QColor c = shadow_color;
        c.setAlpha(shadow_color.alpha() * (m_shadow_radius - i) / m_shadow_radius);
        painter.setPen(c);
        painter.drawRoundedRect(content_rect.adjusted(-i, -i, i, i), m_corner_radius + i, m_corner_radius + i);
    }

    if (!m_rounded_pixmap.isNull()) {
        painter.drawPixmap(content_rect.topLeft(), m_rounded_pixmap.scaled(content_rect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
}

} // namespace sak::ui
