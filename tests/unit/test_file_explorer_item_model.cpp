// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_item_model.cpp
/// @brief Unit tests for File Explorer item model.

#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_sort_filter_model.h"

#include <QDateTime>
#include <QMimeData>
#include <QTimeZone>
#include <QtTest/QtTest>

#include <memory>

namespace {

sak::FileManagementEntry fileEntry(const QString& name, uint64_t bytes) {
    sak::FileManagementEntry entry;
    entry.name = name;
    entry.path = QStringLiteral("/%1").arg(name);
    entry.type = QStringLiteral("file");
    entry.size_bytes = bytes;
    entry.identifier = QStringLiteral("id-%1").arg(name);
    entry.regular_file = true;
    entry.modified_time = QDateTime(QDate(2026, 6, 10), QTime(9, 30), QTimeZone::UTC);
    entry.created_time = QDateTime(QDate(2026, 6, 9), QTime(8, 15), QTimeZone::UTC);
    return entry;
}

sak::FileManagementEntry symlinkEntry(const QString& name, const QString& target) {
    sak::FileManagementEntry entry;
    entry.name = name;
    entry.path = QStringLiteral("/%1").arg(name);
    entry.type = QStringLiteral("symlink");
    entry.identifier = QStringLiteral("ln-%1").arg(name);
    entry.symlink = true;
    entry.link_target = target;
    return entry;
}

sak::FileManagementEntry directoryEntry(const QString& name) {
    sak::FileManagementEntry entry;
    entry.name = name;
    entry.path = QStringLiteral("/%1").arg(name);
    entry.type = QStringLiteral("directory");
    entry.identifier = QStringLiteral("dir-%1").arg(name);
    entry.directory = true;
    return entry;
}

}  // namespace

class FileExplorerItemModelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void dragProviderEnablesDraggingAndBuildsPayload() {
        sak::FileExplorerItemModel model;
        model.setEntries(
            {directoryEntry(QStringLiteral("Docs")), fileEntry(QStringLiteral("notes.txt"), 42)});

        // Without a provider the rows are not draggable and mimeData yields nothing.
        QVERIFY(!model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
        QVERIFY(!model.mimeData({model.index(0, 0)}));

        // With a provider the rows advertise dragging, and mimeData collapses the
        // per-column index list into unique ordered rows before delegating.
        QList<int> captured;
        model.setDragPayloadProvider([&captured](const QList<int>& rows) -> QMimeData* {
            captured = rows;
            auto* mime = new QMimeData;
            mime->setText(QStringLiteral("payload"));
            return mime;
        });
        QVERIFY(model.flags(model.index(0, 0)).testFlag(Qt::ItemIsDragEnabled));
        QVERIFY(model.supportedDragActions().testFlag(Qt::MoveAction));
        std::unique_ptr<QMimeData> mime(
            model.mimeData({model.index(1, 0), model.index(1, 2), model.index(0, 0)}));
        QVERIFY(mime);
        QCOMPARE(mime->text(), QStringLiteral("payload"));
        QCOMPARE(captured, (QList<int>{0, 1}));
    }

    void exposesRowsColumnsAndRoles() {
        sak::FileExplorerItemModel model;
        model.setEntries(
            {directoryEntry(QStringLiteral("Docs")), fileEntry(QStringLiteral("notes.txt"), 42)});

        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.columnCount(), static_cast<int>(sak::FileExplorerItemModel::ColumnCount));
        QCOMPARE(
            model
                .headerData(sak::FileExplorerItemModel::NameColumn, Qt::Horizontal, Qt::DisplayRole)
                .toString(),
            QStringLiteral("Name"));
        QCOMPARE(model
                     .headerData(sak::FileExplorerItemModel::ModifiedColumn,
                                 Qt::Horizontal,
                                 Qt::DisplayRole)
                     .toString(),
                 QStringLiteral("Modified"));
        QCOMPARE(model
                     .headerData(
                         sak::FileExplorerItemModel::CreatedColumn, Qt::Horizontal, Qt::DisplayRole)
                     .toString(),
                 QStringLiteral("Created"));

        const QModelIndex file = model.index(1, sak::FileExplorerItemModel::NameColumn);
        QCOMPARE(model.data(file, Qt::DisplayRole).toString(), QStringLiteral("notes.txt"));
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntryPathRole).toString(),
                 QStringLiteral("/notes.txt"));
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntrySizeRole).toULongLong(), 42ULL);
        QVERIFY(model.data(file, sak::FileExplorerItemModel::EntryModifiedTimeRole)
                    .toDateTime()
                    .isValid());
        QVERIFY(model.data(file, sak::FileExplorerItemModel::EntryCreatedTimeRole)
                    .toDateTime()
                    .isValid());
        QCOMPARE(
            model.data(model.index(1, sak::FileExplorerItemModel::ModifiedColumn), Qt::DisplayRole)
                .toString(),
            sak::FileExplorerItemModel::timeText(
                model.data(file, sak::FileExplorerItemModel::EntryModifiedTimeRole).toDateTime()));
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntryRegularFileRole).toBool(), true);
        QCOMPARE(model
                     .data(model.index(0, sak::FileExplorerItemModel::NameColumn),
                           sak::FileExplorerItemModel::EntryDirectoryRole)
                     .toBool(),
                 true);
        QCOMPARE(model
                     .data(model.index(0, sak::FileExplorerItemModel::AttributesColumn),
                           sak::FileExplorerItemModel::EntryAttributeSummaryRole)
                     .toString(),
                 QStringLiteral("Directory"));
        QCOMPARE(sak::FileExplorerItemModel::attributeSummary(
                     symlinkEntry(QStringLiteral("latest"), QStringLiteral("/notes.txt"))),
                 QStringLiteral("Symlink -> /notes.txt"));
    }

    void sortsDirectoriesFirstAndFilesBySelectedColumn() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("z.bin"), 400),
                          directoryEntry(QStringLiteral("Alpha")),
                          fileEntry(QStringLiteral("a.bin"), 20)});

        model.sort(sak::FileExplorerItemModel::NameColumn, Qt::AscendingOrder);
        QCOMPARE(model.entryAt(0).name, QStringLiteral("Alpha"));
        QCOMPARE(model.entryAt(1).name, QStringLiteral("a.bin"));
        QCOMPARE(model.entryAt(2).name, QStringLiteral("z.bin"));

        model.sort(sak::FileExplorerItemModel::SizeColumn, Qt::DescendingOrder);
        QCOMPARE(model.entryAt(0).name, QStringLiteral("Alpha"));
        QCOMPARE(model.entryAt(1).name, QStringLiteral("z.bin"));
        QCOMPARE(model.entryAt(2).name, QStringLiteral("a.bin"));
    }

    void togglesFileExtensionsForDisplayOnly() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("archive.tar.gz"), 42),
                          fileEntry(QStringLiteral("README"), 16),
                          directoryEntry(QStringLiteral("Docs"))});

        model.setShowFileExtensions(false);
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("archive.tar"));
        QCOMPARE(model.index(1, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("README"));
        QCOMPARE(model.index(2, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Docs"));
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::NameColumn)
                     .data(sak::FileExplorerItemModel::EntryNameRole)
                     .toString(),
                 QStringLiteral("archive.tar.gz"));

        model.setShowFileExtensions(true);
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("archive.tar.gz"));
    }

    void clearsEntries() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("notes.txt"), 42)});
        QCOMPARE(model.rowCount(), 1);
        model.clear();
        QCOMPARE(model.rowCount(), 0);
        QVERIFY(!model.hasEntry(0));
    }

    void proxyFiltersByNameTypeAndPath() {
        sak::FileExplorerItemModel model;
        model.setEntries({directoryEntry(QStringLiteral("Docs")),
                          fileEntry(QStringLiteral("notes.txt"), 42),
                          fileEntry(QStringLiteral("photo.raw"), 900)});

        sak::FileExplorerSortFilterModel proxy;
        proxy.setSourceModel(&model);
        proxy.setNameFilter(QStringLiteral("note"));

        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("notes.txt"));

        proxy.setNameFilter(QStringLiteral("file"));
        QCOMPARE(proxy.rowCount(), 2);

        proxy.setNameFilter(QStringLiteral("/Docs"));
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Docs"));
    }

    void proxyHidesDotfilesUntilHiddenItemsEnabled() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral(".secret"), 42),
                          fileEntry(QStringLiteral("notes.txt"), 42),
                          directoryEntry(QStringLiteral(".config"))});

        sak::FileExplorerSortFilterModel proxy;
        proxy.setSourceModel(&model);

        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("notes.txt"));

        proxy.setShowHiddenItems(true);
        QCOMPARE(proxy.rowCount(), 3);

        proxy.setNameFilter(QStringLiteral("secret"));
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral(".secret"));

        proxy.setShowHiddenItems(false);
        QCOMPARE(proxy.rowCount(), 0);
    }

    void proxyTagFilterRestrictsToTaggedPaths() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("a.txt"), 1),
                          fileEntry(QStringLiteral("b.txt"), 2),
                          fileEntry(QStringLiteral("c.txt"), 3)});

        sak::FileExplorerSortFilterModel proxy;
        proxy.setSourceModel(&model);
        QVERIFY(!proxy.tagFilterActive());
        QCOMPARE(proxy.rowCount(), 3);

        proxy.setTagFilter({QStringLiteral("/a.txt"), QStringLiteral("/c.txt")});
        QVERIFY(proxy.tagFilterActive());
        QCOMPARE(proxy.rowCount(), 2);
        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("a.txt"));
        QCOMPARE(proxy.index(1, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("c.txt"));

        // An empty active set hides everything.
        proxy.setTagFilter({});
        QCOMPARE(proxy.rowCount(), 0);

        proxy.clearTagFilter();
        QVERIFY(!proxy.tagFilterActive());
        QCOMPARE(proxy.rowCount(), 3);
    }

    void tagsColumnReflectsInjectedProvider() {
        sak::FileExplorerItemModel model;
        model.setEntries(
            {fileEntry(QStringLiteral("a.txt"), 1), fileEntry(QStringLiteral("b.txt"), 2)});

        // No provider: the Tags column is empty.
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::TagsColumn).data().toString(),
                 QString());
        QCOMPARE(
            model
                .headerData(sak::FileExplorerItemModel::TagsColumn, Qt::Horizontal, Qt::DisplayRole)
                .toString(),
            QStringLiteral("Tags"));

        // Injected provider: the column and the tags role both reflect it, decoupled
        // from any tag store.
        model.setTagProvider([](const QString& path) -> QStringList {
            if (path == QStringLiteral("/a.txt")) {
                return {QStringLiteral("red"), QStringLiteral("keep")};
            }
            return {};
        });
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::TagsColumn).data().toString(),
                 QStringLiteral("red, keep"));
        QCOMPARE(model.index(0, sak::FileExplorerItemModel::NameColumn)
                     .data(sak::FileExplorerItemModel::EntryTagsRole)
                     .toStringList(),
                 (QStringList{QStringLiteral("red"), QStringLiteral("keep")}));
        QCOMPARE(model.index(1, sak::FileExplorerItemModel::TagsColumn).data().toString(),
                 QString());
    }

    void proxySortKeepsDirectoriesFirst() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("z.bin"), 400),
                          directoryEntry(QStringLiteral("Alpha")),
                          fileEntry(QStringLiteral("a.bin"), 20)});

        sak::FileExplorerSortFilterModel proxy;
        proxy.setSourceModel(&model);
        proxy.sort(sak::FileExplorerItemModel::SizeColumn, Qt::DescendingOrder);

        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Alpha"));
        QCOMPARE(proxy.index(1, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("z.bin"));
        QCOMPARE(proxy.index(2, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("a.bin"));
    }
};

QTEST_MAIN(FileExplorerItemModelTests)
#include "test_file_explorer_item_model.moc"
