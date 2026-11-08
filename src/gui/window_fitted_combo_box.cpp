// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file window_fitted_combo_box.cpp
/// @brief A combo box whose list opens at its widest entry's width, inside its window.

#include "sak/window_fitted_combo_box.h"

#include <QAbstractItemView>
#include <QPoint>
#include <QScrollBar>
#include <QStyleOptionComboBox>
#include <QStylePainter>

#include <algorithm>

namespace sak {

void WindowFittedComboBox::showPopup() {
    QAbstractItemView* list = view();
    list->setMinimumWidth(entriesWidth());
    QComboBox::showPopup();
    QWidget* popup = list->window();
    if (popup != nullptr && popup != window()) {
        popup->setGeometry(fittedInWindow(popup->geometry()));
    }
}

int WindowFittedComboBox::entriesWidth() const {
    QAbstractItemView* list = view();
    list->ensurePolished();  // the theme's item padding is part of each entry's width
    const int frames = list->frameWidth() + list->frameWidth();
    int width = list->sizeHintForColumn(modelColumn()) + frames;
    if (count() > maxVisibleItems()) {
        width += list->verticalScrollBar()->sizeHint().width();
    }
    return width;
}

QRect WindowFittedComboBox::textRect() const {
    QStyleOptionComboBox option;
    initStyleOption(&option);
    // The style draws the label one pixel in from each side of the edit field.
    return style()
        ->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this)
        .adjusted(1, 0, -1, 0);
}

QString WindowFittedComboBox::shownText() const {
    return fontMetrics().elidedText(currentText(), Qt::ElideRight, textRect().width());
}

// QComboBox::paintEvent, with the label ended in an ellipsis where it would be cut.
void WindowFittedComboBox::paintEvent(QPaintEvent* /*event*/) {
    QStylePainter painter(this);
    painter.setPen(palette().color(QPalette::Text));
    QStyleOptionComboBox option;
    initStyleOption(&option);
    painter.drawComplexControl(QStyle::CC_ComboBox, option);
    if (currentIndex() < 0 && !placeholderText().isEmpty()) {
        option.palette.setBrush(QPalette::ButtonText, option.palette.placeholderText());
        option.currentText = placeholderText();
    } else if (!isEditable()) {
        option.currentText = shownText();
    }
    painter.drawControl(QStyle::CE_ComboBoxLabel, option);
}

// The window's client area in screen coordinates. The list keeps inside it: on the side of the
// field with room for it, or else on the roomier side, cut to fit.
QRect WindowFittedComboBox::fittedInWindow(QRect list) const {
    const QWidget* top = window();
    const QRect bounds(top->mapToGlobal(QPoint(0, 0)), top->size());
    const QRect field(mapToGlobal(QPoint(0, 0)), size());
    list.setWidth(std::min(list.width(), bounds.width()));
    if (list.right() > bounds.right()) {
        list.moveRight(bounds.right());
    }
    if (list.left() < bounds.left()) {
        list.moveLeft(bounds.left());
    }
    if (list.top() >= bounds.top() && list.bottom() <= bounds.bottom()) {
        return list;
    }
    const int height = list.height();
    const int below = bounds.bottom() - field.bottom();
    const int above = field.top() - bounds.top();
    if (height <= below) {
        list.moveTop(field.bottom() + 1);
    } else if (height <= above) {
        list.moveBottom(field.top() - 1);
    } else if (below >= above) {
        list.setTop(field.bottom() + 1);
        list.setBottom(bounds.bottom());
    } else {
        list.setTop(bounds.top());
        list.setBottom(field.top() - 1);
    }
    return list;
}

}  // namespace sak
