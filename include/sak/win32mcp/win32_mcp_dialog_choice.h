// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_DIALOG_CHOICE_H
#define SAK_WIN32MCP_WIN32_MCP_DIALOG_CHOICE_H

#include <QString>
#include <QVector>

namespace sak::win32mcp {

// One candidate button the dismiss_dialog tool may invoke. `element_index` is the caller's own
// handle back to the live UI element (an index into its parallel element vector); this pure module
// never dereferences it, it only chooses which candidate wins and returns that index. The caller is
// responsible for having already filtered to enabled, on-screen buttons.
struct DialogButton {
    int element_index{-1};
    QString name;
};

// One control from a UIA walk, as seen by collectButtons. `role` is the friendly control-type name
// (only "button" is a candidate); enabled/offscreen gate whether it can be invoked. BOTH gates
// default to the NON-invokable state: a walk that could not read a property must never leave a
// candidate looking pressable, so the reader has to state enabled/on-screen explicitly.
struct ButtonNode {
    QString role;
    QString name;
    bool enabled{false};
    bool offscreen{true};
};

// Select the invokable candidates from a walked control list: the enabled, on-screen buttons, each
// paired with its position in `nodes` as DialogButton.element_index. A disabled or off-screen
// button, or any non-button control, is excluded so dismiss_dialog never invokes it.
QVector<DialogButton> collectButtons(const QVector<ButtonNode>& nodes);

// Choose which button dismiss_dialog should invoke, given the enabled/on-screen candidates.
//
// Selection order:
//   1. explicit_button non-empty  -> the candidate whose name equals it (case-insensitive), else
//                                    the ONE candidate whose name contains it; two or more
//                                    containing candidates is ambiguous and is refused.
//   2. otherwise                  -> the best-ranked affirmative caption (OK/Close/Continue/...),
//                                    exact match beating a substring match; NEVER Cancel/No/Quit/
//                                    Delete or any caption not on the affirmative list.
//   3. otherwise, a single candidate -> that lone button (an unambiguous one-button dialog, invoked
//                                    even when it is nameless -- the custom-drawn OK case), UNLESS
//                                    its caption carries a destructive or negative verb, which must
//                                    be named through explicit_button rather than auto-pressed.
//
// Returns the winning candidate's element_index, or -1 with `why` set to a caller-surfaceable
// explanation (empty candidate set, a candidate carrying no valid element_index, no explicit match
// or an ambiguous one, a lone destructive button, or an ambiguous set with no affirmative -- in
// which case `why` lists the candidate captions so the caller can retry with explicit_button).
int chooseDialogButton(const QVector<DialogButton>& buttons,
                       const QString& explicit_button,
                       QString& why);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_DIALOG_CHOICE_H
