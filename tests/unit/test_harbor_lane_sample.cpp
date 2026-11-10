// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_harbor_lane_sample.cpp
/// @brief Proves the app reads the fictional sample mailbox the way the wiki's screenshots show.

#include "sak/email_folder_selection.h"
#include "sak/email_search_worker.h"
#include "sak/mbox_parser.h"
#include "sak/ost_conversion_worker.h"
#include "sak/ost_converter_controller.h"
#include "sak/pst_parser.h"
#include "sak/pst_recurrence.h"

#include "../support/harbor_lane_sample.h"

#include <QDirIterator>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {

const QDate kCalendarMonth(2026, 11, 1);

const sak::PstFolder* folderNamed(const sak::PstFolderTree& tree, const QString& name) {
    for (const auto& folder : tree) {
        if (folder.display_name == name) {
            return &folder;
        }
        if (const auto* found = folderNamed(folder.children, name)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

class TestHarborLaneSample : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_path;
    PstParser m_parser;

    [[nodiscard]] QVector<sak::PstItemSummary> itemsIn(const QString& folder_name) {
        const auto* folder = folderNamed(m_parser.folderTree(), folder_name);
        if (folder == nullptr) {
            return {};
        }
        return m_parser.readFolderItems(folder->node_id, 0, 0)
            .value_or(QVector<sak::PstItemSummary>{});
    }

private Q_SLOTS:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        m_path = m_dir.filePath(QStringLiteral("harbor-lane-sample.pst"));
        QFile file(m_path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(sak::pst_fixture::harbor_lane::build(kCalendarMonth));
        file.close();
        m_parser.open(m_path);
        QVERIFY(m_parser.isOpen());
    }

    void itIsNamedAndShapedAsTheScreenshotsExpect() {
        QCOMPARE(m_parser.fileInfo().display_name, QStringLiteral("Harbor Lane Bakery"));
        const auto* ipm = sak::findIpmSubtree(m_parser.folderTree());
        QVERIFY(ipm != nullptr);
        QStringList names;
        for (const auto& folder : ipm->children) {
            names.append(
                QStringLiteral("%1 (%2)").arg(folder.display_name).arg(folder.content_count));
        }
        QCOMPARE(names,
                 QStringList({QStringLiteral("Inbox (6)"),
                              QStringLiteral("Drafts (1)"),
                              QStringLiteral("Sent Items (3)"),
                              QStringLiteral("Deleted Items (2)"),
                              QStringLiteral("Contacts (5)"),
                              QStringLiteral("Calendar (5)"),
                              QStringLiteral("Tasks (2)"),
                              QStringLiteral("Notes (1)")}));
    }

    void theInboxHoldsTheSpecifiedMailAndAttachments() {
        const auto inbox = itemsIn(QStringLiteral("Inbox"));
        QCOMPARE(inbox.size(), 6);
        QHash<QString, sak::PstItemDetail> by_subject;
        for (const auto& item : inbox) {
            const auto detail = m_parser.readItemDetail(item.node_id);
            QVERIFY(detail.has_value());
            by_subject.insert(detail->subject, *detail);
        }
        const auto expect_attachments =
            [&](const QString& subject, const QString& sender, const QStringList& names) {
                QVERIFY2(by_subject.contains(subject), qPrintable(subject));
                const auto& detail = by_subject[subject];
                QCOMPARE(detail.sender_name, sender);
                QVERIFY(!detail.body_plain.isEmpty());
                QVERIFY(!detail.body_html.isEmpty());
                QVERIFY(!detail.transport_headers.isEmpty());
                QStringList found;
                for (const auto& att : detail.attachments) {
                    found.append(att.long_filename);
                    const auto data = m_parser.readAttachmentData(detail.node_id, att.index);
                    QVERIFY(data.has_value());
                    QVERIFY(!data->isEmpty());
                }
                QCOMPARE(found, names);
            };
        expect_attachments(QStringLiteral("Spring menu draft"),
                           QStringLiteral("Marisol Quint"),
                           {QStringLiteral("spring_menu.pdf")});
        expect_attachments(QStringLiteral("Oven repair quote"),
                           QStringLiteral("Theo Brandt"),
                           {QStringLiteral("oven_quote.docx")});
        expect_attachments(QStringLiteral("Invoice 1042 for flour delivery"),
                           QStringLiteral("Priya Venn"),
                           {QStringLiteral("invoice_1042.pdf")});
        expect_attachments(QStringLiteral("Storefront photos"),
                           QStringLiteral("Marisol Quint"),
                           {QStringLiteral("storefront.jpg"), QStringLiteral("awning.jpg")});
        expect_attachments(QStringLiteral("Saturday staff schedule"),
                           QStringLiteral("Owen Lark"),
                           {});
        expect_attachments(QStringLiteral("Farmers market booth"),
                           QStringLiteral("Ines Caldera"),
                           {});
    }

    void itsPicturesAndDocumentsAreRealFiles() {
        for (const auto& item : itemsIn(QStringLiteral("Inbox"))) {
            const auto detail = m_parser.readItemDetail(item.node_id);
            QVERIFY(detail.has_value());
            for (const auto& att : detail->attachments) {
                const auto data = m_parser.readAttachmentData(detail->node_id, att.index);
                QVERIFY(data.has_value());
                if (att.long_filename.endsWith(QStringLiteral(".jpg"))) {
                    QVERIFY(!QImage::fromData(*data).isNull());
                } else if (att.long_filename.endsWith(QStringLiteral(".pdf"))) {
                    QVERIFY(data->startsWith("%PDF-"));
                    QVERIFY(data->contains("%%EOF"));
                } else {
                    QVERIFY(data->startsWith("PK"));
                }
            }
        }
    }

    void contactsCalendarAndTasksCarryTheirFields() {
        const auto contacts = itemsIn(QStringLiteral("Contacts"));
        QCOMPARE(contacts.size(), 5);
        const auto person = m_parser.readItemDetail(contacts.first().node_id);
        QVERIFY(person.has_value());
        QCOMPARE(person->item_type, sak::EmailItemType::Contact);
        QVERIFY(person->email_address.contains(QStringLiteral(".example")));
        QVERIFY(!person->company_name.isEmpty());
        QVERIFY(!person->business_phone.isEmpty());

        const auto events = itemsIn(QStringLiteral("Calendar"));
        QCOMPARE(events.size(), 5);
        for (const auto& item : events) {
            const auto event = m_parser.readItemDetail(item.node_id);
            QVERIFY(event.has_value());
            QCOMPARE(event->start_time.date().month(), kCalendarMonth.month());
            QVERIFY(event->end_time > event->start_time);
            QVERIFY(!event->location.isEmpty());
            if (event->subject == QStringLiteral("Staff meeting")) {
                QCOMPARE(event->attendees.size(), 3);
                QCOMPARE(event->recurrence.state, sak::RecurrenceState::Readable);
                QVERIFY2(event->recurrence.rrule.contains(QStringLiteral("COUNT=12")),
                         qPrintable(event->recurrence.rrule));
                QCOMPARE(event->recurrence.deleted.size(), 1);
                QCOMPARE(sak::pst::zoneWallClockToUtc(event->time_zone,
                                                      event->recurrence.wall_start),
                         event->start_time);
            } else {
                QVERIFY(event->attendees.isEmpty());
                QCOMPARE(event->recurrence.state, sak::RecurrenceState::None);
            }
        }

        const auto tasks = itemsIn(QStringLiteral("Tasks"));
        QCOMPARE(tasks.size(), 2);
    }

    void searchingForOvenFindsTheQuote() {
        EmailSearchWorker worker;
        QStringList subjects;
        QObject::connect(&worker,
                         &EmailSearchWorker::searchHit,
                         &worker,
                         [&subjects](const sak::EmailSearchHit& hit) {
                             subjects.append(hit.subject);
                         });
        sak::EmailSearchCriteria criteria;
        criteria.query_text = QStringLiteral("oven");
        worker.search(&m_parser, criteria);
        QVERIFY2(subjects.contains(QStringLiteral("Oven repair quote")),
                 qPrintable(subjects.join(QLatin1Char('|'))));
    }

    void itConvertsCleanlyToMbox() {
        const QString out = m_dir.filePath(QStringLiteral("mbox"));
        QVERIFY(QDir().mkpath(out));
        sak::OstConversionConfig config;
        config.output_directory = out;
        sak::OstConversionWorker worker;
        sak::OstConversionResult result;
        QObject::connect(&worker,
                         &sak::OstConversionWorker::conversionFinished,
                         &worker,
                         [&result](const sak::OstConversionResult& finished) {
                             result = finished;
                         });
        worker.convert(m_path, config);

        QCOMPARE(result.items_converted, 12);
        QCOMPARE(result.items_failed, 0);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.skipped_contacts, 5);
        QCOMPARE(result.skipped_calendar, 5);
        QCOMPARE(result.skipped_tasks, 2);
        QCOMPARE(result.skipped_notes, 1);
        QCOMPARE(sak::OstConverterController::classifyOutcome(result),
                 sak::OstConversionJob::Status::Complete);

        int total = 0;
        QDirIterator it(out, {QStringLiteral("*.mbox")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            MboxParser parser;
            parser.open(it.next());
            QVERIFY(parser.isOpen());
            QVERIFY(parser.readMessages(0, 1).has_value());
            total += parser.messageCount();
        }
        QCOMPARE(total, 12);
    }
};

QTEST_MAIN(TestHarborLaneSample)
#include "test_harbor_lane_sample.moc"
