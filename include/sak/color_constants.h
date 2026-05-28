// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file color_constants.h
/// @brief Qt-free shared color and status tokens for GUI and report generation.

#pragma once

namespace sak::ui {

// Text hierarchy
constexpr auto kColorTextPrimary = "#0f172a";    ///< slate-900 -- titles, headings
constexpr auto kColorTextHeading = "#1e293b";    ///< slate-800 -- section headings
constexpr auto kColorTextBody = "#334155";       ///< slate-700 -- body text
constexpr auto kColorTextSecondary = "#475569";  ///< slate-600 -- descriptions
constexpr auto kColorTextMuted = "#64748b";      ///< slate-500 -- subtitles, notes
constexpr auto kColorTextDisabled = "#94a3b8";   ///< slate-400 -- disabled text

// Backgrounds
constexpr auto kColorBgPage = "#f3f5f9";            ///< Application background
constexpr auto kColorBgPageHover = "#eef2f7";       ///< Tab/item hover background
constexpr auto kColorBgSurface = "#f8fafc";         ///< slate-50 -- card/panel bg
constexpr auto kColorBgWhite = "#ffffff";           ///< Pure white
constexpr auto kColorBgInfoPanel = "#e0f2fe";       ///< sky-100 -- info/summary boxes
constexpr auto kColorBgWarningPanel = "#fef3c7";    ///< amber-100 -- warning boxes
constexpr auto kColorBgErrorPanel = "#fee2e2";      ///< red-100 -- error boxes
constexpr auto kColorBgUserBubbleText = "#dbeafe";  ///< blue-100 -- primary bubbles
constexpr auto kColorNoteBlue = "#dbeafe";          ///< note background blue
constexpr auto kColorNoteGreen = "#d1fae5";         ///< note background green
constexpr auto kColorNotePink = "#fce7f3";          ///< note background pink
constexpr auto kColorNoteYellow = "#fef9c3";        ///< note background yellow
constexpr auto kColorNoteGray = "#f3f4f6";          ///< note background gray

// Borders
constexpr auto kColorBorderDefault = "#cbd5e1";  ///< slate-300 -- default border
constexpr auto kColorBorderMuted = "#94a3b8";    ///< slate-400 -- muted border

// Dark theme text hierarchy
constexpr auto kColorDarkTextPrimary = "#f8fafc";
constexpr auto kColorDarkTextHeading = "#e2e8f0";
constexpr auto kColorDarkTextBody = "#cbd5e1";
constexpr auto kColorDarkTextSecondary = "#94a3b8";
constexpr auto kColorDarkTextMuted = "#64748b";
constexpr auto kColorDarkTextDisabled = "#475569";

// Dark theme surfaces
constexpr auto kColorDarkBgPage = "#0f172a";
constexpr auto kColorDarkBgChrome = "#111827";
constexpr auto kColorDarkBgSurface = "#1e293b";
constexpr auto kColorDarkBgPanel = "#172033";
constexpr auto kColorDarkBgInput = "#111827";
constexpr auto kColorDarkBgInputFocus = "#0b1220";
constexpr auto kColorDarkBgHover = "#243247";
constexpr auto kColorDarkBgPressed = "#29384f";
constexpr auto kColorDarkNoteBlue = "#1e3a5f";
constexpr auto kColorDarkNoteGreen = "#14532d";
constexpr auto kColorDarkNotePink = "#831843";
constexpr auto kColorDarkNoteYellow = "#713f12";
constexpr auto kColorDarkNoteGray = "#1f2937";

// Dark theme borders
constexpr auto kColorDarkBorderDefault = "#334155";
constexpr auto kColorDarkBorderMuted = "#475569";

// Primary (Blue accent)
constexpr auto kColorPrimary = "#3b82f6";         ///< blue-500 -- buttons, links
constexpr auto kColorPrimaryDark = "#2563eb";     ///< blue-600 -- gradient ends, borders
constexpr auto kColorPrimaryHover = "#4f8efc";    ///< blue-450 -- hover state
constexpr auto kColorPrimaryPressed = "#1d4ed8";  ///< blue-700 -- pressed state

// Success (Green)
constexpr auto kColorSuccess = "#16a34a";            ///< green-600 -- status OK
constexpr auto kColorSuccessBtnNormal = "#43a047";   ///< MUI green -- button normal
constexpr auto kColorSuccessBtnDark = "#2e7d32";     ///< MUI green -- button gradient
constexpr auto kColorSuccessBtnHover = "#66bb6a";    ///< MUI green -- button hover
constexpr auto kColorSuccessBtnPressed = "#1b5e20";  ///< MUI green -- button pressed

// Warning (Orange / Amber)
constexpr auto kColorWarning = "#ea580c";       ///< orange-600 -- caution, SMART warnings
constexpr auto kColorWarningBadge = "#f59e0b";  ///< amber-500 -- badge backgrounds
constexpr auto kColorWarningPhase = "#d97706";  ///< amber-600 -- phase indicators

// Error (Red)
constexpr auto kColorError = "#dc2626";             ///< red-600 -- failures, errors
constexpr auto kColorErrorText = "#b91c1c";         ///< red-700 -- text on error bg
constexpr auto kColorDangerBtnNormal = "#ef5350";   ///< MUI red -- button normal
constexpr auto kColorDangerBtnDark = "#c62828";     ///< MUI red -- button gradient
constexpr auto kColorDangerBtnHover = "#ff7043";    ///< MUI red -- button hover
constexpr auto kColorDangerBtnPressed = "#b71c1c";  ///< MUI red -- button pressed

// Pause (Amber)
constexpr auto kColorPauseBtnNormal = "#ffa726";   ///< MUI amber -- button normal
constexpr auto kColorPauseBtnDark = "#f57c00";     ///< MUI amber -- button gradient
constexpr auto kColorPauseBtnHover = "#ffb74d";    ///< MUI amber -- button hover
constexpr auto kColorPauseBtnPressed = "#e65100";  ///< MUI amber -- button pressed

// Accent
constexpr auto kColorAccentPurple = "#7c3aed";   ///< violet-600 -- special phases
constexpr auto kColorAccentEmerald = "#059669";  ///< emerald-600 -- download phases
constexpr auto kColorAccentWindows = "#0078d4";  ///< Windows accent blue
constexpr auto kColorDiscord = "#5865f2";        ///< Discord brand action
constexpr auto kColorSearchCurrentMatchHighlight = "#ffa500";
constexpr auto kColorSearchOtherMatchHighlight = "#ffff00";
constexpr auto kColorWifiSearchMatchHighlight = "#ffff96";

// Action button text and disabled state
constexpr auto kColorButtonTextOnTone = "#ffffff";
constexpr auto kColorButtonDisabledBackground = "rgba(203, 213, 225, 0.75)";
constexpr auto kColorButtonDisabledBorder = "rgba(203, 213, 225, 0.6)";

// Primary button gradient stops
constexpr auto kColorPrimaryButtonNormalTop = "rgba(79, 142, 252, 0.92)";
constexpr auto kColorPrimaryButtonNormalMiddle = "rgba(59, 130, 246, 0.90)";
constexpr auto kColorPrimaryButtonNormalBottom = "rgba(37, 99, 235, 0.88)";
constexpr auto kColorPrimaryButtonHoverTop = "rgba(96, 155, 253, 0.95)";
constexpr auto kColorPrimaryButtonHoverMiddle = "rgba(79, 142, 252, 0.93)";
constexpr auto kColorPrimaryButtonHoverBottom = "rgba(59, 130, 246, 0.90)";
constexpr auto kColorPrimaryButtonPressedTop = "rgba(37, 99, 235, 0.95)";
constexpr auto kColorPrimaryButtonPressedMiddle = "rgba(29, 78, 216, 0.93)";
constexpr auto kColorPrimaryButtonPressedBottom = "rgba(21, 61, 178, 0.92)";
constexpr auto kColorPrimaryButtonBorder = "rgba(29, 78, 216, 0.7)";

// Secondary button gradient stops
constexpr auto kColorSecondaryButtonNormalTop = "rgba(100, 116, 139, 0.92)";
constexpr auto kColorSecondaryButtonNormalMiddle = "rgba(71, 85, 105, 0.90)";
constexpr auto kColorSecondaryButtonNormalBottom = "rgba(51, 65, 85, 0.88)";
constexpr auto kColorSecondaryButtonHoverTop = "rgba(120, 137, 158, 0.95)";
constexpr auto kColorSecondaryButtonHoverMiddle = "rgba(100, 116, 139, 0.93)";
constexpr auto kColorSecondaryButtonHoverBottom = "rgba(71, 85, 105, 0.90)";
constexpr auto kColorSecondaryButtonPressedTop = "rgba(51, 65, 85, 0.95)";
constexpr auto kColorSecondaryButtonPressedMiddle = "rgba(30, 41, 59, 0.93)";
constexpr auto kColorSecondaryButtonPressedBottom = "rgba(15, 23, 42, 0.92)";
constexpr auto kColorSecondaryButtonBorder = "rgba(51, 65, 85, 0.7)";

// Success button gradient stops
constexpr auto kColorSuccessButtonNormalTop = "rgba(67, 160, 71, 0.92)";
constexpr auto kColorSuccessButtonNormalMiddle = "rgba(56, 142, 60, 0.90)";
constexpr auto kColorSuccessButtonNormalBottom = "rgba(46, 125, 50, 0.88)";
constexpr auto kColorSuccessButtonHoverTop = "rgba(102, 187, 106, 0.95)";
constexpr auto kColorSuccessButtonHoverMiddle = "rgba(67, 160, 71, 0.93)";
constexpr auto kColorSuccessButtonHoverBottom = "rgba(56, 142, 60, 0.90)";
constexpr auto kColorSuccessButtonPressedTop = "rgba(46, 125, 50, 0.95)";
constexpr auto kColorSuccessButtonPressedMiddle = "rgba(27, 94, 32, 0.93)";
constexpr auto kColorSuccessButtonPressedBottom = "rgba(20, 78, 25, 0.92)";
constexpr auto kColorSuccessButtonBorder = "rgba(46, 125, 50, 0.7)";

// Danger button gradient stops
constexpr auto kColorDangerButtonNormalTop = "rgba(239, 83, 80, 0.92)";
constexpr auto kColorDangerButtonNormalMiddle = "rgba(211, 47, 47, 0.90)";
constexpr auto kColorDangerButtonNormalBottom = "rgba(198, 40, 40, 0.88)";
constexpr auto kColorDangerButtonHoverTop = "rgba(255, 112, 67, 0.95)";
constexpr auto kColorDangerButtonHoverMiddle = "rgba(239, 83, 80, 0.93)";
constexpr auto kColorDangerButtonHoverBottom = "rgba(211, 47, 47, 0.90)";
constexpr auto kColorDangerButtonPressedTop = "rgba(198, 40, 40, 0.95)";
constexpr auto kColorDangerButtonPressedMiddle = "rgba(183, 28, 28, 0.93)";
constexpr auto kColorDangerButtonPressedBottom = "rgba(155, 20, 20, 0.92)";
constexpr auto kColorDangerButtonBorder = "rgba(198, 40, 40, 0.7)";

// Pause button gradient stops
constexpr auto kColorPauseButtonNormalTop = "rgba(255, 167, 38, 0.92)";
constexpr auto kColorPauseButtonNormalMiddle = "rgba(251, 140, 0, 0.90)";
constexpr auto kColorPauseButtonNormalBottom = "rgba(245, 124, 0, 0.88)";
constexpr auto kColorPauseButtonHoverTop = "rgba(255, 183, 77, 0.95)";
constexpr auto kColorPauseButtonHoverMiddle = "rgba(255, 167, 38, 0.93)";
constexpr auto kColorPauseButtonHoverBottom = "rgba(251, 140, 0, 0.90)";
constexpr auto kColorPauseButtonPressedTop = "rgba(245, 124, 0, 0.95)";
constexpr auto kColorPauseButtonPressedMiddle = "rgba(230, 81, 0, 0.93)";
constexpr auto kColorPauseButtonPressedBottom = "rgba(210, 70, 0, 0.92)";
constexpr auto kColorPauseButtonBorder = "rgba(245, 124, 0, 0.7)";

// Discord button gradient stops
constexpr auto kColorDiscordButtonNormalTop = "rgba(108, 117, 252, 0.92)";
constexpr auto kColorDiscordButtonNormalMiddle = "rgba(88, 101, 242, 0.90)";
constexpr auto kColorDiscordButtonNormalBottom = "rgba(71, 82, 196, 0.88)";
constexpr auto kColorDiscordButtonHoverTop = "rgba(128, 137, 253, 0.95)";
constexpr auto kColorDiscordButtonHoverMiddle = "rgba(109, 120, 247, 0.93)";
constexpr auto kColorDiscordButtonHoverBottom = "rgba(88, 101, 242, 0.90)";
constexpr auto kColorDiscordButtonPressedTop = "rgba(71, 82, 196, 0.95)";
constexpr auto kColorDiscordButtonPressedMiddle = "rgba(57, 66, 164, 0.93)";
constexpr auto kColorDiscordButtonPressedBottom = "rgba(45, 52, 140, 0.92)";
constexpr auto kColorDiscordButtonBorder = "rgba(71, 82, 196, 0.7)";

// Semantic status colors
constexpr auto kStatusColorIdle = "#64748b";     ///< slate-500 -- idle/pending/default
constexpr auto kStatusColorRunning = "#2563eb";  ///< blue-600 -- in-progress/active
constexpr auto kStatusColorSuccess = "#16a34a";  ///< green-600 -- completed/passed/ready
constexpr auto kStatusColorWarning = "#d97706";  ///< amber-600 -- caution/slow/degraded
constexpr auto kStatusColorError = "#dc2626";    ///< red-600 -- failed/error/critical

// Screen-reader and colorblind status text prefixes
constexpr auto kStatusPrefixIdle = "\u25CB ";     ///< idle/pending
constexpr auto kStatusPrefixRunning = "\u23F3 ";  ///< in-progress
constexpr auto kStatusPrefixSuccess = "\u2714 ";  ///< completed/passed
constexpr auto kStatusPrefixWarning = "\u26A0 ";  ///< caution
constexpr auto kStatusPrefixError = "\u2718 ";    ///< failed/error

}  // namespace sak::ui
