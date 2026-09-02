// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/email_attachments_browser_dialog.h"
#include "sak/email_inspector_controller.h"

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class EmailAttachmentsBrowserDialogTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void controllerNoFileFailuresCarryRequestIdentity();
    void parserFolderErrorRestoresRequestIdentity();
    void parserItemErrorRestoresRequestIdentity();
    void wrongFolderFailureDoesNotAdvance();
    void matchingFolderFailureAdvancesOnce();
    void wrongItemFailureDoesNotAdvance();
    void matchingItemFailureAdvancesOnce();
    void genericErrorIsVisibleWithoutMutatingState();
    void specificAndGenericSaveErrorsCountOnce();

private:
    static void finishInitialEmptyScan(sak::EmailAttachmentsBrowserDialog* dialog);
};

void EmailAttachmentsBrowserDialogTests::finishInitialEmptyScan(
    sak::EmailAttachmentsBrowserDialog* dialog) {
    QApplication::processEvents();
    QVERIFY(dialog->m_scan_complete);
    QVERIFY(dialog->m_status_label != nullptr);
}

void EmailAttachmentsBrowserDialogTests::controllerNoFileFailuresCarryRequestIdentity() {
    EmailInspectorController controller;
    QSignalSpy folder_failed(&controller, &EmailInspectorController::folderItemsFailed);
    QSignalSpy detail_failed(&controller, &EmailInspectorController::itemDetailFailed);
    QSignalSpy attachment_failed(&controller, &EmailInspectorController::attachmentContentFailed);

    controller.loadFolderItems(42, 7, 10);
    controller.loadItemDetail(99);
    controller.loadAttachmentContent(123, 4);

    QCOMPARE(folder_failed.count(), 1);
    QCOMPARE(folder_failed.first().at(0).toULongLong(), 42ULL);
    QCOMPARE(folder_failed.first().at(1).toInt(), 7);
    QCOMPARE(detail_failed.count(), 1);
    QCOMPARE(detail_failed.first().at(0).toULongLong(), 99ULL);
    QCOMPARE(attachment_failed.count(), 1);
    QCOMPARE(attachment_failed.first().at(0).toULongLong(), 123ULL);
    QCOMPARE(attachment_failed.first().at(1).toInt(), 4);
}

void EmailAttachmentsBrowserDialogTests::parserFolderErrorRestoresRequestIdentity() {
    EmailInspectorController controller;
    controller.m_state = EmailInspectorController::State::LoadingFolderItems;
    controller.m_folder_request_pending = true;
    controller.m_pending_folder_id = 42;
    controller.m_pending_folder_offset = 7;
    QSignalSpy folder_failed(&controller, &EmailInspectorController::folderItemsFailed);
    QSignalSpy generic_error(&controller, &EmailInspectorController::errorOccurred);

    controller.handleParserError(QStringLiteral("parser folder failure"));

    QCOMPARE(folder_failed.count(), 1);
    QCOMPARE(folder_failed.first().at(0).toULongLong(), 42ULL);
    QCOMPARE(folder_failed.first().at(1).toInt(), 7);
    QCOMPARE(folder_failed.first().at(2).toString(), QStringLiteral("parser folder failure"));
    QCOMPARE(generic_error.count(), 1);
    QVERIFY(!controller.m_folder_request_pending);
    QCOMPARE(controller.currentState(), EmailInspectorController::State::Idle);
}

void EmailAttachmentsBrowserDialogTests::parserItemErrorRestoresRequestIdentity() {
    EmailInspectorController controller;
    controller.m_state = EmailInspectorController::State::LoadingItemDetail;
    controller.m_detail_request_pending = true;
    controller.m_pending_detail_id = 99;
    QSignalSpy detail_failed(&controller, &EmailInspectorController::itemDetailFailed);
    QSignalSpy generic_error(&controller, &EmailInspectorController::errorOccurred);

    controller.handleParserError(QStringLiteral("parser item failure"));

    QCOMPARE(detail_failed.count(), 1);
    QCOMPARE(detail_failed.first().at(0).toULongLong(), 99ULL);
    QCOMPARE(detail_failed.first().at(1).toString(), QStringLiteral("parser item failure"));
    QCOMPARE(generic_error.count(), 1);
    QVERIFY(!controller.m_detail_request_pending);
    QCOMPARE(controller.currentState(), EmailInspectorController::State::Idle);
}

void EmailAttachmentsBrowserDialogTests::wrongFolderFailureDoesNotAdvance() {
    EmailInspectorController controller;
    sak::EmailAttachmentsBrowserDialog dialog(&controller, {});
    finishInitialEmptyScan(&dialog);
    dialog.m_all_folder_ids = {10};
    dialog.m_folders_scanned = 0;
    dialog.m_current_folder_offset = 3;
    dialog.m_scan_complete = false;

    Q_EMIT controller.folderItemsFailed(11, 3, QStringLiteral("wrong folder"));

    QCOMPARE(dialog.m_folders_scanned, 0);
    QCOMPARE(dialog.m_current_folder_offset, 3);
    QVERIFY(!dialog.m_status_label->text().contains(QStringLiteral("wrong folder")));
}

void EmailAttachmentsBrowserDialogTests::matchingFolderFailureAdvancesOnce() {
    EmailInspectorController controller;
    sak::EmailAttachmentsBrowserDialog dialog(&controller, {});
    finishInitialEmptyScan(&dialog);
    dialog.m_all_folder_ids = {10, 20};
    dialog.m_folders_scanned = 0;
    dialog.m_current_folder_offset = 3;
    dialog.m_scan_complete = false;

    Q_EMIT controller.folderItemsFailed(10, 3, QStringLiteral("folder read failed"));

    QCOMPARE(dialog.m_folders_scanned, 1);
    QCOMPARE(dialog.m_current_folder_offset, 0);
    QCOMPARE(dialog.m_status_label->text(),
             QStringLiteral("Skipped folder 10: folder read failed"));
}

void EmailAttachmentsBrowserDialogTests::wrongItemFailureDoesNotAdvance() {
    EmailInspectorController controller;
    sak::EmailAttachmentsBrowserDialog dialog(&controller, {});
    finishInitialEmptyScan(&dialog);
    dialog.m_pending_detail_ids = {100};
    dialog.m_details_loaded = 0;
    dialog.m_details_total = 1;
    dialog.m_scan_complete = false;

    Q_EMIT controller.itemDetailFailed(101, QStringLiteral("wrong item"));

    QCOMPARE(dialog.m_details_loaded, 0);
    QVERIFY(!dialog.m_status_label->text().contains(QStringLiteral("wrong item")));
}

void EmailAttachmentsBrowserDialogTests::matchingItemFailureAdvancesOnce() {
    EmailInspectorController controller;
    sak::EmailAttachmentsBrowserDialog dialog(&controller, {});
    finishInitialEmptyScan(&dialog);
    dialog.m_pending_detail_ids = {100, 200};
    dialog.m_details_loaded = 0;
    dialog.m_details_total = 2;
    dialog.m_scan_complete = false;

    Q_EMIT controller.itemDetailFailed(100, QStringLiteral("detail read failed"));

    QCOMPARE(dialog.m_details_loaded, 1);
    QCOMPARE(dialog.m_status_label->text(), QStringLiteral("Skipped item 100: detail read failed"));
}

void EmailAttachmentsBrowserDialogTests::genericErrorIsVisibleWithoutMutatingState() {
    EmailInspectorController controller;
    sak::EmailAttachmentsBrowserDialog dialog(&controller, {});
    finishInitialEmptyScan(&dialog);
    dialog.m_all_folder_ids = {10};
    dialog.m_pending_detail_ids = {100};
    dialog.m_folders_scanned = 0;
    dialog.m_current_folder_offset = 3;
    dialog.m_details_loaded = 0;
    dialog.m_details_total = 1;
    dialog.m_scan_complete = false;

    Q_EMIT controller.errorOccurred(QStringLiteral("generic failure"));

    QCOMPARE(dialog.m_folders_scanned, 0);
    QCOMPARE(dialog.m_current_folder_offset, 3);
    QCOMPARE(dialog.m_details_loaded, 0);
    QCOMPARE(dialog.m_status_label->text(),
             QStringLiteral("Attachment browser error: generic failure"));
    QCOMPARE(dialog.m_status_label->textFormat(), Qt::PlainText);
    QCOMPARE(dialog.m_status_label->accessibleName(), QStringLiteral("Attachment browser status"));
}

void EmailAttachmentsBrowserDialogTests::specificAndGenericSaveErrorsCountOnce() {
    EmailInspectorController controller;
    sak::EmailAttachmentsBrowserDialog dialog(&controller, {});
    finishInitialEmptyScan(&dialog);
    dialog.m_scan_complete = true;
    QTemporaryDir output;
    QVERIFY(output.isValid());
    const sak::AttachmentRef first{.message_id = 1, .index = 0};
    const sak::AttachmentRef second{.message_id = 2, .index = 0};
    QVERIFY(dialog.m_batch_save.begin(output.path(), {first, second}));

    Q_EMIT controller.attachmentContentFailed(1, 0, QStringLiteral("specific failure"));
    QCOMPARE(dialog.m_batch_save.failed(), 1);
    QCOMPARE(dialog.m_batch_save.expectedCount(), 2);
    QVERIFY(dialog.m_batch_save.isActive());

    Q_EMIT controller.errorOccurred(QStringLiteral("specific failure"));
    QCOMPARE(dialog.m_batch_save.failed(), 1);
    QCOMPARE(dialog.m_batch_save.expectedCount(), 2);
    QVERIFY(dialog.m_batch_save.isActive());

    Q_EMIT controller.attachmentContentReady(
        2, 0, QByteArrayLiteral("saved"), QStringLiteral("saved.txt"));
    QVERIFY(!dialog.m_batch_save.isActive());
    QCOMPARE(dialog.m_status_label->text(),
             QStringLiteral("Saved 1 of 2 attachment(s) (1 failed)"));
}

QTEST_MAIN(EmailAttachmentsBrowserDialogTests)
#include "test_email_attachments_browser_dialog.moc"
