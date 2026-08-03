// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dialog_choice.h"

#include <QLatin1String>
#include <QRegularExpression>
#include <QStringList>

namespace sak::win32mcp {

QVector<DialogButton> collectButtons(const QVector<ButtonNode>& nodes) {
    QVector<DialogButton> buttons;
    for (int i = 0; i < nodes.size(); ++i) {
        const ButtonNode& node = nodes.at(i);
        if (node.role == QLatin1String("button") && node.enabled && !node.offscreen) {
            buttons.append(DialogButton{i, node.name});
        }
    }
    return buttons;
}

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

// Destructive/negative verbs that turn an otherwise-affirmative caption into a dangerous action.
// If any appears as a whole word the caption is NEVER auto-affirmative -- so "Yes, delete all" or
// "OK, format drive" is refused rather than auto-pressed just because it also contains "yes"/"ok".
const char* const kDestructiveWords[] = {
    "delete",
    "remove",
    "discard",
    "erase",
    "quit",
    "reset",
    "format",
    "wipe",
    "overwrite",
    "replace",
    "uninstall",
};

// Split a lower-cased caption into its alphanumeric word tokens, so matching is whole-word rather
// than substring ("yes" must not match inside "Yes, delete all"; "delete" is its own token).
QStringList captionTokens(const QString& name_lower) {
    static const QRegularExpression kNonWord(QStringLiteral("[^a-z0-9]+"));
    return name_lower.split(kNonWord, Qt::SkipEmptyParts);
}

bool hasDestructiveWord(const QStringList& tokens) {
    for (const QString& token : tokens) {
        for (const char* bad : kDestructiveWords) {
            if (token == QLatin1String(bad)) {
                return true;
            }
        }
    }
    return false;
}

// Rank a button caption as an affirmative: lower is better, -1 = not affirmative. A caption
// carrying a destructive verb is rejected outright; otherwise an EXACT caption match outranks a
// whole-WORD match, so "OK" beats "OK, don't ask again", and neither substring-matches a longer
// destructive phrase.
int affirmativeRank(const QString& name_lower) {
    const QStringList tokens = captionTokens(name_lower);
    if (hasDestructiveWord(tokens)) {
        return -1;  // destructive verb present -> never auto-affirmative (fail closed)
    }
    int i = 0;
    for (const char* caption : kAffirmativeCaptions) {
        if (name_lower == QLatin1String(caption)) {
            return i;  // exact hit -- strongest
        }
        ++i;
    }
    constexpr int kWordBase = 100;
    i = 0;
    for (const char* caption : kAffirmativeCaptions) {
        if (tokens.contains(QString::fromLatin1(caption))) {  // whole word, not substring
            return kWordBase + i;
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
