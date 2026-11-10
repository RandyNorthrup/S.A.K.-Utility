// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_converter_widget.cpp
/// @brief The converter tab: what a queue row says, and the options it hands the worker.

#include "sak/ost_converter_constants.h"
#include "sak/ost_converter_widget.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateEdit>
#include <QLineEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QtTest/QtTest>

class OstConverterWidgetTests : public QObject {
    Q_OBJECT

private:
    static void addQueueRow(sak::OstConverterWidget& widget) {
        sak::OstConversionJob job;
        job.display_name = QStringLiteral("mail.pst");
        widget.onFileAdded(0, job);
    }

    static QString rowStatus(const sak::OstConverterWidget& widget) {
        return widget.m_queue_table->item(0, sak::ost::ColStatus)->text();
    }

private Q_SLOTS:
    // Review 2 (S17): the filters' fields started at 120, 96 and 108 px.
    void theFilterFieldsShareALeftEdge() {
        sak::OstConverterWidget widget;
        widget.resize(1280, 860);
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        const auto left = [&widget](const QWidget* field) {
            return field->mapTo(&widget, QPoint(0, 0)).x();
        };
        QVERIFY(widget.m_date_from_edit->isVisible());
        QCOMPARE(left(widget.m_sender_filter_edit), left(widget.m_date_from_edit));
        QCOMPARE(left(widget.m_recipient_filter_edit), left(widget.m_date_from_edit));
    }

    // A partial conversion once read Complete, because the row had its own rule: "failed" only
    // when nothing at all was written.
    void aRowWithAnyFailedItemReadsFailed() {
        sak::OstConverterWidget widget;
        addQueueRow(widget);
        sak::OstConversionResult result;
        result.items_converted = 780;
        result.items_failed = 3;
        widget.onFileConversionComplete(0, result);
        QCOMPARE(rowStatus(widget), QStringLiteral("Failed"));
        QCOMPARE(widget.m_queue_table->item(0, sak::ost::ColProgress)->text(),
                 QStringLiteral("780 messages converted, 3 failed"));
    }

    void aRowWithARecordedErrorReadsFailed() {
        sak::OstConverterWidget widget;
        addQueueRow(widget);
        sak::OstConversionResult result;
        result.items_converted = 10;
        result.errors.append(QStringLiteral("MBOX export did not finalize"));
        widget.onFileConversionComplete(0, result);
        QCOMPARE(rowStatus(widget), QStringLiteral("Failed"));
    }

    void aCleanRunReadsCompleteAndSaysWhatItLeftOut() {
        sak::OstConverterWidget widget;
        addQueueRow(widget);
        sak::OstConversionResult result;
        result.items_converted = 12;
        result.skipped_contacts = 4;
        result.skipped_calendar = 1;
        widget.onFileConversionComplete(0, result);
        QCOMPARE(rowStatus(widget), QStringLiteral("Complete"));
        QCOMPARE(widget.m_queue_table->item(0, sak::ost::ColProgress)->text(),
                 QStringLiteral("12 messages converted, 5 items left out (not mail)"));
    }

    void aCancelledRunReadsCancelled() {
        sak::OstConverterWidget widget;
        addQueueRow(widget);
        sak::OstConversionResult result;
        result.items_converted = 2;
        result.cancelled = true;
        widget.onFileConversionComplete(0, result);
        QCOMPARE(rowStatus(widget), QStringLiteral("Cancelled"));
    }

    // Midnight on the end date left out everything sent that day.
    void theDateFilterCoversAllOfTheEndDate() {
        sak::OstConverterWidget widget;
        widget.m_filters_enabled_check->setChecked(true);
        widget.m_date_filter_check->setChecked(true);
        widget.m_date_from_edit->setDate(QDate(2026, 3, 1));
        widget.m_date_to_edit->setDate(QDate(2026, 3, 9));
        const sak::OstConversionConfig config = widget.buildConfig();
        QCOMPARE(config.date_from, QDate(2026, 3, 1).startOfDay());
        QCOMPARE(config.date_to, QDate(2026, 3, 9).endOfDay());
        QVERIFY(QDateTime(QDate(2026, 3, 9), QTime(17, 30)) <= config.date_to);
    }

    void theQueueHasNoItemsColumn() {
        sak::OstConverterWidget widget;
        QCOMPARE(widget.m_queue_table->columnCount(), static_cast<int>(sak::ost::ColCount));
        QStringList headers;
        for (int col = 0; col < widget.m_queue_table->columnCount(); ++col) {
            headers.append(widget.m_queue_table->horizontalHeaderItem(col)->text());
        }
        QCOMPARE(headers,
                 QStringList({QStringLiteral("File"),
                              QStringLiteral("Size"),
                              QStringLiteral("Status"),
                              QStringLiteral("Progress")}));
    }

    // A whole store holding nothing is an answer, not a blank line in the queue.
    void aStoreWithNoMessagesSaysSo() {
        sak::OstConversionResult empty;
        QCOMPARE(sak::OstConverterWidget::progressSummary(empty),
                 QStringLiteral("No messages in this file"));
        sak::OstConversionResult only_other;
        only_other.skipped_calendar = 2;
        QCOMPARE(sak::OstConverterWidget::progressSummary(only_other),
                 QStringLiteral("0 messages converted, 2 items left out (not mail)"));
    }
};

QTEST_MAIN(OstConverterWidgetTests)
#include "test_ost_converter_widget.moc"
