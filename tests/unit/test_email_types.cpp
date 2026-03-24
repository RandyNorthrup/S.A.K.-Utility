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
}

void TestEmailTypes::pstAttachmentInfoDefaults() {
    sak::PstAttachmentInfo att;
    QCOMPARE(att.index, 0);
    QVERIFY(att.filename.isEmpty());
    QCOMPARE(att.size_bytes, qint64(0));
    QCOMPARE(att.is_embedded_message, false);
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
    QCOMPARE(static_cast<int>(sak::EmailItemType::Email), 0);
    QCOMPARE(static_cast<int>(sak::EmailItemType::Unknown), 8);
}

void TestEmailTypes::exportFormatValues() {
    // Verify all export format values are distinct
    QSet<int> values;
    values.insert(static_cast<int>(sak::ExportFormat::Eml));
    values.insert(static_cast<int>(sak::ExportFormat::CsvEmails));
    values.insert(static_cast<int>(sak::ExportFormat::Vcf));
    values.insert(static_cast<int>(sak::ExportFormat::CsvContacts));
    values.insert(static_cast<int>(sak::ExportFormat::Ics));
    values.insert(static_cast<int>(sak::ExportFormat::CsvCalendar));
    values.insert(static_cast<int>(sak::ExportFormat::CsvTasks));
    values.insert(static_cast<int>(sak::ExportFormat::PlainTextNotes));
    values.insert(static_cast<int>(sak::ExportFormat::Attachments));
    QCOMPARE(values.size(), 9);
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
    QVERIFY(sak::email::kMaxFolderDepth > 0);
    QVERIFY(sak::email::kMaxItemsPerLoad > 0);
    QVERIFY(sak::email::kMaxSearchResults > 0);
    QVERIFY(sak::email::kMaxAttachmentSize > 0);
    QVERIFY(sak::email::kMaxFileSize > 0);
    QVERIFY(sak::email::kMaxExportBatchSize > 0);
    QVERIFY(sak::email::kMaxFilenameLength > 0);
}

void TestEmailTypes::uiConstantsReasonable() {
    QVERIFY(sak::email::kFolderTreeMinWidth > 0);
    QVERIFY(sak::email::kFolderTreeDefaultWidth >= sak::email::kFolderTreeMinWidth);
    QVERIFY(sak::email::kItemListMinHeight > 0);
    QVERIFY(sak::email::kDetailPanelMinHeight > 0);
    QVERIFY(sak::email::kSearchDebounceMs > 0);
}

QTEST_MAIN(TestEmailTypes)
#include "test_email_types.moc"
