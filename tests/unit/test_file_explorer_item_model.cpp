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

        // Without a provider the rows are not draggable and mimeData yields nothing. The whole
        // flag set, not one bit: returning Qt::NoItemFlags -- rows neither selectable nor
        // renameable -- satisfies a negative one-bit probe.
        QCOMPARE(model.flags(model.index(0, 0)),
                 Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));
        QVERIFY(!model.mimeData({model.index(0, 0)}));  // guard 1: no payload provider

        // Guard 2 (the empty-row-list return) is otherwise never exercised: with a provider
        // installed, an index list that filters down to nothing must still yield nullptr
        // WITHOUT invoking the provider.
        bool empty_list_provider_called = false;
        model.setDragPayloadProvider(
            [&empty_list_provider_called](const QList<int>&) -> QMimeData* {
                empty_list_provider_called = true;
                return new QMimeData;
            });
        QVERIFY(!model.mimeData({}));
        QVERIFY(!model.mimeData({QModelIndex()}));
        QVERIFY(!empty_list_provider_called);
        model.setDragPayloadProvider(nullptr);

        // With a provider the rows advertise dragging, and mimeData collapses the
        // per-column index list into unique ordered rows before delegating.
        QList<int> captured;
        model.setDragPayloadProvider([&captured](const QList<int>& rows) -> QMimeData* {
            captured = rows;
            auto* mime = new QMimeData;
            mime->setText(QStringLiteral("payload"));
            return mime;
        });
        QCOMPARE(model.flags(model.index(0, 0)),
                 Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                               Qt::ItemIsDragEnabled));
        // Dropping CopyAction silently kills ctrl-drag copy; the one-bit probe cannot see it.
        QCOMPARE(model.supportedDragActions(), Qt::DropActions(Qt::CopyAction | Qt::MoveAction));
        std::unique_ptr<QMimeData> mime(
            model.mimeData({model.index(1, 0), model.index(1, 2), model.index(0, 0)}));
        QVERIFY(mime);
        QCOMPARE(mime->text(), QStringLiteral("payload"));
        QCOMPARE(captured, (QList<int>{0, 1}));
    }

    // Every header, not three of nine: swapping the Type and Size labels leaves the three
    // sections the old probe checked correct while every other column header in the view is
    // wrong. Section 9 (ColumnCount) is off the end of the table and must stay invalid.
    static void verifyAllColumnHeaders(const sak::FileExplorerItemModel& model) {
        const auto headerText = [&model](const int section) {
            return model.headerData(section, Qt::Horizontal, Qt::DisplayRole).toString();
        };
        QCOMPARE(headerText(sak::FileExplorerItemModel::NameColumn), QStringLiteral("Name"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::TypeColumn), QStringLiteral("Type"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::SizeColumn), QStringLiteral("Size"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::ModifiedColumn),
                 QStringLiteral("Modified"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::CreatedColumn), QStringLiteral("Created"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::IdentifierColumn), QStringLiteral("ID"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::AttributesColumn),
                 QStringLiteral("Attributes"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::TagsColumn), QStringLiteral("Tags"));
        QCOMPARE(headerText(sak::FileExplorerItemModel::PathColumn), QStringLiteral("Path"));
        QVERIFY(!model
                     .headerData(
                         sak::FileExplorerItemModel::ColumnCount, Qt::Horizontal, Qt::DisplayRole)
                     .isValid());
    }

    void exposesRowsColumnsAndRoles() {
        sak::FileExplorerItemModel model;
        model.setEntries(
            {directoryEntry(QStringLiteral("Docs")), fileEntry(QStringLiteral("notes.txt"), 42)});

        QCOMPARE(model.rowCount(), 2);
        // columnCount() returns ColumnCount, so comparing the two was the same symbol on both
        // sides and could not fail. The ordinals ARE serialized -- the details view persists a
        // POSITIONAL column-width list into QSettings and reapplies it by index.
        QCOMPARE(model.columnCount(), 9);
        QCOMPARE(static_cast<int>(sak::FileExplorerItemModel::PathColumn), 8);
        verifyAllColumnHeaders(model);

        const QModelIndex file = model.index(1, sak::FileExplorerItemModel::NameColumn);
        QCOMPARE(model.data(file, Qt::DisplayRole).toString(), QStringLiteral("notes.txt"));
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntryPathRole).toString(),
                 QStringLiteral("/notes.txt"));
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntrySizeRole).toULongLong(), 42ULL);
        // The two role lambdas are adjacent copy-paste; isValid() on both cannot see either one
        // returning the OTHER field. Fixture instants are UTC-anchored, so this is
        // timezone-independent.
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntryModifiedTimeRole).toDateTime(),
                 QDateTime(QDate(2026, 6, 10), QTime(9, 30), QTimeZone::UTC));
        QCOMPARE(model.data(file, sak::FileExplorerItemModel::EntryCreatedTimeRole).toDateTime(),
                 QDateTime(QDate(2026, 6, 9), QTime(8, 15), QTimeZone::UTC));
        // The display strings were compared against the model's OWN role output, so a swapped
        // role table made both sides swap together. Derive them from the fixture constants
        // instead; toLocalTime() mirrors production, so no literal timestamp is baked in.
        const QString expected_modified =
            QDateTime(QDate(2026, 6, 10), QTime(9, 30), QTimeZone::UTC)
                .toLocalTime()
                .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        const QString expected_created = QDateTime(QDate(2026, 6, 9), QTime(8, 15), QTimeZone::UTC)
                                             .toLocalTime()
                                             .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        QCOMPARE(
            model.data(model.index(1, sak::FileExplorerItemModel::ModifiedColumn), Qt::DisplayRole)
                .toString(),
            expected_modified);
        QCOMPARE(
            model.data(model.index(1, sak::FileExplorerItemModel::CreatedColumn), Qt::DisplayRole)
                .toString(),
            expected_created);
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
        // Which two rows, not just how many: an off-by-one inside filterAcceptsRow (reading
        // row+1's type role) accepts Docs and notes.txt -- still exactly 2 rows, entirely
        // different identities. No sort() runs on this proxy, so source order is preserved.
        QCOMPARE(proxy.rowCount(), 2);
        QCOMPARE(proxy.index(0, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("notes.txt"));
        QCOMPARE(proxy.index(1, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("photo.raw"));

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

    // P04-27: a file the host marks hidden (Windows FILE_ATTRIBUTE_HIDDEN) is
    // NOT a dotfile, so the leading-dot gate alone let it leak into the listing.
    void proxyHidesHostAttributeHiddenEntries() {
        using Model = sak::FileExplorerItemModel;
        sak::FileExplorerItemModel model;
        sak::FileManagementEntry visible = fileEntry(QStringLiteral("visible.txt"), 10);
        sak::FileManagementEntry hidden = fileEntry(QStringLiteral("secret.txt"), 20);
        hidden.hidden = true;  // hidden attribute, ordinary (non-dot) name
        model.setEntries({visible, hidden});

        sak::FileExplorerSortFilterModel proxy;
        proxy.setSourceModel(&model);

        // With hidden items off the attribute-hidden file is filtered out.
        QCOMPARE(proxy.rowCount(), 1);
        QCOMPARE(proxy.index(0, Model::NameColumn).data().toString(),
                 QStringLiteral("visible.txt"));

        // Turning hidden items on reveals it.
        proxy.setShowHiddenItems(true);
        QCOMPARE(proxy.rowCount(), 2);
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
        // The RANGE half carried the shipped B8-24 GiB/MiB/KiB unit defect, which the name-only
        // ladder could not see; and the bucket rank is what orders the sections.
        QCOMPARE(sizeText(10), QStringLiteral("Tiny (0 B - 16 KB)"));
        QCOMPARE(sizeText(16'000), QStringLiteral("Tiny (0 B - 16 KB)"));
        QCOMPARE(sizeText(20'000), QStringLiteral("Small (16 KB - 1 MB)"));
        QCOMPARE(sizeText(2'000'000), QStringLiteral("Medium (1 MB - 128 MB)"));
        QCOMPARE(sizeText(200'000'000), QStringLiteral("Large (128 MB - 1 GB)"));
        QCOMPARE(sizeText(2'000'000'000ULL), QStringLiteral("Very large (1 GB - 5 GB)"));
        QCOMPARE(sizeText(6'000'000'000ULL), QStringLiteral("Huge (5 GB +)"));
        const auto sizeIndex = [&](const quint64 bytes) {
            sized.size_bytes = bytes;
            return info(sized, FileExplorerGroupOption::Size).sort_index;
        };
        QCOMPARE(sizeIndex(10), 0);
        QCOMPARE(sizeIndex(20'000), 1);
        QCOMPARE(sizeIndex(2'000'000), 2);
        QCOMPARE(sizeIndex(200'000'000), 3);
        QCOMPARE(sizeIndex(2'000'000'000ULL), 4);
        QCOMPARE(sizeIndex(6'000'000'000ULL), 5);
        sized.directory = true;
        QCOMPARE(info(sized, FileExplorerGroupOption::Size).text, QStringLiteral("Folders"));
        // `< 0` accepts -7 or INT_MIN; the sibling branches in this slot pin ranks exactly.
        QCOMPARE(info(sized, FileExplorerGroupOption::Size).sort_index, -1);
    }

    void groupInfoMatchesFilesTypeAndTagBuckets() {
        using sak::FileExplorerGroupDateUnit;
        using sak::FileExplorerGroupOption;
        const auto info = [](const sak::FileExplorerGroupSource& source,
                             const FileExplorerGroupOption option) {
            return sak::fileExplorerGroupInfo(source,
                                              option,
                                              FileExplorerGroupDateUnit::Year,
                                              QDateTime(QDate(2026, 7, 16), QTime(12, 0)));
        };

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
        // The folder branch returning the extension instead of the type yields "" for "README".
        QCOMPARE(info(typed, FileExplorerGroupOption::FileType).text, QStringLiteral("directory"));

        // FileTag: first tag, Untagged bucket sorts below tagged.
        sak::FileExplorerGroupSource tagged;
        tagged.tags = {QStringLiteral("red"), QStringLiteral("keep")};
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).text, QStringLiteral("red"));
        // Returning rank 1 for BOTH branches keeps this line and the Untagged rank below green
        // while the tagged/Untagged section ordering collapses to a text tiebreak.
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).sort_index, 0);
        tagged.tags.clear();
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).text, QStringLiteral("Untagged"));
        QCOMPARE(info(tagged, FileExplorerGroupOption::FileTag).sort_index, 1);
    }

    // These strings are PERSISTED: the panel writes fileExplorerGroupOptionName() into QSettings
    // and reads it back through fileExplorerGroupOptionFromName(), so the literals are a real
    // cross-version contract. A round trip through the pair never observes the stored string, so
    // swapping the "size" and "fileType" literals round-tripped perfectly while silently
    // reinterpreting every persisted settings file.
    void groupOptionNamesArePersistedLiterals() {
        using sak::FileExplorerGroupOption;
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::Name),
                 QStringLiteral("name"));
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::DateModified),
                 QStringLiteral("dateModified"));
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::DateCreated),
                 QStringLiteral("dateCreated"));
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::Size),
                 QStringLiteral("size"));
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::FileType),
                 QStringLiteral("fileType"));
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::FileTag),
                 QStringLiteral("fileTag"));
        QCOMPARE(sak::fileExplorerGroupOptionName(FileExplorerGroupOption::None),
                 QStringLiteral("none"));
        QCOMPARE(sak::fileExplorerGroupOptionFromName(QStringLiteral("dateModified")),
                 FileExplorerGroupOption::DateModified);
        // The reader trims, and anything unrecognised must fail closed to None.
        QCOMPARE(sak::fileExplorerGroupOptionFromName(QStringLiteral("  size  ")),
                 FileExplorerGroupOption::Size);
        QCOMPARE(sak::fileExplorerGroupOptionFromName(QStringLiteral("bogus")),
                 FileExplorerGroupOption::None);
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

    void checkboxProvidersMirrorSelectionAndRouteToggles() {
        sak::FileExplorerItemModel model;
        model.setEntries(
            {fileEntry(QStringLiteral("a.txt"), 1), fileEntry(QStringLiteral("b.txt"), 2)});

        QSet<QString> selected;
        QString toggled_path;
        bool toggled_checked = false;
        model.setCheckboxProviders(
            [&selected](const QString& path) { return selected.contains(path); },
            [&toggled_path, &toggled_checked](const QString& path, const bool checked) {
                toggled_path = path;
                toggled_checked = checked;
            });

        // Hidden (the Files setting off): no check state, not user-checkable.
        QVERIFY(!model.checkboxesVisible());
        QVERIFY(!model.index(0, 0).data(Qt::CheckStateRole).isValid());
        // Three guards return the same invalid QVariant. Isolate the COLUMN guard: with
        // checkboxes on, only the Name column may carry a check state -- drop that guard and
        // every column paints one, which the visibility-off probe above cannot see.
        model.setCheckboxesVisible(true);
        QVERIFY(model.index(0, sak::FileExplorerItemModel::NameColumn)
                    .data(Qt::CheckStateRole)
                    .isValid());
        QVERIFY(!model.index(0, sak::FileExplorerItemModel::SizeColumn)
                     .data(Qt::CheckStateRole)
                     .isValid());
        QVERIFY(!model.index(0, sak::FileExplorerItemModel::PathColumn)
                     .data(Qt::CheckStateRole)
                     .isValid());
        model.setCheckboxesVisible(false);
        QVERIFY(!model.index(0, 0).data(Qt::CheckStateRole).isValid());
        QCOMPARE(model.flags(model.index(0, 0)),
                 Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));

        // Visible: the box mirrors the selection provider.
        model.setCheckboxesVisible(true);
        // Returning only Enabled|UserCheckable loses selection and inline rename the moment
        // checkboxes are turned on -- invisible to the one-bit probe.
        QCOMPARE(model.flags(model.index(0, 0)),
                 Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable |
                               Qt::ItemIsUserCheckable));
        QCOMPARE(model.index(0, 0).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Unchecked);
        selected.insert(QStringLiteral("/a.txt"));
        model.refreshChecks();
        QCOMPARE(model.index(0, 0).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Checked);
        QCOMPARE(model.index(1, 0).data(Qt::CheckStateRole).value<Qt::CheckState>(), Qt::Unchecked);

        // Checking routes through the toggle handler (the panel selects).
        QVERIFY(model.setData(model.index(1, 0), Qt::Checked, Qt::CheckStateRole));
        QCOMPARE(toggled_path, QStringLiteral("/b.txt"));
        QVERIFY(toggled_checked);
        QVERIFY(model.setData(model.index(0, 0), Qt::Unchecked, Qt::CheckStateRole));
        QCOMPARE(toggled_path, QStringLiteral("/a.txt"));
        QVERIFY(!toggled_checked);
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
        // A proxy that synthesised its own flags and dropped ItemIsEditable would kill inline
        // rename in the grouped view while still passing the selectable probe.
        QCOMPARE(group_proxy.flags(group_proxy.index(0, 0)),
                 Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable));

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
        // Qt::NoItemFlags (header renders greyed, stops receiving mouse events) and a header
        // that lost ItemNeverHasChildren (views probe it for children every layout pass) both
        // lack ItemIsSelectable, so one negative probe cannot tell them from the right value.
        QCOMPARE(group_proxy.flags(group_proxy.index(0, 0)),
                 Qt::ItemFlags(Qt::ItemIsEnabled | Qt::ItemNeverHasChildren));
        QVERIFY(!group_proxy.mapToSource(group_proxy.index(0, 0)).isValid());
        QCOMPARE(group_proxy.index(1, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Alpha"));
        QCOMPARE(group_proxy.index(2, 0).data().toString(), QStringLiteral("Tiny (0 B - 16 KB)"));
        QCOMPARE(group_proxy.index(3, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("a.bin"));
        QCOMPARE(group_proxy.index(4, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("notes.txt"));
        QCOMPARE(group_proxy.index(5, 0).data().toString(),
                 QStringLiteral("Medium (1 MB - 128 MB)"));
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

        verifyDescendingAndNoneGrouping(group_proxy);
    }

    // Descending reverses the section order; None restores the identity mapping.
    static void verifyDescendingAndNoneGrouping(sak::FileExplorerGroupProxyModel& group_proxy) {
        group_proxy.setGrouping(sak::FileExplorerGroupOption::Size,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::DescendingOrder);
        // The label half is what proves the sections actually reversed.
        QCOMPARE(group_proxy.index(0, 0).data().toString(),
                 QStringLiteral("Medium (1 MB - 128 MB)"));
        QCOMPARE(group_proxy.index(6, sak::FileExplorerItemModel::NameColumn).data().toString(),
                 QStringLiteral("Alpha"));
        group_proxy.setGrouping(sak::FileExplorerGroupOption::None,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::AscendingOrder);
        QCOMPARE(group_proxy.rowCount(), 4);
    }

    // P04-22/23: a persistent index (selection / current) held across a source
    // re-sort must still point at the SAME item. The mapping has to be snapshot
    // in layoutAboutToBeChanged (before the source re-sorts); capturing it in
    // layoutChanged read stale rows and silently moved the selection.
    void groupProxyKeepsPersistentIndexAcrossResort() {
        using Model = sak::FileExplorerItemModel;
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("a.bin"), 20),
                          fileEntry(QStringLiteral("b.bin"), 30),
                          fileEntry(QStringLiteral("c.bin"), 40)});
        sak::FileExplorerSortFilterModel sort_proxy;
        sort_proxy.setSourceModel(&model);
        sort_proxy.sort(Model::NameColumn, Qt::AscendingOrder);
        sak::FileExplorerGroupProxyModel group_proxy;
        group_proxy.setSourceModel(&sort_proxy);
        // Group by Name: each file lands in its own A/B/C section, so a re-sort
        // moves items to different proxy rows -- exposing a bad remap.
        group_proxy.setGrouping(sak::FileExplorerGroupOption::Name,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::AscendingOrder);

        // Pin "a.bin" wherever it currently sits in the grouped proxy.
        int row_a = -1;
        for (int row = 0; row < group_proxy.rowCount(); ++row) {
            const QModelIndex idx = group_proxy.index(row, Model::NameColumn);
            if (idx.data(Model::EntryNameRole).toString() == QStringLiteral("a.bin")) {
                row_a = row;
                break;
            }
        }
        // `>= 0` also passes if the three sections merged into one (rows [A, a.bin, b.bin,
        // c.bin] -- a.bin is still row 1) or if no headers were emitted at all.
        QCOMPARE(row_a, 1);  // row 0 is the "A" header; a.bin is that section's only member
        QCOMPARE(group_proxy.rowCount(), 6);
        const QPersistentModelIndex pinned(group_proxy.index(row_a, Model::NameColumn));
        QCOMPARE(pinned.data(Model::EntryNameRole).toString(), QStringLiteral("a.bin"));

        // Re-sort the underlying model: layoutChanged ripples up to the group
        // proxy, which must remap the pinned index to a.bin's new location.
        sort_proxy.sort(Model::NameColumn, Qt::DescendingOrder);

        QVERIFY(pinned.isValid());
        QCOMPARE(pinned.data(Model::EntryNameRole).toString(), QStringLiteral("a.bin"));
    }

    // B8-22: FileExplorerItemModel::sort permutes m_entries and emits
    // layoutChanged, so it MUST remap its persistent indexes; without that a
    // pinned selection / current / open editor silently retargets to whatever
    // item lands at the old row number.
    void itemModelSortRemapsPersistentIndexes() {
        using Model = sak::FileExplorerItemModel;
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("a.bin"), 20),
                          fileEntry(QStringLiteral("z.bin"), 400),
                          fileEntry(QStringLiteral("m.bin"), 50)});
        model.sort(Model::NameColumn, Qt::AscendingOrder);  // a, m, z
        const QPersistentModelIndex pinned(model.index(0, Model::NameColumn));
        QCOMPARE(pinned.data(Model::EntryNameRole).toString(), QStringLiteral("a.bin"));

        model.sort(Model::NameColumn, Qt::DescendingOrder);  // z, m, a
        QVERIFY(pinned.isValid());
        // The pinned index follows a.bin to its new row, not the item now at row 0.
        QCOMPARE(pinned.data(Model::EntryNameRole).toString(), QStringLiteral("a.bin"));
        QCOMPARE(pinned.row(), 2);
    }

    // B8-22: while grouped by a key, an in-place dataChanged that touches that
    // key (a tag refresh under group-by-tag) must regroup, not leave the item
    // stranded in its old section.
    void groupProxyRegroupsOnGroupingKeyChange() {
        using Model = sak::FileExplorerItemModel;
        sak::FileExplorerItemModel model;
        model.setEntries(
            {fileEntry(QStringLiteral("a.txt"), 1), fileEntry(QStringLiteral("b.txt"), 2)});
        QStringList a_tags{QStringLiteral("red")};
        model.setTagProvider([&a_tags](const QString& path) -> QStringList {
            return path == QStringLiteral("/a.txt") ? a_tags : QStringList{QStringLiteral("blue")};
        });
        sak::FileExplorerSortFilterModel sort_proxy;
        sort_proxy.setSourceModel(&model);
        sort_proxy.sort(Model::NameColumn, Qt::AscendingOrder);
        sak::FileExplorerGroupProxyModel group_proxy;
        group_proxy.setSourceModel(&sort_proxy);
        group_proxy.setGrouping(sak::FileExplorerGroupOption::FileTag,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::AscendingOrder);

        const auto sectionOf = [&group_proxy](const QString& name) -> QString {
            QString header;
            for (int row = 0; row < group_proxy.rowCount(); ++row) {
                const QModelIndex idx = group_proxy.index(row, Model::NameColumn);
                if (idx.data(sak::FileExplorerGroupProxyModel::IsGroupHeaderRole).toBool()) {
                    header = group_proxy.index(row, 0).data().toString();
                } else if (idx.data(Model::EntryNameRole).toString() == name) {
                    return header;
                }
            }
            return QString();
        };

        QCOMPARE(sectionOf(QStringLiteral("a.txt")), QStringLiteral("red"));

        // Re-tag a.txt and refresh: the tag dataChanged must move it to "green".
        a_tags = QStringList{QStringLiteral("green")};
        model.refreshTags();
        QCOMPARE(sectionOf(QStringLiteral("a.txt")), QStringLiteral("green"));
    }

    // R5-G14-2a: persistent indexes created by a VIEW, inside its own
    // layoutAboutToBeChanged slot, must also be remapped.
    //
    // The proxy used to call persistentIndexList() BEFORE emitting
    // layoutAboutToBeChanged(). Qt views and selection models create their
    // persistent indexes in response to that signal, so everything they created
    // was missing from the snapshot and kept its pre-change row. In the real
    // failure the proxy held 1 index at snapshot time and 8 after the signal;
    // two of the seven unremapped ones then resolved to the same position, and
    // changePersistentIndex() inserts into a QHash keyed by index, so one was
    // silently displaced and destroying it tripped Qt's "persistent model
    // indexes corrupted" assert.
    //
    // The assert only fires in a Debug build. This test instead checks the
    // consequence that is visible in EVERY configuration: an index that is not
    // remapped silently retargets to whatever item lands on its old row.
    void groupProxyRemapsIndexesCreatedDuringLayoutChange() {
        using Model = sak::FileExplorerItemModel;
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("a.bin"), 20),
                          fileEntry(QStringLiteral("b.bin"), 30),
                          fileEntry(QStringLiteral("c.bin"), 40)});
        sak::FileExplorerSortFilterModel sort_proxy;
        sort_proxy.setSourceModel(&model);
        sort_proxy.sort(Model::NameColumn, Qt::AscendingOrder);  // a, b, c
        sak::FileExplorerGroupProxyModel group_proxy;
        group_proxy.setSourceModel(&sort_proxy);
        // Grouping stays off, matching the case that actually failed (a plain
        // file listing with a selection held across a hidden-items toggle).

        // Held BEFORE the layout change begins, the way an existing selection is.
        const QPersistentModelIndex before(group_proxy.index(0, Model::NameColumn));
        QCOMPARE(before.data(Model::EntryNameRole).toString(), QStringLiteral("a.bin"));

        // Created DURING layoutAboutToBeChanged, the way QAbstractItemView and
        // QItemSelectionModel create theirs.
        QPersistentModelIndex during;
        QObject::connect(&group_proxy,
                         &QAbstractItemModel::layoutAboutToBeChanged,
                         &group_proxy,
                         [&group_proxy, &during]() {
                             if (during.isValid()) {
                                 return;  // only the first bracket
                             }
                             during =
                                 QPersistentModelIndex(group_proxy.index(2, Model::NameColumn));
                         });

        sort_proxy.sort(Model::NameColumn, Qt::DescendingOrder);  // c, b, a

        // Both must still name the item they were pinned to, not the item that
        // now occupies their old row.
        QVERIFY(before.isValid());
        QCOMPARE(before.data(Model::EntryNameRole).toString(), QStringLiteral("a.bin"));
        QCOMPARE(before.row(), 2);
        QVERIFY(during.isValid());
        QCOMPARE(during.data(Model::EntryNameRole).toString(), QStringLiteral("c.bin"));
        QCOMPARE(during.row(), 0);
    }

    // R5-G14-2a: a group HEADER row has no source index, so mapToSource returns
    // an invalid index for it. The remap used to push that invalid index straight
    // into changePersistentIndex, which detached the header permanently -- the
    // header the user had selected or made current went invalid on every single
    // layout change. Headers are now tracked by section text instead.
    void groupProxyKeepsHeaderPersistentIndexAcrossResort() {
        using Model = sak::FileExplorerItemModel;
        sak::FileExplorerItemModel model;
        model.setEntries({fileEntry(QStringLiteral("a.bin"), 20),
                          fileEntry(QStringLiteral("b.bin"), 30),
                          fileEntry(QStringLiteral("c.bin"), 40)});
        sak::FileExplorerSortFilterModel sort_proxy;
        sort_proxy.setSourceModel(&model);
        sort_proxy.sort(Model::NameColumn, Qt::AscendingOrder);
        sak::FileExplorerGroupProxyModel group_proxy;
        group_proxy.setSourceModel(&sort_proxy);
        // Group by Name: each file gets its own single-letter section.
        group_proxy.setGrouping(sak::FileExplorerGroupOption::Name,
                                sak::FileExplorerGroupDateUnit::Year,
                                Qt::AscendingOrder);

        const QVector<int> headers = group_proxy.headerRows();
        // Three files with distinct leading letters (a/b/c) group into exactly three
        // single-letter sections. `!isEmpty()` guarded the .first() deref but would not
        // catch a grouping regression that merged or dropped sections.
        // The positions, not the count: emitting each header AFTER its rows ({1,3,5}) keeps
        // size() == 3 while every header row the view paints lands on the wrong item.
        QCOMPARE(headers, (QVector<int>{0, 2, 4}));
        QCOMPARE(group_proxy.rowCount(), 6);
        const int header_row = headers.first();
        const QString section = group_proxy.index(header_row, 0).data().toString();
        // A dropped .toUpper() ("a"), the whole name ("a.bin") and the empty-name fallback
        // ("?") all pass !isEmpty() -- and line 746 compares the surviving header back against
        // this same value, so both ends of that round trip could be wrong together.
        QCOMPARE(section, QStringLiteral("A"));
        const QPersistentModelIndex header_pin(group_proxy.index(header_row, 0));
        QVERIFY(header_pin.data(sak::FileExplorerGroupProxyModel::IsGroupHeaderRole).toBool());

        sort_proxy.sort(Model::NameColumn, Qt::DescendingOrder);

        // Survives the layout change and still names the same section.
        QVERIFY(header_pin.isValid());
        QCOMPARE(header_pin.data().toString(), section);
        QVERIFY(header_pin.data(sak::FileExplorerGroupProxyModel::IsGroupHeaderRole).toBool());
    }
};

QTEST_MAIN(FileExplorerItemModelTests)
#include "test_file_explorer_item_model.moc"
