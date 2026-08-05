// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_folder_selection.cpp
/// @brief Mail-folder classification and node-id collection for an opened store.

#include "sak/email_folder_selection.h"

#include <QStringList>

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

void collectInto(const PstFolder& folder, QVector<uint64_t>& out) {
    if (!isMailFolder(folder.display_name, folder.container_class)) {
        return;
    }
    out.append(folder.node_id);
    for (const auto& child : folder.children) {
        collectInto(child, out);
    }
}

}  // namespace

bool isMailFolder(const QString& display_name, const QString& container_class) {
    for (const QString& prefix : nonMailContainerPrefixes()) {
        if (container_class.startsWith(prefix)) {
            return false;
        }
    }
    if (container_class == QLatin1String("IPF.Configuration")) {
        return false;
    }
    for (const QString& name : nonMailFolderNames()) {
        if (display_name.compare(name, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

const PstFolder* findIpmSubtree(const QVector<PstFolder>& folders) {
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
        const PstFolder* found = findIpmSubtree(folder.children);
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
    return ids;
}

}  // namespace sak
