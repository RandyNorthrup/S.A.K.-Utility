// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_folder_selection.h
/// @brief Which folders of an opened mail store are MAIL folders, and their node ids.
///
/// Extracted from EmailInspectorPanel so the rule has one definition and can be tested
/// without constructing a panel. The panel's folder tree and its "Export ALL Mail
/// Folders" action must agree exactly: if the export walked a different set than the
/// tree displays, a technician would either silently lose folders or receive calendar
/// entries written out as .eml files.

#pragma once

#include "sak/email_types.h"

#include <QString>
#include <QVector>

#include <cstdint>
#include <optional>

namespace sak {

/// @brief Whether an item is mail a person sent or received, wherever it is filed: a message, or a
///        meeting invitation or reply. Contacts, events, tasks, notes, journal entries and the
///        store's own bookkeeping are not.
[[nodiscard]] bool isRealEmail(EmailItemType type);

/// @brief True when a folder holds mail and belongs in the folder tree.
///
/// Contacts, calendar, tasks, sticky notes and journal folders are excluded because they
/// are not email and are surfaced through their own dialogs, which export them in the
/// formats those item types actually have (VCF, ICS, CSV). Configuration folders and the
/// known Outlook/Exchange bookkeeping folders are excluded because they carry no user
/// mail.
[[nodiscard]] bool isMailFolder(const QString& display_name, const QString& container_class);

/// @brief The IPM_SUBTREE node holding the user-facing folders, or nullptr.
///
/// An OST has TWO such nodes -- one under Root - Public (empty) and one under
/// Root - Mailbox (holding every user folder) -- so the one WITH children wins. A
/// childless node is returned only when no populated one exists anywhere in @p folders.
[[nodiscard]] const PstFolder* findIpmSubtree(const QVector<PstFolder>& folders);

/// @brief Node ids of every folder with mail in @p tree, parents before children.
///
/// Starts at the populated IPM_SUBTREE when there is one and falls back to the tree
/// roots otherwise. A folder that fails isMailFolder is skipped together with its
/// descendants, unless real emails turned up in it or under it. System folders outside
/// the mail tree come last, for the same reason. This mirrors how the tree renders.
[[nodiscard]] QVector<uint64_t> mailFolderNodeIds(const QVector<PstFolder>& tree);

/// Whether a folder, or anything under it, holds real emails that an ordinary mail folder would
/// not have shown. Such a folder appears in the tree even though it is a system folder.
[[nodiscard]] bool holdsSystemFolderEmails(const PstFolder& folder);

/// Folders outside the store's mail tree that hold real emails, for showing apart from it.
[[nodiscard]] QVector<const PstFolder*> foldersWithMailOutsideMailTree(
    const QVector<PstFolder>& tree);

/// Whether a folder holds tasks, sticky notes or journal entries. They are not mail, so "all mail
/// folders" leaves them out, but they are browsed in the folder tree like mail.
[[nodiscard]] bool isTaskNoteOrJournalFolder(const QString& display_name,
                                             const QString& container_class);

/// The folder a node id names, or nullptr when the id is empty or not in the tree.
[[nodiscard]] const PstFolder* findFolderByNodeId(const QVector<PstFolder>& tree,
                                                  std::optional<uint64_t> node_id);

}  // namespace sak
