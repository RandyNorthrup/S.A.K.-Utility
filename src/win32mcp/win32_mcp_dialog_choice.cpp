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

// Destructive OR negative verbs that turn an otherwise-affirmative caption into a dangerous or
// contradictory action. If any appears as a whole word the caption is NEVER auto-affirmative --
// so "Yes, delete all", "OK, format drive", and "Yes, cancel" are refused rather than auto-pressed
// just because they also contain "yes"/"ok". The negative words (cancel/no/abort/...) stop a
// caption that pairs an affirmative token with a cancellation from being clicked.
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
    "cancel",
    "no",
    "abort",
    "deny",
    "decline",
    "stop",
    "never",
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

// Every candidate whose lower-cased name contains `needle`.
QVector<DialogButton> buttonsContaining(const QVector<DialogButton>& buttons,
                                        const QString& needle) {
    QVector<DialogButton> matches;
    for (const DialogButton& button : buttons) {
        if (button.name.toLower().contains(needle)) {
            matches.append(button);
        }
    }
    return matches;
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

// element_index of the button an explicit caption names, or -1 with `why` set. An EXACT
// (case-insensitive) caption wins outright; otherwise the containing match must be UNIQUE --
// a needle that matches several captions ("save" against Save / Do Not Save) is an ambiguous
// request and is refused rather than resolved by walk order (fail closed).
int explicitButtonChoice(const QVector<DialogButton>& buttons,
                         const QString& explicit_button,
                         QString& why) {
    const QString needle = explicit_button.toLower();
    for (const DialogButton& button : buttons) {
        if (button.name.toLower() == needle) {
            return button.element_index;
        }
    }
    const QVector<DialogButton> matches = buttonsContaining(buttons, needle);
    if (matches.isEmpty()) {
        why = QStringLiteral("No enabled button matches '%1'.").arg(explicit_button);
        return -1;
    }
    if (matches.size() > 1) {
        why = QStringLiteral("'%1' matches more than one button (%2); pass the exact caption.")
                  .arg(explicit_button, buttonListText(matches));
        return -1;
    }
    return matches.first().element_index;
}

}  // namespace

int chooseDialogButton(const QVector<DialogButton>& buttons,
                       const QString& explicit_button,
                       QString& why) {
    if (buttons.isEmpty()) {
        why = QStringLiteral("The window has no enabled button to invoke.");
        return -1;
    }
    // A candidate must carry the caller's own handle back to a live element. A negative index is
    // malformed input (a default-constructed DialogButton), and returning it would hand the caller
    // an index it cannot dereference -- refuse the whole choice instead.
    for (const DialogButton& button : buttons) {
        if (button.element_index < 0) {
            why = QStringLiteral("A dialog button candidate carries no valid element index.");
            return -1;
        }
    }
    if (!explicit_button.isEmpty()) {
        return explicitButtonChoice(buttons, explicit_button, why);
    }
    const int affirmative = bestAffirmativeButton(buttons);
    if (affirmative >= 0) {
        return affirmative;
    }
    if (buttons.size() == 1) {
        // Single-button dialog: unambiguous even if nameless (the custom-drawn OK case). But a
        // lone DESTRUCTIVE or negative caption ("Delete", "Quit") is never auto-pressed just for
        // being alone -- the caller must name it through explicit_button.
        const DialogButton& lone = buttons.first();
        if (!hasDestructiveWord(captionTokens(lone.name.toLower()))) {
            return lone.element_index;
        }
        why = QStringLiteral(
                  "The only button ('%1') is destructive or negative; pass 'button' to invoke it "
                  "deliberately.")
                  .arg(lone.name);
        return -1;
    }
    why = QStringLiteral("No affirmative button found; pass 'button' to pick one of: %1")
              .arg(buttonListText(buttons));
    return -1;
}

}  // namespace sak::win32mcp
