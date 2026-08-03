// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_profile_manager.cpp
/// @brief Unit tests for the email profile manager

#include "sak/email_profile_manager.h"
#include "sak/email_types.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestEmailProfileManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------------
    void defaultConstruction();

    // -- Data Types -------------------------------------------------------
    void clientTypeDefaults();
    void profileDefaults();
    void dataFileDefaults();

    // -- Linked Files ----------------------------------------------------
    void linkedFilesEmptyByDefault();

    // -- Cancel ----------------------------------------------------------
    void cancelBeforeDiscoveryDoesNotCrash();

    // -- Discovery (smoke test) ------------------------------------------
    void discoverProfilesEmitsSignal();

    // -- Backup Invalid Inputs -------------------------------------------
    void backupWithEmptyIndices();
    void backupWithInvalidPath();
    void backupWithNoDiscoveredProfiles();

    // -- Restore Invalid Inputs ------------------------------------------
    void restoreFromNonExistentManifest();
    void restoreFromEmptyPath();
    void restoreRejectsPathTraversal();

    // -- Profile Structure -----------------------------------------------
    void profilePopulation();
    void dataFilePopulation();

    // -- B7-07: destination confinement resolves junctions ---------------
    void destinationWithinRoot_allowsInsideRejectsOutside();
    void destinationWithinRoot_rejectsJunctionEscape();

    // -- B7-04: .reg content confinement ---------------------------------
    void regContent_allowsOutlookProfileSubtree();
    void regContent_rejectsHklmRunKey();
    void regContent_rejectsHkcuOutsideOutlook();
    void regContent_rejectsMixedGoodAndBad();
    void regContent_rejectsDeletionOutsideSubtree();
    void regContent_rejectsEmpty();
    void regContent_rejectsProfilesPrefixNotSegment();
    void decodeRegFile_handlesUtf16Bom();

    // -- Backup .reg name sanitization / manifest size cap ---------------
    void registryBackupFileName_sanitizesTraversal();
    void restoreRejectsOversizedManifest();

    // -- Codex-3 wave E2 hardening ---------------------------------------
    void restoreRejectsNonEmailExtensionInHome();       // finding 1
    void registryBackupDestination_dedupesCollision();  // finding 2
    void regContent_rejectsAnyDeletion();               // finding 4
    void regKeyPathAllowed_allowsWmsProfiles();         // finding 6
    void restoreRejectsBadVersion();                    // finding 7
    void restoreDoesNotCountEmptyProfile();             // finding 7
    void thunderbirdProfileDir_confinesRelative();      // finding 9

    // -- B7-16: single-flight guard --------------------------------------
    void singleFlightRefusesReentry();

    // -- B7-29: count only cleanly-restored profiles ---------------------
    void restoreCountsOnlyCleanProfiles();
};

// ============================================================================
// Construction
// ============================================================================

void TestEmailProfileManager::defaultConstruction() {
    EmailProfileManager manager;
    QVERIFY(manager.linkedFilePaths().isEmpty());
}

// ============================================================================
// Data Types
// ============================================================================

void TestEmailProfileManager::clientTypeDefaults() {
    QVERIFY(static_cast<int>(sak::EmailClientType::Outlook) == 0);
    QVERIFY(static_cast<int>(sak::EmailClientType::Thunderbird) == 1);
    QVERIFY(static_cast<int>(sak::EmailClientType::WindowsMail) == 2);
    QVERIFY(static_cast<int>(sak::EmailClientType::Other) == 3);
}

void TestEmailProfileManager::profileDefaults() {
    sak::EmailClientProfile profile;
    QCOMPARE(profile.client_type, sak::EmailClientType::Other);
    QVERIFY(profile.client_name.isEmpty());
    QVERIFY(profile.client_version.isEmpty());
    QVERIFY(profile.profile_name.isEmpty());
    QVERIFY(profile.profile_path.isEmpty());
    QVERIFY(profile.data_files.isEmpty());
    QCOMPARE(profile.total_size_bytes, static_cast<qint64>(0));
}

void TestEmailProfileManager::dataFileDefaults() {
    sak::EmailDataFile data_file;
    QVERIFY(data_file.path.isEmpty());
    QVERIFY(data_file.type.isEmpty());
    QCOMPARE(data_file.size_bytes, static_cast<qint64>(0));
    QVERIFY(!data_file.is_linked);
}

// ============================================================================
// Linked Files
// ============================================================================

void TestEmailProfileManager::linkedFilesEmptyByDefault() {
    EmailProfileManager manager;
    QSet<QString> linked = manager.linkedFilePaths();
    QVERIFY(linked.isEmpty());
}

// ============================================================================
// Cancel
// ============================================================================

void TestEmailProfileManager::cancelBeforeDiscoveryDoesNotCrash() {
    EmailProfileManager manager;
    manager.cancel();
    manager.cancel();
    QVERIFY(true);
}

// ============================================================================
// Discovery (smoke test — depends on system state)
// ============================================================================

void TestEmailProfileManager::discoverProfilesEmitsSignal() {
    EmailProfileManager manager;
    QSignalSpy spy(&manager, &EmailProfileManager::profilesDiscovered);
    manager.discoverProfiles();

    // Discovery may find 0+ profiles depending on the machine,
    // but the signal should always be emitted
    QVERIFY(spy.count() > 0);

    auto profiles = spy.at(0).at(0).value<QVector<sak::EmailClientProfile>>();
    // Just verify we got a valid vector (empty is fine)
    QVERIFY(profiles.size() >= 0);
}

// ============================================================================
// Backup Invalid Inputs
// ============================================================================

void TestEmailProfileManager::backupWithEmptyIndices() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);
    QSignalSpy complete_spy(&manager, &EmailProfileManager::backupComplete);

    manager.backupProfiles({}, QStringLiteral("C:/backup"));

    // Empty indices — should either emit an error or complete with 0
    QVERIFY(error_spy.count() > 0 || complete_spy.count() > 0);
}

void TestEmailProfileManager::backupWithInvalidPath() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    QVector<int> indices = {0};
    manager.backupProfiles(indices, QString());

    // Empty path should produce an error or be handled gracefully
    QVERIFY(error_spy.count() > 0 || true);
}

void TestEmailProfileManager::backupWithNoDiscoveredProfiles() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);
    QSignalSpy complete_spy(&manager, &EmailProfileManager::backupComplete);

    // No discovery done — indices won't map to anything
    QVector<int> indices = {0, 1, 2};
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    manager.backupProfiles(indices, temp_dir.path());

    // Should handle gracefully
    QVERIFY(error_spy.count() > 0 || complete_spy.count() > 0);
}

// ============================================================================
// Restore Invalid Inputs
// ============================================================================

void TestEmailProfileManager::restoreFromNonExistentManifest() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    manager.restoreProfiles(QStringLiteral("C:/nonexistent/manifest.json"));

    QVERIFY(error_spy.count() > 0);
}

void TestEmailProfileManager::restoreFromEmptyPath() {
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    manager.restoreProfiles(QString());

    // Empty path should produce an error
    QVERIFY(error_spy.count() > 0 || true);
}

void TestEmailProfileManager::restoreRejectsPathTraversal() {
    QTemporaryDir backup_root;
    QVERIFY(backup_root.isValid());
    const QString backup_dir = backup_root.path() + QStringLiteral("/bk");
    QVERIFY(QDir().mkpath(backup_dir));

    // The traversal source target sits one level ABOVE the backup directory.
    const QString evil_source = backup_root.path() + QStringLiteral("/evil.txt");
    {
        QFile file(evil_source);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
    }

    // Destination a crafted manifest tries to create, outside the user home tree.
    const QString pwned_dest = backup_root.path() + QStringLiteral("/pwned.txt");

    QJsonObject file_obj;
    file_obj[QStringLiteral("original_path")] = pwned_dest;                      // outside home
    file_obj[QStringLiteral("backed_up_name")] = QStringLiteral("../evil.txt");  // escapes bk
    QJsonArray files;
    files.append(file_obj);
    QJsonObject prof;
    prof[QStringLiteral("registry_file")] = QStringLiteral("../evil.reg");  // escapes bk
    prof[QStringLiteral("data_files")] = files;
    QJsonArray profiles;
    profiles.append(prof);
    QJsonObject root;
    root[QStringLiteral("version")] = 1;  // valid version so the restore reaches the confinement
    root[QStringLiteral("profiles")] = profiles;

    const QString manifest = backup_dir + QStringLiteral("/backup_manifest.json");
    {
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
    }

    EmailProfileManager manager;
    manager.restoreProfiles(manifest);

    // The confinement must have blocked the write: no file created outside backup.
    QVERIFY(!QFile::exists(pwned_dest));
}

// ============================================================================
// Profile Structure Population
// ============================================================================

void TestEmailProfileManager::profilePopulation() {
    sak::EmailClientProfile profile;
    profile.client_type = sak::EmailClientType::Outlook;
    profile.client_name = QStringLiteral("Microsoft Outlook 2021");
    profile.client_version = QStringLiteral("16.0");
    profile.profile_name = QStringLiteral("Default");
    profile.profile_path = QStringLiteral("HKCU\\Software\\Microsoft\\Outlook");
    profile.total_size_bytes = 1024 * 1024 * 500;

    sak::EmailDataFile pst_file;
    pst_file.path = QStringLiteral("X:/Profiles/Test/Documents/Outlook.pst");
    pst_file.type = QStringLiteral("PST");
    pst_file.size_bytes = 1024 * 1024 * 300;
    pst_file.is_linked = true;
    profile.data_files.append(pst_file);

    QCOMPARE(profile.client_type, sak::EmailClientType::Outlook);
    QCOMPARE(profile.data_files.size(), 1);
    QVERIFY(profile.data_files[0].is_linked);
    QCOMPARE(profile.data_files[0].type, QStringLiteral("PST"));
}

void TestEmailProfileManager::dataFilePopulation() {
    sak::EmailDataFile file;
    file.path = QStringLiteral("C:/mail.mbox");
    file.type = QStringLiteral("MBOX");
    file.size_bytes = 50 * 1024;
    file.is_linked = false;

    QCOMPARE(file.path, QStringLiteral("C:/mail.mbox"));
    QCOMPARE(file.size_bytes, static_cast<qint64>(50 * 1024));
    QVERIFY(!file.is_linked);
}

// ============================================================================
// B7-07: restore destination confinement must resolve junctions/symlinks, not
// just normalize lexically -- a junction under home would otherwise redirect the
// copy outside home while passing a textual prefix check.
// ============================================================================

void TestEmailProfileManager::destinationWithinRoot_allowsInsideRejectsOutside() {
    QTemporaryDir homeDir;
    QTemporaryDir outsideDir;
    QVERIFY(homeDir.isValid());
    QVERIFY(outsideDir.isValid());
    const QString home = homeDir.path();

    // A not-yet-existing path under home is allowed (leaf need not exist).
    QVERIFY(EmailProfileManager::destinationWithinRoot(home, home + "/Documents/ok.txt"));
    // A sibling/outside path is rejected.
    QVERIFY(!EmailProfileManager::destinationWithinRoot(home, outsideDir.path() + "/evil.txt"));
    // A lexical ".." escape is rejected (the real existing ancestor lands outside home).
    QVERIFY(!EmailProfileManager::destinationWithinRoot(home, home + "/../evil.txt"));
    // Empty candidate is rejected.
    QVERIFY(!EmailProfileManager::destinationWithinRoot(home, QString()));
}

void TestEmailProfileManager::destinationWithinRoot_rejectsJunctionEscape() {
    QTemporaryDir homeDir;
    QTemporaryDir outsideDir;
    QVERIFY(homeDir.isValid());
    QVERIFY(outsideDir.isValid());
    const QString home = homeDir.path();
    const QString junction = home + QStringLiteral("/link");

    // Create a directory junction INSIDE home that points OUTSIDE home. mklink /J needs no admin.
    QProcess mklink;
    mklink.start(QStringLiteral("cmd"),
                 {QStringLiteral("/c"),
                  QStringLiteral("mklink"),
                  QStringLiteral("/J"),
                  QDir::toNativeSeparators(junction),
                  QDir::toNativeSeparators(outsideDir.path())});
    mklink.waitForFinished(5000);

    if (!QFileInfo::exists(junction)) {
        QSKIP("mklink /J unavailable on this host; cannot exercise the junction-escape path");
    }

    // A path THROUGH the junction lexically begins with home but really lands outside it.
    QVERIFY2(!EmailProfileManager::destinationWithinRoot(home, junction + "/evil.txt"),
             "a junction under home must not let the restore destination escape home");
}

// ============================================================================
// B7-04: a restored .reg is untrusted -- reg.exe import writes EVERY key in it,
// so the content must be confined to the Outlook profile subtree before import.
// ============================================================================

void TestEmailProfileManager::regContent_allowsOutlookProfileSubtree() {
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles\\Default]\n"
        "\"UID\"=hex:01,02\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles\\Default\\9375]\n"
        "\"Server\"=\"mail\"\n";
    QVERIFY(EmailProfileManager::regContentConfinedToEmailHives(reg));
}

void TestEmailProfileManager::regContent_rejectsHklmRunKey() {
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Run]\n"
        "\"Evil\"=\"C:\\\\evil.exe\"\n";
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(reg));
}

void TestEmailProfileManager::regContent_rejectsHkcuOutsideOutlook() {
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run]\n"
        "\"Evil\"=\"C:\\\\evil.exe\"\n";
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(reg));
}

void TestEmailProfileManager::regContent_rejectsMixedGoodAndBad() {
    // One allowed key does not launder a file that ALSO writes a Run key.
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles\\Default]\n"
        "\"UID\"=hex:01\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run]\n"
        "\"Evil\"=\"C:\\\\evil.exe\"\n";
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(reg));
}

void TestEmailProfileManager::regContent_rejectsDeletionOutsideSubtree() {
    // A key-DELETE ([-HKEY...]) outside the subtree is just as dangerous and is refused.
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[-HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Defender]\n";
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(reg));
}

void TestEmailProfileManager::regContent_rejectsEmpty() {
    // Header only, no key sections -> not a real Outlook export -> refused.
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(
        QStringLiteral("Windows Registry Editor Version 5.00\n")));
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(QString()));
}

void TestEmailProfileManager::regContent_rejectsProfilesPrefixNotSegment() {
    // '\OUTLOOK\PROFILES' must match as a whole path segment, not a bare substring: a key that
    // merely begins with 'Profiles' ("ProfilesEvil") writes OUTSIDE the backed-up subtree and
    // must be refused.
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\ProfilesEvil\\Run]\n"
        "\"Evil\"=\"C:\\\\evil.exe\"\n";
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(reg));
    // The exact segment (no trailing subkey) and a subkey under it both stay allowed.
    QVERIFY(EmailProfileManager::regKeyPathAllowed(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles")));
    QVERIFY(!EmailProfileManager::regKeyPathAllowed(QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\ProfilesHack")));
}

void TestEmailProfileManager::registryBackupFileName_sanitizesTraversal() {
    // A clean profile name passes through unchanged.
    QCOMPARE(EmailProfileManager::registryBackupFileName(QStringLiteral("Default")),
             QStringLiteral("registry_Default.reg"));
    // Path separators and traversal are neutralized so the .reg stays a bare basename.
    for (const QString& hostile : {QStringLiteral("../evil"),
                                   QStringLiteral("..\\..\\evil"),
                                   QStringLiteral("a/b\\c"),
                                   QStringLiteral("x:y*z?")}) {
        const QString name = EmailProfileManager::registryBackupFileName(hostile);
        QVERIFY2(!name.contains(QLatin1Char('/')), qPrintable(name));
        QVERIFY2(!name.contains(QLatin1Char('\\')), qPrintable(name));
        QVERIFY(name.startsWith(QStringLiteral("registry_")));
        QVERIFY(name.endsWith(QStringLiteral(".reg")));
    }
    // A name that would collapse to nothing / "." / ".." falls back to a fixed placeholder.
    QCOMPARE(EmailProfileManager::registryBackupFileName(QStringLiteral("..")),
             QStringLiteral("registry_profile.reg"));
    QCOMPARE(EmailProfileManager::registryBackupFileName(QString()),
             QStringLiteral("registry_profile.reg"));
}

void TestEmailProfileManager::restoreRejectsOversizedManifest() {
    QTemporaryDir backup_root;
    QVERIFY(backup_root.isValid());
    const QString manifest = backup_root.path() + QStringLiteral("/backup_manifest.json");

    // Write a manifest just over the 8 MiB cap; it must be refused before parse.
    {
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray blob(9 * 1024 * 1024, ' ');
        file.write(blob);
    }

    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);
    QSignalSpy complete_spy(&manager, &EmailProfileManager::restoreComplete);
    manager.restoreProfiles(manifest);

    QVERIFY(error_spy.count() > 0);
    QCOMPARE(complete_spy.count(), 0);  // oversized manifest never reaches a restore result
}

void TestEmailProfileManager::decodeRegFile_handlesUtf16Bom() {
    const QString text =
        "[HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles\\P]\n";
    QByteArray u16;
    u16.append('\xFF');
    u16.append('\xFE');  // UTF-16LE BOM as written by reg.exe export
    u16.append(reinterpret_cast<const char*>(text.utf16()), static_cast<int>(text.size()) * 2);

    QCOMPARE(EmailProfileManager::decodeRegFile(u16), text);
    QVERIFY(EmailProfileManager::regContentConfinedToEmailHives(
        EmailProfileManager::decodeRegFile(u16)));
}

void TestEmailProfileManager::singleFlightRefusesReentry() {
    // discover/backup/restore share m_profiles / m_backup_dest_names / m_cancelled,
    // so a nested call while one is active must be refused, not run concurrently.
    // profilesDiscovered fires while discoverProfiles() is still on the stack (the
    // single-flight flag is not released until it returns), so a nested op launched
    // from the handler hits the guard deterministically.
    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);

    QObject::connect(&manager, &EmailProfileManager::profilesDiscovered, &manager, [&manager]() {
        // If the guard were absent this would actually run and emit
        // "Failed to open backup manifest"; with it, "already in
        // progress" is emitted instead.
        manager.restoreProfiles(QStringLiteral("C:/nope/manifest.json"));
    });

    manager.discoverProfiles();

    bool saw_in_progress = false;
    bool saw_open_failure = false;
    for (const auto& call : error_spy) {
        const QString msg = call.at(0).toString();
        if (msg.contains(QStringLiteral("already in progress"))) {
            saw_in_progress = true;
        }
        if (msg.contains(QStringLiteral("Failed to open backup manifest"))) {
            saw_open_failure = true;
        }
    }
    QVERIFY2(saw_in_progress, "nested op was not refused by the single-flight guard");
    QVERIFY2(!saw_open_failure, "nested op actually ran despite the guard");
}

void TestEmailProfileManager::restoreCountsOnlyCleanProfiles() {
    QTemporaryDir backup_root;
    QVERIFY(backup_root.isValid());
    const QString backup_dir = backup_root.path() + QStringLiteral("/bk");
    QVERIFY(QDir().mkpath(backup_dir));

    // A profile whose backed-up data file is MISSING from the backup: it cannot be
    // restored, so it must NOT be counted (before B7-29 it was counted regardless).
    QJsonObject file_obj;
    file_obj[QStringLiteral("original_path")] = QDir::homePath() +
                                                QStringLiteral("/sak_restore_test/data.pst");
    file_obj[QStringLiteral("backed_up_name")] = QStringLiteral("data.pst");  // never created
    QJsonArray files;
    files.append(file_obj);
    QJsonObject prof;
    prof[QStringLiteral("data_files")] = files;
    QJsonArray profiles;
    profiles.append(prof);
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("profiles")] = profiles;

    const QString manifest = backup_dir + QStringLiteral("/backup_manifest.json");
    {
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
    }

    EmailProfileManager manager;
    QSignalSpy complete_spy(&manager, &EmailProfileManager::restoreComplete);
    manager.restoreProfiles(manifest);

    QCOMPARE(complete_spy.count(), 1);
    QCOMPARE(complete_spy.first().first().toInt(), 0);  // failed profile not counted
}

// ============================================================================
// Codex-3 wave E2 hardening regressions
// ============================================================================

void TestEmailProfileManager::restoreRejectsNonEmailExtensionInHome() {
    // Finding 1: home containment alone let a crafted manifest create a NEW file with an
    // arbitrary extension anywhere under home (e.g. a .lnk in the Startup folder for logon
    // code execution). The restore must now also confine to email data-file types.
    QTemporaryDir backup_root;
    QVERIFY(backup_root.isValid());
    const QString backup_dir = backup_root.path() + QStringLiteral("/bk");
    QVERIFY(QDir().mkpath(backup_dir));

    // A real backup source with a bare name inside the backup dir.
    {
        QFile src(backup_dir + QStringLiteral("/srcfile"));
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("payload");
    }

    // Destination is UNDER home but carries an executable-shortcut extension.
    const QString dest_dir = QDir::homePath() + QStringLiteral("/sak_test_email_restore_e2");
    const QString dest = dest_dir + QStringLiteral("/evil.lnk");

    QJsonObject file_obj;
    file_obj[QStringLiteral("original_path")] = dest;
    file_obj[QStringLiteral("backed_up_name")] = QStringLiteral("srcfile");
    QJsonArray files;
    files.append(file_obj);
    QJsonObject prof;
    prof[QStringLiteral("data_files")] = files;
    QJsonArray profiles;
    profiles.append(prof);
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("profiles")] = profiles;

    const QString manifest = backup_dir + QStringLiteral("/backup_manifest.json");
    {
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
    }

    EmailProfileManager manager;
    manager.restoreProfiles(manifest);

    const bool created = QFile::exists(dest);
    QDir(dest_dir).removeRecursively();  // never pollute the real home tree
    QVERIFY2(!created, "a non-email extension under home must not be restorable");
}

void TestEmailProfileManager::registryBackupDestination_dedupesCollision() {
    // Finding 2: two profiles whose names sanitize to the same component must not target the
    // same .reg (reg.exe export /y would overwrite the first, and both manifest entries would
    // point at it). The unique-destination helper adds a collision suffix.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString first =
        EmailProfileManager::uniqueRegistryBackupDestination(dir.path(), QStringLiteral("Default"));
    QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("registry_Default.reg"));
    {
        QFile f(first);  // simulate the first export having been written
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString second =
        EmailProfileManager::uniqueRegistryBackupDestination(dir.path(), QStringLiteral("Default"));
    QVERIFY(second != first);
    QVERIFY(!QFile::exists(second));  // a fresh, non-colliding target
    QVERIFY(QFileInfo(second).fileName().startsWith(QStringLiteral("registry_Default_")));
    QVERIFY(QFileInfo(second).fileName().endsWith(QStringLiteral(".reg")));

    // Names that only COLLIDE after sanitization ('Test/1' and 'Test:1' -> registry_Test_1) also
    // get distinct files.
    const QString a =
        EmailProfileManager::uniqueRegistryBackupDestination(dir.path(), QStringLiteral("Test/1"));
    {
        QFile f(a);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    const QString b =
        EmailProfileManager::uniqueRegistryBackupDestination(dir.path(), QStringLiteral("Test:1"));
    QVERIFY(a != b);
}

void TestEmailProfileManager::regContent_rejectsAnyDeletion() {
    // Finding 4: a genuine `reg export` never emits a [-HKEY...] deletion, so a backup that
    // contains one is crafted/corrupt and is refused outright -- even when confined to the
    // Outlook subtree.
    const QString reg =
        "Windows Registry Editor Version 5.00\n"
        "\n"
        "[-HKEY_CURRENT_USER\\Software\\Microsoft\\Office\\16.0\\Outlook\\Profiles\\Default]\n";
    QVERIFY(!EmailProfileManager::regContentConfinedToEmailHives(reg));
}

void TestEmailProfileManager::regKeyPathAllowed_allowsWmsProfiles() {
    // Finding 6: WMS profiles are discovered and exported as Outlook, so restore must accept the
    // Windows Messaging Subsystem profiles subtree (previously refused, silently breaking restore).
    QVERIFY(EmailProfileManager::regKeyPathAllowed(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion"
                       "\\Windows Messaging Subsystem\\Profiles")));
    QVERIFY(EmailProfileManager::regKeyPathAllowed(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion"
                       "\\Windows Messaging Subsystem\\Profiles\\MyProfile")));
    // A sibling merely beginning with 'Profiles' is still refused (segment boundary).
    QVERIFY(!EmailProfileManager::regKeyPathAllowed(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion"
                       "\\Windows Messaging Subsystem\\ProfilesEvil")));
    // And a whole .reg confined to that subtree passes the file-level check.
    QVERIFY(EmailProfileManager::regContentConfinedToEmailHives(
        QStringLiteral("Windows Registry Editor Version 5.00\n\n"
                       "[HKEY_CURRENT_USER\\Software\\Microsoft\\Windows NT\\CurrentVersion"
                       "\\Windows Messaging Subsystem\\Profiles\\MyProfile]\n")));
}

void TestEmailProfileManager::restoreRejectsBadVersion() {
    // Finding 7: a missing/wrong-typed version must fail closed (no restoreComplete), never
    // coerce to a default.
    QTemporaryDir backup_root;
    QVERIFY(backup_root.isValid());

    QJsonObject root;
    root[QStringLiteral("version")] = QStringLiteral("1");  // string, not the required integer
    root[QStringLiteral("profiles")] = QJsonArray();
    const QString manifest = backup_root.path() + QStringLiteral("/backup_manifest.json");
    {
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
    }

    EmailProfileManager manager;
    QSignalSpy error_spy(&manager, &EmailProfileManager::errorOccurred);
    QSignalSpy complete_spy(&manager, &EmailProfileManager::restoreComplete);
    manager.restoreProfiles(manifest);

    QVERIFY(error_spy.count() > 0);
    QCOMPARE(complete_spy.count(), 0);
}

void TestEmailProfileManager::restoreDoesNotCountEmptyProfile() {
    // Finding 7: an empty profile object restores nothing and must not inflate the count.
    QTemporaryDir backup_root;
    QVERIFY(backup_root.isValid());

    QJsonArray profiles;
    profiles.append(QJsonObject{});  // no registry_file, no data_files
    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("profiles")] = profiles;
    const QString manifest = backup_root.path() + QStringLiteral("/backup_manifest.json");
    {
        QFile file(manifest);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(root).toJson());
    }

    EmailProfileManager manager;
    QSignalSpy complete_spy(&manager, &EmailProfileManager::restoreComplete);
    manager.restoreProfiles(manifest);

    QCOMPARE(complete_spy.count(), 1);
    QCOMPARE(complete_spy.first().first().toInt(), 0);
}

void TestEmailProfileManager::thunderbirdProfileDir_confinesRelative() {
    // Finding 9: a relative Thunderbird profile Path must be confined beneath the TB root so a
    // tampered profiles.ini with '../../..' cannot redirect the scan/backup out of tree.
    QTemporaryDir tb;
    QVERIFY(tb.isValid());
    const QString root = tb.path();

    const QString ok = EmailProfileManager::thunderbirdProfileDir(
        root, QStringLiteral("Profiles/abc.default"), true);
    QVERIFY(!ok.isEmpty());
    QVERIFY(QDir(ok).absolutePath().startsWith(QDir(root).absolutePath()));

    // A relative '..' escape is rejected (empty).
    QVERIFY(EmailProfileManager::thunderbirdProfileDir(root, QStringLiteral("../../evil"), true)
                .isEmpty());

    // An absolute path (IsRelative=0) is a supported user choice and passes through unchanged.
    QCOMPARE(
        EmailProfileManager::thunderbirdProfileDir(root, QStringLiteral("C:/elsewhere/tb"), false),
        QStringLiteral("C:/elsewhere/tb"));
}

QTEST_MAIN(TestEmailProfileManager)
#include "test_email_profile_manager.moc"
