// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_session_store.cpp
/// @brief Round-trip tests for File Explorer tab session persistence.

#include "sak/file_explorer_session_store.h"

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

sak::FileExplorerTabState makeTab(const QString& title,
                                  const QString& target_id,
                                  const QString& path,
                                  const sak::FileExplorerViewMode mode) {
    sak::FileExplorerTabState tab;
    tab.title = title;
    tab.primary.location.target_id.value = target_id;
    tab.primary.location.path = path;
    tab.primary.view.mode = mode;
    // Transient state that must NOT be persisted.
    tab.primary.back_stack.append(sak::FileExplorerLocation{{target_id}, QStringLiteral("/old")});
    return tab;
}

}  // namespace

class FileExplorerSessionStoreTests : public QObject {
    Q_OBJECT

private:
    QString settingsPath(const QTemporaryDir& dir) const {
        return QDir(dir.path()).filePath(QStringLiteral("session.ini"));
    }

private Q_SLOTS:
    void savesAndLoadsDurableTabFields() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        sak::FileExplorerTabSession session;
        session.tabs.append(makeTab(QStringLiteral("Docs"),
                                    QStringLiteral("disk:2:partition:1"),
                                    QStringLiteral("/docs"),
                                    sak::FileExplorerViewMode::Grid));
        session.tabs.append(makeTab(QStringLiteral("Home"),
                                    QStringLiteral("local:C:"),
                                    QStringLiteral("C:/Users"),
                                    sak::FileExplorerViewMode::List));
        session.tabs[0].primary.view.show_hidden = true;
        session.tabs[0].primary.view.sizes.grid = 12;
        session.tabs[0].primary.view.sizes.details = 5;
        session.tabs[0].primary.view.group_option = sak::FileExplorerGroupOption::Size;
        session.tabs[0].primary.view.group_order = Qt::DescendingOrder;
        session.tabs[0].primary.view.group_date_unit = sak::FileExplorerGroupDateUnit::Month;
        session.tabs[1].secondary_pane_enabled = true;
        session.tabs[1].secondary.location.target_id.value = QStringLiteral("local:D:");
        session.tabs[1].secondary.location.path = QStringLiteral("D:/data");
        session.tabs[1].split = sak::FileExplorerPaneSplit::Vertical;
        session.tabs[1].active_pane_index = 1;
        session.active_index = 1;

        {
            QSettings settings(settingsPath(dir), QSettings::IniFormat);
            sak::FileExplorerSessionStore::save(settings, QStringLiteral("Session"), session);
        }

        QSettings settings(settingsPath(dir), QSettings::IniFormat);
        const auto loaded = sak::FileExplorerSessionStore::load(settings,
                                                                QStringLiteral("Session"));

        QCOMPARE(loaded.tabs.size(), 2);
        QCOMPARE(loaded.active_index, 1);

        QCOMPARE(loaded.tabs[0].title, QStringLiteral("Docs"));
        QCOMPARE(loaded.tabs[0].primary.location.target_id.value,
                 QStringLiteral("disk:2:partition:1"));
        QCOMPARE(loaded.tabs[0].primary.location.path, QStringLiteral("/docs"));
        QCOMPARE(loaded.tabs[0].primary.view.mode, sak::FileExplorerViewMode::Grid);
        QVERIFY(loaded.tabs[0].primary.view.show_hidden);
        // A hardcoded `true` satisfies the line above: the tab that never enabled the flag, and
        // the unused secondary pane, must come back false (per pane, per tab).
        QVERIFY(!loaded.tabs[1].primary.view.show_hidden);
        QVERIFY(!loaded.tabs[0].secondary.view.show_hidden);
        QCOMPARE(loaded.tabs[0].primary.view.sizes.grid, 12);
        QCOMPARE(loaded.tabs[0].primary.view.sizes.details, 5);
        // A size that equals its default proves nothing -- readPane's fallback yields the same
        // number whether or not writePane persisted the key.
        QCOMPARE(loaded.tabs[0].primary.view.sizes.cards, sak::FileExplorerLayoutSizes{}.cards);
        QCOMPARE(loaded.tabs[0].primary.view.group_option, sak::FileExplorerGroupOption::Size);
        QCOMPARE(loaded.tabs[0].primary.view.group_order, Qt::DescendingOrder);
        QCOMPARE(loaded.tabs[0].primary.view.group_date_unit,
                 sak::FileExplorerGroupDateUnit::Month);
        QCOMPARE(loaded.tabs[1].primary.view.group_option, sak::FileExplorerGroupOption::None);
        // Transient history is intentionally dropped.
        QVERIFY(loaded.tabs[0].primary.back_stack.isEmpty());

        QCOMPARE(loaded.tabs[1].primary.view.mode, sak::FileExplorerViewMode::List);
        QVERIFY(loaded.tabs[1].secondary_pane_enabled);
        verifyPersistedLocations(loaded);
        QCOMPARE(loaded.tabs[1].split, sak::FileExplorerPaneSplit::Vertical);
        QCOMPARE(loaded.tabs[1].active_pane_index, 1);
    }

    // A location is target_id + path, and only one half of one pane was ever compared. The
    // unused secondary pane of tab 0 must also stay EMPTY rather than inherit the primary
    // pane's location.
    static void verifyPersistedLocations(const sak::FileExplorerTabSession& loaded) {
        QCOMPARE(loaded.tabs[1].secondary.location.path, QStringLiteral("D:/data"));
        QCOMPARE(loaded.tabs[1].secondary.location.target_id.value, QStringLiteral("local:D:"));
        QCOMPARE(loaded.tabs[1].primary.location.target_id.value, QStringLiteral("local:C:"));
        QCOMPARE(loaded.tabs[1].primary.location.path, QStringLiteral("C:/Users"));
        QCOMPARE(loaded.tabs[0].secondary.location.target_id.value, QString());
        QCOMPARE(loaded.tabs[0].secondary.location.path, QString());
    }

    void loadReturnsEmptyWhenNoSessionStored() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QSettings settings(settingsPath(dir), QSettings::IniFormat);
        const auto loaded = sak::FileExplorerSessionStore::load(settings,
                                                                QStringLiteral("Missing"));
        QVERIFY(loaded.isEmpty());
        QCOMPARE(loaded.active_index, 0);
    }

    void saveReplacesPriorSessionAndClampsActiveIndex() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Session");

        sak::FileExplorerTabSession first;
        first.tabs.append(makeTab(QStringLiteral("A"),
                                  QStringLiteral("t1"),
                                  QStringLiteral("/a"),
                                  sak::FileExplorerViewMode::Details));
        first.tabs.append(makeTab(QStringLiteral("B"),
                                  QStringLiteral("t2"),
                                  QStringLiteral("/b"),
                                  sak::FileExplorerViewMode::Details));
        first.tabs.append(makeTab(QStringLiteral("C"),
                                  QStringLiteral("t3"),
                                  QStringLiteral("/c"),
                                  sak::FileExplorerViewMode::Details));

        // A single-tab session with an out-of-range active index must replace the
        // three-tab session entirely, and the index is clamped on load.
        sak::FileExplorerTabSession second;
        second.tabs.append(makeTab(QStringLiteral("Only"),
                                   QStringLiteral("t9"),
                                   QStringLiteral("/only"),
                                   sak::FileExplorerViewMode::Cards));
        second.active_index = 5;

        {
            QSettings settings(settingsPath(dir), QSettings::IniFormat);
            sak::FileExplorerSessionStore::save(settings, group, first);
            sak::FileExplorerSessionStore::save(settings, group, second);
        }

        QSettings settings(settingsPath(dir), QSettings::IniFormat);
        const auto loaded = sak::FileExplorerSessionStore::load(settings, group);
        QCOMPARE(loaded.tabs.size(), 1);
        QCOMPARE(loaded.tabs[0].title, QStringLiteral("Only"));
        QCOMPARE(loaded.active_index, 0);

        // With a single tab, clamping, wrapping and collapsing-to-zero are indistinguishable.
        // Pin the real bound: an out-of-range index over the THREE-tab session must land on the
        // LAST tab, and a negative one on the first.
        first.active_index = 99;
        sak::FileExplorerSessionStore::save(settings, group, first);
        const auto high = sak::FileExplorerSessionStore::load(settings, group);
        QCOMPARE(high.tabs.size(), 3);
        QCOMPARE(high.active_index, 2);
        QCOMPARE(high.tabs[2].title, QStringLiteral("C"));
        first.active_index = -4;
        sak::FileExplorerSessionStore::save(settings, group, first);
        const auto low = sak::FileExplorerSessionStore::load(settings, group);
        QCOMPARE(low.tabs.size(), 3);
        QCOMPARE(low.active_index, 0);
    }

    void corruptEnumValuesFallBackToDefaults() {
        // B8-24: a persisted enum int outside the valid range (a corrupt or
        // forward-version settings file) must fall back to the default instead
        // of yielding an out-of-range enum that later switches would mishandle.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Session");

        sak::FileExplorerTabSession session;
        session.tabs.append(makeTab(QStringLiteral("A"),
                                    QStringLiteral("t1"),
                                    QStringLiteral("/a"),
                                    sak::FileExplorerViewMode::Grid));
        session.tabs[0].primary.view.group_option = sak::FileExplorerGroupOption::Size;
        {
            QSettings settings(settingsPath(dir), QSettings::IniFormat);
            sak::FileExplorerSessionStore::save(settings, group, session);
        }

        // Overwrite the persisted enum keys for tab 0's primary pane with
        // out-of-range ints (QSettings arrays are 1-based).
        {
            QSettings settings(settingsPath(dir), QSettings::IniFormat);
            settings.setValue(QStringLiteral("Session/tabs/1/primary/viewMode"), 999);
            settings.setValue(QStringLiteral("Session/tabs/1/primary/groupOption"), -3);
            settings.setValue(QStringLiteral("Session/tabs/1/primary/groupDateUnit"), 42);
            settings.setValue(QStringLiteral("Session/tabs/1/primary/folderPlacement"), 77);
            settings.setValue(QStringLiteral("Session/tabs/1/primary/sortOrder"), 5);
        }

        QSettings settings(settingsPath(dir), QSettings::IniFormat);
        const auto loaded = sak::FileExplorerSessionStore::load(settings, group);
        QCOMPARE(loaded.tabs.size(), 1);
        const auto& view = loaded.tabs[0].primary.view;
        QCOMPARE(view.mode, sak::FileExplorerViewMode::Details);
        QCOMPARE(view.group_option, sak::FileExplorerGroupOption::None);
        QCOMPARE(view.group_date_unit, sak::FileExplorerGroupDateUnit::Year);
        QCOMPARE(view.folder_placement, sak::FileExplorerFolderSortPlacement::FoldersFirst);
        QCOMPARE(view.sort_order, Qt::AscendingOrder);

        // Falling back to a default must be a decision ABOUT the persisted value, not a
        // constant: the same keys have to carry an in-range NON-default value through a clean
        // round trip, or a reader that ignored the store entirely passes every line above.
        sak::FileExplorerTabSession valid;
        valid.tabs.append(makeTab(QStringLiteral("V"),
                                  QStringLiteral("t2"),
                                  QStringLiteral("/v"),
                                  sak::FileExplorerViewMode::Columns));
        valid.tabs[0].primary.view.folder_placement =
            sak::FileExplorerFolderSortPlacement::FilesFirst;
        valid.tabs[0].primary.view.sort_order = Qt::DescendingOrder;
        valid.tabs[0].primary.view.sort_key = QStringLiteral("size");
        valid.tabs[0].primary.view.show_extensions = false;
        sak::FileExplorerSessionStore::save(settings, QStringLiteral("Valid"), valid);
        const auto reloaded = sak::FileExplorerSessionStore::load(settings,
                                                                  QStringLiteral("Valid"));
        QCOMPARE(reloaded.tabs.size(), 1);
        const auto& reloaded_view = reloaded.tabs[0].primary.view;
        QCOMPARE(reloaded_view.mode, sak::FileExplorerViewMode::Columns);
        QCOMPARE(reloaded_view.folder_placement, sak::FileExplorerFolderSortPlacement::FilesFirst);
        QCOMPARE(reloaded_view.sort_order, Qt::DescendingOrder);
        QCOMPARE(reloaded_view.sort_key, QStringLiteral("size"));
        QVERIFY(!reloaded_view.show_extensions);
    }

    void clearRemovesStoredSession() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Session");

        sak::FileExplorerTabSession session;
        session.tabs.append(makeTab(QStringLiteral("A"),
                                    QStringLiteral("t1"),
                                    QStringLiteral("/a"),
                                    sak::FileExplorerViewMode::Details));

        {
            QSettings settings(settingsPath(dir), QSettings::IniFormat);
            sak::FileExplorerSessionStore::save(settings, group, session);
            sak::FileExplorerSessionStore::clear(settings, group);
        }

        QSettings settings(settingsPath(dir), QSettings::IniFormat);
        QVERIFY(sak::FileExplorerSessionStore::load(settings, group).isEmpty());

        // clear() brackets remove(QString()) between beginGroup/endGroup, and that scoping is
        // the whole contract: dropping the bracket wipes the ROOT scope, which satisfies the
        // emptiness check above while destroying every unrelated setting sharing this QSettings.
        sak::FileExplorerSessionStore::save(settings, group, session);
        sak::FileExplorerSessionStore::save(settings, QStringLiteral("Other"), session);
        settings.setValue(QStringLiteral("Unrelated/keep"), QStringLiteral("intact"));
        sak::FileExplorerSessionStore::clear(settings, group);
        QVERIFY(sak::FileExplorerSessionStore::load(settings, group).isEmpty());
        QVERIFY(settings.allKeys().filter(group + QStringLiteral("/")).isEmpty());
        const auto other = sak::FileExplorerSessionStore::load(settings, QStringLiteral("Other"));
        QCOMPARE(other.tabs.size(), 1);
        QCOMPARE(other.tabs[0].title, QStringLiteral("A"));
        QCOMPARE(settings.value(QStringLiteral("Unrelated/keep")).toString(),
                 QStringLiteral("intact"));
    }
};

QTEST_MAIN(FileExplorerSessionStoreTests)
#include "test_file_explorer_session_store.moc"
