// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file wheel_policy.cpp
/// @brief The application's one mouse-wheel policy: standard Windows behaviour.

#include "sak/wheel_policy.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QScrollBar>
#include <QTabBar>
#include <QWidget>

namespace sak::ui {

namespace {

/// A control whose value the wheel steps: a combo box, a spin box or date/time edit, or a
/// slider. A scroll bar is a slider too, but the wheel is what it is for.
bool isWheelValueControl(const QWidget* widget) {
    if ((qobject_cast<const QComboBox*>(widget) != nullptr) ||
        (qobject_cast<const QAbstractSpinBox*>(widget) != nullptr)) {
        return true;
    }
    return (qobject_cast<const QAbstractSlider*>(widget) != nullptr) &&
           (qobject_cast<const QScrollBar*>(widget) == nullptr);
}

/// The control or something inside it (an editable combo's line edit) has keyboard focus.
bool ownsKeyboardFocus(const QWidget* widget) {
    const QWidget* focus = QApplication::focusWidget();
    return (focus != nullptr) && (focus == widget || widget->isAncestorOf(focus));
}

class WheelPolicyFilter final : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Polish) {
            // QApplication gives wheel focus before any event filter sees the wheel, so the
            // policy has to be gone by the time the first wheel arrives.
            auto* widget = qobject_cast<QWidget*>(watched);
            if ((widget != nullptr) && widget->focusPolicy() == Qt::WheelFocus) {
                widget->setFocusPolicy(Qt::StrongFocus);
            }
            return false;
        }
        if (event->type() != QEvent::Wheel) {
            return false;
        }
        const auto* widget = qobject_cast<QWidget*>(watched);
        if (widget == nullptr) {
            return false;
        }
        const bool passes_wheel_on = (qobject_cast<const QTabBar*>(widget) != nullptr) ||
                                     (isWheelValueControl(widget) && !ownsKeyboardFocus(widget));
        if (!passes_wheel_on) {
            return false;
        }
        // Withheld from the control and left unaccepted: QApplication then offers the same
        // wheel to each parent in turn, and the first view that can scroll takes it.
        event->ignore();
        return true;
    }
};

}  // namespace

void installWheelPolicy(QApplication& app) {
    app.installEventFilter(new WheelPolicyFilter(&app));
}

}  // namespace sak::ui
