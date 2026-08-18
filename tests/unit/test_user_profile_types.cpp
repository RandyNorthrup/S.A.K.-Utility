// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_types.cpp
/// @brief Unit tests for user profile type serialization (TST-05)

#include "sak/user_profile_types.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

using namespace sak;

class UserProfileTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // --- Enum-string round-trips ---

    void folderTypeRoundTrip_data() {
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("expected");

        QTest::newRow("Documents") << static_cast<int>(FolderType::Documents) << "Documents";
        QTest::newRow("Desktop") << static_cast<int>(FolderType::Desktop) << "Desktop";
        QTest::newRow("Pictures") << static_cast<int>(FolderType::Pictures) << "Pictures";
        QTest::newRow("Videos") << static_cast<int>(FolderType::Videos) << "Videos";
        QTest::newRow("Music") << static_cast<int>(FolderType::Music) << "Music";
        QTest::newRow("Downloads") << static_cast<int>(FolderType::Downloads) << "Downloads";
    }

    void folderTypeRoundTrip() {
        QFETCH(int, type);
        QFETCH(QString, expected);

        auto ft = static_cast<FolderType>(type);
        QString str = folderTypeToString(ft);
        QCOMPARE(str, expected);

        FolderType back = stringToFolderType(str);
        QCOMPARE(back, ft);
    }

    void unknownFolderTypeReturnsCustom() {
        FolderType result = stringToFolderType("NonExistent");
        QCOMPARE(result, FolderType::Custom);
    }

    // --- FolderSelection JSON round-trip ---

    void folderSelectionSerialize() {
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

    void userProfileSerialize() {
        UserProfile profile;
        profile.username = "TestUser";
        profile.sid = "S-1-5-21-123456789";
        profile.profile_path = "X:\\Profiles\\TestUser";
        profile.is_current_user = true;
        profile.is_selected = true;  // was dropped on round-trip (B7-34)

        QJsonObject json = profile.toJson();
        UserProfile restored = UserProfile::fromJson(json);

        QCOMPARE(restored.username, profile.username);
        QCOMPARE(restored.sid, profile.sid);
        QCOMPARE(restored.profile_path, profile.profile_path);
        QCOMPARE(restored.is_current_user, profile.is_current_user);
        QCOMPARE(restored.is_selected, profile.is_selected);
    }

    // --- SmartFilter ---

    void smartFilterDefaults() {
        SmartFilter filter;
        filter.initializeDefaults();
        // Should have populated exclude patterns
        QVERIFY(!filter.exclude_patterns.isEmpty());
    }

    void smartFilterSerialize() {
        SmartFilter filter;
        filter.initializeDefaults();

        QJsonObject json = filter.toJson();
        SmartFilter restored = SmartFilter::fromJson(json);

        QCOMPARE(restored.exclude_patterns.size(), filter.exclude_patterns.size());
    }

    // --- BackupManifest ---

    void backupManifestSerialize() {
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

    void backupManifestFileRoundTrip() {
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

    void permissionModeToStringValid() {
        QCOMPARE(permissionModeToString(PermissionMode::StripAll), QStringLiteral("StripAll"));
        QCOMPARE(permissionModeToString(PermissionMode::PreserveOriginal),
                 QStringLiteral("PreserveOriginal"));
        QCOMPARE(permissionModeToString(PermissionMode::AssignToDestination),
                 QStringLiteral("AssignToDestination"));
    }

    /// A manifest written before PermissionMode::Hybrid was removed must still load, and it
    /// must load as StripAll - that is what a Hybrid backup actually did on both the backup
    /// and the restore side, whatever the four different descriptions of it claimed.
    void legacyHybridPermissionModeReadsAsStripAll() {
        QJsonObject json;
        json[QStringLiteral("username")] = QStringLiteral("John");
        json[QStringLiteral("permissions_mode")] = QStringLiteral("Hybrid");

        const BackupUserData restored = BackupUserData::fromJson(json);
        QCOMPARE(restored.permissions_mode, PermissionMode::StripAll);
    }

    /// An unknown or absent mode must also land on the safest option rather than whatever
    /// the enum's first value happens to be at the time.
    void unknownPermissionModeReadsAsStripAll() {
        QJsonObject json;
        json[QStringLiteral("username")] = QStringLiteral("John");
        json[QStringLiteral("permissions_mode")] = QStringLiteral("NotAMode");
        QCOMPARE(BackupUserData::fromJson(json).permissions_mode, PermissionMode::StripAll);

        QJsonObject empty;
        empty[QStringLiteral("username")] = QStringLiteral("John");
        QCOMPARE(BackupUserData::fromJson(empty).permissions_mode, PermissionMode::StripAll);
    }

    // --- BackupUserData round-trip ---

    void backupUserDataSerialize() {
        BackupUserData data;
        data.username = "John";
        data.sid = "S-1-5-21-999";
        data.profile_path = "X:\\Profiles\\John";

        QJsonObject json = data.toJson();
        BackupUserData restored = BackupUserData::fromJson(json);

        QCOMPARE(restored.username, data.username);
        QCOMPARE(restored.sid, data.sid);
        QCOMPARE(restored.profile_path, data.profile_path);
    }

    // --- Manifest integrity checksum (B7-13) ---

    static BackupManifest sampleManifest() {
        BackupManifest m;
        m.source_machine = "PC-1";
        BackupUserData u;
        u.username = "Alice";
        u.sid = "S-1-5-21-1";
        m.users.append(u);
        return m;
    }

    void manifestChecksumMatchesAfterCompute() {
        BackupManifest m = sampleManifest();
        m.manifest_checksum = m.computeManifestChecksum();
        QVERIFY(!m.manifest_checksum.isEmpty());
        QVERIFY(m.verifyManifestChecksum());
    }

    void manifestChecksumExcludesItself() {
        // Recomputing after storing the digest must not change the digest (the
        // field is removed before hashing), so verify stays true.
        BackupManifest m = sampleManifest();
        m.manifest_checksum = m.computeManifestChecksum();
        const QString again = m.computeManifestChecksum();
        QCOMPARE(again, m.manifest_checksum);
    }

    void manifestChecksumDetectsTamper() {
        BackupManifest m = sampleManifest();
        m.manifest_checksum = m.computeManifestChecksum();
        // Mutate a covered field without updating the stored digest.
        m.source_machine = "PC-EVIL";
        QVERIFY(!m.verifyManifestChecksum());
    }

    void manifestLegacyEmptyChecksumAccepted() {
        BackupManifest m = sampleManifest();
        m.manifest_checksum.clear();
        QVERIFY(m.verifyManifestChecksum());  // nothing stored -> unverifiable, not a failure
    }

    void manifestChecksumCoversNetworkSelections() {
        // CODEX_REVIEW_4 H3: the WiFi/Ethernet/AppData selections are embedded in the
        // manifest so its SHA-256 authenticates them (restore trusts these, not the
        // unprotected *.json sidecars). A tampered embedded profile must be detected.
        BackupManifest m = sampleManifest();
        WifiProfileInfo w;
        w.profile_name = "HomeNet";
        w.xml_data = "<WLANProfile/>";
        m.wifi_profiles.append(w);
        m.manifest_checksum = m.computeManifestChecksum();
        QVERIFY(m.verifyManifestChecksum());
        // Tamper the embedded profile without updating the digest.
        m.wifi_profiles[0].xml_data = "<WLANProfile>evil</WLANProfile>";
        QVERIFY(!m.verifyManifestChecksum());
    }

    void manifestChecksumSurvivesSaveLoad() {
        BackupManifest m = sampleManifest();
        m.manifest_checksum = m.computeManifestChecksum();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("manifest.json");
        QVERIFY(m.saveToFile(path));
        BackupManifest loaded = BackupManifest::loadFromFile(path);
        QCOMPARE(loaded.manifest_checksum, m.manifest_checksum);
        QVERIFY(loaded.verifyManifestChecksum());
    }

    // --- Directory-tree payload digest (B7-13) ---

    static void writeFile(const QString& path, const QByteArray& bytes) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        f.close();
    }

    void dirHashMissingPathIsEmpty() {
        QCOMPARE(BackupManifest::hashDirectoryTree("X:/no/such/dir/here"), QString());
    }

    void dirHashDeterministicAndOrderIndependent() {
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(a.isValid() && b.isValid());
        QVERIFY(QDir(a.path()).mkpath("sub"));
        QVERIFY(QDir(b.path()).mkpath("sub"));
        // Same content, files created in a different order in each tree.
        writeFile(a.filePath("one.txt"), "hello");
        writeFile(a.filePath("sub/two.txt"), "world");
        writeFile(b.filePath("sub/two.txt"), "world");
        writeFile(b.filePath("one.txt"), "hello");
        const QString ha = BackupManifest::hashDirectoryTree(a.path());
        const QString hb = BackupManifest::hashDirectoryTree(b.path());
        QVERIFY(!ha.isEmpty());
        QCOMPARE(ha, hb);
    }

    void dirHashDetectsContentChange() {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        writeFile(d.filePath("f.txt"), "original");
        const QString before = BackupManifest::hashDirectoryTree(d.path());
        writeFile(d.filePath("f.txt"), "tampered");
        const QString after = BackupManifest::hashDirectoryTree(d.path());
        QVERIFY(before != after);
    }

    void dirHashDetectsNameChange() {
        // Same bytes under a different relative path must change the digest (path is
        // folded in), so a rename inside the payload is caught.
        QTemporaryDir a;
        QTemporaryDir b;
        QVERIFY(a.isValid() && b.isValid());
        writeFile(a.filePath("name1.txt"), "same");
        writeFile(b.filePath("name2.txt"), "same");
        QVERIFY(BackupManifest::hashDirectoryTree(a.path()) !=
                BackupManifest::hashDirectoryTree(b.path()));
    }

    // --- SmartFilter defaults preservation (B7-14) ---

    void filterFromEmptyJsonKeepsDefaults() {
        // A missing/empty filter_rules object must degrade to the safe defaults,
        // not clear them (and must not abort).
        SmartFilter f = SmartFilter::fromJson(QJsonObject{});
        for (const QString& mandatory : SmartFilter::mandatoryDangerousFiles()) {
            QVERIFY2(f.dangerous_files.contains(mandatory, Qt::CaseInsensitive),
                     qPrintable("missing mandatory exclusion: " + mandatory));
        }
        QVERIFY(!f.exclude_patterns.isEmpty());
        QVERIFY(!f.exclude_folders.isEmpty());
    }

    void filterEmptyDangerousArrayStillMandatory() {
        // Even an EXPLICIT empty dangerous_files array cannot drop the mandatory set.
        QJsonObject json;
        json["dangerous_files"] = QJsonArray{};
        SmartFilter f = SmartFilter::fromJson(json);
        for (const QString& mandatory : SmartFilter::mandatoryDangerousFiles()) {
            QVERIFY(f.dangerous_files.contains(mandatory, Qt::CaseInsensitive));
        }
    }

    void filterCustomDangerousUnionsMandatory() {
        QJsonObject json;
        json["dangerous_files"] = QJsonArray{QStringLiteral("custom_secret.dat")};
        SmartFilter f = SmartFilter::fromJson(json);
        QVERIFY(f.dangerous_files.contains(QStringLiteral("custom_secret.dat")));
        QVERIFY(f.dangerous_files.contains(QStringLiteral("NTUSER.DAT"), Qt::CaseInsensitive));
    }

    void filterSizeDefaultPreservedWhenMissing() {
        SmartFilter f = SmartFilter::fromJson(QJsonObject{});
        QCOMPARE(f.max_single_file_size_bytes, kDefaultMaxSingleFileSizeBytes);
        QCOMPARE(f.max_folder_size_bytes, kDefaultMaxFolderSizeBytes);
    }

    void filterExplicitListOverridesDefault() {
        QJsonObject json;
        json["exclude_folders"] = QJsonArray{QStringLiteral("OnlyThis")};
        SmartFilter f = SmartFilter::fromJson(json);
        QCOMPARE(f.exclude_folders, QStringList{QStringLiteral("OnlyThis")});
    }

    // --- Empty-object fromJson degrades to defaults, never aborts (B7-14) ---

    void emptyJsonDegradesGracefully() {
        // Previously each of these asserted !json.isEmpty() and aborted in debug.
        const BackupManifest m = BackupManifest::fromJson(QJsonObject{});
        QVERIFY(m.users.isEmpty());
        // filter_rules defaulted -> mandatory exclusions present.
        QVERIFY(m.filter_rules.dangerous_files.contains(QStringLiteral("NTUSER.DAT"),
                                                        Qt::CaseInsensitive));
        const FolderSelection fs = FolderSelection::fromJson(QJsonObject{});
        QVERIFY(fs.relative_path.isEmpty());
        const BackupUserData ud = BackupUserData::fromJson(QJsonObject{});
        QVERIFY(ud.username.isEmpty());
    }

    // --- EthernetConfigInfo language-neutral scan (G23-4) ---

    void prefixLengthToSubnetMask_knownPrefixes() {
        QCOMPARE(EthernetConfigInfo::prefixLengthToSubnetMask(24), QStringLiteral("255.255.255.0"));
        QCOMPARE(EthernetConfigInfo::prefixLengthToSubnetMask(16), QStringLiteral("255.255.0.0"));
        QCOMPARE(EthernetConfigInfo::prefixLengthToSubnetMask(8), QStringLiteral("255.0.0.0"));
        QCOMPARE(EthernetConfigInfo::prefixLengthToSubnetMask(32),
                 QStringLiteral("255.255.255.255"));
        QCOMPARE(EthernetConfigInfo::prefixLengthToSubnetMask(0), QStringLiteral("0.0.0.0"));
        QCOMPARE(EthernetConfigInfo::prefixLengthToSubnetMask(23), QStringLiteral("255.255.254.0"));
    }

    void prefixLengthToSubnetMask_outOfRangeIsEmpty() {
        QVERIFY(EthernetConfigInfo::prefixLengthToSubnetMask(-1).isEmpty());
        QVERIFY(EthernetConfigInfo::prefixLengthToSubnetMask(33).isEmpty());
    }

    void parseNetIpConfig_mapsAdapterFields() {
        // Shape mirrors a real `Get-NetIPConfiguration ... | ConvertTo-Json` dump.
        const QString json = QStringLiteral(
            R"([{"Name":"Ethernet 2","Dhcp":"Enabled","IPv4":"10.0.0.5","Prefix":24,)"
            R"("Gateway":"10.0.0.1","Dns":["10.0.0.1","8.8.8.8"]}])");
        const auto configs = EthernetConfigInfo::parseNetIpConfigJson(json);
        QCOMPARE(configs.size(), qsizetype(1));
        const EthernetConfigInfo& c = configs.at(0);
        QCOMPARE(c.adapter_name, QStringLiteral("Ethernet 2"));
        QVERIFY(c.dhcp_enabled);
        QCOMPARE(c.ip_address, QStringLiteral("10.0.0.5"));
        QCOMPARE(c.subnet_mask, QStringLiteral("255.255.255.0"));  // CIDR 24 -> dotted quad
        QCOMPARE(c.default_gateway, QStringLiteral("10.0.0.1"));
        QCOMPARE(c.dns_primary, QStringLiteral("10.0.0.1"));
        QCOMPARE(c.dns_secondary, QStringLiteral("8.8.8.8"));
    }

    void parseNetIpConfig_staticAdapterNullGatewayEmptyDns() {
        // A statically-configured adapter with no gateway and no DNS: Dhcp Disabled, Gateway null,
        // Dns []. The null/empty fields must map to empty strings, not crash.
        const QString json = QStringLiteral(
            R"({"Name":"Ethernet 3","Dhcp":"Disabled","IPv4":"192.168.56.1","Prefix":24,)"
            R"("Gateway":null,"Dns":[]})");
        const auto configs = EthernetConfigInfo::parseNetIpConfigJson(json);
        QCOMPARE(configs.size(), qsizetype(1));
        const EthernetConfigInfo& c = configs.at(0);
        QVERIFY(!c.dhcp_enabled);
        QCOMPARE(c.subnet_mask, QStringLiteral("255.255.255.0"));
        QVERIFY(c.default_gateway.isEmpty());
        QVERIFY(c.dns_primary.isEmpty());
        QVERIFY(c.dns_secondary.isEmpty());
    }

    void parseNetIpConfig_skipsNamelessAndHandlesEmpty() {
        // A record with no Name carries no restorable adapter and is skipped.
        const auto skipped = EthernetConfigInfo::parseNetIpConfigJson(
            QStringLiteral(R"([{"Name":"","Dhcp":"Enabled","IPv4":"1.2.3.4","Prefix":24}])"));
        QVERIFY(skipped.isEmpty());
        // Empty / null / malformed input is an empty adapter set, not a crash.
        QVERIFY(EthernetConfigInfo::parseNetIpConfigJson(QString()).isEmpty());
        QVERIFY(EthernetConfigInfo::parseNetIpConfigJson(QStringLiteral("null")).isEmpty());
        QVERIFY(EthernetConfigInfo::parseNetIpConfigJson(QStringLiteral("not json")).isEmpty());
    }
};

QTEST_MAIN(UserProfileTypesTests)
#include "test_user_profile_types.moc"
