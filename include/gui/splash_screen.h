#pragma once

#include <QWidget>
#include <QPixmap>

namespace sak::ui {

class SplashScreen final : public QWidget {
    Q_OBJECT

public:
    explicit SplashScreen(const QPixmap& pixmap, QWidget* parent = nullptr);

    void showCentered();
    void finish();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QPixmap m_pixmap;
    QPixmap m_rounded_pixmap;
    int m_corner_radius{24};
    int m_shadow_radius{24};
    int m_shadow_offset{8};
};

} // namespace sak::ui
