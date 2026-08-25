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
    /// The Images extensions are deliberately UPPER-case. categorizeFile lower-cases the file's
    /// own extension and then compares case-insensitively against the mapping, but every fixture
    /// used to be lower case on both sides, so neither normalization was ever the reason a match
    /// happened. The mapping side is the one that is not redundant: the panel hands the extension
    /// column to the worker as the user typed it, and the AI path preserves the case it was
    /// given -- so a user who types "JPG, PNG" gets an uppercase mapping.
    static QMap<QString, QStringList> defaultCategoryMapping() {
        return {{"Images", {"JPG", "JPEG", "PNG", "GIF", "BMP", "SVG", "WEBP"}},
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
        // An UNCLAIMED file. Every fixture used to be claimed by a category, so files-SCANNED and
        // operations-PLANNED were the same number everywhere and no assertion could tell which
        // one previewResults carries. It also reaches execute()'s `if (!category.isEmpty())`
        // guard, which nothing exercised: without it planMove builds target/""/filename, which
        // collapses back onto the SOURCE, and the collision path then renames the user's
        // unmatched file in place to <stem>_1<ext>.
        createDummyFile(tmpDir.path(), "notes.xyz");
        // A nested file, to prove the scan does NOT recurse. The walk uses a plain
        // directory_iterator ("Only scan immediate files, not subdirectories"), and with a flat
        // fixture a recursive walk would look identical -- there was nothing nested to miss.
        QVERIFY(QDir(tmpDir.path()).mkpath(QStringLiteral("sub")));
        createDummyFile(QDir(tmpDir.path()).filePath(QStringLiteral("sub")), "nested.jpg");

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
        const QDir root(tmpDir.path());
        verifyPlanShapeAndThatNothingWasWritten(root, worker.plannedOperations());
    }

    /// The plan a dry run was supposed to build, and that it wrote NOTHING -- not even a folder.
    static void verifyPlanShapeAndThatNothingWasWritten(
        const QDir& root, const std::vector<OrganizerWorker::MoveOperation>& operations) {
        QMap<QString, QString> planned;  // source filename -> "category|destination"
        for (const auto& op : operations) {
            QVERIFY(!op.would_overwrite);
            planned.insert(QString::fromStdString(op.source.filename().string()),
                           op.category + QLatin1Char('|') +
                               QDir::fromNativeSeparators(
                                   QString::fromStdString(op.destination.string())));
        }
        QCOMPARE(planned.value("photo.jpg"),
                 QStringLiteral("Images|") + root.filePath("Images/photo.jpg"));
        QCOMPARE(planned.value("report.pdf"),
                 QStringLiteral("Documents|") + root.filePath("Documents/report.pdf"));
        QCOMPARE(planned.value("song.mp3"),
                 QStringLiteral("Audio|") + root.filePath("Audio/song.mp3"));
        QVERIFY(!root.exists("Images"));
        QVERIFY(!root.exists("Documents"));
        QVERIFY(!root.exists("Audio"));
        verifyUnplannedFilesAreUntouched(root, planned);
    }

    /// The unclaimed file is neither planned nor touched, and the nested one is never seen: four
    /// immediate files were scanned but only three planned, so previewResults' count is pinned to
    /// the PLAN rather than to the scan -- two sources every previous fixture made agree.
    static void verifyUnplannedFilesAreUntouched(const QDir& root,
                                                 const QMap<QString, QString>& planned) {
        QVERIFY(!planned.contains(QStringLiteral("notes.xyz")));
        QVERIFY(!planned.contains(QStringLiteral("nested.jpg")));
        QVERIFY(QFile::exists(root.filePath(QStringLiteral("notes.xyz"))));
        QVERIFY(QFile::exists(root.filePath(QStringLiteral("sub/nested.jpg"))));
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

    /// The destructive half of an apply. Every fixture organizes into EMPTY category folders, so
    /// no apply ever hit a collision -- leaving the execute-time existence re-check and the whole
    /// of handleCollision() unobserved. That re-check is what stops a plain std::filesystem::
    /// rename from silently REPLACING an existing destination (MOVEFILE_REPLACE_EXISTING on
    /// Windows), i.e. it is what stands between an organize and destroying a user's file. It also
    /// means the shipped default collision_strategy was pinned only to "one of the three accepted
    /// names": flipping it to "overwrite" kept every test green.
    void applyDoesNotOverwriteAnExistingDestination() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        const QDir root(tmpDir.path());

        createDummyFile(tmpDir.path(), "image.png");
        // A DIFFERENT file already sitting at the destination the plan will choose.
        QVERIFY(root.mkpath(QStringLiteral("Images")));
        QFile existing(root.filePath(QStringLiteral("Images/image.png")));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write(QByteArrayLiteral("PRECIOUS"));
        existing.close();

        OrganizerWorker::Config config;
        config.target_directory = tmpDir.path();
        config.preview_mode = false;
        config.create_subdirectories = true;
        config.category_mapping = defaultCategoryMapping();
        // The shipped default, stated rather than assumed.
        QCOMPARE(config.collision_strategy, QStringLiteral("rename"));
        OrganizerWorker worker(config);

        QSignalSpy spy(&worker, &OrganizerWorker::finished);
        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);

        // The pre-existing file is untouched, byte for byte.
        QFile kept(root.filePath(QStringLiteral("Images/image.png")));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArrayLiteral("PRECIOUS"));
        kept.close();
        // ... and the incoming file still landed, under a counter-suffixed name.
        QVERIFY2(QFile::exists(root.filePath(QStringLiteral("Images/image_1.png"))),
                 "the rename strategy must place the incoming file beside the existing one");
        QVERIFY(!QFile::exists(root.filePath(QStringLiteral("image.png"))));
        QCOMPARE(worker.movedCount(), 1);
    }

    /// planTruncated()'s TRUE arm, which is asserted nowhere in the repository -- the flag is
    /// false at construction and re-zeroed at the top of execute(), so every existing assertion
    /// held before the call was even made. It is what makes previewOrganize report "at least N
    /// file(s) ... (scan stopped at the preview limit)" instead of an exact count, i.e. the
    /// difference between an honest lower bound and a lie about how many files a directory holds.
    /// The cap is a THREE-arm condition whose first arm (`preview_mode &&`) is the only thing
    /// keeping an APPLY uncapped -- the documented contract that an apply must move every
    /// matching file -- and no test anywhere set max_preview_files on an apply.
    void previewScanCapMarksThePlanTruncated() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        for (int i = 0; i < 6; ++i) {
            createDummyFile(tmpDir.path(), QStringLiteral("photo_%1.jpg").arg(i));
        }

        OrganizerWorker::Config capped;
        capped.target_directory = tmpDir.path();
        capped.preview_mode = true;
        capped.create_subdirectories = true;
        capped.category_mapping = defaultCategoryMapping();
        capped.max_preview_files = 2;
        OrganizerWorker preview(capped);

        QSignalSpy preview_done(&preview, &OrganizerWorker::finished);
        preview.start();
        QTRY_COMPARE_WITH_TIMEOUT(preview_done.count(), 1, 5000);
        QVERIFY2(preview.planTruncated(), "a capped preview scan must mark the plan truncated");
        QCOMPARE(static_cast<int>(preview.plannedOperations().size()), 2);

        // The same cap on an APPLY must be ignored: preview_mode is the arm that keeps it
        // uncapped, and it must move every matching file.
        OrganizerWorker::Config applied = capped;
        applied.preview_mode = false;
        OrganizerWorker apply(applied);
        QSignalSpy apply_done(&apply, &OrganizerWorker::finished);
        apply.start();
        QTRY_COMPARE_WITH_TIMEOUT(apply_done.count(), 1, 5000);
        QVERIFY2(!apply.planTruncated(), "an apply is always uncapped");
        QCOMPARE(apply.movedCount(), 6);
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

    /// The flag test above proves only that WorkerBase writes an atomic bool and reads it back --
    /// a two-line base-class fact. The worker is never STARTED there, so none of OrganizerWorker's
    /// three cancellation checks is reached, and nothing in the tree observes whether a cancelled
    /// organize actually stops BEFORE relocating bytes. This is deterministic rather than racy:
    /// run() deliberately does not clear m_stop_requested, so a stop requested before start() is
    /// honoured at the very first check.
    void cancelledApplyRelocatesNothing() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());
        const QDir root(tmpDir.path());
        for (int i = 0; i < 4; ++i) {
            createDummyFile(tmpDir.path(), QStringLiteral("photo_%1.jpg").arg(i));
        }

        OrganizerWorker::Config config;
        config.target_directory = tmpDir.path();
        config.preview_mode = false;
        config.create_subdirectories = true;
        config.category_mapping = defaultCategoryMapping();
        OrganizerWorker worker(config);

        QSignalSpy done(&worker, &OrganizerWorker::finished);
        QSignalSpy cancelled(&worker, &OrganizerWorker::cancelled);
        worker.requestStop();  // before start(): honoured at the first checkStop
        worker.start();
        QVERIFY(worker.wait(5000));

        // Not a single byte relocated, and no category folder created.
        QCOMPARE(worker.movedCount(), 0);
        for (int i = 0; i < 4; ++i) {
            QVERIFY2(QFile::exists(root.filePath(QStringLiteral("photo_%1.jpg").arg(i))),
                     "a cancelled organize must leave every source file where it was");
        }
        QVERIFY2(!root.exists(QStringLiteral("Images")),
                 "a cancelled organize must not create category folders");
        // A cancelled run reports itself as cancelled, not as a completed organize.
        QCOMPARE(cancelled.count(), 1);
        QCOMPARE(done.count(), 0);
    }
};

QTEST_MAIN(OrganizerWorkerTests)
#include "test_organizer_worker.moc"
