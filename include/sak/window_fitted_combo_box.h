// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file window_fitted_combo_box.h
/// @brief A combo box whose list opens at its widest entry's width, inside its window.

#pragma once

#include <QComboBox>
#include <QRect>

namespace sak {

/// A combo box whose list opens at least as wide as its widest entry, and inside its window. Qt's
/// Windows styles open the list at the combo's own width, so the AI workflow picker cut
/// "Troubleshooting - Browser Issue Cleanup (9 phas" behind its scroll bar, and its 18 rows ran
/// 39 px past the foot of the window (review 4, RG4). A list wider than the window is cut to the
/// window and scrolls sideways; one taller than the room beside the field is cut and scrolls.
/// The field itself ends the chosen entry with an ellipsis when it has less room than the entry
/// needs: with the side panel's scroll bar beside it, the workflow picker cut its name through a
/// glyph, "Technician Service Report (5 phas" (review 4, DF11). The whole name is in the list.
class WindowFittedComboBox final : public QComboBox {
public:
    using QComboBox::QComboBox;

    void showPopup() override;

    /// The width the list needs to show its widest entry whole, with its frame and, when not every
    /// entry fits in the visible rows, its scroll bar.
    [[nodiscard]] int entriesWidth() const;

    /// The chosen entry as the field shows it at its present width: whole, or ending in an
    /// ellipsis.
    [[nodiscard]] QString shownText() const;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] QRect fittedInWindow(QRect list) const;
    [[nodiscard]] QRect textRect() const;
};

}  // namespace sak
