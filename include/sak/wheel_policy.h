// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wheel_policy.h
/// @brief The application's one mouse-wheel policy: standard Windows behaviour.

#pragma once

class QApplication;

namespace sak::ui {

/// Installs the application-wide mouse-wheel policy (owner ruling 2026-10-24: the app behaves
/// like a normal Windows tool). On Windows the wheel scrolls the view under the pointer and
/// never moves keyboard focus, so:
///
/// - No widget takes keyboard focus from the wheel (a Qt::WheelFocus policy becomes
///   Qt::StrongFocus when the widget is polished; click and Tab still focus it).
/// - A combo box, spin box (including date and time edits) or slider that does not have
///   keyboard focus leaves the wheel alone: the event goes on to the view it sits in, which
///   scrolls. With focus, the control steps its value, as a Windows control does.
/// - A tab bar never switches tabs on the wheel; the wheel goes on to the view around it.
///
/// Scroll bars and scrolling views are untouched: they scroll under the pointer.
void installWheelPolicy(QApplication& app);

}  // namespace sak::ui
