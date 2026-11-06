// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dialog_button_tones.cpp
/// @brief One tone per role for a dialog's buttons.

#include "sak/dialog_button_tones.h"

#include "sak/style_constants.h"

#include <QApplication>
#include <QEvent>
#include <QPushButton>

namespace sak::ui {

bool isDismissRole(QDialogButtonBox::ButtonRole role) {
    return role == QDialogButtonBox::RejectRole || role == QDialogButtonBox::NoRole;
}

namespace {

class DialogButtonToneFilter final : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::Polish) {
            return false;
        }
        auto* button = qobject_cast<QPushButton*>(watched);
        if (button == nullptr || !button->styleSheet().isEmpty()) {
            return false;
        }
        // A message box keeps its buttons in a button box of its own, so it is covered too.
        const auto* box = qobject_cast<const QDialogButtonBox*>(button->parentWidget());
        if (box != nullptr && isDismissRole(box->buttonRole(button))) {
            button->setStyleSheet(kSecondaryButtonStyle);
        }
        return false;
    }
};

}  // namespace

void installDialogButtonTones(QApplication& app) {
    app.installEventFilter(new DialogButtonToneFilter(&app));
}

}  // namespace sak::ui
