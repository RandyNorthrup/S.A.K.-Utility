// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_item_model.cpp
/// @brief Unit tests for File Explorer item model.

#include "sak/file_explorer_group_proxy_model.h"
#include "sak/file_explorer_grouping.h"
#include "sak/file_explorer_item_model.h"
#include "sak/file_explorer_sort_filter_model.h"

#include <QDateTime>
#include <QLocale>
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

    void groupInfoMatchesFilesBuckets() {
        using sak::FileExplorerGroupDateUnit;
        using sak::FileExplorerGroupOption;
        const QDateTime now(QDate(2026, 7, 16), QTime(12, 0));  // a Thursday

        const auto info =
            [&now](const sak::FileExplorerGroupSource& source,
                   const FileExplorerGroupOption option,
                   const FileExplorerGroupDateUnit unit = FileExplorerGroupDateUnit::Year) {
                return sak::fileExplorerGroupInfo(source, option, unit, now);
            };

        // Name: first character uppercased, digits as-is (GroupingHelper).
        sak::FileExplorerGroupSource named;
        named.name = QStringLiteral("alpha.txt");
        QCOMPARE(info(named, FileExplorerGroupOption::Name).text, QStringLiteral("A"));
        named.name = QStringLiteral("1a");
        QCOMPARE(info(named, FileExplorerGroupOption::Name).text, QStringLiteral("1"));

        // Size buckets with the Files thresholds (strict greater-than).
        sak::FileExplorerGroupSource sized;
        const auto sizeText = [&](const quint64 bytes) {
            sized.size_bytes = bytes;
            return info(sized, FileExplorerGroupOption::Size).text;
        };
        QVERIFY(sizeText(10).startsWith(QStringLiteral("Tiny")));
        QVERIFY(sizeText(16'000).startsWith(QStringLiteral("Tiny")));
        QVERIFY(sizeText(20'000).startsWith(QStringLiteral("Small")));
        QVERIFY(sizeText(2'000'000).startsWith(QStringLiteral("Medium")));
        QVERIFY(sizeText(200'000'000).startsWith(QStringLiteral("Large")));
        QVERIFY(sizeText(2'000'000'000ULL).startsWith(QStringLiteral("Very large")));
        QVERIFY(sizeText(6'000'000'000ULL).startsWith(QStringLiteral("Huge")));
        sized.directory = true;
        QCOMPARE(info(sized, FileExplorerGroupOption::Size).text, QStringLiteral("Folders"));
        QVERIFY(info(sized, FileExplorerGroupOption::Size).sort_index < 0);

        // FileType: folders by type above files by lower-cased extension.
        sak::FileExplorerGroupSource typed;
        typed.name = QStringLiteral("Notes.TXT");
        typed.type = QStringLiteral("file");
        QCOMPARE(info(typed, FileExplorerGroupOption::FileType).text, QStringLiteral("txt"));
        QCOMPARE(info(typed, FileExplorerGroupOption::FileType).sort_index, 1);
        typed.name = QStringLiteral("README");
        QCOMPARE(info(typed, FileExplorerGroupOption::FileType).text, QStringLiteral("file"));
        typed.directory = true;
        typed.type = QStringLiteral("directory");
        QCOMPARE(info(typed, FileExplorerGroupOption::FileType).sort_index, 0);

        // FileTag: first tag, Untagged bucket sorts below tagged.
        sak::FileExplorerGroupSource tagged;
        tagged.tags = {QStringLiteral("red"), QStringLiteral("keep")};
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).text, QStringLiteral("red"));
        tagged.tags.clear();
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).text, QStringLiteral("Untagged"));
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).sort_index, 1);

        // Option names round-trip for persistence.
        QCOMPARE(sak::fileExplorerGroupOptionFromName(
                     sak::fileExplorerGroupOptionName(FileExplorerGroupOption::DateModified)),
                 FileExplorerGroupOption::DateModified);
    }

    void groupInfoMatchesFilesDateLadder() {
        using sak::FileExplorerGroupDateUnit;
        using sak::FileExplorerGroupOption;
        const QDateTime now(QDate(2026, 7, 16), QTime(12, 0));  // a Thursday

        // Date ladder (AbstractDateTimeFormatter.ToTimeSpanLabel).
        sak::FileExplorerGroupSource dated;
        const auto dateText = [&](const QDate date, const FileExplorerGroupDateUnit unit) {
            dated.modified_time = QDateTime(date, QTime(9, 0));
            return sak::fileExplorerGroupInfo(
                       dated, FileExplorerGroupOption::DateModified, unit, now)
                .text;
        };
        const auto yearUnit = FileExplorerGroupDateUnit::Year;
        QCOMPARE(dateText(QDate(2026, 7, 20), yearUnit), QStringLiteral("Future"));
        QCOMPARE(dateText(QDate(2026, 7, 16), yearUnit), QStringLiteral("Today"));
        QCOMPARE(dateText(QDate(2026, 7, 15), yearUnit), QStringLiteral("Yesterday"));
        QCOMPARE(dateText(QDate(2026, 7, 14), yearUnit), QStringLiteral("Earlier this week"));
        QCOMPARE(dateText(QDate(2026, 7, 8), yearUnit), QStringLiteral("Last week"));
        QCOMPARE(dateText(QDate(2026, 7, 1), yearUnit), QStringLiteral("Earlier this month"));
        QCOMPARE(dateText(QDate(2026, 6, 20), yearUnit), QStringLiteral("Last month"));
        QCOMPARE(dateText(QDate(2026, 3, 1), yearUnit), QStringLiteral("Earlier this year"));
        QCOMPARE(dateText(QDate(2025, 12, 1), yearUnit), QStringLiteral("Last year"));
        QCOMPARE(dateText(QDate(2023, 5, 1), yearUnit), QStringLiteral("2023"));
        // Month unit: beyond Last month the label is the specific month.
        QCOMPARE(dateText(QDate(2026, 3, 1), FileExplorerGroupDateUnit::Month),
                 QLocale().toString(QDate(2026, 3, 1), QStringLiteral("MMMM yyyy")));
        // Day unit: beyond Yesterday the label is the specific date.
        QCOMPARE(dateText(QDate(2026, 7, 14), FileExplorerGroupDateUnit::Day),
                 QLocale().toString(QDate(2026, 7, 14), QLocale::LongFormat));
    }

    void groupProxyInjectsHeadersAndMapsRows() {
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("z-big.bin"), 20'000'000),
                          directoryEntry(QStringLiteral("Alpha")),
                          fileEntry(QStringLiteral("a.bin"), 20),
                          fileEntry(QStringLiteral("notes.txt"), 42)});
        sak::FileExplorerSortFilterModel sort_proxy;
        sort_proxy.setSourceModel(&model);
        sort_proxy.sort(sak::FileExplorerItemModel::NameColumn, Qt::AscendingOrder);
        sak::FileExplorerGroupProxyModel group_proxy;
        group_proxy.setSourceModel(&sort_proxy);

        // Identity while grouping is off: same rows, selectable, no headers.
        QCOMPARE(group_proxy.rowCount(), 4);
        QVERIFY(group_proxy.headerRows().isEmpty());
        QCOMPARE(group_proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Alpha"));
        QVERIFY(group_proxy.flags(group_proxy.index(0, 0)).testFlag(Qt::ItemIsSelectable));

        // Size grouping: Folders, then Tiny (both small files), then Medium.
        group_proxy.setGrouping(sak::FileExplorerGroupOption::Size,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::AscendingOrder);
        QCOMPARE(group_proxy.rowCount(), 7);
        QCOMPARE(group_proxy.headerRows(), (QVector<int>{0, 2, 5}));
        QCOMPARE(group_proxy.index(0, 0).data().toString(), QStringLiteral("Folders"));
        QVERIFY(group_proxy.index(0, 0)
                    .data(sak::FileExplorerGroupProxyModel::IsGroupHeaderRole)
                    .toBool());
        QVERIFY(!group_proxy.flags(group_proxy.index(0, 0)).testFlag(Qt::ItemIsSelectable));
        QVERIFY(!group_proxy.mapToSource(group_proxy.index(0, 0)).isValid());
        QCOMPARE(group_proxy.index(1, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Alpha"));
        QVERIFY(group_proxy.index(2, 0).data().toString().startsWith(QStringLiteral("Tiny")));
        QCOMPARE(group_proxy.index(3, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("a.bin"));
        QCOMPARE(group_proxy.index(4, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("notes.txt"));
        QVERIFY(group_proxy.index(5, 0).data().toString().startsWith(QStringLiteral("Medium")));
        QCOMPARE(group_proxy.index(6, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("z-big.bin"));

        // Column headers forward by section even with an injected row 0.
        QCOMPARE(
            group_proxy
                .headerData(sak::FileExplorerItemModel::NameColumn, Qt::Horizontal, Qt::DisplayRole)
                .toString(),
            QStringLiteral("Name"));

        // mapFromSource lands on the grouped position.
        const QModelIndex source_alpha = sort_proxy.index(0,
                                                          sak::FileExplorerItemModel::NameColumn);
        QCOMPARE(source_alpha.data().toString(), QStringLiteral("Alpha"));
        QCOMPARE(group_proxy.mapFromSource(source_alpha).row(), 1);

        // Descending direction reverses the group section order.
        group_proxy.setGrouping(sak::FileExplorerGroupOption::Size,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::DescendingOrder);
        QVERIFY(group_proxy.index(0, 0).data().toString().startsWith(QStringLiteral("Medium")));
        QCOMPARE(group_proxy.index(6, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Alpha"));

        // None restores the identity mapping.
        group_proxy.setGrouping(sak::FileExplorerGroupOption::None,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::AscendingOrder);
        QCOMPARE(group_proxy.rowCount(), 4);
    }
};

QTEST_MAIN(FileExplorerItemModelTests)
#include "test_file_explorer_item_model.moc"
