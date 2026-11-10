// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ribbon_tool_button.h
/// @brief Ribbon tool button whose caption can never be elided.

#pragma once

#include <QSize>
#include <QToolButton>

namespace sak::ui {

/// Tool button for icon-above-caption ribbon rows that can never elide its
/// caption. The layout size comes from the active style's own size hint, which
/// accounts for style-sheet padding, borders, and the current font, floored at
/// a uniform base cell so short captions keep one consistent ribbon rhythm.
/// The geometry is recomputed whenever the font or style changes, so a theme
/// or style-sheet applied after construction can not reintroduce truncation.
class RibbonToolButton : public QToolButton {
public:
    RibbonToolButton(QWidget* parent, QSize base_cell);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void changeEvent(QEvent* event) override;

private:
    QSize m_base_cell;
};

}  // namespace sak::ui
