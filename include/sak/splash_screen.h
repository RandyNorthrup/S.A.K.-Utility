// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QPixmap>
#include <QWidget>

namespace sak::ui {

/// @brief Animated splash screen displayed during application startup
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

}  // namespace sak::ui
