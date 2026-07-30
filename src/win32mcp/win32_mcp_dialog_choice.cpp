// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dialog_choice.h"

#include <QLatin1String>
#include <QStringList>

namespace sak::win32mcp {

namespace {

// Affirmative captions, best-first. Only safe "acknowledge / proceed" verbs -- never Cancel / No /
// Quit / Delete, which change meaning; a caller that must press one of those names it explicitly.
const char* const kAffirmativeCaptions[] = {
    "ok",
    "okay",
    "close",
    "continue",
    "finish",
    "done",
    "got it",
    "dismiss",
    "accept",
    "yes",
    "great",
    "next",
};

// Rank a button caption as an affirmative: lower is better, -1 = not affirmative. An exact caption
// match outranks a substring match, so "OK" beats "OK, don't ask again" when both are present.
int affirmativeRank(const QString& name_lower) {
    int i = 0;
    for (const char* caption : kAffirmativeCaptions) {
        if (name_lower == QLatin1String(caption)) {
            return i;  // exact hit -- strongest
        }
        ++i;
    }
    constexpr int kSubstringBase = 100;
    i = 0;
    for (const char* caption : kAffirmativeCaptions) {
        if (name_lower.contains(QLatin1String(caption))) {
            return kSubstringBase + i;
        }
        ++i;
    }
    return -1;
}

// element_index of the first button whose lower-cased name contains `needle`, or -1.
int buttonMatching(const QVector<DialogButton>& buttons, const QString& needle) {
    for (const DialogButton& button : buttons) {
        if (button.name.toLower().contains(needle)) {
            return button.element_index;
        }
    }
    return -1;
}

// element_index of the best-ranked affirmative button, or -1 when none of them is affirmative.
int bestAffirmativeButton(const QVector<DialogButton>& buttons) {
    int best_index = -1;
    int best_rank = -1;
    for (const DialogButton& button : buttons) {
        const int rank = affirmativeRank(button.name.toLower());
        if (rank >= 0 && (best_index < 0 || rank < best_rank)) {
            best_rank = rank;
            best_index = button.element_index;
        }
    }
    return best_index;
}

// Comma-joined button captions (nameless ones shown as "(unnamed)") for an ambiguous-choice error.
QString buttonListText(const QVector<DialogButton>& buttons) {
    QStringList names;
    for (const DialogButton& button : buttons) {
        names << (button.name.isEmpty() ? QStringLiteral("(unnamed)") : button.name);
    }
    return names.join(QStringLiteral(", "));
}

}  // namespace

int chooseDialogButton(const QVector<DialogButton>& buttons,
                       const QString& explicit_button,
                       QString& why) {
    if (buttons.isEmpty()) {
        why = QStringLiteral("The window has no enabled button to invoke.");
        return -1;
    }
    if (!explicit_button.isEmpty()) {
        const int index = buttonMatching(buttons, explicit_button.toLower());
        if (index < 0) {
            why = QStringLiteral("No enabled button matches '%1'.").arg(explicit_button);
        }
        return index;
    }
    const int affirmative = bestAffirmativeButton(buttons);
    if (affirmative >= 0) {
        return affirmative;
    }
    if (buttons.size() == 1) {
        return buttons.first().element_index;  // single-button dialog: unambiguous even if nameless
    }
    why = QStringLiteral("No affirmative button found; pass 'button' to pick one of: %1")
              .arg(buttonListText(buttons));
    return -1;
}

}  // namespace sak::win32mcp
