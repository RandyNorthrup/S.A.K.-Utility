// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file elevation_banner.h
/// @brief Non-intrusive info banner for panels that contain elevated operations
///
/// Phase 4 UX Polish: shows a subtle informational strip at the top of
/// Tier 2 / Mixed-tier panels so users know ahead of time that some
/// operations may trigger a UAC prompt.

#include "sak/elevation_manager.h"
#include "sak/style_constants.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QWidget>

namespace sak {

/// @brief Create an elevation info banner for panels with elevated operations
///
/// Returns nullptr if the process is already running as admin (no banner
/// needed). Otherwise returns a styled QFrame with an info message.
///
/// @param parent  Parent widget (typically the panel)
/// @return Banner widget, or nullptr if already elevated
[[nodiscard]] inline QFrame* createElevationBanner(QWidget* parent) {
    if (ElevationManager::isElevated()) {
        return nullptr;
    }

    auto* banner = new QFrame(parent);
    banner->setObjectName(QStringLiteral("elevationBanner"));

    banner->setStyleSheet(
        QStringLiteral("QFrame#elevationBanner {"
                       "  background-color: %1;"
                       "  border: 1px solid %2;"
                       "  border-radius: 6px;"
                       "  padding: %3px %4px;"
                       "}")
            .arg(QLatin1String(ui::kColorBgInfoPanel), QLatin1String(ui::kColorPrimary))
            .arg(ui::kSpacingTight)
            .arg(ui::kSpacingMedium));

    auto* layout = new QHBoxLayout(banner);
    layout->setContentsMargins(
        ui::kSpacingMedium, ui::kSpacingTight, ui::kSpacingMedium, ui::kSpacingTight);
    layout->setSpacing(ui::kSpacingSmall);

    auto* icon_label = new QLabel(banner);
    icon_label->setPixmap(
        QIcon(QStringLiteral(":/icons/icons/icons8-keyhole-shield.svg")).pixmap(16, 16));
    icon_label->setFixedWidth(20);
    layout->addWidget(icon_label);

    auto* text_label = new QLabel(QObject::tr("Some operations on this tab require administrator "
                                              "privileges. You will be prompted when needed."),
                                  banner);
    text_label->setWordWrap(true);
    text_label->setStyleSheet(QStringLiteral("color: %1; font-size: %2pt;")
                                  .arg(QLatin1String(ui::kColorTextBody))
                                  .arg(ui::kFontSizeBody));
    layout->addWidget(text_label, 1);

    return banner;
}

}  // namespace sak
