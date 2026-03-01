// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows11_theme.h
/// @brief Windows 11 Fluent-style visual theme for the SAK Utility GUI

#pragma once

#include <QApplication>
#include <QString>

namespace sak::ui {

/// @brief Generate a complete Qt stylesheet mimicking Windows 11 Fluent Design
/// @return CSS stylesheet string covering all standard Qt widgets
QString windows11ThemeStyleSheet();

/// @brief Apply the Windows 11 theme to the application (palette + stylesheet + font)
/// @param app Reference to the QApplication instance
void applyWindows11Theme(QApplication& app);

/// @brief Install an event filter that positions tooltips near the cursor
/// @param app Reference to the QApplication instance
void installTooltipHelper(QApplication& app);

} // namespace sak::ui
