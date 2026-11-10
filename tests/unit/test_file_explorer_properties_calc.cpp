// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_properties_calc.cpp
/// @brief Unit tests for the properties dialog size-walk and hash labeling.

#include "sak/file_explorer_properties_calc.h"

#include <QMap>
#include <QtTest/QtTest>

using namespace sak;

namespace {

FileManagementEntry fileEntry(const QString& path, quint64 bytes) {
    FileManagementEntry entry;
    entry.name = path;
    entry.path = path;
    entry.size_bytes = bytes;
    entry.regular_file = true;
    return entry;
}

FileManagementEntry dirEntry(const QString& path) {
    FileManagementEntry entry;
    entry.name = path;
    entry.path = path;
    entry.directory = true;
    return entry;
}

// A synthetic file system: path -> its listing. Missing paths list as !ok.
class FakeFs {
public:
    void add(const QString& path, const QVector<FileManagementEntry>& entries, bool ok = true) {
        FileManagementListResult listing;
        listing.ok = ok;
        listing.entries = entries;
        m_dirs.insert(path, listing);
    }

    DirectoryLister lister() {
        return [this](const QString& path, int max) -> FileManagementListResult {
            FileManagementListResult listing =
                m_dirs.value(path, FileManagementListResult{});  // default: ok=false
            // Mimic the real lister's cap so the walk's one-past-the-cap probe
            // can distinguish an exactly-full directory from a truncated one.
            if (max > 0 && listing.entries.size() > max) {
                listing.entries.resize(max);
            }
            return listing;
        };
    }

private:
    QMap<QString, FileManagementListResult> m_dirs;
};

}  // namespace

class PropertiesCalcTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void flatFilesSumComplete();
    void nestedTreeSumsComplete();
    void depthCapMarksIncomplete();
    void unreadableSubdirMarksIncompleteButKeepsRest();
    void entryCapMarksIncomplete();
    void hashTruncationDetection();
    void hashValueLabeling();
};

void PropertiesCalcTests::flatFilesSumComplete() {
    const QVector<FileManagementEntry> entries = {fileEntry(QStringLiteral("/a"), 100),
                                                  fileEntry(QStringLiteral("/b"), 200)};
    FakeFs fs;
    const TreeSizeResult result = combinedSize(fs.lister(), entries, 32, 10'000);
    QCOMPARE(result.bytes, static_cast<quint64>(300));
    QVERIFY(result.complete);
}

void PropertiesCalcTests::nestedTreeSumsComplete() {
    FakeFs fs;
    fs.add(QStringLiteral("/dir"),
           {fileEntry(QStringLiteral("/dir/f1"), 10), dirEntry(QStringLiteral("/dir/sub"))});
    fs.add(QStringLiteral("/dir/sub"), {fileEntry(QStringLiteral("/dir/sub/f2"), 5)});

    const TreeSizeResult result =
        combinedSize(fs.lister(), {dirEntry(QStringLiteral("/dir"))}, 32, 10'000);
    QCOMPARE(result.bytes, static_cast<quint64>(15));
    QVERIFY(result.complete);
}

void PropertiesCalcTests::depthCapMarksIncomplete() {
    FakeFs fs;
    fs.add(QStringLiteral("/dir"),
           {fileEntry(QStringLiteral("/dir/f1"), 10), dirEntry(QStringLiteral("/dir/sub"))});
    fs.add(QStringLiteral("/dir/sub"), {fileEntry(QStringLiteral("/dir/sub/f2"), 5)});

    // Max depth 0: the top dir is listed but its subdirectory is beyond the cap.
    const TreeSizeResult result =
        combinedSize(fs.lister(), {dirEntry(QStringLiteral("/dir"))}, 0, 10'000);
    QCOMPARE(result.bytes, static_cast<quint64>(10));  // f1 only; sub not descended
    QVERIFY(!result.complete);
}

void PropertiesCalcTests::unreadableSubdirMarksIncompleteButKeepsRest() {
    FakeFs fs;
    fs.add(QStringLiteral("/dir"),
           {fileEntry(QStringLiteral("/dir/f1"), 10), dirEntry(QStringLiteral("/dir/locked"))});
    // "/dir/locked" is never added -> lister returns ok=false for it.

    const TreeSizeResult result =
        combinedSize(fs.lister(), {dirEntry(QStringLiteral("/dir"))}, 32, 10'000);
    QCOMPARE(result.bytes, static_cast<quint64>(10));  // readable file still counted
    QVERIFY(!result.complete);
}

void PropertiesCalcTests::entryCapMarksIncomplete() {
    // A directory holding EXACTLY the cap count is COMPLETE: the walk probes one
    // past the cap to tell "exactly full" from "truncated", so an exact-cap dir
    // is no longer wrongly marked incomplete (B8-24).
    FakeFs exact;
    exact.add(QStringLiteral("/dir"),
              {fileEntry(QStringLiteral("/dir/a"), 1), fileEntry(QStringLiteral("/dir/b"), 2)});
    const TreeSizeResult exact_result =
        combinedSize(exact.lister(), {dirEntry(QStringLiteral("/dir"))}, 32, /*cap*/ 2);
    QCOMPARE(exact_result.bytes, static_cast<quint64>(3));
    QVERIFY(exact_result.complete);

    // MORE entries than the cap: the listing was truncated -> incomplete.
    FakeFs over;
    over.add(QStringLiteral("/dir"),
             {fileEntry(QStringLiteral("/dir/a"), 1),
              fileEntry(QStringLiteral("/dir/b"), 2),
              fileEntry(QStringLiteral("/dir/c"), 4)});
    const TreeSizeResult over_result =
        combinedSize(over.lister(), {dirEntry(QStringLiteral("/dir"))}, 32, /*cap*/ 2);
    QVERIFY(!over_result.complete);
}

void PropertiesCalcTests::hashTruncationDetection() {
    constexpr quint64 cap = 512ULL * 1024 * 1024;
    QVERIFY(hashInputTruncated(cap + 1, cap));
    QVERIFY(!hashInputTruncated(cap, cap));
    QVERIFY(!hashInputTruncated(10, cap));
    QVERIFY(!hashInputTruncated(10, 0));  // no cap -> never truncated
}

void PropertiesCalcTests::hashValueLabeling() {
    constexpr quint64 cap = 512ULL * 1024 * 1024;
    QCOMPARE(formatHashValue(QStringLiteral("deadbeef"), false, cap), QStringLiteral("deadbeef"));
    const QString marked = formatHashValue(QStringLiteral("deadbeef"), true, cap);
    // cap/kBytesPerMebibyte = 512, so the whole annotated value is deterministic.
    QCOMPARE(marked, QStringLiteral("deadbeef (hash of first 512 MB only)"));
}

QTEST_MAIN(PropertiesCalcTests)
#include "test_file_explorer_properties_calc.moc"
