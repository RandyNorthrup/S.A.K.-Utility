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
        session.tabs[0].primary.view.item_size_px = 96;
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
        QCOMPARE(loaded.tabs[0].primary.view.item_size_px, 96);
        // Transient history is intentionally dropped.
        QVERIFY(loaded.tabs[0].primary.back_stack.isEmpty());

        QCOMPARE(loaded.tabs[1].primary.view.mode, sak::FileExplorerViewMode::List);
        QVERIFY(loaded.tabs[1].secondary_pane_enabled);
        QCOMPARE(loaded.tabs[1].secondary.location.path, QStringLiteral("D:/data"));
        QCOMPARE(loaded.tabs[1].split, sak::FileExplorerPaneSplit::Vertical);
        QCOMPARE(loaded.tabs[1].active_pane_index, 1);
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
    }
};

QTEST_MAIN(FileExplorerSessionStoreTests)
#include "test_file_explorer_session_store.moc"
