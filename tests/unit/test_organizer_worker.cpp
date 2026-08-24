// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_organizer_worker.cpp
/// @brief Unit tests for OrganizerWorker file categorization (TST-11)

#include "sak/organizer_worker.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class OrganizerWorkerTests : public QObject {
    Q_OBJECT

private:
    void createDummyFile(const QString& dir,
                         const QString& name,
                         const QByteArray& content = "test") {
        QFile f(QDir(dir).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    }

    /// @brief Build a default category mapping for common extensions
    static QMap<QString, QStringList> defaultCategoryMapping() {
        return {{"Images", {"jpg", "jpeg", "png", "gif", "bmp", "svg", "webp"}},
                {"Documents", {"pdf", "doc", "docx", "txt", "csv", "xls", "xlsx", "odt"}},
                {"Audio", {"mp3", "wav", "flac", "aac", "ogg", "wma"}},
                {"Video", {"mp4", "avi", "mkv", "mov", "wmv", "flv"}}};
    }

private Q_SLOTS:
    void previewModeDoesNotMoveFiles() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        createDummyFile(tmpDir.path(), "photo.jpg");
        createDummyFile(tmpDir.path(), "report.pdf");
        createDummyFile(tmpDir.path(), "song.mp3");

        OrganizerWorker::Config config;
        config.target_directory = tmpDir.path();
        config.preview_mode = true;
        config.create_subdirectories = true;
        config.category_mapping = defaultCategoryMapping();
        OrganizerWorker worker(config);

        QSignalSpy spy(&worker, &OrganizerWorker::finished);
        QSignalSpy preview_spy(&worker, &OrganizerWorker::previewResults);
        worker.start();
        // QTRY_COMPARE_WITH_TIMEOUT, not spy.wait(). QSignalSpy connects with
        // Qt::DirectConnection, so the worker thread records finished() the moment it is
        // emitted, and wait() only returns true for an emission it has not already seen.
        // A three-file run finishes almost instantly, so whenever the worker beats the
        // main thread to the wait, wait() blocks for a second signal that never comes.
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);

        // A dry run must REPORT its whole plan, not merely succeed. finished() alone is
        // also emitted by a preview that planned nothing, so pin the exact summary
        // generatePreviewSummary() builds and the count previewResults carries.
        QTRY_COMPARE_WITH_TIMEOUT(preview_spy.count(), 1, 5000);
        QCOMPARE(preview_spy.at(0).at(1).toInt(), 3);
        QCOMPARE(preview_spy.at(0).at(0).toString(),
                 QStringLiteral("Preview Results:\n\n"
                                "Total files to organize: 3\n\n"
                                "Files by category:\n"
                                "  Audio: 1 files\n"
                                "  Documents: 1 files\n"
                                "  Images: 1 files\n"));

        // Files should still be in place (preview mode = dry run)
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("photo.jpg")));
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("report.pdf")));
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("song.mp3")));

        // "Nothing moved" is also true of a preview that never scanned. Pin the plan the
        // dry run was supposed to build, and that it wrote NOTHING -- not even a folder.
        QCOMPARE(worker.movedCount(), 0);
        QVERIFY(!worker.planTruncated());
        QCOMPARE(static_cast<int>(worker.plannedOperations().size()), 3);
        QMap<QString, QString> planned;  // source filename -> "category|destination"
        for (const auto& op : worker.plannedOperations()) {
            QVERIFY(!op.would_overwrite);
            planned.insert(QString::fromStdString(op.source.filename().string()),
                           op.category + QLatin1Char('|') +
                               QDir::fromNativeSeparators(
                                   QString::fromStdString(op.destination.string())));
        }
        const QDir root(tmpDir.path());
        QCOMPARE(planned.value("photo.jpg"),
                 QStringLiteral("Images|") + root.filePath("Images/photo.jpg"));
        QCOMPARE(planned.value("report.pdf"),
                 QStringLiteral("Documents|") + root.filePath("Documents/report.pdf"));
        QCOMPARE(planned.value("song.mp3"),
                 QStringLiteral("Audio|") + root.filePath("Audio/song.mp3"));
        QVERIFY(!root.exists("Images"));
        QVERIFY(!root.exists("Documents"));
        QVERIFY(!root.exists("Audio"));
    }

    void movesModeOrganizesFiles() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        createDummyFile(tmpDir.path(), "image.png");
        createDummyFile(tmpDir.path(), "notes.txt");

        OrganizerWorker::Config config;
        config.target_directory = tmpDir.path();
        config.preview_mode = false;
        config.create_subdirectories = true;
        config.category_mapping = defaultCategoryMapping();
        OrganizerWorker worker(config);

        QSignalSpy spy(&worker, &OrganizerWorker::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);

        // Original files should be moved to category subdirectories
        QVERIFY(!QFile::exists(QDir(tmpDir.path()).filePath("image.png")));
        QVERIFY(!QFile::exists(QDir(tmpDir.path()).filePath("notes.txt")));

        // Verify files ended up in correct category subdirectories
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("Images/image.png")));
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("Documents/notes.txt")));

        // An apply RELOCATES bytes and counts what it relocated. Existence alone also
        // passes an executeMove() that drops the ++m_moved_count, or one that deletes the
        // source and leaves an empty placeholder at the destination.
        QCOMPARE(worker.movedCount(), 2);
        QCOMPARE(static_cast<int>(worker.plannedOperations().size()), 2);
        QVERIFY(!worker.planTruncated());
        for (const QString& moved : QStringList{"Images/image.png", "Documents/notes.txt"}) {
            QFile moved_file(QDir(tmpDir.path()).filePath(moved));
            QVERIFY(moved_file.open(QIODevice::ReadOnly));
            QCOMPARE(moved_file.readAll(), QByteArray("test"));
        }
    }

    void cancellationFlag() {
        OrganizerWorker::Config config;
        config.target_directory = "C:\\nonexistent";
        OrganizerWorker worker(config);
        // A fresh worker starts un-cancelled (WorkerBase::m_stop_requested{false}) and
        // requestStop() is what flips it. Without the pre-state pin, a stopRequested()
        // hard-wired to true -- a worker that believes it is cancelled from birth --
        // satisfies the post-state assertion just as well.
        QVERIFY(!worker.stopRequested());
        worker.requestStop();
        QVERIFY(worker.stopRequested());
    }
};

QTEST_MAIN(OrganizerWorkerTests)
#include "test_organizer_worker.moc"
