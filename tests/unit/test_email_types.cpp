// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_types.cpp
/// @brief Unit tests for email data structures and enum defaults

#include "sak/email_constants.h"
#include "sak/email_types.h"

#include <QtTest/QtTest>

class TestEmailTypes : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Struct Defaults -------------------------------------------------
    void pstFileInfoDefaults();
    void pstFolderDefaults();
    void pstItemSummaryDefaults();
    void pstItemDetailDefaults();
    void pstAttachmentInfoDefaults();
    void mapiPropertyDefaults();
    void mboxMessageDefaults();
    void mboxMessageDetailDefaults();
    void emailSearchCriteriaDefaults();
    void emailExportConfigDefaults();
    void emailExportResultDefaults();
    void emailClientProfileDefaults();
    void emailSearchHitDefaults();

    // -- Enum Coverage ---------------------------------------------------
    void emailItemTypeValues();
    void exportFormatValues();
    void clientTypeValues();

    // -- Constants -------------------------------------------------------
    void pstMagicConstant();
    void parserLimitsPositive();
    void uiConstantsReasonable();
};

// ============================================================================
// Struct Defaults
// ============================================================================

void TestEmailTypes::pstFileInfoDefaults() {
    sak::PstFileInfo info;
    QVERIFY(info.file_path.isEmpty());
    QVERIFY(info.display_name.isEmpty());
    QCOMPARE(info.file_size_bytes, qint64(0));
    QCOMPARE(info.is_unicode, false);
    QCOMPARE(info.is_ost, false);
    QCOMPARE(info.encryption_type, uint8_t(0));
    QCOMPARE(info.total_folders, 0);
    QCOMPARE(info.total_items, 0);
}

void TestEmailTypes::pstFolderDefaults() {
    sak::PstFolder folder;
    QCOMPARE(folder.node_id, uint64_t(0));
    QCOMPARE(folder.parent_node_id, uint64_t(0));
    QVERIFY(folder.display_name.isEmpty());
    QCOMPARE(folder.content_count, 0);
    QCOMPARE(folder.unread_count, 0);
    QCOMPARE(folder.subfolder_count, 0);
    // container_class is the field that ROUTES the folder: isMailFolder() treats an unset
    // class as mail, so a non-empty default would hide every PST folder that omits
    // PR_CONTAINER_CLASS from the tree AND drop it from "Export ALL Mail Folders".
    QVERIFY(folder.container_class.isEmpty());
    QVERIFY(folder.children.isEmpty());
}

void TestEmailTypes::pstItemSummaryDefaults() {
    sak::PstItemSummary item;
    QCOMPARE(item.node_id, uint64_t(0));
    QCOMPARE(item.item_type, sak::EmailItemType::Unknown);
    QVERIFY(item.subject.isEmpty());
    QCOMPARE(item.has_attachments, false);
    QCOMPARE(item.is_read, false);
    QCOMPARE(item.importance, 1);  // Normal priority default
    QCOMPARE(item.size_bytes, qint64(0));
    // pst_parser.cpp:698 derives is_read from bit 0x01 of this word and :700-702 derives
    // has_attachments from bit 0x10, so a nonzero default would contradict the two bools above.
    QCOMPARE(item.message_flags, uint32_t(0));
}

void TestEmailTypes::pstItemDetailDefaults() {
    sak::PstItemDetail detail;
    QCOMPARE(detail.node_id, uint64_t(0));
    QCOMPARE(detail.item_type, sak::EmailItemType::Unknown);
    QVERIFY(detail.subject.isEmpty());
    QVERIFY(detail.body_plain.isEmpty());
    QVERIFY(detail.body_html.isEmpty());
    QVERIFY(detail.transport_headers.isEmpty());
    QVERIFY(detail.attachments.isEmpty());
    QCOMPARE(detail.is_all_day, false);
    QCOMPARE(detail.task_percent_complete, 0.0);
    // The two NON-ZERO defaults in this struct; nothing else in tests/ pins either, and both are
    // live behaviour: importance 0 makes mbox_writer.cpp:713 -> appendImportanceHeader stamp
    // "Importance: Low" (kImportanceLow == 0) and email_export_worker.cpp:412 write "Low" into the
    // CSV column, while note_color is assigned NOWHERE in src/ -- this default alone picks the
    // swatch email_inspector_panel.cpp:2234-2239 renders (index 3 == Yellow, 0 == Blue).
    QCOMPARE(detail.importance, 1);  // Normal, not Low
    QCOMPARE(detail.note_color, 3);  // Yellow
    QCOMPARE(sak::kDefaultNoteColorYellow, 3);
}

void TestEmailTypes::pstAttachmentInfoDefaults() {
    sak::PstAttachmentInfo att;
    QCOMPARE(att.index, 0);
    QVERIFY(att.filename.isEmpty());
    QCOMPARE(att.size_bytes, qint64(0));
    QCOMPARE(att.is_embedded_message, false);
    QCOMPARE(att.attach_method, 0);  // unset: neither EmbeddedMessage(5) nor OLE(6), so not skipped
    // derived flag must stay coherent with the raw method it is computed from
    // (pst_parser.cpp:756-757)
    QCOMPARE(att.is_embedded_message, att.attach_method == sak::email::kAttachEmbeddedMessage);
    QVERIFY(att.attach_method != sak::email::kAttachOle);
    QVERIFY(att.long_filename.isEmpty());
    QVERIFY(att.mime_type.isEmpty());
    QVERIFY(att.content_id.isEmpty());
}

void TestEmailTypes::mapiPropertyDefaults() {
    sak::MapiProperty prop;
    QCOMPARE(prop.tag_id, uint16_t(0));
    QCOMPARE(prop.tag_type, uint16_t(0));
    QVERIFY(prop.property_name.isEmpty());
    QVERIFY(prop.display_value.isEmpty());
    QVERIFY(prop.raw_value.isEmpty());
}

void TestEmailTypes::mboxMessageDefaults() {
    sak::MboxMessage msg;
    QCOMPARE(msg.message_index, 0);
    QCOMPARE(msg.file_offset, qint64(0));
    QCOMPARE(msg.message_size, qint64(0));
    QVERIFY(msg.subject.isEmpty());
    QVERIFY(msg.from.isEmpty());
    QVERIFY(msg.to.isEmpty());
    QVERIFY(msg.cc.isEmpty());
    QVERIFY(!msg.date.isValid());
    QCOMPARE(msg.has_attachments, false);
}

void TestEmailTypes::mboxMessageDetailDefaults() {
    sak::MboxMessageDetail detail;
    QCOMPARE(detail.message_index, 0);
    QVERIFY(detail.subject.isEmpty());
    QVERIFY(detail.body_plain.isEmpty());
    QVERIFY(detail.body_html.isEmpty());
}

void TestEmailTypes::emailSearchCriteriaDefaults() {
    sak::EmailSearchCriteria criteria;
    QVERIFY(criteria.query_text.isEmpty());
    QCOMPARE(criteria.search_subject, true);
    QCOMPARE(criteria.search_body, true);
    QCOMPARE(criteria.search_sender, true);
    QCOMPARE(criteria.case_sensitive, false);
    QCOMPARE(criteria.has_attachment_only, false);
}

void TestEmailTypes::emailExportConfigDefaults() {
    sak::EmailExportConfig config;
    QCOMPARE(config.format, sak::ExportFormat::Eml);
    QVERIFY(config.output_path.isEmpty());
    QVERIFY(config.item_ids.isEmpty());
    QCOMPARE(config.recurse_subfolders, false);
    QCOMPARE(config.csv_include_header, true);
    QCOMPARE(config.flatten_attachments, true);
    QCOMPARE(config.skip_inline_images, true);
    QCOMPARE(config.save_attachments_with_messages, true);
    QCOMPARE(config.prefix_with_date, true);
}

void TestEmailTypes::emailExportResultDefaults() {
    sak::EmailExportResult result;
    QVERIFY(result.export_path.isEmpty());
    QCOMPARE(result.items_exported, 0);
    QCOMPARE(result.items_failed, 0);
    QCOMPARE(result.total_bytes, qint64(0));
}

void TestEmailTypes::emailClientProfileDefaults() {
    sak::EmailClientProfile profile;
    QCOMPARE(profile.client_type, sak::EmailClientType::Other);
    QVERIFY(profile.client_name.isEmpty());
    QVERIFY(profile.data_files.isEmpty());
    QCOMPARE(profile.total_size_bytes, qint64(0));
}

void TestEmailTypes::emailSearchHitDefaults() {
    sak::EmailSearchHit hit;
    QCOMPARE(hit.item_node_id, uint64_t(0));
    QCOMPARE(hit.item_type, sak::EmailItemType::Unknown);
    QVERIFY(hit.subject.isEmpty());
    QVERIFY(hit.context_snippet.isEmpty());
}

// ============================================================================
// Enum Coverage
// ============================================================================

void TestEmailTypes::emailItemTypeValues() {
    // Full-catalog census: naming all nine enumerators makes a removal or rename
    // a compile error here, and pinning every ordinal makes a permutation or a
    // mid-catalog insertion fail rather than slide past the endpoints.
    QCOMPARE(static_cast<int>(sak::EmailItemType::Email), 0);
    QCOMPARE(static_cast<int>(sak::EmailItemType::Contact), 1);
    QCOMPARE(static_cast<int>(sak::EmailItemType::Calendar), 2);
    QCOMPARE(static_cast<int>(sak::EmailItemType::Task), 3);
    QCOMPARE(static_cast<int>(sak::EmailItemType::StickyNote), 4);
    QCOMPARE(static_cast<int>(sak::EmailItemType::JournalEntry), 5);
    QCOMPARE(static_cast<int>(sak::EmailItemType::DistList), 6);
    QCOMPARE(static_cast<int>(sak::EmailItemType::MeetingRequest), 7);

    // Unknown is the terminal member: the search worker reads it as "match every
    // type" (email_search_worker.cpp:379-382) and the parser as "message class
    // not resolved yet" (pst_parser.cpp:3176, 3189), so it must stay the highest
    // of the nine and must not collide with a real type.
    QCOMPARE(static_cast<int>(sak::EmailItemType::Unknown), 8);
    QVERIFY(static_cast<int>(sak::EmailItemType::Unknown) >
            static_cast<int>(sak::EmailItemType::MeetingRequest));
}

void TestEmailTypes::exportFormatValues() {
    // Verify all export format values are distinct
    QSet<int> values;
    values.insert(static_cast<int>(sak::ExportFormat::Eml));
    values.insert(static_cast<int>(sak::ExportFormat::Html));
    values.insert(static_cast<int>(sak::ExportFormat::Text));
    values.insert(static_cast<int>(sak::ExportFormat::Pdf));
    values.insert(static_cast<int>(sak::ExportFormat::CsvEmails));
    values.insert(static_cast<int>(sak::ExportFormat::Vcf));
    values.insert(static_cast<int>(sak::ExportFormat::CsvContacts));
    values.insert(static_cast<int>(sak::ExportFormat::Ics));
    values.insert(static_cast<int>(sak::ExportFormat::CsvCalendar));
    values.insert(static_cast<int>(sak::ExportFormat::CsvTasks));
    values.insert(static_cast<int>(sak::ExportFormat::PlainTextNotes));
    values.insert(static_cast<int>(sak::ExportFormat::Attachments));
    QCOMPARE(static_cast<int>(sak::ExportFormat::Eml), 0);
    QCOMPARE(static_cast<int>(sak::ExportFormat::Html), 1);
    QCOMPARE(static_cast<int>(sak::ExportFormat::Text), 2);
    QCOMPARE(static_cast<int>(sak::ExportFormat::Pdf), 3);
    QCOMPARE(static_cast<int>(sak::ExportFormat::CsvEmails), 4);
    QCOMPARE(static_cast<int>(sak::ExportFormat::Vcf), 5);
    QCOMPARE(static_cast<int>(sak::ExportFormat::CsvContacts), 6);
    QCOMPARE(static_cast<int>(sak::ExportFormat::Ics), 7);
    QCOMPARE(static_cast<int>(sak::ExportFormat::CsvCalendar), 8);
    QCOMPARE(static_cast<int>(sak::ExportFormat::CsvTasks), 9);
    QCOMPARE(static_cast<int>(sak::ExportFormat::PlainTextNotes), 10);
    QCOMPARE(static_cast<int>(sak::ExportFormat::Attachments), 11);
    // Attachments==11 plus a 12-element set pins the catalog length from both ends.
    QCOMPARE(values.size(), 12);
}

void TestEmailTypes::clientTypeValues() {
    QSet<int> values;
    values.insert(static_cast<int>(sak::EmailClientType::Outlook));
    values.insert(static_cast<int>(sak::EmailClientType::Thunderbird));
    values.insert(static_cast<int>(sak::EmailClientType::WindowsMail));
    values.insert(static_cast<int>(sak::EmailClientType::Other));
    QCOMPARE(values.size(), 4);
}

// ============================================================================
// Constants
// ============================================================================

void TestEmailTypes::pstMagicConstant() {
    QCOMPARE(sak::email::kPstMagic, uint32_t(0x4E'44'42'21));
}

void TestEmailTypes::parserLimitsPositive() {
    QCOMPARE(sak::email::kMaxFolderDepth, 50);
    QCOMPARE(sak::email::kMaxItemsPerLoad, 500);
    QCOMPARE(sak::email::kMaxSearchResults, 10'000);
    QCOMPARE(sak::email::kMaxAttachmentSize, static_cast<int64_t>(524'288'000));
    QCOMPARE(sak::email::kMaxFileSize, static_cast<int64_t>(53'687'091'200));
    QCOMPARE(sak::email::kMaxExportBatchSize, 50'000);
    QCOMPARE(sak::email::kMaxFilenameLength, 200);
}

void TestEmailTypes::uiConstantsReasonable() {
    QCOMPARE(sak::email::kFolderTreeMinWidth, 200);
    QCOMPARE(sak::email::kFolderTreeDefaultWidth, 280);
    QCOMPARE(sak::email::kItemListMinHeight, 150);
    QCOMPARE(sak::email::kDetailPanelMinHeight, 200);
    QCOMPARE(sak::email::kSearchDebounceMs, 300);
}

QTEST_MAIN(TestEmailTypes)
#include "test_email_types.moc"
