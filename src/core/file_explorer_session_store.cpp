// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_session_store.h"

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
}

FileExplorerPaneState readPane(QSettings& settings, const QString& prefix) {
    FileExplorerPaneState pane;
    pane.location.target_id.value = settings.value(prefix + QStringLiteral("targetId")).toString();
    pane.location.path = settings.value(prefix + QStringLiteral("path")).toString();
    pane.view.mode = static_cast<FileExplorerViewMode>(
        settings
            .value(prefix + QStringLiteral("viewMode"),
                   static_cast<int>(FileExplorerViewMode::Details))
            .toInt());
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
    pane.view.sort_order = static_cast<Qt::SortOrder>(
        settings.value(prefix + QStringLiteral("sortOrder"), static_cast<int>(Qt::AscendingOrder))
            .toInt());
    pane.view.group_option = static_cast<FileExplorerGroupOption>(
        settings
            .value(prefix + QStringLiteral("groupOption"),
                   static_cast<int>(FileExplorerGroupOption::None))
            .toInt());
    pane.view.group_order = static_cast<Qt::SortOrder>(
        settings.value(prefix + QStringLiteral("groupOrder"), static_cast<int>(Qt::AscendingOrder))
            .toInt());
    pane.view.group_date_unit = static_cast<FileExplorerGroupDateUnit>(
        settings
            .value(prefix + QStringLiteral("groupDateUnit"),
                   static_cast<int>(FileExplorerGroupDateUnit::Year))
            .toInt());
    return pane;
}

}  // namespace

void FileExplorerSessionStore::save(QSettings& settings,
                                    const QString& group,
                                    const FileExplorerTabSession& session) {
    settings.beginGroup(group);
    settings.remove(QString());  // drop any prior session under this group
    settings.setValue(QString::fromLatin1(kActiveIndexKey), session.active_index);
    settings.beginWriteArray(QString::fromLatin1(kTabsArray), session.tabs.size());
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
}

FileExplorerTabSession FileExplorerSessionStore::load(QSettings& settings, const QString& group) {
    FileExplorerTabSession session;
    settings.beginGroup(group);
    const int count = settings.beginReadArray(QString::fromLatin1(kTabsArray));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        FileExplorerTabState tab;
        tab.title = settings.value(QStringLiteral("title")).toString();
        tab.split = static_cast<FileExplorerPaneSplit>(
            settings.value(QStringLiteral("split"), static_cast<int>(FileExplorerPaneSplit::None))
                .toInt());
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

    if (!session.tabs.isEmpty()) {
        session.active_index =
            std::clamp(session.active_index, 0, static_cast<int>(session.tabs.size()) - 1);
    } else {
        session.active_index = 0;
    }
    return session;
}

void FileExplorerSessionStore::clear(QSettings& settings, const QString& group) {
    settings.beginGroup(group);
    settings.remove(QString());
    settings.endGroup();
}

}  // namespace sak
