// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_integration.cpp
/// @brief Integration test: opens a real OST file and reports parse results

#include "sak/error_codes.h"
#include "sak/pst_parser.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QtTest/QtTest>

static const QString kOstPath = QStringLiteral(
    "C:/Users/Randy/AppData/Local/Microsoft/Outlook/"
    "randy.northrup@outlook.com.ost");

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

static void verifyFolderItems(PstParser& parser, const sak::PstFolderTree& tree) {
    qDebug() << "\n=== LOADING FOLDER ITEMS ===";

    const auto* target = findNonEmptyFolder(tree);
    if (!target) {
        qDebug() << "No non-empty folders found!";
        return;
    }

    qDebug() << "Loading items from folder:" << target->display_name << "NID:" << Qt::hex
             << target->node_id;

    constexpr int kMaxItems = 10;
    auto items_result = parser.readFolderItems(target->node_id, 0, kMaxItems);
    if (!items_result) {
        qDebug() << "FAILED to load folder items:"
                 << QString::fromUtf8(sak::to_string(items_result.error()));
        return;
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
        return;
    }

    auto first_nid = items_result->first().node_id;
    qDebug() << "\n=== READING ITEM DETAIL ===";
    qDebug() << "Item NID:" << Qt::hex << first_nid;

    auto detail = parser.readItemDetail(first_nid);
    if (!detail) {
        qDebug() << "FAILED to read item detail:"
                 << QString::fromUtf8(sak::to_string(detail.error()));
        return;
    }

    constexpr int kBodyPreviewLen = 200;
    qDebug() << "Subject:" << detail->subject;
    qDebug() << "From:" << detail->sender_name << detail->sender_email;
    qDebug() << "To:" << detail->display_to;
    qDebug() << "Date:" << detail->date;
    qDebug() << "Body (plain, first 200 chars):" << detail->body_plain.left(kBodyPreviewLen);
    qDebug() << "Has HTML:" << !detail->body_html.isEmpty();
    qDebug() << "Attachments:" << detail->attachments.size();
}

class TestOstIntegration : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void openRealOstFile() {
        QFileInfo fi(kOstPath);
        if (!fi.exists()) {
            QSKIP("OST file not found — skipping integration test");
        }
        qDebug() << "OST file:" << kOstPath;
        qDebug() << "Size:" << fi.size() << "bytes";

        PstParser parser;

        // Capture signals
        QVector<QString> errors;
        QVector<int> progress_pct;
        sak::PstFileInfo captured_info;
        sak::PstFolderTree captured_tree;

        connect(&parser, &PstParser::errorOccurred, [&](const QString& msg) {
            errors.append(msg);
            qDebug() << "ERROR:" << msg;
        });
        connect(&parser, &PstParser::progressUpdated, [&](int pct, const QString& msg) {
            progress_pct.append(pct);
            qDebug() << "PROGRESS:" << pct << "%" << msg;
        });
        connect(&parser, &PstParser::fileOpened, [&](const sak::PstFileInfo& info) {
            captured_info = info;
            qDebug() << "FILE OPENED:";
            qDebug() << "  display_name:" << info.display_name;
            qDebug() << "  is_unicode:" << info.is_unicode;
            qDebug() << "  is_ost:" << info.is_ost;
            qDebug() << "  encryption:" << info.encryption_type;
            qDebug() << "  total_folders:" << info.total_folders;
            qDebug() << "  total_items:" << info.total_items;
        });
        connect(&parser, &PstParser::folderTreeLoaded, [&](const sak::PstFolderTree& tree) {
            captured_tree = tree;
            qDebug() << "FOLDER TREE: root count =" << tree.size();
        });

        // Attempt open
        parser.open(kOstPath);

        if (!errors.isEmpty()) {
            qDebug() << "\n=== PARSE ERRORS ===";
            for (const auto& err : errors) {
                qDebug() << "  " << err;
            }
            QFAIL("Parser reported errors during open");
        }

        QVERIFY2(parser.isOpen(), "Parser should be open after successful parse");

        printFolderTree(captured_tree);
        verifyFolderItems(parser, captured_tree);

        parser.close();
    }
};

QTEST_MAIN(TestOstIntegration)
#include "test_ost_integration.moc"
