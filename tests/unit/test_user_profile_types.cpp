// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_types.cpp
/// @brief Unit tests for user profile type serialization (TST-05)

#include "sak/user_profile_types.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
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
        // The remaining five names were untested: dropping or swapping one of those rows
        // keeps folderTypeToString/stringToFolderType mutually inverse, so the round trip
        // stayed green while a standard folder loaded back as Custom.
        QTest::newRow("AppData_Roaming")
            << static_cast<int>(FolderType::AppData_Roaming) << "AppData_Roaming";
        QTest::newRow("AppData_Local")
            << static_cast<int>(FolderType::AppData_Local) << "AppData_Local";
        QTest::newRow("Favorites") << static_cast<int>(FolderType::Favorites) << "Favorites";
        QTest::newRow("StartMenu") << static_cast<int>(FolderType::StartMenu) << "StartMenu";
        QTest::newRow("Custom") << static_cast<int>(FolderType::Custom) << "Custom";
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
        fs.display_name = "My Documents";
        fs.relative_path = "Documents";
        fs.selected = true;
        fs.include_patterns = QStringList{"*.docx", "*.pdf"};
        fs.exclude_patterns = QStringList{"~$*", "*.tmp"};
        fs.size_bytes = 1024 * 1024;
        fs.file_count = 37;

        QJsonObject json = fs.toJson();
        FolderSelection restored = FolderSelection::fromJson(json);

        // All EIGHT fields: four of them were written by toJson and read by fromJson with
        // nothing asserting they survived, so dropping either pattern list -- which is the
        // per-folder include/exclude scope of the backup -- round-tripped green.
        QCOMPARE(restored.type, fs.type);
        QCOMPARE(restored.display_name, fs.display_name);
        QCOMPARE(restored.relative_path, fs.relative_path);
        QCOMPARE(restored.selected, fs.selected);
        QCOMPARE(restored.include_patterns, fs.include_patterns);
        QCOMPARE(restored.exclude_patterns, fs.exclude_patterns);
        QCOMPARE(restored.size_bytes, fs.size_bytes);
        QCOMPARE(restored.file_count, fs.file_count);

        // `selected` must round-trip in BOTH arms: a true-only fixture is satisfied by a
        // fromJson that hard-codes true, which would silently re-select deselected folders.
        FolderSelection unselected = fs;
        unselected.selected = false;
        QCOMPARE(FolderSelection::fromJson(unselected.toJson()).selected, false);
    }

    // --- UserProfile JSON round-trip ---

    void userProfileSerialize() {
        UserProfile profile;
        profile.username = "TestUser";
        profile.sid = "S-1-5-21-123456789";
        profile.profile_path = "X:\\Profiles\\TestUser";
        profile.is_current_user = true;
        profile.is_selected = true;  // was dropped on round-trip (B7-34)
        profile.total_size_estimated = 5'368'709'120LL;
        FolderSelection docs;
        docs.type = FolderType::Documents;
        docs.relative_path = "Documents";
        FolderSelection music;
        music.type = FolderType::Music;
        music.relative_path = "Music";
        profile.folder_selections << docs << music;

        QJsonObject json = profile.toJson();
        UserProfile restored = UserProfile::fromJson(json);

        QCOMPARE(restored.username, profile.username);
        QCOMPARE(restored.sid, profile.sid);
        QCOMPARE(restored.profile_path, profile.profile_path);
        QCOMPARE(restored.is_current_user, profile.is_current_user);
        QCOMPARE(restored.is_selected, profile.is_selected);
        QCOMPARE(restored.total_size_estimated, profile.total_size_estimated);
        // The folder selections ARE the backup scope: carry them, and in order. Nothing asserted
        // them, so a toJson that omitted the array entirely round-tripped a profile whose backup
        // would copy nothing.
        QCOMPARE(restored.folder_selections.size(), qsizetype(2));
        QCOMPARE(restored.folder_selections.at(0).type, FolderType::Documents);
        QCOMPARE(restored.folder_selections.at(0).relative_path, QStringLiteral("Documents"));
        QCOMPARE(restored.folder_selections.at(1).type, FolderType::Music);
        QCOMPARE(restored.folder_selections.at(1).relative_path, QStringLiteral("Music"));
    }

    // --- SmartFilter ---

    void smartFilterDefaults() {
        SmartFilter filter;
        filter.initializeDefaults();
        // initializeDefaults seeds the exact 11-pattern list; a truncation must fail.
        QCOMPARE(filter.exclude_patterns,
                 (QStringList{".*\\.tmp$",
                              ".*\\.temp$",
                              ".*\\.cache$",
                              ".*\\.lock$",
                              ".*\\.lck$",
                              ".*~$",
                              ".*\\.crdownload$",
                              ".*\\.part$",
                              "desktop\\.ini$",
                              "thumbs\\.db$",
                              "\\.DS_Store$"}));
    }

    void smartFilterSerialize() {
        SmartFilter filter;
        filter.initializeDefaults();
        // A DEFAULT-valued fixture cannot prove this round trip: SmartFilter::fromJson starts
        // from a default-constructed filter whose ctor already re-seeds the exact same lists and
        // sizes, so a toJson that wrote NO KEYS AT ALL still compared equal. Every field below is
        // deliberately not its seeded default.
        filter.enable_file_size_limit = true;
        filter.enable_folder_size_limit = true;
        filter.max_single_file_size_bytes = 123'456'789;
        filter.max_folder_size_bytes = 987'654'321;
        filter.exclude_patterns = QStringList{"z_last$", "a_first$"};
        filter.exclude_folders = QStringList{"ZFolder", "AFolder"};
        // Deliberately OMITS every mandatory entry: dangerous_files is a UNION with the built-in
        // protected set, not a replacement, so a stored profile cannot un-protect NTUSER.DAT.
        filter.dangerous_files = QStringList{"extra_secret.dat"};

        QJsonObject json = filter.toJson();
        SmartFilter restored = SmartFilter::fromJson(json);

        // Round-trip must preserve content AND order, not merely the count.
        QCOMPARE(restored.exclude_patterns, filter.exclude_patterns);
        QCOMPARE(restored.exclude_folders, filter.exclude_folders);
        // The caller's extra survives AND all seven mandatory files are re-added, in
        // mandatoryDangerousFiles() order, after the supplied entries. Written as literals
        // rather than by calling mandatoryDangerousFiles(), which would compare production
        // against itself and stay green if the whole protected set were emptied.
        QCOMPARE(restored.dangerous_files,
                 (QStringList{"extra_secret.dat",
                              "NTUSER.DAT",
                              "NTUSER.DAT.LOG1",
                              "NTUSER.DAT.LOG2",
                              "ntuser.ini",
                              "UsrClass.dat",
                              "UsrClass.dat.LOG1",
                              "UsrClass.dat.LOG2"}));
        QCOMPARE(restored.enable_file_size_limit, true);
        QCOMPARE(restored.enable_folder_size_limit, true);
        QCOMPARE(restored.max_single_file_size_bytes, qint64(123'456'789));
        QCOMPARE(restored.max_folder_size_bytes, qint64(987'654'321));
    }

    // --- BackupManifest ---

    void backupManifestSerialize() {
        BackupManifest manifest;
        manifest.version = "1.0";
        // A fixed whole-second timestamp: ISODate is second-precision, so this round-trips
        // exactly (currentDateTime() carries milliseconds that the format silently drops).
        manifest.created = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5));
        manifest.source_machine = "WORKSTATION01";
        manifest.sak_version = "9.9.9";
        manifest.backup_type = "user_profiles";
        manifest.total_backup_size_bytes = 4'294'967'296LL;
        manifest.compressed = true;
        manifest.encrypted = true;

        BackupUserData user;
        user.username = "Admin";
        user.sid = "S-1-5-21-000";
        user.permissions_mode = PermissionMode::PreserveOriginal;
        manifest.users.append(user);
        BackupUserData second;
        second.username = "Guest";
        second.sid = "S-1-5-21-001";
        manifest.users.append(second);

        QJsonObject json = manifest.toJson();
        BackupManifest restored = BackupManifest::fromJson(json);

        QCOMPARE(restored.version, manifest.version);
        QCOMPARE(restored.source_machine, manifest.source_machine);
        QCOMPARE(restored.sak_version, manifest.sak_version);
        QCOMPARE(restored.backup_type, manifest.backup_type);
        QCOMPARE(restored.created, manifest.created);
        QCOMPARE(restored.total_backup_size_bytes, manifest.total_backup_size_bytes);
        // Restore reads these to know a payload is a codec container / needs a password.
        QCOMPARE(restored.compressed, true);
        QCOMPARE(restored.encrypted, true);
        // Users carry identity, permission mode AND order - not just a count.
        QCOMPARE(restored.users.size(), qsizetype(2));
        QCOMPARE(restored.users.at(0).username, QStringLiteral("Admin"));
        QCOMPARE(restored.users.at(0).sid, QStringLiteral("S-1-5-21-000"));
        QCOMPARE(restored.users.at(0).permissions_mode, PermissionMode::PreserveOriginal);
        QCOMPARE(restored.users.at(1).username, QStringLiteral("Guest"));
        QCOMPARE(restored.users.at(1).sid, QStringLiteral("S-1-5-21-001"));
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

        // Fail closed on the two failure arms, which the happy path alone never reaches:
        // an unreadable file must NOT come back as the ctor's default manifest (version
        // "1.0"), because callers gate validity on version.isEmpty().
        const BackupManifest missing =
            BackupManifest::loadFromFile(path + QStringLiteral(".absent"));
        QVERIFY(missing.version.isEmpty());
        QVERIFY(missing.users.isEmpty());
        QVERIFY(missing.source_machine.isEmpty());

        // A truncated/corrupt manifest parses to an empty object and must be rejected by
        // that same gate rather than degrading into a plausible-looking default manifest.
        writeFile(path, QByteArrayLiteral("{ not json"));
        QVERIFY(BackupManifest::loadFromFile(path).version.isEmpty());
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
        // 64 lowercase hex characters = a full SHA-256. !isEmpty() was satisfied by an MD5 hex, a
        // truncated digest, or any placeholder string.
        QCOMPARE(m.manifest_checksum.size(), qsizetype(64));
        QCOMPARE(m.manifest_checksum, m.manifest_checksum.toLower());
        QCOMPARE(QByteArray::fromHex(m.manifest_checksum.toLatin1()).size(), qsizetype(32));
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

    void wifiProfile_plaintextKeyAndHiddenAreNeverSerialized() {
        // WifiProfileInfo::plaintext_key is runtime-only BY DESIGN, and that is a security
        // boundary rather than an omission. A WifiKeyMaterial::Plaintext scan fills it so the
        // WiFi panel can hand a technician the passphrase; serializing it would write every PSK
        // in the clear into a backup manifest, whose JSON is not encrypted. The `hidden` flag
        // rides along on the same rule so the serialized shape stays exactly what it was.
        WifiProfileInfo info;
        info.profile_name = "HomeNet";
        info.security_type = "WPA2-Personal";
        info.xml_data = "<WLANProfile/>";
        info.selected = false;
        info.plaintext_key = "hunter2-correct-horse";
        info.hidden = true;

        const QJsonObject obj = info.toJson();
        // Nothing in the emitted object may carry the key -- not under this name, and not under
        // any other. Checking the serialized TEXT catches a future field that copies the value
        // across under a different key, which a per-key assertion would sail straight past.
        QVERIFY(!obj.contains("plaintext_key"));
        QVERIFY(!obj.contains("hidden"));
        const QString serialized = QString::fromUtf8(QJsonDocument(obj).toJson());
        QVERIFY(!serialized.contains("hunter2-correct-horse"));

        // The fields that ARE the persisted contract must still all be there: proving the key is
        // absent is worthless if it was achieved by emitting nothing.
        QCOMPARE(obj["profile_name"].toString(), QStringLiteral("HomeNet"));
        QCOMPARE(obj["security_type"].toString(), QStringLiteral("WPA2-Personal"));
        QCOMPARE(obj["xml_data"].toString(), QStringLiteral("<WLANProfile/>"));
        QCOMPARE(obj["selected"].toBool(), false);

        // Reading back yields a record with no key and no hidden flag, whatever a hand-edited or
        // hostile document claims. A manifest that supplies "plaintext_key" must not be able to
        // inject one into a live scan result.
        QJsonObject hostile = obj;
        hostile["plaintext_key"] = "injected-from-json";
        hostile["hidden"] = true;
        const WifiProfileInfo back = WifiProfileInfo::fromJson(hostile);
        QVERIFY(back.plaintext_key.isEmpty());
        QVERIFY(!back.hidden);
        QCOMPARE(back.profile_name, QStringLiteral("HomeNet"));
        QCOMPARE(back.security_type, QStringLiteral("WPA2-Personal"));
        QCOMPARE(back.xml_data, QStringLiteral("<WLANProfile/>"));
        QCOMPARE(back.selected, false);
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

        // The contract names the WiFi/Ethernet/AppData trio but only WiFi was proved, and
        // toJson adds ethernet_configs / app_data_sources CONDITIONALLY - a manifest that
        // left either array out of the hashed JSON authenticated nothing about it.
        BackupManifest eth_manifest = sampleManifest();
        EthernetConfigInfo eth_info;
        eth_info.adapter_name = "Ethernet";
        eth_info.ip_address = "10.0.0.5";
        eth_manifest.ethernet_configs.append(eth_info);
        eth_manifest.manifest_checksum = eth_manifest.computeManifestChecksum();
        QVERIFY(eth_manifest.verifyManifestChecksum());
        eth_manifest.ethernet_configs[0].ip_address = "10.6.6.6";
        QVERIFY(!eth_manifest.verifyManifestChecksum());

        BackupManifest app_manifest = sampleManifest();
        AppDataSourceInfo app_info;
        app_info.name = "Chrome Profiles";
        app_info.relative_path = "AppData/Local/Google/Chrome";
        app_manifest.app_data_sources.append(app_info);
        app_manifest.manifest_checksum = app_manifest.computeManifestChecksum();
        QVERIFY(app_manifest.verifyManifestChecksum());
        app_manifest.app_data_sources[0].relative_path = "AppData/Local/Evil";
        QVERIFY(!app_manifest.verifyManifestChecksum());
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
        // Pin the digest shape too: order-independence and change-detection are both satisfied by
        // a shorter or alternate hash, so !isEmpty() left the algorithm entirely unconstrained.
        QCOMPARE(ha.size(), qsizetype(64));
        QCOMPARE(ha, ha.toLower());
        QCOMPARE(QByteArray::fromHex(ha.toLatin1()).size(), qsizetype(32));
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
        // fromJson({}) leaves the seeded defaults intact: pin both exact lists.
        QCOMPARE(f.exclude_patterns,
                 (QStringList{".*\\.tmp$",
                              ".*\\.temp$",
                              ".*\\.cache$",
                              ".*\\.lock$",
                              ".*\\.lck$",
                              ".*~$",
                              ".*\\.crdownload$",
                              ".*\\.part$",
                              "desktop\\.ini$",
                              "thumbs\\.db$",
                              "\\.DS_Store$"}));
        QCOMPARE(f.exclude_folders,
                 (QStringList{"Temp",
                              "temp",
                              "$RECYCLE.BIN",
                              "Cache",
                              "GPUCache",
                              "Code Cache",
                              "Service Worker",
                              "Session Storage",
                              "WebCache",
                              "node_modules",
                              ".git",
                              ".svn",
                              "__pycache__",
                              "Packages"}));
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
        // Pin the literal byte counts, not the production constants: the members are seeded FROM
        // kDefaultMax* by the constructor, so comparing them against those same constants is
        // green for ANY value the constants take -- including zero, which would refuse every file.
        QCOMPARE(f.max_single_file_size_bytes, qint64(2) * 1024 * 1024 * 1024);  // 2 GiB
        QCOMPARE(f.max_folder_size_bytes, qint64(50) * 1024 * 1024 * 1024);      // 50 GiB
        QCOMPARE(kDefaultMaxSingleFileSizeBytes, qint64(2) * 1024 * 1024 * 1024);
        QCOMPARE(kDefaultMaxFolderSizeBytes, qint64(50) * 1024 * 1024 * 1024);
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

    void parseNetIpConfig_nonEnglishAdapterNameSurvives() {
        // R5-G23-4 hostile environment, LOCALE dimension. This parser exists BECAUSE the previous
        // implementation scraped English netsh labels and so captured nothing on a non-English
        // Windows -- silently losing every adapter from a profile backup that a restore then feeds
        // from. Get-NetIPConfiguration's PROPERTY names are language-neutral, but the adapter NAME
        // is not: on a German or Russian install it is whatever the OS calls the NIC, and it is
        // carried through to the restore. Pin that a non-ASCII name survives byte-for-byte rather
        // than being mangled or dropped by the JSON decode.
        // The escape is split before "ber": \xBC would otherwise swallow the following hex
        // digits ('b', 'e') and overflow the escape sequence.
        const QString german = QString::fromUtf8(
            "Ethernet-Verbindung \xC3\xBC"
            "ber LAN");
        const QString russian = QString::fromUtf8(
            "\xD0\x9F\xD0\xBE\xD0\xB4\xD0\xBA\xD0\xBB"
            "\xD1\x8E\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5");
        const QString json =
            QStringLiteral(R"([{"Name":"%1","Dhcp":"Enabled","IPv4":"10.0.0.5","Prefix":24,)"
                           R"("Gateway":"10.0.0.1","Dns":["10.0.0.1"]},)"
                           R"({"Name":"%2","Dhcp":"Disabled","IPv4":"192.168.1.7","Prefix":16,)"
                           R"("Gateway":null,"Dns":[]}])")
                .arg(german, russian);

        const auto configs = EthernetConfigInfo::parseNetIpConfigJson(json);
        QCOMPARE(configs.size(), qsizetype(2));
        // Byte-for-byte: a name that came back transliterated or with replacement characters
        // would target a DIFFERENT adapter (or none) when the restore runs netsh against it.
        QCOMPARE(configs.at(0).adapter_name, german);
        QCOMPARE(configs.at(1).adapter_name, russian);
        // The rest of the mapping is unaffected by the non-ASCII name -- the language-neutral
        // property names are still what the parser reads.
        QVERIFY(configs.at(0).dhcp_enabled);
        QCOMPARE(configs.at(0).ip_address, QStringLiteral("10.0.0.5"));
        QCOMPARE(configs.at(0).subnet_mask, QStringLiteral("255.255.255.0"));
        QVERIFY(!configs.at(1).dhcp_enabled);
        QCOMPARE(configs.at(1).subnet_mask, QStringLiteral("255.255.0.0"));
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

        // An absent gateway may render as an empty JSON object ({}) rather than null depending on
        // how PowerShell serializes the (empty) NextHop selection; both must map to an empty
        // string (QJsonValue::toString() is "" for a null AND for an object), never a crash.
        const auto object_gateway = EthernetConfigInfo::parseNetIpConfigJson(
            QStringLiteral(R"({"Name":"Ethernet 4","Dhcp":"Enabled","IPv4":"10.1.1.2","Prefix":16,)"
                           R"("Gateway":{},"Dns":[]})"));
        QCOMPARE(object_gateway.size(), qsizetype(1));
        QVERIFY(object_gateway.at(0).default_gateway.isEmpty());

        // A record with NO Prefix (or a stringly-typed one) must leave subnet_mask EMPTY.
        // Without the isDouble() guard, toInt() yields 0 and restore is handed the mask
        // "0.0.0.0", which netsh accepts - no other fixture here omits Prefix.
        const auto no_prefix = EthernetConfigInfo::parseNetIpConfigJson(
            QStringLiteral(R"({"Name":"Ethernet 5","Dhcp":"Disabled","IPv4":"172.16.0.9"})"));
        QCOMPARE(no_prefix.size(), qsizetype(1));
        QCOMPARE(no_prefix.at(0).ip_address, QStringLiteral("172.16.0.9"));
        QVERIFY(no_prefix.at(0).subnet_mask.isEmpty());
        const auto string_prefix = EthernetConfigInfo::parseNetIpConfigJson(QStringLiteral(
            R"({"Name":"Ethernet 6","Dhcp":"Disabled","IPv4":"172.16.0.9","Prefix":"24"})"));
        QCOMPARE(string_prefix.size(), qsizetype(1));
        QVERIFY(string_prefix.at(0).subnet_mask.isEmpty());

        // Single-DNS adapter: primary set, secondary empty (the middle arm - the existing
        // cases only cover two entries and none).
        const auto one_dns = EthernetConfigInfo::parseNetIpConfigJson(
            QStringLiteral(R"({"Name":"Ethernet 7","Dhcp":"Enabled","IPv4":"10.2.2.2",)"
                           R"("Prefix":24,"Dns":["9.9.9.9"]})"));
        QCOMPARE(one_dns.size(), qsizetype(1));
        QCOMPARE(one_dns.at(0).dns_primary, QStringLiteral("9.9.9.9"));
        QVERIFY(one_dns.at(0).dns_secondary.isEmpty());
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
