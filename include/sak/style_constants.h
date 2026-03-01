// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file style_constants.h
/// @brief Centralized design tokens for the SAK Utility GUI
///
/// All panels should reference these constants instead of hardcoding
/// color hex values, margin/spacing values, and font sizes.
/// This enables maintainable theming and eventual dark-mode support.

#pragma once

namespace sak::ui {

// ── Color Palette (Tailwind CSS naming) ─────────────────────────────────────

// Text hierarchy
constexpr auto kColorTextPrimary   = "#0f172a";  ///< slate-900 — titles, headings
constexpr auto kColorTextHeading   = "#1e293b";  ///< slate-800 — section headings
constexpr auto kColorTextBody      = "#334155";  ///< slate-700 — body text
constexpr auto kColorTextSecondary = "#475569";  ///< slate-600 — descriptions
constexpr auto kColorTextMuted     = "#64748b";  ///< slate-500 — subtitles, notes
constexpr auto kColorTextDisabled  = "#94a3b8";  ///< slate-400 — disabled text

// Backgrounds
constexpr auto kColorBgPage         = "#f3f5f9";  ///< Application background
constexpr auto kColorBgPageHover    = "#eef2f7";  ///< Tab/item hover background
constexpr auto kColorBgSurface      = "#f8fafc";  ///< slate-50 — card/panel bg
constexpr auto kColorBgWhite        = "#ffffff";  ///< Pure white
constexpr auto kColorBgInfoPanel    = "#e0f2fe";  ///< sky-100 — info/summary boxes
constexpr auto kColorBgWarningPanel = "#fef3c7";  ///< amber-100 — warning boxes
constexpr auto kColorBgErrorPanel   = "#fee2e2";  ///< red-100 — error boxes

// Borders
constexpr auto kColorBorderDefault = "#cbd5e1";  ///< slate-300 — default border
constexpr auto kColorBorderMuted   = "#94a3b8";  ///< slate-400 — muted border

// Primary (Blue accent)
constexpr auto kColorPrimary        = "#3b82f6";  ///< blue-500 — buttons, links
constexpr auto kColorPrimaryDark    = "#2563eb";  ///< blue-600 — gradient ends, borders
constexpr auto kColorPrimaryHover   = "#4f8efc";  ///< blue-450 — hover state
constexpr auto kColorPrimaryPressed = "#1d4ed8";  ///< blue-700 — pressed state

// Success (Green)
constexpr auto kColorSuccess           = "#16a34a";  ///< green-600 — status OK
constexpr auto kColorSuccessBtnNormal  = "#43a047";  ///< MUI green — button normal
constexpr auto kColorSuccessBtnDark    = "#2e7d32";  ///< MUI green — button gradient
constexpr auto kColorSuccessBtnHover   = "#66bb6a";  ///< MUI green — button hover
constexpr auto kColorSuccessBtnPressed = "#1b5e20";  ///< MUI green — button pressed

// Warning (Orange / Amber)
constexpr auto kColorWarning      = "#ea580c";  ///< orange-600 — caution, SMART warnings
constexpr auto kColorWarningBadge = "#f59e0b";  ///< amber-500 — badge backgrounds
constexpr auto kColorWarningPhase = "#d97706";  ///< amber-600 — phase indicators

// Error (Red)
constexpr auto kColorError           = "#dc2626";  ///< red-600 — failures, errors
constexpr auto kColorErrorText       = "#b91c1c";  ///< red-700 — text on error bg
constexpr auto kColorDangerBtnNormal = "#ef5350";  ///< MUI red — button normal
constexpr auto kColorDangerBtnDark   = "#c62828";  ///< MUI red — button gradient
constexpr auto kColorDangerBtnHover  = "#ff7043";  ///< MUI red — button hover
constexpr auto kColorDangerBtnPressed= "#b71c1c";  ///< MUI red — button pressed

// Accent
constexpr auto kColorAccentPurple  = "#7c3aed";  ///< violet-600 — special phases
constexpr auto kColorAccentEmerald = "#059669";  ///< emerald-600 — download phases

// ── Spacing & Margins ───────────────────────────────────────────────────────

constexpr int kMarginNone   = 0;   ///< No margin (flush layouts)
constexpr int kMarginTight  = 6;   ///< Tight margin (compact widgets)
constexpr int kMarginSmall  = 8;   ///< Small margin (compact panels)
constexpr int kMarginMedium = 12;  ///< Standard panel margin (most panels)
constexpr int kMarginLarge  = 16;  ///< Large margin (dialogs, roomy layouts)
constexpr int kMarginXLarge = 20;  ///< Extra-large margin (wide panels)

constexpr int kSpacingTight    = 4;   ///< Tight spacing (icon-text pairs)
constexpr int kSpacingSmall    = 6;   ///< Small spacing (dense groups)
constexpr int kSpacingMedium   = 8;   ///< Standard widget spacing
constexpr int kSpacingDefault  = 10;  ///< Default layout spacing
constexpr int kSpacingLarge    = 12;  ///< Section/dialog spacing
constexpr int kSpacingXLarge   = 15;  ///< Extra-large spacing

// ── Status Indicator Tokens ──────────────────────────────────────────────────

/// Semantic status colors — use these whenever displaying state/phase to the user
constexpr auto kStatusColorIdle     = "#64748b";  ///< slate-500 — idle/pending/default
constexpr auto kStatusColorRunning  = "#2563eb";  ///< blue-600 — in-progress/active
constexpr auto kStatusColorSuccess  = "#16a34a";  ///< green-600 — completed/passed/ready
constexpr auto kStatusColorWarning  = "#d97706";  ///< amber-600 — caution/slow/degraded
constexpr auto kStatusColorError    = "#dc2626";  ///< red-600 — failed/error/critical

/// Emoji prefixes for accessible status text (screen reader + colorblind fallback)
constexpr auto kStatusPrefixIdle    = "\u25CB ";  ///< ○ — idle/pending
constexpr auto kStatusPrefixRunning = "\u23F3 ";  ///< ⏳ — in-progress
constexpr auto kStatusPrefixSuccess = "\u2714 ";  ///< ✔ — completed/passed
constexpr auto kStatusPrefixWarning = "\u26A0 ";  ///< ⚠ — caution
constexpr auto kStatusPrefixError   = "\u2718 ";  ///< ✘ — failed/error

// ── Font Sizes (points) ─────────────────────────────────────────────────────

constexpr int kFontSizeTitle   = 18;  ///< Application title (about dialog)
constexpr int kFontSizeSection = 13;  ///< Panel/section headers
constexpr int kFontSizeStatus  = 11;  ///< Status labels
constexpr int kFontSizeBody    = 10;  ///< Base body text
constexpr int kFontSizeNote    = 9;   ///< Notes, hints, descriptions
constexpr int kFontSizeSmall   = 8;   ///< Sub-labels, fine print

// ── Button Stylesheets ──────────────────────────────────────────────────────

/// Primary action button — blue gradient with hover/pressed/disabled states.
/// Apply via `button->setStyleSheet(sak::ui::kPrimaryButtonStyle);`
constexpr auto kPrimaryButtonStyle =
    "QPushButton {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 #3b82f6, stop:1 #2563eb);"
    "  color: white; font-weight: 600;"
    "  padding: 8px 20px; border-radius: 6px; border: none;"
    "}"
    "QPushButton:hover {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 #4f8efc, stop:1 #3b82f6);"
    "}"
    "QPushButton:pressed {"
    "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
    "    stop:0 #2563eb, stop:1 #1d4ed8);"
    "}"
    "QPushButton:disabled {"
    "  background-color: #cbd5e1; color: #64748b;"
    "}";

} // namespace sak::ui
