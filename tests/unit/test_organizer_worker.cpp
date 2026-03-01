// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_organizer_worker.cpp
/// @brief Unit tests for OrganizerWorker file categorization (TST-11)

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "sak/organizer_worker.h"

class OrganizerWorkerTests : public QObject {
    Q_OBJECT

private:
    void createDummyFile(const QString& dir, const QString& name,
                         const QByteArray& content = "test")
    {
        QFile f(QDir(dir).filePath(name));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(content);
    }

    /// @brief Build a default category mapping for common extensions
    static QMap<QString, QStringList> defaultCategoryMapping()
    {
        return {
            {"Images",    {"jpg", "jpeg", "png", "gif", "bmp", "svg", "webp"}},
            {"Documents", {"pdf", "doc", "docx", "txt", "csv", "xls", "xlsx", "odt"}},
            {"Audio",     {"mp3", "wav", "flac", "aac", "ogg", "wma"}},
            {"Video",     {"mp4", "avi", "mkv", "mov", "wmv", "flv"}}
        };
    }

private Q_SLOTS:
    void previewModeDoesNotMoveFiles()
    {
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
        worker.start();
        QVERIFY(spy.wait(5000));

        // Files should still be in place (preview mode = dry run)
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("photo.jpg")));
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("report.pdf")));
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("song.mp3")));
    }

    void movesModeOrganizesFiles()
    {
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
        QVERIFY(spy.wait(5000));

        // Original files should be moved to category subdirectories
        QVERIFY(!QFile::exists(QDir(tmpDir.path()).filePath("image.png")));
        QVERIFY(!QFile::exists(QDir(tmpDir.path()).filePath("notes.txt")));

        // Verify files ended up in correct category subdirectories
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("Images/image.png")));
        QVERIFY(QFile::exists(QDir(tmpDir.path()).filePath("Documents/notes.txt")));
    }

    void cancellationFlag()
    {
        OrganizerWorker::Config config;
        config.target_directory = "C:\\nonexistent";
        OrganizerWorker worker(config);
        worker.requestStop();
        QVERIFY(worker.stopRequested());
    }
};

QTEST_MAIN(OrganizerWorkerTests)
#include "test_organizer_worker.moc"
