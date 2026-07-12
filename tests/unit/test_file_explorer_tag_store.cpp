// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_tag_store.cpp
/// @brief Tests for app-level File Explorer item tag persistence.

#include "sak/file_explorer_tag_store.h"

#include <QDir>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

class FileExplorerTagStoreTests : public QObject {
    Q_OBJECT

private:
    QString iniPath(const QTemporaryDir& dir) const {
        return QDir(dir.path()).filePath(QStringLiteral("tags.ini"));
    }

private Q_SLOTS:
    void setAndReadTagsNormalizesAndSorts() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Tags");

        {
            QSettings settings(iniPath(dir), QSettings::IniFormat);
            sak::FileExplorerTagStore::setTags(settings,
                                               group,
                                               QStringLiteral("disk:2:partition:1"),
                                               QStringLiteral("/docs/report.pdf"),
                                               QStringList{QStringLiteral("  work "),
                                                           QStringLiteral("urgent"),
                                                           QStringLiteral("work"),
                                                           QString()});
        }

        QSettings settings(iniPath(dir), QSettings::IniFormat);
        const QStringList tags =
            sak::FileExplorerTagStore::tagsFor(settings,
                                               group,
                                               QStringLiteral("disk:2:partition:1"),
                                               QStringLiteral("/docs/report.pdf"));
        // Trimmed, de-duplicated (case-insensitive), sorted.
        QCOMPARE(tags, (QStringList{QStringLiteral("urgent"), QStringLiteral("work")}));
    }

    void tagsAreKeyedByTargetAndPath() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Tags");

        {
            QSettings settings(iniPath(dir), QSettings::IniFormat);
            sak::FileExplorerTagStore::setTags(settings,
                                               group,
                                               QStringLiteral("t1"),
                                               QStringLiteral("/a"),
                                               QStringList{QStringLiteral("red")});
            sak::FileExplorerTagStore::setTags(settings,
                                               group,
                                               QStringLiteral("t2"),
                                               QStringLiteral("/a"),
                                               QStringList{QStringLiteral("blue")});
        }

        QSettings settings(iniPath(dir), QSettings::IniFormat);
        // Same path, different target id => independent tag sets.
        QCOMPARE(sak::FileExplorerTagStore::tagsFor(
                     settings, group, QStringLiteral("t1"), QStringLiteral("/a")),
                 QStringList{QStringLiteral("red")});
        QCOMPARE(sak::FileExplorerTagStore::tagsFor(
                     settings, group, QStringLiteral("t2"), QStringLiteral("/a")),
                 QStringList{QStringLiteral("blue")});
    }

    void emptyTagsRemoveTheRecord() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Tags");

        QSettings settings(iniPath(dir), QSettings::IniFormat);
        sak::FileExplorerTagStore::setTags(settings,
                                           group,
                                           QStringLiteral("t1"),
                                           QStringLiteral("/a"),
                                           QStringList{QStringLiteral("keep")});
        QVERIFY(!sak::FileExplorerTagStore::allTags(settings, group).isEmpty());

        sak::FileExplorerTagStore::setTags(
            settings, group, QStringLiteral("t1"), QStringLiteral("/a"), QStringList{});
        QVERIFY(sak::FileExplorerTagStore::tagsFor(
                    settings, group, QStringLiteral("t1"), QStringLiteral("/a"))
                    .isEmpty());
        QVERIFY(sak::FileExplorerTagStore::allTags(settings, group).isEmpty());
    }

    void allTagsAndItemsWithTagAggregateAcrossItems() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString group = QStringLiteral("Tags");

        {
            QSettings settings(iniPath(dir), QSettings::IniFormat);
            sak::FileExplorerTagStore::setTags(settings,
                                               group,
                                               QStringLiteral("t1"),
                                               QStringLiteral("/a"),
                                               QStringList{QStringLiteral("work"),
                                                           QStringLiteral("urgent")});
            sak::FileExplorerTagStore::setTags(settings,
                                               group,
                                               QStringLiteral("t1"),
                                               QStringLiteral("/b"),
                                               QStringList{QStringLiteral("work")});
        }

        QSettings settings(iniPath(dir), QSettings::IniFormat);
        QCOMPARE(sak::FileExplorerTagStore::allTags(settings, group),
                 (QStringList{QStringLiteral("urgent"), QStringLiteral("work")}));

        const auto workItems =
            sak::FileExplorerTagStore::itemsWithTag(settings, group, QStringLiteral("work"));
        QCOMPARE(workItems.size(), 2);
        QStringList paths;
        for (const auto& item : workItems) {
            paths.append(item.path);
        }
        std::sort(paths.begin(), paths.end());
        QCOMPARE(paths, (QStringList{QStringLiteral("/a"), QStringLiteral("/b")}));

        const auto urgentItems =
            sak::FileExplorerTagStore::itemsWithTag(settings, group, QStringLiteral("urgent"));
        QCOMPARE(urgentItems.size(), 1);
        QCOMPARE(urgentItems.first().path, QStringLiteral("/a"));
    }
};

QTEST_MAIN(FileExplorerTagStoreTests)
#include "test_file_explorer_tag_store.moc"
