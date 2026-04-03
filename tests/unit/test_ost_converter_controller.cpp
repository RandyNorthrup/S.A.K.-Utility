// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_converter_controller.cpp
/// @brief Unit tests for OstConverterController queue management

#include "sak/ost_converter_constants.h"
#include "sak/ost_converter_controller.h"
#include "sak/ost_converter_types.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

/// Helper: create a dummy file in a temp directory and return its path
static QString createTempFile(QTemporaryDir& dir, const QString& name) {
    QString path = dir.path() + "/" + name;
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write("dummy ost content");
    file.close();
    return path;
}

class TestOstConverterController : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Queue Management — Add Files
    // ====================================================================

    void testAddSingleFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;
        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileAdded);

        ctrl.addFile(path);

        QCOMPARE(ctrl.queue().size(), 1);
        QVERIFY(spy.count() >= 1);
    }

    void testAddMultipleFiles() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.pst");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);

        QCOMPARE(ctrl.queue().size(), 2);
    }

    void testAddDuplicateFileRejected() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path);
        ctrl.addFile(path);

        QCOMPARE(ctrl.queue().size(), 1);
    }

    // ====================================================================
    // Queue Management — Remove Files
    // ====================================================================

    void testRemoveFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);
        ctrl.removeFile(0);

        QCOMPARE(ctrl.queue().size(), 1);
    }

    void testRemoveInvalidIndex() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "one.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path);
        ctrl.removeFile(5);                // Out of bounds

        QCOMPARE(ctrl.queue().size(), 1);  // Unchanged
    }

    // ====================================================================
    // Queue Management — Clear
    // ====================================================================

    void testClearQueue() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "one.ost");
        QString path2 = createTempFile(temp, "two.ost");
        QString path3 = createTempFile(temp, "three.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path1);
        ctrl.addFile(path2);
        ctrl.addFile(path3);
        ctrl.clearQueue();

        QCOMPARE(ctrl.queue().size(), 0);
    }

    void testClearEmptyQueue() {
        sak::OstConverterController ctrl;
        ctrl.clearQueue();  // Should not crash
        QCOMPARE(ctrl.queue().size(), 0);
    }

    // ====================================================================
    // Queue Access
    // ====================================================================

    void testQueueJobAccess() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "email.ost");

        sak::OstConverterController ctrl;

        ctrl.addFile(path);

        const auto& jobs = ctrl.queue();
        QCOMPARE(jobs.size(), 1);
        QVERIFY(jobs[0].source_path.endsWith("email.ost"));
        QCOMPARE(jobs[0].status, sak::OstConversionJob::Status::Queued);
    }

    // ====================================================================
    // Conversion State
    // ====================================================================

    void testIsRunningInitiallyFalse() {
        sak::OstConverterController ctrl;
        QVERIFY(!ctrl.isRunning());
    }

    void testCannotStartWithEmptyQueue() {
        sak::OstConverterController ctrl;

        sak::OstConversionConfig config;
        config.output_directory = QStringLiteral("C:/output");
        config.format = sak::OstOutputFormat::Eml;

        ctrl.startConversion(config);
        QVERIFY(!ctrl.isRunning());
    }

    // ====================================================================
    // Signal Emission
    // ====================================================================

    void testFileAddedSignalOnAdd() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "a.ost");

        sak::OstConverterController ctrl;
        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileAdded);

        ctrl.addFile(path);
        QVERIFY(spy.count() >= 1);
    }

    void testFileRemovedSignalOnRemove() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path = createTempFile(temp, "a.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path);

        QSignalSpy spy(&ctrl, &sak::OstConverterController::fileRemoved);
        ctrl.removeFile(0);

        QVERIFY(spy.count() >= 1);
    }

    void testQueueClearedSignalOnClear() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QString path1 = createTempFile(temp, "a.ost");
        QString path2 = createTempFile(temp, "b.ost");

        sak::OstConverterController ctrl;
        ctrl.addFile(path1);
        ctrl.addFile(path2);

        QSignalSpy spy(&ctrl, &sak::OstConverterController::queueCleared);
        ctrl.clearQueue();

        QVERIFY(spy.count() >= 1);
    }

    // ====================================================================
    // Cancel When Not Running
    // ====================================================================

    void testCancelWhenNotRunning() {
        sak::OstConverterController ctrl;
        ctrl.cancelAll();  // Should not crash
        QVERIFY(!ctrl.isRunning());
    }
};

QTEST_MAIN(TestOstConverterController)
#include "test_ost_converter_controller.moc"
