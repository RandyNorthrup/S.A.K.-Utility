// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_dialog_choice.h"

#include <QtTest/QtTest>

using sak::win32mcp::ButtonNode;
using sak::win32mcp::chooseDialogButton;
using sak::win32mcp::collectButtons;
using sak::win32mcp::DialogButton;

namespace {

// Build a candidate list; element_index is set to the position so a test can assert which one won.
QVector<DialogButton> buttons(const QStringList& names) {
    QVector<DialogButton> out;
    for (int i = 0; i < names.size(); ++i) {
        out.append(DialogButton{i, names.at(i)});
    }
    return out;
}

ButtonNode btn(const QString& role, const QString& name, bool enabled, bool offscreen) {
    return ButtonNode{role, name, enabled, offscreen};
}

}  // namespace

class Win32McpDialogChoiceTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void emptySetRefuses();
    void singleAffirmativeChosen();
    void loneNamelessButtonChosen();
    void exactBeatsSubstring();
    void neverChoosesCancelOverAffirmative();
    void refusesWhenNoAffirmativeAndListsCaptions();
    void refusesMultipleNamelessButtons();
    void explicitButtonMatchesSubstringCaseInsensitive();
    void explicitButtonCanSelectCancelDeliberately();
    void explicitButtonNoMatchRefuses();
    void affirmativeRankingPrefersEarlierCaption();
    void collectButtonsFiltersToEnabledOnScreenButtons();
    void destructiveCaptionRejectedDespiteAffirmativeWord();
    void exactAffirmativeChosenOverDestructiveSibling();
    void substringAffirmativeNoLongerMatches();
};

void Win32McpDialogChoiceTests::collectButtonsFiltersToEnabledOnScreenButtons() {
    const QVector<ButtonNode> nodes{
        btn(QStringLiteral("window"), QStringLiteral("App"), true, false),    // 0: not a button
        btn(QStringLiteral("button"), QStringLiteral("OK"), true, false),     // 1: keep
        btn(QStringLiteral("button"), QStringLiteral("Grey"), false, false),  // 2: disabled -> drop
        btn(QStringLiteral("button"),
            QStringLiteral("Hidden"),
            true,
            true),                                                       // 3: offscreen -> drop
        btn(QStringLiteral("text"), QStringLiteral("OK"), true, false),  // 4: not a button
        btn(QStringLiteral("button"), QString(), true, false),           // 5: keep (nameless)
    };
    const QVector<DialogButton> got = collectButtons(nodes);
    QCOMPARE(got.size(), 2);
    QCOMPARE(got[0].element_index, 1);  // index preserved from the source list
    QCOMPARE(got[0].name, QStringLiteral("OK"));
    QCOMPARE(got[1].element_index, 5);  // the nameless enabled on-screen button
    QVERIFY(got[1].name.isEmpty());
}

void Win32McpDialogChoiceTests::emptySetRefuses() {
    QString why;
    QCOMPARE(chooseDialogButton({}, QString(), why), -1);
    QVERIFY(!why.isEmpty());
}

void Win32McpDialogChoiceTests::singleAffirmativeChosen() {
    QString why;
    const int idx = chooseDialogButton(buttons({QStringLiteral("OK")}), QString(), why);
    QCOMPARE(idx, 0);
    QVERIFY(why.isEmpty());
}

void Win32McpDialogChoiceTests::loneNamelessButtonChosen() {
    // The custom-drawn completion dialog: one button, no accessible name. It is unambiguous, so it
    // is invoked even without an affirmative caption -- this is the whole point of dismiss_dialog.
    QString why;
    const int idx = chooseDialogButton(buttons({QString()}), QString(), why);
    QCOMPARE(idx, 0);
    QVERIFY(why.isEmpty());
}

void Win32McpDialogChoiceTests::exactBeatsSubstring() {
    // "OK" (exact) must beat "OK, don't ask again" (substring) regardless of order.
    QString why;
    const int idx = chooseDialogButton(
        buttons({QStringLiteral("OK, don't ask again"), QStringLiteral("OK")}), QString(), why);
    QCOMPARE(idx, 1);  // the exact "OK"
}

void Win32McpDialogChoiceTests::neverChoosesCancelOverAffirmative() {
    // SAFETY: with OK + Cancel present, the affirmative OK is chosen; Cancel is never auto-pressed.
    QString why;
    const int idx = chooseDialogButton(buttons({QStringLiteral("Cancel"), QStringLiteral("OK")}),
                                       QString(),
                                       why);
    QCOMPARE(idx, 1);  // OK, not Cancel(0)
}

void Win32McpDialogChoiceTests::refusesWhenNoAffirmativeAndListsCaptions() {
    // SAFETY: no caption is affirmative and there is more than one, so it refuses rather than
    // guess, and the reason lists the captions so the caller can retry with an explicit button.
    QString why;
    const int idx = chooseDialogButton(
        buttons({QStringLiteral("Abort"), QStringLiteral("Retry"), QStringLiteral("Ignore")}),
        QString(),
        why);
    QCOMPARE(idx, -1);
    QVERIFY(why.contains(QStringLiteral("Abort")));
    QVERIFY(why.contains(QStringLiteral("Retry")));
    QVERIFY(why.contains(QStringLiteral("Ignore")));
}

void Win32McpDialogChoiceTests::refusesMultipleNamelessButtons() {
    // Two nameless buttons is ambiguous (the lone-button rule needs exactly one) -> refuse, and
    // surface them as (unnamed) so the error is still actionable.
    QString why;
    const int idx = chooseDialogButton(buttons({QString(), QString()}), QString(), why);
    QCOMPARE(idx, -1);
    QVERIFY(why.contains(QStringLiteral("(unnamed)")));
}

void Win32McpDialogChoiceTests::explicitButtonMatchesSubstringCaseInsensitive() {
    QString why;
    const int idx =
        chooseDialogButton(buttons({QStringLiteral("Save"), QStringLiteral("Do Not Save")}),
                           QStringLiteral("do not"),
                           why);
    QCOMPARE(idx, 1);
    QVERIFY(why.isEmpty());
}

void Win32McpDialogChoiceTests::explicitButtonCanSelectCancelDeliberately() {
    // An explicit caption is a deliberate caller choice, so it may target a non-affirmative button
    // (e.g. Cancel) that the auto path would never pick.
    QString why;
    const int idx = chooseDialogButton(buttons({QStringLiteral("OK"), QStringLiteral("Cancel")}),
                                       QStringLiteral("Cancel"),
                                       why);
    QCOMPARE(idx, 1);
}

void Win32McpDialogChoiceTests::explicitButtonNoMatchRefuses() {
    QString why;
    const int idx =
        chooseDialogButton(buttons({QStringLiteral("OK")}), QStringLiteral("Retry"), why);
    QCOMPARE(idx, -1);
    QVERIFY(why.contains(QStringLiteral("Retry")));
}

void Win32McpDialogChoiceTests::affirmativeRankingPrefersEarlierCaption() {
    // "ok" ranks above "close" in the affirmative list, so OK wins when both are present.
    QString why;
    const int idx = chooseDialogButton(buttons({QStringLiteral("Close"), QStringLiteral("OK")}),
                                       QString(),
                                       why);
    QCOMPARE(idx, 1);  // OK outranks Close
}

void Win32McpDialogChoiceTests::destructiveCaptionRejectedDespiteAffirmativeWord() {
    // SAFETY: "Yes, delete all" must NOT be auto-pressed just because it contains "yes"; the
    // destructive verb "delete" disqualifies it, so with a Cancel sibling the choice refuses.
    QString why;
    const int idx = chooseDialogButton(
        buttons({QStringLiteral("Yes, delete all"), QStringLiteral("Cancel")}), QString(), why);
    QCOMPARE(idx, -1);
    QVERIFY(why.contains(QStringLiteral("Yes, delete all")));
}

void Win32McpDialogChoiceTests::exactAffirmativeChosenOverDestructiveSibling() {
    // A clean "OK" is still chosen; the destructive "Delete" sibling is never auto-pressed.
    QString why;
    const int idx = chooseDialogButton(buttons({QStringLiteral("Delete"), QStringLiteral("OK")}),
                                       QString(),
                                       why);
    QCOMPARE(idx, 1);  // OK, not Delete(0)
    QVERIFY(why.isEmpty());
}

void Win32McpDialogChoiceTests::substringAffirmativeNoLongerMatches() {
    // "Yesterday" contains the substring "yes" but is not the whole word "yes", so it is not an
    // affirmative; with a non-affirmative sibling the choice refuses instead of pressing it.
    QString why;
    const int idx = chooseDialogButton(
        buttons({QStringLiteral("Yesterday"), QStringLiteral("Retry")}), QString(), why);
    QCOMPARE(idx, -1);
}

QTEST_GUILESS_MAIN(Win32McpDialogChoiceTests)
#include "test_win32_mcp_dialog_choice.moc"
