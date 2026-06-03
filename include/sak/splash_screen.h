// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QPixmap>
#include <QWidget>

namespace sak::ui {

inline constexpr int kSplashSizePx = 300;
inline constexpr int kSplashCornerRadiusPx = 24;
inline constexpr int kSplashShadowRadiusPx = 0;
inline constexpr int kSplashShadowOffsetPx = 0;

/// @brief Fixed-size splash screen displayed during application startup
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
    int m_corner_radius{kSplashCornerRadiusPx};
    int m_shadow_radius{kSplashShadowRadiusPx};
    int m_shadow_offset{kSplashShadowOffsetPx};
};

}  // namespace sak::ui
