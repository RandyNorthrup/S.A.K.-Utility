// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_session_store.h"

#include "sak/logger.h"

#include <QSettings>

#include <algorithm>

namespace sak {
namespace {

constexpr auto kActiveIndexKey = "activeIndex";
constexpr auto kTabsArray = "tabs";

void writePane(QSettings& settings, const QString& prefix, const FileExplorerPaneState& pane) {
    settings.setValue(prefix + QStringLiteral("targetId"), pane.location.target_id.value);
    settings.setValue(prefix + QStringLiteral("path"), pane.location.path);
    settings.setValue(prefix + QStringLiteral("viewMode"), static_cast<int>(pane.view.mode));
    settings.setValue(prefix + QStringLiteral("showHidden"), pane.view.show_hidden);
    settings.setValue(prefix + QStringLiteral("showExtensions"), pane.view.show_extensions);
    settings.setValue(prefix + QStringLiteral("detailsSize"), pane.view.sizes.details);
    settings.setValue(prefix + QStringLiteral("listSize"), pane.view.sizes.list);
    settings.setValue(prefix + QStringLiteral("cardsSize"), pane.view.sizes.cards);
    settings.setValue(prefix + QStringLiteral("gridSize"), pane.view.sizes.grid);
    settings.setValue(prefix + QStringLiteral("columnsSize"), pane.view.sizes.columns);
    settings.setValue(prefix + QStringLiteral("sortKey"), pane.view.sort_key);
    settings.setValue(prefix + QStringLiteral("sortOrder"), static_cast<int>(pane.view.sort_order));
    settings.setValue(prefix + QStringLiteral("groupOption"),
                      static_cast<int>(pane.view.group_option));
    settings.setValue(prefix + QStringLiteral("groupOrder"),
                      static_cast<int>(pane.view.group_order));
    settings.setValue(prefix + QStringLiteral("groupDateUnit"),
                      static_cast<int>(pane.view.group_date_unit));
    settings.setValue(prefix + QStringLiteral("folderPlacement"),
                      static_cast<int>(pane.view.folder_placement));
}

// Cast a persisted int to enum E only when it names a valid enumerator in the
// contiguous [0, max_valid] range; a corrupt or forward-version settings value
// falls back to `fallback` rather than producing an out-of-range enum that later
// switches would mishandle (B8-24).
template <typename E>
E validatedEnum(const int value, const E max_valid, const E fallback) {
    return (value >= 0 && value <= static_cast<int>(max_valid)) ? static_cast<E>(value) : fallback;
}

FileExplorerPaneState readPane(QSettings& settings, const QString& prefix) {
    FileExplorerPaneState pane;
    pane.location.target_id.value = settings.value(prefix + QStringLiteral("targetId")).toString();
    pane.location.path = settings.value(prefix + QStringLiteral("path")).toString();
    pane.view.mode = validatedEnum(settings
                                       .value(prefix + QStringLiteral("viewMode"),
                                              static_cast<int>(FileExplorerViewMode::Details))
                                       .toInt(),
                                   FileExplorerViewMode::Adaptive,
                                   FileExplorerViewMode::Details);
    pane.view.show_hidden = settings.value(prefix + QStringLiteral("showHidden"), false).toBool();
    pane.view.show_extensions =
        settings.value(prefix + QStringLiteral("showExtensions"), true).toBool();
    const FileExplorerLayoutSizes size_defaults;
    pane.view.sizes.details =
        settings.value(prefix + QStringLiteral("detailsSize"), size_defaults.details).toInt();
    pane.view.sizes.list =
        settings.value(prefix + QStringLiteral("listSize"), size_defaults.list).toInt();
    pane.view.sizes.cards =
        settings.value(prefix + QStringLiteral("cardsSize"), size_defaults.cards).toInt();
    pane.view.sizes.grid =
        settings.value(prefix + QStringLiteral("gridSize"), size_defaults.grid).toInt();
    pane.view.sizes.columns =
        settings.value(prefix + QStringLiteral("columnsSize"), size_defaults.columns).toInt();
    pane.view.sort_key =
        settings.value(prefix + QStringLiteral("sortKey"), QStringLiteral("name")).toString();
    pane.view.sort_order = validatedEnum(
        settings.value(prefix + QStringLiteral("sortOrder"), static_cast<int>(Qt::AscendingOrder))
            .toInt(),
        Qt::DescendingOrder,
        Qt::AscendingOrder);
    pane.view.group_option =
        validatedEnum(settings
                          .value(prefix + QStringLiteral("groupOption"),
                                 static_cast<int>(FileExplorerGroupOption::None))
                          .toInt(),
                      FileExplorerGroupOption::FileTag,
                      FileExplorerGroupOption::None);
    pane.view.group_order = validatedEnum(
        settings.value(prefix + QStringLiteral("groupOrder"), static_cast<int>(Qt::AscendingOrder))
            .toInt(),
        Qt::DescendingOrder,
        Qt::AscendingOrder);
    pane.view.group_date_unit =
        validatedEnum(settings
                          .value(prefix + QStringLiteral("groupDateUnit"),
                                 static_cast<int>(FileExplorerGroupDateUnit::Year))
                          .toInt(),
                      FileExplorerGroupDateUnit::Day,
                      FileExplorerGroupDateUnit::Year);
    pane.view.folder_placement =
        validatedEnum(settings
                          .value(prefix + QStringLiteral("folderPlacement"),
                                 static_cast<int>(FileExplorerFolderSortPlacement::FoldersFirst))
                          .toInt(),
                      FileExplorerFolderSortPlacement::Together,
                      FileExplorerFolderSortPlacement::FoldersFirst);
    return pane;
}

}  // namespace

void FileExplorerSessionStore::save(QSettings& settings,
                                    const QString& group,
                                    const FileExplorerTabSession& session) {
    // Fail closed on an empty group: beginGroup("") + remove(QString()) below would target the
    // root QSettings scope and wipe unrelated application settings.
    if (group.isEmpty()) {
        return;
    }
    settings.beginGroup(group);
    settings.remove(QString());  // drop any prior session under this group
    settings.setValue(QString::fromLatin1(kActiveIndexKey), session.active_index);
    settings.beginWriteArray(QString::fromLatin1(kTabsArray),
                             static_cast<int>(session.tabs.size()));
    for (int i = 0; i < session.tabs.size(); ++i) {
        settings.setArrayIndex(i);
        const FileExplorerTabState& tab = session.tabs.at(i);
        settings.setValue(QStringLiteral("title"), tab.title);
        settings.setValue(QStringLiteral("split"), static_cast<int>(tab.split));
        settings.setValue(QStringLiteral("secondaryEnabled"), tab.secondary_pane_enabled);
        settings.setValue(QStringLiteral("activePane"), tab.active_pane_index);
        writePane(settings, QStringLiteral("primary/"), tab.primary);
        writePane(settings, QStringLiteral("secondary/"), tab.secondary);
    }
    settings.endArray();
    settings.endGroup();
    // QSettings buffers writes; sync() flushes them and exposes a real disk/registry failure that
    // would otherwise be reported as a successful save.
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        sak::logWarning(
            "FileExplorerSessionStore::save could not durably persist group '{}': QSettings "
            "status {}",
            group.toStdString(),
            static_cast<int>(settings.status()));
    }
}

FileExplorerTabSession FileExplorerSessionStore::load(QSettings& settings, const QString& group) {
    FileExplorerTabSession session;
    // An empty group would read from the root scope; refuse and return an empty session.
    if (group.isEmpty()) {
        return session;
    }
    settings.beginGroup(group);
    // Cap the declared tab count: settings are attacker-writable, and a huge array size would
    // otherwise drive an unbounded read loop. Clamp the work instead of trusting the count.
    constexpr int kMaxSessionTabs = 512;
    const int count =
        std::clamp(settings.beginReadArray(QString::fromLatin1(kTabsArray)), 0, kMaxSessionTabs);
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        FileExplorerTabState tab;
        tab.title = settings.value(QStringLiteral("title")).toString();
        tab.split = validatedEnum(
            settings.value(QStringLiteral("split"), static_cast<int>(FileExplorerPaneSplit::None))
                .toInt(),
            FileExplorerPaneSplit::Horizontal,
            FileExplorerPaneSplit::None);
        tab.secondary_pane_enabled =
            settings.value(QStringLiteral("secondaryEnabled"), false).toBool();
        tab.active_pane_index = settings.value(QStringLiteral("activePane"), 0).toInt();
        tab.primary = readPane(settings, QStringLiteral("primary/"));
        tab.secondary = readPane(settings, QStringLiteral("secondary/"));
        session.tabs.append(tab);
    }
    settings.endArray();
    session.active_index = settings.value(QString::fromLatin1(kActiveIndexKey), 0).toInt();
    settings.endGroup();

    // A read error (inaccessible or malformed backing store) otherwise hides behind a plausible
    // empty/partial session; surface it rather than silently returning valid-looking state.
    if (settings.status() != QSettings::NoError) {
        sak::logWarning(
            "FileExplorerSessionStore::load hit a read error for group '{}': QSettings status {}",
            group.toStdString(),
            static_cast<int>(settings.status()));
    }

    if (!session.tabs.isEmpty()) {
        session.active_index =
            std::clamp(session.active_index, 0, static_cast<int>(session.tabs.size()) - 1);
    } else {
        session.active_index = 0;
    }
    return session;
}

void FileExplorerSessionStore::clear(QSettings& settings, const QString& group) {
    // Fail closed on an empty group: remove(QString()) under the root scope would erase unrelated
    // application settings rather than just this session.
    if (group.isEmpty()) {
        return;
    }
    settings.beginGroup(group);
    settings.remove(QString());
    settings.endGroup();
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        sak::logWarning(
            "FileExplorerSessionStore::clear could not durably remove group '{}': QSettings "
            "status {}",
            group.toStdString(),
            static_cast<int>(settings.status()));
    }
}

}  // namespace sak
