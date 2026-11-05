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
// Muted text was slate-500 (#64748b), 4.36:1 on the page; this is the least darkening, same hue,
// that reaches WCAG AA 4.5:1 on the page and the window background (4.68 and 4.55).
constexpr auto kColorTextMuted = "#606f85";     ///< slate-500, darkened -- subtitles, notes
constexpr auto kColorTextDisabled = "#94a3b8";  ///< slate-400 -- disabled text

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
constexpr auto kColorDarkBgWarningPanel = "#422006";  ///< amber-950 -- warning rows/boxes
constexpr auto kColorDarkBgErrorPanel = "#450a0a";    ///< red-950 -- error rows/boxes

// Dark theme borders
constexpr auto kColorDarkBorderDefault = "#334155";
constexpr auto kColorDarkBorderMuted = "#475569";

// Dark theme status text. The light status colours below were darkened to reach WCAG AA on the
// light page, which would take them under AA on the dark surfaces; these are the dark theme's own
// (the old values, lightened the least that reaches 4.5:1 on every dark surface).
constexpr auto kColorDarkStatusSuccess = "#16a54b";
constexpr auto kColorDarkStatusWarning = "#d97706";
constexpr auto kColorDarkWarning = "#f36013";
constexpr auto kColorDarkStatusError = "#e66767";

// Primary (Blue accent)
constexpr auto kColorPrimary = "#3b82f6";         ///< blue-500 -- buttons, links
constexpr auto kColorPrimaryDark = "#2563eb";     ///< blue-600 -- gradient ends, borders
constexpr auto kColorPrimaryHover = "#4f8efc";    ///< blue-450 -- hover state
constexpr auto kColorPrimaryPressed = "#1d4ed8";  ///< blue-700 -- pressed state
// The selected tab's gradient under white text. It ran from blue-500 to blue-600, 3.78:1 at the
// middle; these are both stops darkened the least, same hue, that gives 4.5:1 there.
constexpr auto kColorSelectedTabTop = "#1c6ef5";
constexpr auto kColorSelectedTabBottom = "#1452db";

// Success (Green)
// Status text colours below were darkened the least, same hue, to reach WCAG AA 4.5:1 on the light
// page and window background: success #16a34a (3.02:1), warning #ea580c (3.26:1), amber #d97706
// (2.92:1), error #dc2626 (4.42:1). The dark theme uses kColorDark* instead (themedColor,
// cssColor).
constexpr auto kColorSuccess = "#117f3a";            ///< green-600, darkened -- status OK
constexpr auto kColorSuccessBtnNormal = "#43a047";   ///< MUI green -- button normal
constexpr auto kColorSuccessBtnDark = "#2e7d32";     ///< MUI green -- button gradient
constexpr auto kColorSuccessBtnHover = "#66bb6a";    ///< MUI green -- button hover
constexpr auto kColorSuccessBtnPressed = "#1b5e20";  ///< MUI green -- button pressed

// Warning (Orange / Amber)
constexpr auto kColorWarning = "#bf480a";       ///< orange-600, darkened -- caution, SMART
constexpr auto kColorWarningBadge = "#f59e0b";  ///< amber-500 -- badge backgrounds
constexpr auto kColorWarningPhase = "#d97706";  ///< amber-600 -- phase indicators

// Error (Red)
constexpr auto kColorError = "#d62323";             ///< red-600, darkened -- failures
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

// Action button text and disabled state. The disabled fill and border are translucent slate so
// one value reads as a muted control on the light page and on the dark one.
constexpr auto kColorButtonTextOnTone = "#ffffff";
constexpr auto kColorButtonDisabledBackground = "rgba(148, 163, 184, 0.35)";
constexpr auto kColorButtonDisabledBorder = "rgba(148, 163, 184, 0.45)";

// Button gradient stops. White label text sits on the middle stop; each tone's normal and hover
// stops were darkened together, the least amount and keeping their hue, until that middle stop
// over a white dialog gives WCAG AA 4.5:1 (primary was 3.21:1, hover 2.91:1).

// Primary button gradient stops
constexpr auto kColorPrimaryButtonNormalTop = "rgba(22, 105, 251, 0.92)";
constexpr auto kColorPrimaryButtonNormalMiddle = "rgba(11, 96, 236, 0.90)";
constexpr auto kColorPrimaryButtonNormalBottom = "rgba(18, 74, 196, 0.88)";
constexpr auto kColorPrimaryButtonHoverTop = "rgba(28, 112, 252, 0.95)";
constexpr auto kColorPrimaryButtonHoverMiddle = "rgba(11, 98, 251, 0.93)";
constexpr auto kColorPrimaryButtonHoverBottom = "rgba(10, 92, 225, 0.90)";
constexpr auto kColorPrimaryButtonPressedTop = "rgba(37, 99, 235, 0.95)";
constexpr auto kColorPrimaryButtonPressedMiddle = "rgba(29, 78, 216, 0.93)";
constexpr auto kColorPrimaryButtonPressedBottom = "rgba(21, 61, 178, 0.92)";
constexpr auto kColorPrimaryButtonBorder = "rgba(29, 78, 216, 0.7)";

// Secondary button gradient stops
constexpr auto kColorSecondaryButtonNormalTop = "rgba(100, 116, 139, 0.92)";
constexpr auto kColorSecondaryButtonNormalMiddle = "rgba(71, 85, 105, 0.90)";
constexpr auto kColorSecondaryButtonNormalBottom = "rgba(51, 65, 85, 0.88)";
constexpr auto kColorSecondaryButtonHoverTop = "rgba(112, 130, 152, 0.95)";
constexpr auto kColorSecondaryButtonHoverMiddle = "rgba(94, 110, 131, 0.93)";
constexpr auto kColorSecondaryButtonHoverBottom = "rgba(66, 79, 97, 0.90)";
constexpr auto kColorSecondaryButtonPressedTop = "rgba(51, 65, 85, 0.95)";
constexpr auto kColorSecondaryButtonPressedMiddle = "rgba(30, 41, 59, 0.93)";
constexpr auto kColorSecondaryButtonPressedBottom = "rgba(15, 23, 42, 0.92)";
constexpr auto kColorSecondaryButtonBorder = "rgba(51, 65, 85, 0.7)";

// Success button gradient stops
constexpr auto kColorSuccessButtonNormalTop = "rgba(57, 137, 61, 0.92)";
constexpr auto kColorSuccessButtonNormalMiddle = "rgba(47, 118, 50, 0.90)";
constexpr auto kColorSuccessButtonNormalBottom = "rgba(37, 101, 40, 0.88)";
constexpr auto kColorSuccessButtonHoverTop = "rgba(73, 164, 77, 0.95)";
constexpr auto kColorSuccessButtonHoverMiddle = "rgba(52, 123, 55, 0.93)";
constexpr auto kColorSuccessButtonHoverBottom = "rgba(41, 105, 44, 0.90)";
constexpr auto kColorSuccessButtonPressedTop = "rgba(46, 125, 50, 0.95)";
constexpr auto kColorSuccessButtonPressedMiddle = "rgba(27, 94, 32, 0.93)";
constexpr auto kColorSuccessButtonPressedBottom = "rgba(20, 78, 25, 0.92)";
constexpr auto kColorSuccessButtonBorder = "rgba(46, 125, 50, 0.7)";

// Danger button gradient stops
constexpr auto kColorDangerButtonNormalTop = "rgba(239, 78, 75, 0.92)";
constexpr auto kColorDangerButtonNormalMiddle = "rgba(208, 44, 44, 0.90)";
constexpr auto kColorDangerButtonNormalBottom = "rgba(193, 39, 39, 0.88)";
constexpr auto kColorDangerButtonHoverTop = "rgba(250, 60, 0, 0.95)";
constexpr auto kColorDangerButtonHoverMiddle = "rgba(226, 25, 21, 0.93)";
constexpr auto kColorDangerButtonHoverBottom = "rgba(154, 32, 32, 0.90)";
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
constexpr auto kColorDiscordButtonNormalTop = "rgba(91, 101, 252, 0.92)";
constexpr auto kColorDiscordButtonNormalMiddle = "rgba(72, 86, 241, 0.90)";
constexpr auto kColorDiscordButtonNormalBottom = "rgba(61, 72, 189, 0.88)";
constexpr auto kColorDiscordButtonHoverTop = "rgba(96, 107, 252, 0.95)";
constexpr auto kColorDiscordButtonHoverMiddle = "rgba(78, 91, 245, 0.93)";
constexpr auto kColorDiscordButtonHoverBottom = "rgba(58, 73, 240, 0.90)";
constexpr auto kColorDiscordButtonPressedTop = "rgba(71, 82, 196, 0.95)";
constexpr auto kColorDiscordButtonPressedMiddle = "rgba(57, 66, 164, 0.93)";
constexpr auto kColorDiscordButtonPressedBottom = "rgba(45, 52, 140, 0.92)";
constexpr auto kColorDiscordButtonBorder = "rgba(71, 82, 196, 0.7)";

// Semantic status colors
constexpr auto kStatusColorIdle = "#606f85";     ///< kColorTextMuted -- idle/pending/default
constexpr auto kStatusColorRunning = "#2563eb";  ///< blue-600 -- in-progress/active
constexpr auto kStatusColorSuccess = "#117f3a";  ///< kColorSuccess -- completed/passed/ready
constexpr auto kStatusColorWarning = "#a65b05";  ///< amber-600, darkened -- caution/slow/degraded
constexpr auto kStatusColorError = "#d62323";    ///< kColorError -- failed/error/critical

// Screen-reader and colorblind status text prefixes
constexpr auto kStatusPrefixIdle = "\u25CB ";     ///< idle/pending
constexpr auto kStatusPrefixRunning = "\u23F3 ";  ///< in-progress
constexpr auto kStatusPrefixSuccess = "\u2714 ";  ///< completed/passed
constexpr auto kStatusPrefixWarning = "\u26A0 ";  ///< caution
constexpr auto kStatusPrefixError = "\u2718 ";    ///< failed/error

}  // namespace sak::ui
