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

// Choose which button dismiss_dialog should invoke, given the enabled/on-screen candidates.
//
// Selection order:
//   1. explicit_button non-empty  -> first candidate whose name contains it (case-insensitive).
//   2. otherwise                  -> the best-ranked affirmative caption (OK/Close/Continue/...),
//                                    exact match beating a substring match; NEVER Cancel/No/Quit/
//                                    Delete or any caption not on the affirmative list.
//   3. otherwise, a single candidate -> that lone button (an unambiguous one-button dialog, invoked
//                                    even when it is nameless -- the custom-drawn OK case).
//
// Returns the winning candidate's element_index, or -1 with `why` set to a caller-surfaceable
// explanation (empty candidate set, no explicit match, or an ambiguous set with no affirmative --
// in which case `why` lists the candidate captions so the caller can retry with explicit_button).
int chooseDialogButton(const QVector<DialogButton>& buttons,
                       const QString& explicit_button,
                       QString& why);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_DIALOG_CHOICE_H
