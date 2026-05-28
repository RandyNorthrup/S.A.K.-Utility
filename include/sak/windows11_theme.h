// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows11_theme.h
/// @brief Windows 11 Fluent-style visual theme for the SAK Utility GUI

#pragma once

#include <QApplication>
#include <QString>

namespace sak::ui {

/// @brief Supported app color themes.
enum class AppThemeMode {
    Light,
    Dark
};

/// @brief Generate a complete Qt stylesheet mimicking Windows 11 Fluent Design
/// @return CSS stylesheet string covering all standard Qt widgets
QString windows11ThemeStyleSheet();

/// @brief Generate the application stylesheet for a specific theme mode.
/// @param mode Light or dark theme mode
/// @return CSS stylesheet string covering all standard Qt widgets
QString windows11ThemeStyleSheet(AppThemeMode mode);

/// @brief Apply the Windows 11 theme to the application (palette + stylesheet + font)
/// @param app Reference to the QApplication instance
/// @param mode Light or dark theme mode
void applyWindows11Theme(QApplication& app, AppThemeMode mode = AppThemeMode::Light);

/// @brief Read the currently applied application theme mode.
[[nodiscard]] AppThemeMode currentThemeMode(const QApplication& app);

/// @brief Install non-semantic visual polish helpers (shadows only).
/// @param app Reference to the QApplication instance
void installThemePolishHelper(QApplication& app);

}  // namespace sak::ui
