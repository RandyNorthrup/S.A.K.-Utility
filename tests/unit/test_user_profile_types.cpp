// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_types.cpp
/// @brief Unit tests for user profile type serialization (TST-05)

#include <QtTest/QtTest>
#include <QTemporaryFile>
#include "sak/user_profile_types.h"

using namespace sak;

class UserProfileTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- Enum-string round-trips ---

    void folderTypeRoundTrip_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("expected");

        QTest::newRow("Documents") << static_cast<int>(FolderType::Documents) << "Documents";
        QTest::newRow("Desktop") << static_cast<int>(FolderType::Desktop) << "Desktop";
        QTest::newRow("Pictures") << static_cast<int>(FolderType::Pictures) << "Pictures";
        QTest::newRow("Videos") << static_cast<int>(FolderType::Videos) << "Videos";
        QTest::newRow("Music") << static_cast<int>(FolderType::Music) << "Music";
        QTest::newRow("Downloads") << static_cast<int>(FolderType::Downloads) << "Downloads";
    }

    void folderTypeRoundTrip()
    {
        QFETCH(int, type);
        QFETCH(QString, expected);

        auto ft = static_cast<FolderType>(type);
        QString str = folderTypeToString(ft);
        QCOMPARE(str, expected);

        FolderType back = stringToFolderType(str);
        QCOMPARE(back, ft);
    }

    void unknownFolderTypeReturnsCustom()
    {
        FolderType result = stringToFolderType("NonExistent");
        QCOMPARE(result, FolderType::Custom);
    }

    // --- FolderSelection JSON round-trip ---

    void folderSelectionSerialize()
    {
        FolderSelection fs;
        fs.type = FolderType::Documents;
        fs.relative_path = "Documents";
        fs.selected = true;
        fs.size_bytes = 1024 * 1024;

        QJsonObject json = fs.toJson();
        FolderSelection restored = FolderSelection::fromJson(json);

        QCOMPARE(restored.type, fs.type);
        QCOMPARE(restored.relative_path, fs.relative_path);
        QCOMPARE(restored.selected, fs.selected);
        QCOMPARE(restored.size_bytes, fs.size_bytes);
    }

    // --- UserProfile JSON round-trip ---

    void userProfileSerialize()
    {
        UserProfile profile;
        profile.username = "TestUser";
        profile.sid = "S-1-5-21-123456789";
        profile.profile_path = "C:\\Users\\TestUser";
        profile.is_current_user = true;

        QJsonObject json = profile.toJson();
        UserProfile restored = UserProfile::fromJson(json);

        QCOMPARE(restored.username, profile.username);
        QCOMPARE(restored.sid, profile.sid);
        QCOMPARE(restored.profile_path, profile.profile_path);
        QCOMPARE(restored.is_current_user, profile.is_current_user);
    }

    // --- SmartFilter ---

    void smartFilterDefaults()
    {
        SmartFilter filter;
        filter.initializeDefaults();
        // Should have populated exclude patterns
        QVERIFY(!filter.exclude_patterns.isEmpty());
    }

    void smartFilterSerialize()
    {
        SmartFilter filter;
        filter.initializeDefaults();

        QJsonObject json = filter.toJson();
        SmartFilter restored = SmartFilter::fromJson(json);

        QCOMPARE(restored.exclude_patterns.size(), filter.exclude_patterns.size());
    }

    // --- BackupManifest ---

    void backupManifestSerialize()
    {
        BackupManifest manifest;
        manifest.version = "1.0";
        manifest.created = QDateTime::currentDateTime();
        manifest.source_machine = "WORKSTATION01";

        BackupUserData user;
        user.username = "Admin";
        user.sid = "S-1-5-21-000";
        manifest.users.append(user);

        QJsonObject json = manifest.toJson();
        BackupManifest restored = BackupManifest::fromJson(json);

        QCOMPARE(restored.version, manifest.version);
        QCOMPARE(restored.source_machine, manifest.source_machine);
        QCOMPARE(restored.users.size(), 1);
        QCOMPARE(restored.users.first().username, QStringLiteral("Admin"));
    }

    void backupManifestFileRoundTrip()
    {
        BackupManifest manifest;
        manifest.version = "1.0";
        manifest.source_machine = "TEST-PC";

        QTemporaryFile tempFile;
        QVERIFY(tempFile.open());
        QString path = tempFile.fileName();
        tempFile.close();

        QVERIFY(manifest.saveToFile(path));

        BackupManifest loaded = BackupManifest::loadFromFile(path);
        QCOMPARE(loaded.version, manifest.version);
        QCOMPARE(loaded.source_machine, manifest.source_machine);
    }

    // --- PermissionMode string conversion ---

    void permissionModeToStringValid()
    {
        QCOMPARE(permissionModeToString(PermissionMode::StripAll), QStringLiteral("StripAll"));
        QCOMPARE(permissionModeToString(PermissionMode::PreserveOriginal),
            QStringLiteral("PreserveOriginal"));
    }

    // --- BackupUserData round-trip ---

    void backupUserDataSerialize()
    {
        BackupUserData data;
        data.username = "John";
        data.sid = "S-1-5-21-999";
        data.profile_path = "C:\\Users\\John";

        QJsonObject json = data.toJson();
        BackupUserData restored = BackupUserData::fromJson(json);

        QCOMPARE(restored.username, data.username);
        QCOMPARE(restored.sid, data.sid);
        QCOMPARE(restored.profile_path, data.profile_path);
    }
};

QTEST_MAIN(UserProfileTypesTests)
#include "test_user_profile_types.moc"
