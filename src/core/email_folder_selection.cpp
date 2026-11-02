// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_folder_selection.cpp
/// @brief Mail-folder classification and node-id collection for an opened store.

#include "sak/email_folder_selection.h"

#include <QStringList>

#include <algorithm>

namespace sak {

namespace {

/// Container classes whose folders hold something other than mail. Matched by prefix
/// because Outlook appends variants (IPF.Contact.MOC.ImContactList, IPF.Appointment.Birthday).
const QStringList& nonMailContainerPrefixes() {
    static const QStringList kPrefixes = {
        QStringLiteral("IPF.Contact"),
        QStringLiteral("IPF.Appointment"),
        QStringLiteral("IPF.Task"),
        QStringLiteral("IPF.StickyNote"),
        QStringLiteral("IPF.Journal"),
    };
    return kPrefixes;
}

/// Outlook/Exchange bookkeeping folders that carry no user mail.
const QStringList& nonMailFolderNames() {
    static const QStringList kNames = {
        QStringLiteral("PersonMetadata"),
        QStringLiteral("MeContact"),
        QStringLiteral("ExternalContacts"),
        QStringLiteral("Quick Step Settings"),
        QStringLiteral("Conversation Action Settings"),
        QStringLiteral("Yammer Root"),
        QStringLiteral("Social Activity Notifications"),
        QStringLiteral("Conversation History"),
        QStringLiteral("Files"),
        QStringLiteral("Sync Issues"),
        QStringLiteral("Conflicts"),
        QStringLiteral("Local Failures"),
        QStringLiteral("Server Failures"),
    };
    return kNames;
}

const PstFolder* findFlaggedIpmSubtree(const QVector<PstFolder>& folders) {
    for (const auto& folder : folders) {
        if (folder.is_ipm_subtree) {
            return &folder;
        }
        if (const PstFolder* found = findFlaggedIpmSubtree(folder.children)) {
            return found;
        }
    }
    return nullptr;
}

void collectInto(const PstFolder& folder, QVector<uint64_t>& out) {
    if (!isMailFolder(folder.display_name, folder.container_class) &&
        !holdsSystemFolderEmails(folder)) {
        return;
    }
    out.append(folder.node_id);
    for (const auto& child : folder.children) {
        collectInto(child, out);
    }
}

void collectOutside(const PstFolder& folder, const PstFolder* ipm, QVector<const PstFolder*>& out) {
    if (&folder == ipm) {
        return;
    }
    if (folder.system_folder_emails > 0) {
        out.append(&folder);
    }
    for (const auto& child : folder.children) {
        collectOutside(child, ipm, out);
    }
}

}  // namespace

bool isRealEmail(EmailItemType type) {
    return type == EmailItemType::Email || type == EmailItemType::MeetingRequest;
}

bool holdsSystemFolderEmails(const PstFolder& folder) {
    return folder.system_folder_emails > 0 ||
           std::ranges::any_of(folder.children, [](const PstFolder& child) {
               return holdsSystemFolderEmails(child);
           });
}

QVector<const PstFolder*> foldersWithMailOutsideMailTree(const QVector<PstFolder>& tree) {
    QVector<const PstFolder*> found;
    const PstFolder* ipm = findIpmSubtree(tree);
    if (ipm == nullptr || ipm->children.isEmpty()) {
        return found;  // With no mail tree every folder is listed from the roots already
    }
    for (const auto& root : tree) {
        collectOutside(root, ipm, found);
    }
    return found;
}

bool isMailFolder(const QString& display_name, const QString& container_class) {
    const bool has_non_mail_prefix =
        std::ranges::any_of(nonMailContainerPrefixes(), [&container_class](const QString& prefix) {
            return container_class.startsWith(prefix);
        });
    if (has_non_mail_prefix) {
        return false;
    }
    if (container_class == QLatin1String("IPF.Configuration")) {
        return false;
    }
    return std::ranges::none_of(nonMailFolderNames(), [&display_name](const QString& name) {
        return display_name.compare(name, Qt::CaseInsensitive) == 0;
    });
}

static const PstFolder* findNamedIpmSubtree(const QVector<PstFolder>& folders);

bool isTaskNoteOrJournalFolder(const QString& display_name, const QString& container_class) {
    const bool holds_them = container_class.startsWith(QLatin1String("IPF.Task")) ||
                            container_class.startsWith(QLatin1String("IPF.StickyNote")) ||
                            container_class.startsWith(QLatin1String("IPF.Journal"));
    // Exchange's own bookkeeping folders can carry these classes too.
    return holds_them &&
           std::ranges::none_of(nonMailFolderNames(), [&display_name](const QString& n) {
               return display_name.compare(n, Qt::CaseInsensitive) == 0;
           });
}

const PstFolder* findFolderByNodeId(const QVector<PstFolder>& tree,
                                    std::optional<uint64_t> node_id) {
    if (!node_id.has_value()) {
        return nullptr;
    }
    for (const auto& folder : tree) {
        if (folder.node_id == *node_id) {
            return &folder;
        }
        if (const PstFolder* found = findFolderByNodeId(folder.children, node_id)) {
            return found;
        }
    }
    return nullptr;
}

const PstFolder* findIpmSubtree(const QVector<PstFolder>& folders) {
    // The store's own pointer wins: the folder's name is localized and varies by Outlook version.
    if (const PstFolder* flagged = findFlaggedIpmSubtree(folders)) {
        return flagged;
    }
    return findNamedIpmSubtree(folders);
}

static const PstFolder* findNamedIpmSubtree(const QVector<PstFolder>& folders) {
    const PstFolder* best = nullptr;

    for (const auto& folder : folders) {
        if (folder.display_name == QLatin1String("IPM_SUBTREE") ||
            folder.display_name == QLatin1String("Top of Information Store")) {
            if (!folder.children.isEmpty()) {
                return &folder;
            }
            if (best == nullptr) {
                best = &folder;
            }
        }
        const PstFolder* found = findNamedIpmSubtree(folder.children);
        if (found != nullptr && !found->children.isEmpty()) {
            return found;
        }
        if (found != nullptr && best == nullptr) {
            best = found;
        }
    }
    return best;
}

QVector<uint64_t> mailFolderNodeIds(const QVector<PstFolder>& tree) {
    QVector<uint64_t> ids;
    const PstFolder* ipm = findIpmSubtree(tree);
    const QVector<PstFolder>& source = (ipm != nullptr && !ipm->children.isEmpty()) ? ipm->children
                                                                                    : tree;
    for (const auto& folder : source) {
        collectInto(folder, ids);
    }
    for (const PstFolder* outside : foldersWithMailOutsideMailTree(tree)) {
        ids.append(outside->node_id);
    }
    return ids;
}

}  // namespace sak
