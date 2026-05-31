// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_integration.cpp
/// @brief Integration test: opens real OST/PST files and reports parse results

#include "sak/email_export_worker.h"
#include "sak/error_codes.h"
#include "sak/pst_parser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QtTest/QtTest>

static QString configuredOstPath() {
    return QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("SAK_TEST_OST_PATH"))
        .trimmed();
}

static QString configuredPstPath() {
    return QProcessEnvironment::systemEnvironment()
        .value(QStringLiteral("SAK_TEST_PST_PATH"))
        .trimmed();
}

// ================================================================
// Helpers: tree printing and folder item verification
// ================================================================

static void printFolder(const sak::PstFolder& folder, int indent) {
    QString pad(indent * 2, ' ');
    qDebug().noquote()
        << pad << QString("NID=0x%1").arg(folder.node_id, 0, 16) << folder.display_name
        << QString("(%1 items, %2 unread)").arg(folder.content_count).arg(folder.unread_count)
        << folder.container_class;
    for (const auto& child : folder.children) {
        printFolder(child, indent + 1);
    }
}

static void printFolderTree(const sak::PstFolderTree& tree) {
    qDebug() << "\n=== FOLDER TREE ===";
    for (const auto& root : tree) {
        printFolder(root, 0);
    }
}

static const sak::PstFolder* findNonEmptyFolder(const sak::PstFolderTree& tree) {
    for (const auto& folder : tree) {
        uint8_t nid_type = static_cast<uint8_t>(folder.node_id & 0x1F);
        bool is_normal = nid_type == sak::email::kNidTypeNormalFolder;
        if (folder.content_count > 0 && is_normal) {
            return &folder;
        }
        const auto* child = findNonEmptyFolder(folder.children);
        if (child) {
            return child;
        }
    }
    return nullptr;
}

static bool hasExportedFile(const QString& dir_path, const QString& suffix) {
    QDirIterator it(dir_path,
                    QStringList{QStringLiteral("*.") + suffix},
                    QDir::Files,
                    QDirIterator::Subdirectories);
    return it.hasNext();
}

static void verifySingleItemExports(PstParser& parser, uint64_t item_id) {
    QTemporaryDir temp_dir;
    QVERIFY2(temp_dir.isValid(), "Temporary export directory should be valid");

    struct ExportSmokeCase {
        sak::ExportFormat format;
        QString suffix;
    };
    const QVector<ExportSmokeCase> cases = {
        {sak::ExportFormat::Html, QStringLiteral("html")},
        {sak::ExportFormat::Text, QStringLiteral("txt")},
        {sak::ExportFormat::Eml, QStringLiteral("eml")},
        {sak::ExportFormat::Pdf, QStringLiteral("pdf")},
    };

    for (const auto& smoke_case : cases) {
        const QString out_dir = temp_dir.path() + QLatin1Char('/') + smoke_case.suffix;
        QVERIFY(QDir().mkpath(out_dir));

        EmailExportWorker worker;
        sak::EmailExportResult result;
        bool complete = false;
        QObject::connect(&worker,
                         &EmailExportWorker::exportComplete,
                         [&](sak::EmailExportResult emitted) {
                             result = emitted;
                             complete = true;
                         });

        sak::EmailExportConfig config;
        config.format = smoke_case.format;
        config.output_path = out_dir;
        config.item_ids = {item_id};
        config.save_attachments_with_messages = true;
        worker.exportItems(&parser, config);

        QVERIFY2(complete, "Export worker should emit completion");
        QCOMPARE(result.items_exported, 1);
        QCOMPARE(result.items_failed, 0);
        QVERIFY2(hasExportedFile(out_dir, smoke_case.suffix),
                 qPrintable(QStringLiteral("Expected .%1 export file").arg(smoke_case.suffix)));
    }
}

static void verifyFolderItems(PstParser& parser, const sak::PstFolderTree& tree) {
    qDebug() << "\n=== LOADING FOLDER ITEMS ===";

    const auto* target = findNonEmptyFolder(tree);
    if (!target) {
        qDebug() << "No non-empty folders found!";
        QFAIL("No non-empty folder found in fixture");
    }

    qDebug() << "Loading items from folder:" << target->display_name << "NID:" << Qt::hex
             << target->node_id;

    constexpr int kMaxItems = 10;
    auto items_result = parser.readFolderItems(target->node_id, 0, kMaxItems);
    if (!items_result) {
        qDebug() << "FAILED to load folder items:"
                 << QString::fromUtf8(sak::to_string(items_result.error()));
        QFAIL("Failed to load folder items");
    }

    qDebug() << "Loaded" << items_result->size() << "items";
    for (int idx = 0; idx < items_result->size() && idx < kMaxItems; ++idx) {
        const auto& item = (*items_result)[idx];
        qDebug().noquote() << QString("  [%1] NID=0x%2 %3 | from: %4 | %5")
                                  .arg(idx)
                                  .arg(item.node_id, 0, 16)
                                  .arg(item.subject)
                                  .arg(item.sender_name)
                                  .arg(item.date.toString(Qt::ISODate));
    }

    if (items_result->isEmpty()) {
        QFAIL("Fixture non-empty folder returned no items");
    }

    auto first_nid = items_result->first().node_id;
    qDebug() << "\n=== READING ITEM DETAIL ===";
    qDebug() << "Item NID:" << Qt::hex << first_nid;

    auto detail = parser.readItemDetail(first_nid);
    if (!detail) {
        qDebug() << "FAILED to read item detail:"
                 << QString::fromUtf8(sak::to_string(detail.error()));
        QFAIL("Failed to read item detail");
    }

    constexpr int kBodyPreviewLen = 200;
    qDebug() << "Subject:" << detail->subject;
    qDebug() << "From:" << detail->sender_name << detail->sender_email;
    qDebug() << "To:" << detail->display_to;
    qDebug() << "Date:" << detail->date;
    qDebug() << "Body (plain, first 200 chars):" << detail->body_plain.left(kBodyPreviewLen);
    qDebug() << "Has HTML:" << !detail->body_html.isEmpty();
    qDebug() << "Attachments:" << detail->attachments.size();

    verifySingleItemExports(parser, first_nid);
}

static void openConfiguredStoreFile(const QString& store_path,
                                    bool expected_is_ost,
                                    const char* missing_env_message,
                                    const char* missing_file_message) {
    if (store_path.isEmpty()) {
        QSKIP(missing_env_message);
    }
    QFileInfo fi(store_path);
    if (!fi.exists()) {
        QSKIP(missing_file_message);
    }
    qDebug() << (expected_is_ost ? "OST file:" : "PST file:") << store_path;
    qDebug() << "Size:" << fi.size() << "bytes";

    PstParser parser;

    QVector<QString> errors;
    QVector<int> progress_pct;
    sak::PstFileInfo captured_info;
    sak::PstFolderTree captured_tree;

    QObject::connect(&parser, &PstParser::errorOccurred, [&](const QString& msg) {
        errors.append(msg);
        qDebug() << "ERROR:" << msg;
    });
    QObject::connect(&parser, &PstParser::progressUpdated, [&](int pct, const QString& msg) {
        progress_pct.append(pct);
        qDebug() << "PROGRESS:" << pct << "%" << msg;
    });
    QObject::connect(&parser, &PstParser::fileOpened, [&](const sak::PstFileInfo& info) {
        captured_info = info;
        qDebug() << "FILE OPENED:";
        qDebug() << "  display_name:" << info.display_name;
        qDebug() << "  is_unicode:" << info.is_unicode;
        qDebug() << "  is_ost:" << info.is_ost;
        qDebug() << "  encryption:" << info.encryption_type;
        qDebug() << "  total_folders:" << info.total_folders;
        qDebug() << "  total_items:" << info.total_items;
    });
    QObject::connect(&parser, &PstParser::folderTreeLoaded, [&](const sak::PstFolderTree& tree) {
        captured_tree = tree;
        qDebug() << "FOLDER TREE: root count =" << tree.size();
    });

    parser.open(store_path);

    if (!errors.isEmpty()) {
        qDebug() << "\n=== PARSE ERRORS ===";
        for (const auto& err : errors) {
            qDebug() << "  " << err;
        }
        QFAIL("Parser reported errors during open");
    }

    QVERIFY2(parser.isOpen(), "Parser should be open after successful parse");
    QCOMPARE(captured_info.is_ost, expected_is_ost);

    printFolderTree(captured_tree);
    verifyFolderItems(parser, captured_tree);

    parser.close();
}

class TestOstIntegration : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void openRealOstFile() {
        openConfiguredStoreFile(configuredOstPath(),
                                true,
                                "Set SAK_TEST_OST_PATH to run this integration test",
                                "OST file not found - skipping integration test");
    }

    void openRealPstFile() {
        openConfiguredStoreFile(configuredPstPath(),
                                false,
                                "Set SAK_TEST_PST_PATH to run this integration test",
                                "PST file not found - skipping integration test");
    }
};

QTEST_MAIN(TestOstIntegration)
#include "test_ost_integration.moc"
