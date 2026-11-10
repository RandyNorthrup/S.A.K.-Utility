// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ribbon_tool_button.h"

#include <QEvent>

namespace sak::ui {

RibbonToolButton::RibbonToolButton(QWidget* parent, QSize base_cell)
    : QToolButton(parent), m_base_cell(base_cell) {
    // Fixed policy makes the style-aware hint authoritative in both
    // directions: the layout can neither stretch the button nor shrink it
    // below the size the style needs to draw the full caption.
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize RibbonToolButton::sizeHint() const {
    return QToolButton::sizeHint().expandedTo(m_base_cell);
}

QSize RibbonToolButton::minimumSizeHint() const {
    return sizeHint();
}

void RibbonToolButton::changeEvent(QEvent* event) {
    QToolButton::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        updateGeometry();
    }
}

}  // namespace sak::ui
