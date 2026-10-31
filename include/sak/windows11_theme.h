// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows11_theme.h
/// @brief Windows 11 Fluent-style visual theme for the SAK Utility GUI

#pragma once

#include <QApplication>
#include <QString>
#include <Qt>

namespace sak::ui {

/// @brief Supported app color themes.
enum class AppThemeMode {
    Light,
    Dark
};

/// @brief The one saved theme setting (ConfigManager "ui/theme"). System follows the Windows app
///        theme while the app runs.
enum class ThemePreference {
    System,
    Light,
    Dark
};

/// @brief "system" / "light" / "dark" to a preference; anything else is System. Pure.
[[nodiscard]] ThemePreference themePreferenceFromSetting(const QString& setting);

/// @brief A preference as ConfigManager stores it. Pure.
[[nodiscard]] QString themePreferenceSetting(ThemePreference preference);

/// @brief The theme to show for @p preference while Windows' app theme is @p windows_scheme.
///        Windows reports Light or Dark; an unknown scheme (no platform theme) is Light. Pure.
[[nodiscard]] AppThemeMode themeModeFor(ThemePreference preference, Qt::ColorScheme windows_scheme);

/// @brief The saved preference.
[[nodiscard]] ThemePreference savedThemePreference();

/// @brief At startup: apply the saved preference and, while it is System, keep following the
///        Windows app theme (QStyleHints::colorSchemeChanged). Needs the data folder decided.
void initializeAppTheme(QApplication& app);

/// @brief Save @p preference and apply it now. Light and Dark are also requested from Qt, so the
///        window frame (title bar) matches; System hands the frame back to Windows.
void setThemePreference(QApplication& app, ThemePreference preference);

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

}  // namespace sak::ui
