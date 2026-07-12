// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_tag_store.h"

#include <QCryptographicHash>
#include <QSettings>

#include <algorithm>

namespace sak {
namespace {

// Content-addressed sub-group key for one item, so arbitrary path characters never
// break QSettings key parsing.
QString itemKey(const QString& target_id, const QString& path) {
    const QString raw = QStringLiteral("%1\n%2").arg(target_id, path);
    return QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QStringList normalizeTags(const QStringList& tags) {
    QStringList cleaned;
    for (const QString& tag : tags) {
        const QString trimmed = tag.trimmed();
        if (!trimmed.isEmpty() && !cleaned.contains(trimmed, Qt::CaseInsensitive)) {
            cleaned.append(trimmed);
        }
    }
    std::sort(cleaned.begin(), cleaned.end(), [](const QString& a, const QString& b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    return cleaned;
}

}  // namespace

QStringList FileExplorerTagStore::tagsFor(QSettings& settings,
                                          const QString& group,
                                          const QString& target_id,
                                          const QString& path) {
    settings.beginGroup(group);
    settings.beginGroup(itemKey(target_id, path));
    const QStringList tags = settings.value(QStringLiteral("tags")).toStringList();
    settings.endGroup();
    settings.endGroup();
    return normalizeTags(tags);
}

void FileExplorerTagStore::setTags(QSettings& settings,
                                   const QString& group,
                                   const QString& target_id,
                                   const QString& path,
                                   const QStringList& tags) {
    const QStringList cleaned = normalizeTags(tags);
    settings.beginGroup(group);
    const QString key = itemKey(target_id, path);
    if (cleaned.isEmpty()) {
        settings.remove(key);
    } else {
        settings.beginGroup(key);
        settings.setValue(QStringLiteral("targetId"), target_id);
        settings.setValue(QStringLiteral("path"), path);
        settings.setValue(QStringLiteral("tags"), cleaned);
        settings.endGroup();
    }
    settings.endGroup();
}

QStringList FileExplorerTagStore::allTags(QSettings& settings, const QString& group) {
    settings.beginGroup(group);
    const QStringList keys = settings.childGroups();
    QStringList tags;
    for (const QString& key : keys) {
        settings.beginGroup(key);
        tags.append(settings.value(QStringLiteral("tags")).toStringList());
        settings.endGroup();
    }
    settings.endGroup();
    return normalizeTags(tags);
}

QVector<FileExplorerTaggedItem> FileExplorerTagStore::itemsWithTag(QSettings& settings,
                                                                   const QString& group,
                                                                   const QString& tag) {
    const QString needle = tag.trimmed();
    settings.beginGroup(group);
    const QStringList keys = settings.childGroups();
    QVector<FileExplorerTaggedItem> items;
    for (const QString& key : keys) {
        settings.beginGroup(key);
        const QStringList tags = settings.value(QStringLiteral("tags")).toStringList();
        if (tags.contains(needle, Qt::CaseInsensitive)) {
            FileExplorerTaggedItem item;
            item.target_id = settings.value(QStringLiteral("targetId")).toString();
            item.path = settings.value(QStringLiteral("path")).toString();
            item.tags = normalizeTags(tags);
            items.append(item);
        }
        settings.endGroup();
    }
    settings.endGroup();
    return items;
}

}  // namespace sak
