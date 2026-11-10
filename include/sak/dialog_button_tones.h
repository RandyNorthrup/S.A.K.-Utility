// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dialog_button_tones.h
/// @brief One tone per role for a dialog's buttons.

#pragma once

#include <QDialogButtonBox>

class QApplication;

namespace sak::ui {

/// @brief True for a button role that only leaves a dialog: Close, Cancel, No. Such a button is
///        secondary; the one that does the dialog's work keeps the primary tone.
[[nodiscard]] bool isDismissRole(QDialogButtonBox::ButtonRole role);

/// Installs the application-wide button tone policy (UI review F27: Close was primary in some
/// dialogs and secondary in others, Cancel likewise). The theme makes every push button primary,
/// so a dialog button box's Close, Cancel and No, and a message box's, came out in the colour of
/// the action beside them. When such a button is polished it takes the secondary tone, unless it
/// already carries a style of its own (a Cancel that stops running work is the danger tone).
void installDialogButtonTones(QApplication& app);

}  // namespace sak::ui
