// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_data_manager.cpp
/// @brief Unit tests for UserDataManager data locations and checksums (TST-04)

#include "sak/user_data_manager.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

using namespace sak;

namespace {
// A random blob straddling empty / tiny / multi-KB so a truncated or partial write is visible.
QByteArray randomBlob(QRandomGenerator& rng) {
    const int size = static_cast<int>(rng.bounded(0u, 4096u));
    QByteArray blob(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i) {
        blob[i] = static_cast<char>(rng.bounded(256u));
    }
    return blob;
}
}  // namespace

class UserDataManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commonDataLocationsNotEmpty() {
        UserDataManager mgr;
        auto locations = mgr.getCommonDataLocations();
        // Pin the exact contractual set (4 entries). `!empty()` could not catch a refactor that
        // dropped the BitLocker Recovery Keys sentinel (the entry the backup wizard handles
        // specially) -- it would leave 3 and still be non-empty.
        QCOMPARE(locations.size(), static_cast<size_t>(4));
        QStringList patterns;
        for (const auto& loc : locations) {
            patterns << loc.pattern;
        }
        QVERIFY(patterns.contains(QStringLiteral("Google Chrome")));
        QVERIFY(patterns.contains(QStringLiteral("Mozilla Firefox")));
        QVERIFY(patterns.contains(QStringLiteral("Visual Studio Code")));
        QVERIFY(patterns.contains(QStringLiteral("BitLocker Recovery Keys")));
    }

    void commonDataLocationsHaveDescriptions() {
        UserDataManager mgr;
        auto locations = mgr.getCommonDataLocations();
        // The descriptions are what the backup wizard shows beside each entry, so pin them:
        // !isEmpty() also passed on a description copied onto the WRONG pattern, and the two
        // browsers legitimately share one string, which is exactly the kind of duplication a
        // per-entry emptiness check cannot distinguish from a copy-paste error.
        QCOMPARE(locations[0].description,
                 QStringLiteral("Browser profile, history, bookmarks, extensions"));
        QCOMPARE(locations[1].description,
                 QStringLiteral("Browser profile, history, bookmarks, extensions"));
        QCOMPARE(locations[2].description,
                 QStringLiteral("Settings, keybindings, extensions, snippets"));
        QCOMPARE(locations[3].description,
                 QStringLiteral("BitLocker recovery keys for all encrypted volumes"));
        // The BitLocker entry is handled by a sentinel PATH, not a filesystem glob.
        QCOMPARE(locations[3].paths, QStringList{QStringLiteral("bitlocker://recovery-keys")});
    }

    void checksumDeterministic() {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("hello world checksum test");
        tmp.flush();

        UserDataManager mgr;
        QString hash1 = mgr.generateChecksum(tmp.fileName());
        QString hash2 = mgr.generateChecksum(tmp.fileName());

        // The real SHA-256 of the written bytes, cross-checked with an independent
        // implementation. Determinism alone was satisfied by any stable function of the file --
        // including one that hashed the PATH, or a truncated/hex-mangled digest.
        QCOMPARE(hash1,
                 QStringLiteral(
                     "cd448eb2aaa5c3a4a197d2113dbd0f35080d90aff2c275ca67868a20350cf50c"));
        QCOMPARE(hash2, hash1);
    }

    void checksumDifferentForDifferentContent() {
        QTemporaryFile tmp1, tmp2;
        QVERIFY(tmp1.open());
        QVERIFY(tmp2.open());
        tmp1.write("content A");
        tmp2.write("content B");
        tmp1.flush();
        tmp2.flush();

        UserDataManager mgr;
        QString hash1 = mgr.generateChecksum(tmp1.fileName());
        QString hash2 = mgr.generateChecksum(tmp2.fileName());

        // Both exact digests. "different and non-empty" held for a hash of the file NAME (the
        // two temp files differ there too), which would make every content change invisible.
        QCOMPARE(hash1,
                 QStringLiteral(
                     "49114a9a2b7d46ec27be62ae3eade12f78d46cf5a99c52cd4f80381d723eed6e"));
        QCOMPARE(hash2,
                 QStringLiteral(
                     "d27a54dc662fff702c2183d536e87414d5fe6fc072f6bc270b01a34f6de265bc"));
    }

    void compareChecksumsMatch() {
        QTemporaryFile tmp1, tmp2;
        QVERIFY(tmp1.open());
        QVERIFY(tmp2.open());
        QByteArray content("identical content for comparison");
        tmp1.write(content);
        tmp2.write(content);
        tmp1.flush();
        tmp2.flush();

        UserDataManager mgr;
        QVERIFY(mgr.compareChecksums(tmp1.fileName(), tmp2.fileName()));
    }

    void compareChecksumsDoNotMatch() {
        QTemporaryFile tmp1, tmp2;
        QVERIFY(tmp1.open());
        QVERIFY(tmp2.open());
        tmp1.write("file one");
        tmp2.write("file two");
        tmp1.flush();
        tmp2.flush();

        UserDataManager mgr;
        QVERIFY(!mgr.compareChecksums(tmp1.fileName(), tmp2.fileName()));
    }

    void checksumNonexistentFileReturnsEmpty() {
        UserDataManager mgr;
        QString hash = mgr.generateChecksum("C:\\nonexistent\\path\\file.dat");
        QVERIFY(hash.isEmpty());
    }

    void calculateSizeOnTempFiles() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QFile f1(QDir(tmpDir.path()).filePath("a.txt"));
        QVERIFY(f1.open(QIODevice::WriteOnly));
        f1.write(QByteArray(100, 'A'));
        f1.close();

        QFile f2(QDir(tmpDir.path()).filePath("b.txt"));
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.write(QByteArray(200, 'B'));
        f2.close();

        UserDataManager mgr;
        QStringList paths = {tmpDir.path()};
        qint64 size = mgr.calculateSize(paths);

        // Exactly 100 + 200 logical bytes. `>= 300` could not catch an over-count regression:
        // a double-visited file (~600), summing on-disk cluster allocation (~8192), or a spurious
        // directory/entry all satisfy `>= 300` while breaking the logical-byte contract.
        QCOMPARE(size, static_cast<qint64>(300));
    }

    void listBackupsOnEmptyDir() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        UserDataManager mgr;
        auto backups = mgr.listBackups(tmpDir.path());
        QVERIFY(backups.empty());
    }

    // --- Helpers -------------------------------------------------------------

    static QString makeSourceDir(const QTemporaryDir& parent,
                                 const QString& name,
                                 const QString& file_name,
                                 const QByteArray& content) {
        const QString dir = QDir(parent.path()).filePath(name);
        if (!QDir().mkpath(dir)) {
            return {};
        }
        QFile f(QDir(dir).filePath(file_name));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(content);
            f.close();
        }
        return dir;
    }

    // --- Regression tests (P06-18/19/20/21/22/23) ----------------------------

    // P06-18: the metadata sidecar must persist the real checksum so restore
    // and verifyBackup actually integrity-check the archive.
    void checksumPersistedAndVerified() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "payload bytes");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.verify_checksum = true;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        // The checksum must be the archive's REAL digest in the canonical 64-hex-char form:
        // !isEmpty() also passed on a placeholder, a truncated digest, or a hash of the wrong
        // file -- any of which makes the later corruption check pass for the wrong reason.
        QCOMPARE(e->checksum, mgr.generateChecksum(e->backup_path));
        QCOMPARE(e->checksum.size(), 64);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(e->checksum).hasMatch());

        QFile meta(e->backup_path + ".json");
        QVERIFY(meta.open(QIODevice::ReadOnly));
        const auto obj = QJsonDocument::fromJson(meta.readAll()).object();
        QCOMPARE(obj.value("checksum").toString(), e->checksum);
        // The rest of the sidecar is what a later restore reads back, so pin it too.
        QCOMPARE(obj.value("app_name").toString(), QStringLiteral("App"));
        QCOMPARE(obj.value("backup_path").toString(), e->backup_path);
        QCOMPARE(obj.value("encrypted").toBool(), false);
        QCOMPARE(obj.value("total_size").toInteger(), static_cast<qint64>(13));

        // Corrupt the archive: verifyBackup must now fail closed.
        QFile arc(e->backup_path);
        QVERIFY(arc.open(QIODevice::Append));
        arc.write("corruption");
        arc.close();
        QVERIFY(!mgr.verifyBackup(e->backup_path));
    }

    // P06-22: a same-named file across two sources collides in the flat
    // destination, so the copy primitive (and the backup) must fail closed.
    void copyDirectoryFailClosedOnCollision() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString a = makeSourceDir(work, "a", "same.txt", "from a");
        const QString b = makeSourceDir(work, "b", "same.txt", "from b");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {a, b}, backupDir.path(), cfg);
        QVERIFY(!e.has_value());
    }

    // P06-19: encryption requested with no password must not emit plaintext.
    void encryptWithoutPasswordFailsClosed() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "secret");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.encrypt = true;
        cfg.password = "";
        QSignalSpy err(&mgr, &UserDataManager::operationError);
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(!e.has_value());
        // WHICH precondition refused: the two encryption guards sit side by side and both
        // return nullopt, so a nullopt alone could not show the password check ran.
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QStringLiteral("App"));
        QCOMPARE(err.at(0).at(1).toString(), QStringLiteral("Encryption requires a password"));
        // Nothing at all was written -- not just no .zip. A plaintext copy DIRECTORY is the
        // exact leak this guard exists to prevent, and a *.zip glob cannot see one.
        QCOMPARE(QDir(backupDir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(),
                 0);
    }

    // P06-19: encryption without compression would leave a plaintext copy dir.
    void encryptRequiresCompression() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "secret");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        cfg.encrypt = true;
        cfg.password = "pw";
        QSignalSpy err(&mgr, &UserDataManager::operationError);
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(!e.has_value());
        // The COMPRESSION precondition, not the password one that sits beside it.
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QStringLiteral("App"));
        QCOMPARE(err.at(0).at(1).toString(), QStringLiteral("Encryption requires compression"));
        // The plaintext copy directory this guard exists to prevent was never created.
        QCOMPARE(QDir(backupDir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(),
                 0);
    }

    // P06-21: two backups sharing a one-second timestamp must both survive.
    void twoBackupsSameSecondBothKept() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "payload");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        auto e1 = mgr.backupAppData("Foo", {src}, backupDir.path(), cfg);
        auto e2 = mgr.backupAppData("Foo", {src}, backupDir.path(), cfg);
        QVERIFY(e1.has_value());
        QVERIFY(e2.has_value());
        QVERIFY(e1->backup_path != e2->backup_path);
        QVERIFY(QFileInfo::exists(e1->backup_path));
        QVERIFY(QFileInfo::exists(e2->backup_path));
        QCOMPARE(static_cast<int>(mgr.listBackups(backupDir.path()).size()), 2);
    }

    // P06-20: an uncompressed backup is a directory; metadata must be truthful
    // and the payload must round-trip through list + restore.
    void uncompressedBackupRoundTrip() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "round trip");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(!e->backup_path.endsWith(".zip"));
        QVERIFY(QFileInfo(e->backup_path).isDir());
        // The uncompressed entry's own fields: no checksum is generated for a directory backup,
        // and the two size fields must agree (nothing was compressed).
        QCOMPARE(e->checksum, QString());
        QCOMPARE(e->total_size_bytes, static_cast<qint64>(10));
        QCOMPARE(e->compressed_size_bytes, static_cast<qint64>(10));
        QCOMPARE(e->encrypted, false);
        QCOMPARE(e->app_name, QStringLiteral("App"));
        QCOMPARE(e->source_paths, QStringList{src});
        QCOMPARE(static_cast<int>(mgr.listBackups(backupDir.path()).size()), 1);

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = false;
        rcfg.create_backup = false;
        QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));
        // Round-trip means the BYTES came back, not merely that a file with the right name
        // exists: an empty or truncated restore satisfies an exists() check unchanged.
        QFile restored(QDir(restoreDir.path()).filePath("data.txt"));
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), QByteArray("round trip"));
    }

    // P06-24: a directory junction inside the source must NOT be followed --
    // otherwise the recursive copy pulls in data from outside the source root
    // (or loops forever on a junction that targets an ancestor).
    void backupSkipsJunctionReparsePoints() {
        QTemporaryDir work;
        QVERIFY(work.isValid());

        // External content the junction points at -- must never be backed up.
        const QString external = QDir(work.path()).filePath("external");
        QVERIFY(QDir().mkpath(external));
        QFile ext(QDir(external).filePath("secret.txt"));
        QVERIFY(ext.open(QIODevice::WriteOnly));
        ext.write("EXTERNAL SECRET");
        ext.close();

        // Source tree: a real file, a real nested subdir, and a junction.
        const QString src = makeSourceDir(work, "src", "data.txt", "payload");
        QVERIFY(!src.isEmpty());
        const QString real_sub = QDir(src).filePath("real");
        QVERIFY(QDir().mkpath(real_sub));
        QFile inner(QDir(real_sub).filePath("inner.txt"));
        QVERIFY(inner.open(QIODevice::WriteOnly));
        inner.write("inner");
        inner.close();

        // mklink /J creates a junction without elevation (unlike symlinks).
        const QString junction = QDir(src).filePath("jn");
        QProcess mklink;
        mklink.start("cmd",
                     {"/c",
                      "mklink",
                      "/J",
                      QDir::toNativeSeparators(junction),
                      QDir::toNativeSeparators(external)});
        QVERIFY(mklink.waitForFinished());
        QVERIFY2(mklink.exitCode() == 0, "junction creation failed");
        QVERIFY(QFileInfo(junction).isDir());

        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());

        const QDir root(e->backup_path);
        // Real content copied through.
        QVERIFY(root.exists("data.txt"));
        QVERIFY(QFileInfo::exists(root.filePath("real/inner.txt")));
        // Junction not followed: neither the junction dir nor its external
        // payload is present in the backup.
        QVERIFY(!QFileInfo::exists(root.filePath("jn")));
        QVERIFY(!QFileInfo::exists(root.filePath("jn/secret.txt")));
    }

    // --- B7-31: deleteBackup must remove a DIRECTORY payload, not just a file ---
    void deleteBackupRemovesDirectoryPayload() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "x");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;  // uncompressed -> the payload is a directory
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(QFileInfo(e->backup_path).isDir());

        QVERIFY(mgr.deleteBackup(e->backup_path));
        QVERIFY(!QFileInfo::exists(e->backup_path));  // QFile::remove could never delete a dir
        QVERIFY(!QFileInfo::exists(e->backup_path + ".json"));
    }

    // --- CR2: deleteBackup must not recursively erase an arbitrary tree ---
    // Pure decision-seam coverage for the confinement / identity / screen gate.
    void deletionRefusalScreensDriveRoot() {
        const QString r = UserDataManager::backupDeletionRefusal(
            QStringLiteral("C:/"), std::optional<QString>(QStringLiteral("C:/")));
        // Layer 1 fires FIRST even though the metadata matches: pin its text, or a regression that
        // reordered the layers would still "refuse" here via the identity check and look correct.
        QCOMPARE(r, QStringLiteral("refusing a drive root"));
    }

    void deletionRefusalNeedsMetadataSidecar() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString p = QDir(work.path()).filePath("App_backup");
        const QString r = UserDataManager::backupDeletionRefusal(p, std::nullopt);
        QCOMPARE(r, QStringLiteral("no backup metadata sidecar identifies this path"));
    }

    void deletionRefusalNeedsIdentityMatch() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString p = QDir(work.path()).filePath("App_backup");
        const QString other = QDir(work.path()).filePath("Somewhere_else");
        const QString r = UserDataManager::backupDeletionRefusal(p, std::optional<QString>(other));
        // Distinct from the no-sidecar text above: these two are the only way to tell an
        // unmanaged tree from a forged sidecar, and !isEmpty() collapsed them into one case.
        QCOMPARE(r, QStringLiteral("backup metadata does not identify this target"));
    }

    void deletionRefusalAllowsManagedBackup() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString p = QDir(work.path()).filePath("App_backup");
        const QString r = UserDataManager::backupDeletionRefusal(p, std::optional<QString>(p));
        QVERIFY2(r.isEmpty(), qPrintable(r));  // safe local path + matching sidecar -> allowed
    }

    // --- CODEX_REVIEW_4 M-A1-20: entry guards were Q_ASSERT_X (release no-op) so an empty
    // path ran CWD-relative. allPathsPresent is the release-effective fail-closed replacement.
    void allPathsPresentRejectsEmpty() {
        QVERIFY(UserDataManager::allPathsPresent({QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(!UserDataManager::allPathsPresent({QStringLiteral(""), QStringLiteral("x")}));
        QVERIFY(!UserDataManager::allPathsPresent({QStringLiteral("x"), QStringLiteral("")}));
        QVERIFY(!UserDataManager::allPathsPresent({QString()}));
    }

    // --- CODEX_REVIEW_4 M-A1-25: cap the encrypted-archive read before readAll() so an
    // attacker file cannot OOM the app ahead of the (post-decrypt) zip-bomb preflight.
    void encryptedArchiveSizeCapBoundary() {
        constexpr qint64 kCap = 4LL * 1024 * 1024 * 1024;
        QVERIFY(UserDataManager::encryptedArchiveSizeOk(0));
        QVERIFY(UserDataManager::encryptedArchiveSizeOk(kCap));
        QVERIFY(!UserDataManager::encryptedArchiveSizeOk(-1));        // unreadable -> fail closed
        QVERIFY(!UserDataManager::encryptedArchiveSizeOk(kCap + 1));  // oversized -> fail closed
    }

    // --- CODEX_REVIEW_4 M-A1-26: atomicReplaceFile renames the original aside first (never a
    // delete-then-rename that could destroy the original on a rename failure).
    void atomicReplaceFileSwapsWithoutDataLossWindow() {
        QTemporaryDir work;
        QVERIFY(work.isValid());

        // Replacing an existing target: content becomes the staged one; tmp + the aside-copy gone.
        const QString target = QDir(work.path()).filePath("data.bin");
        const QString tmp = QDir(work.path()).filePath("data.bin.new");
        {
            QFile t(target);
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("old");
        }
        {
            QFile s(tmp);
            QVERIFY(s.open(QIODevice::WriteOnly));
            s.write("new");
        }
        QVERIFY(UserDataManager::atomicReplaceFile(tmp, target));
        {
            QFile r(target);
            QVERIFY(r.open(QIODevice::ReadOnly));
            QCOMPARE(r.readAll(), QByteArray("new"));
        }
        QVERIFY(!QFileInfo::exists(tmp));
        // No residue of ANY kind: the staged file and the .sak_old rollback copy are both gone,
        // and nothing else was left behind. Naming only the two expected leftovers could not see
        // a third temporary the replace forgot to clean up.
        QCOMPARE(QDir(work.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("data.bin")});

        // Replacing a not-yet-existing target: the staged file is simply moved into place.
        const QString target2 = QDir(work.path()).filePath("fresh.bin");
        const QString tmp2 = QDir(work.path()).filePath("fresh.bin.new");
        {
            QFile s2(tmp2);
            QVERIFY(s2.open(QIODevice::WriteOnly));
            s2.write("hello");
        }
        QVERIFY(UserDataManager::atomicReplaceFile(tmp2, target2));
        // The staged CONTENT arrived, not just a file at that name -- a zero-byte target would
        // satisfy exists() while losing everything the caller staged.
        QFile r2(target2);
        QVERIFY(r2.open(QIODevice::ReadOnly));
        QCOMPARE(r2.readAll(), QByteArray("hello"));
        QVERIFY(!QFileInfo::exists(tmp2));
    }

    // --- R5-G23-7 destructive-operation invariants as PROPERTY TESTS:
    //   invariant 4 (rollback / fail closed) + invariant 2 (source stays intact until the replace
    //   is known-good). atomicReplaceFile must leave the target in exactly ONE of two states -- the
    //   full original or the full new content -- and NEVER absent, truncated, or partial, whether
    //   the replace succeeds or fails. Fuzz random contents across a 4-way
    //   (target-pre-exists x induced-failure) matrix with a fixed seed. Induced failure = a MISSING
    //   staged tmp, which makes the underlying atomic move (MoveFileExW on Windows) fail
    //   deterministically; the guard must then drop the stage and leave the original untouched.
    //   Non-vacuous (G18-4): a naive delete-then-rename would leave the target absent on the
    //   failure rows (red), and returning true on a failed move would leave target != new (red).
    void atomicReplaceFile_neverLeavesTargetPartialOrAbsent() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        QRandomGenerator rng(0x5A'7C'0D'E5u);
        constexpr int kIterations = 2000;
        for (int i = 0; i < kIterations; ++i) {
            const QString target = QDir(work.path()).filePath(QStringLiteral("t_%1.bin").arg(i));
            const QString tmp = target + QStringLiteral(".new");
            const bool targetPreExists = (rng.bounded(2u) == 0u);
            const bool induceFailure = (rng.bounded(2u) == 0u);

            QByteArray original;
            if (targetPreExists) {
                original = randomBlob(rng);
                QFile t(target);
                QVERIFY(t.open(QIODevice::WriteOnly));
                QCOMPARE(t.write(original), static_cast<qint64>(original.size()));
                t.close();
            }
            QByteArray staged;
            if (!induceFailure) {
                staged = randomBlob(rng);
                QFile s(tmp);
                QVERIFY(s.open(QIODevice::WriteOnly));
                QCOMPARE(s.write(staged), static_cast<qint64>(staged.size()));
                s.close();
            }  // else: leave tmp absent so the atomic move fails deterministically.

            const bool ok = UserDataManager::atomicReplaceFile(tmp, target);

            // The stage is ALWAYS gone afterwards (success moves it; failure drops it).
            QVERIFY2(!QFileInfo::exists(tmp),
                     qPrintable(QStringLiteral("stage left behind at i=%1").arg(i)));
            if (ok) {
                QVERIFY2(!induceFailure,
                         qPrintable(
                             QStringLiteral("success returned on induced failure at i=%1").arg(i)));
                QFile r(target);
                QVERIFY(r.open(QIODevice::ReadOnly));
                QCOMPARE(r.readAll(), staged);  // full NEW content, never partial
            } else if (targetPreExists) {
                // Fail closed: the FULL original survives, never truncated or half-new.
                QFile r(target);
                QVERIFY2(r.open(QIODevice::ReadOnly),
                         qPrintable(
                             QStringLiteral("original destroyed on failure at i=%1").arg(i)));
                QCOMPARE(r.readAll(), original);
            } else {
                QVERIFY2(!QFileInfo::exists(target),
                         qPrintable(
                             QStringLiteral("partial target created on failure at i=%1").arg(i)));
            }
            QFile::remove(target);
            QFile::remove(tmp);
        }
    }

    // End-to-end: an arbitrary directory with no sidecar is left intact.
    void deleteBackupRefusesUnmanagedDirectory() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString victim = QDir(work.path()).filePath("not_a_backup");
        QVERIFY(QDir().mkpath(victim));
        QFile f(QDir(victim).filePath("important.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("keep me");
        f.close();

        UserDataManager mgr;
        QSignalSpy err(&mgr, &UserDataManager::operationError);
        QVERIFY(!mgr.deleteBackup(victim));  // no metadata sidecar -> refused
        // Which LAYER refused matters: the shared path screens (layer 1) and the metadata
        // identity check (layer 2) both return false, so pinning the reason is the only way to
        // show an unmanaged directory is rejected for lacking a sidecar rather than, say, for
        // sitting under a screened root.
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QStringLiteral("not_a_backup"));
        QCOMPARE(err.at(0).at(1).toString(),
                 QStringLiteral("Refusing to delete backup: no backup metadata sidecar identifies "
                                "this path"));
        QVERIFY(QFileInfo::exists(victim));  // tree left fully intact
        // The payload survived byte-for-byte and nothing else was added or removed.
        QFile kept(QDir(victim).filePath("important.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("keep me"));
        QCOMPARE(QDir(victim).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 QStringList{QStringLiteral("important.txt")});
    }

    // End-to-end: a forged sidecar that points elsewhere must not authorize deletion.
    void deleteBackupRefusesForgedSidecarMismatch() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString victim = QDir(work.path()).filePath("victim");
        QVERIFY(QDir().mkpath(victim));
        QJsonObject obj;
        obj["app_name"] = QStringLiteral("X");
        obj["backup_path"] = QDir(work.path()).filePath("elsewhere");
        // parseMetadataObject requires a STRING checksum (an absent one must not become "" and
        // silently disable verification). Without this field the forged sidecar fails to parse,
        // readMetadata returns nullopt, and this test lands on the SAME no-sidecar branch as
        // deleteBackupRefusesUnmanagedDirectory -- i.e. it was a stealth duplicate that never
        // exercised the identity-mismatch guard it is named for.
        obj["checksum"] = QString();
        QFile meta(victim + ".json");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(QJsonDocument(obj).toJson());
        meta.close();

        UserDataManager mgr;
        QSignalSpy err(&mgr, &UserDataManager::operationError);
        QVERIFY(!mgr.deleteBackup(victim));  // sidecar identity mismatch -> refused
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QStringLiteral("victim"));
        QCOMPARE(err.at(0).at(1).toString(),
                 QStringLiteral("Refusing to delete backup: backup metadata does not identify "
                                "this target"));
        QVERIFY(QFileInfo::exists(victim));
    }

    // --- B7-30: a stray metadata .json without its payload must NOT verify ---
    void verifyBackupFailsWhenPayloadMissing() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "x");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(mgr.verifyBackup(e->backup_path));   // valid while the payload is present

        QVERIFY(QFile::remove(e->backup_path));      // delete payload, keep .json
        QVERIFY(!mgr.verifyBackup(e->backup_path));  // metadata alone must not verify
    }

    // --- B7-30: two unreadable files both hash to "" -- that is NOT "equal" ---
    void compareChecksumsRejectsUnreadablePair() {
        UserDataManager mgr;
        QVERIFY(!mgr.compareChecksums(QStringLiteral("C:/nope/a.dat"),
                                      QStringLiteral("C:/nope/b.dat")));
    }

    // --- B7-09: directory restore must honor overwrite_existing ---
    void dirRestoreHonorsOverwriteFlag() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "NEW");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());

        const auto seedThenRestore = [&](bool overwrite) -> QByteArray {
            QTemporaryDir restoreDir;
            QFile old(QDir(restoreDir.path()).filePath("data.txt"));
            [&] {
                QVERIFY(old.open(QIODevice::WriteOnly));
            }();
            old.write("OLD");
            old.close();
            UserDataManager::RestoreConfig rcfg;
            rcfg.verify_checksum = false;
            rcfg.create_backup = false;
            rcfg.overwrite_existing = overwrite;
            [&] {
                QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));
            }();
            QFile f(QDir(restoreDir.path()).filePath("data.txt"));
            [&] {
                QVERIFY(f.open(QIODevice::ReadOnly));
            }();
            return f.readAll();
        };
        QCOMPARE(seedThenRestore(false), QByteArray("OLD"));  // existing file preserved
        QCOMPARE(seedThenRestore(true), QByteArray("NEW"));   // existing file replaced
    }

    // --- B7-11: an ENCRYPTED backup must round-trip (decrypt temp needs .zip ext) ---
    void encryptedBackupRestoreRoundTrip() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "secret.txt", "TOP SECRET DATA");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.encrypt = true;
        cfg.password = QStringLiteral("hunter2");
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QCOMPARE(QFileInfo(e->backup_path).suffix(), QStringLiteral("zip"));
        // The entry and its sidecar must both RECORD the encryption, or a later restore has no
        // way to know a password is required and reports a confusing extraction failure.
        QVERIFY(e->encrypted);
        QFile meta(e->backup_path + ".json");
        QVERIFY(meta.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(meta.readAll()).object().value("encrypted").toBool(),
                 true);
        meta.close();

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = false;
        rcfg.create_backup = false;
        rcfg.overwrite_existing = true;
        rcfg.password = QStringLiteral("hunter2");
        // Before the .zip-suffix fix, Expand-Archive rejected the extensionless decrypted temp and
        // this returned false -- encrypted backups were unrestorable.
        QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));

        QDirIterator it(restoreDir.path(),
                        {QStringLiteral("secret.txt")},
                        QDir::Files,
                        QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
        QFile restored(it.next());
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), QByteArray("TOP SECRET DATA"));
    }

    // --- CR3-1: a COMPRESSED backup must honor exclude_patterns ---
    // Previously Compress-Archive ran over the raw sources and ignored exclusions, so a
    // "secret" file the user asked to drop was archived anyway. It must now be filtered.
    void compressedBackupHonorsExcludePatterns() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "keep.txt", "KEEP");
        QVERIFY(!src.isEmpty());
        QFile secret(QDir(src).filePath("password.key"));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("SECRET");
        secret.close();

        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.verify_checksum = false;
        cfg.exclude_patterns = QStringList{QStringLiteral("*.key")};
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = false;
        rcfg.create_backup = false;
        rcfg.overwrite_existing = true;
        QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));

        QDirIterator kept(restoreDir.path(),
                          {QStringLiteral("keep.txt")},
                          QDir::Files,
                          QDirIterator::Subdirectories);
        QVERIFY(kept.hasNext());  // non-excluded file present
        // ...with its real bytes: "a file named keep.txt exists" also held if the exclusion
        // filter had archived an empty placeholder for every source file.
        QFile keptFile(kept.next());
        QVERIFY(keptFile.open(QIODevice::ReadOnly));
        QCOMPARE(keptFile.readAll(), QByteArray("KEEP"));
        // The pattern the caller asked for is recorded on the entry, so a restore can explain
        // why the secret is absent rather than looking like data loss.
        QCOMPARE(e->excluded_patterns, QStringList{QStringLiteral("*.key")});
        QDirIterator leaked(restoreDir.path(),
                            {QStringLiteral("password.key")},
                            QDir::Files,
                            QDirIterator::Subdirectories);
        QVERIFY(!leaked.hasNext());  // excluded secret was NOT archived
    }

    // --- CR3-3: an emptied sidecar checksum must not disable verification for a .zip ---
    void restoreRejectsCompressedBackupWithEmptiedChecksum() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "payload");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.verify_checksum = true;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(!e->checksum.isEmpty());

        // Tamper: empty the sidecar checksum to try to skip integrity checking.
        const QString metaPath = e->backup_path + ".json";
        QFile in(metaPath);
        QVERIFY(in.open(QIODevice::ReadOnly));
        QJsonObject obj = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
        obj["checksum"] = QString();
        QFile out(metaPath);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write(QJsonDocument(obj).toJson());
        out.close();

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = true;  // integrity is REQUIRED
        rcfg.create_backup = false;
        // A compressed archive with no checksum cannot be verified -> fail closed, with the
        // reason that names the emptied checksum. A bare false was equally produced by the
        // sidecar failing to parse, or by the extraction failing -- neither of which shows the
        // tamper was DETECTED rather than merely getting in the way.
        QSignalSpy err(&mgr, &UserDataManager::operationError);
        QVERIFY(!mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QStringLiteral("App"));
        QCOMPARE(err.at(0).at(1).toString(),
                 QStringLiteral("Compressed backup has no checksum to verify - refusing restore"));
        // Nothing was extracted before the refusal.
        QCOMPARE(QDir(restoreDir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(),
                 0);
    }

    // --- CR3-7: strict metadata parse rejects a wrong-typed / oversized sidecar ---
    void listBackupsRejectsWrongTypedChecksumSidecar() {
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        const QString payload = QDir(backupDir.path()).filePath("App_20250101_000000.zip");
        QFile pf(payload);
        QVERIFY(pf.open(QIODevice::WriteOnly));
        pf.write("PK\x03\x04");
        pf.close();
        QJsonObject obj;
        obj["app_name"] = QStringLiteral("App");
        obj["backup_path"] = payload;
        obj["checksum"] = 12'345;  // wrong type: must not coerce to "" (which disables verify)
        QFile meta(payload + ".json");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(QJsonDocument(obj).toJson());
        meta.close();

        UserDataManager mgr;
        QVERIFY(mgr.listBackups(backupDir.path()).empty());  // rejected, not silently coerced
    }

    void listBackupsRejectsOversizedSidecar() {
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        const QString payload = QDir(backupDir.path()).filePath("App_big.zip");
        QFile pf(payload);
        QVERIFY(pf.open(QIODevice::WriteOnly));
        pf.write("PK");
        pf.close();
        QByteArray big = "{\"app_name\":\"App\",\"backup_path\":\"" + payload.toUtf8() +
                         "\",\"checksum\":\"\",\"pad\":\"";
        big += QByteArray(1024 * 1024 + 16, 'a');  // push the sidecar past the 1 MiB cap
        big += "\"}";
        QFile meta(payload + ".json");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(big);
        meta.close();

        UserDataManager mgr;
        QVERIFY(mgr.listBackups(backupDir.path()).empty());  // oversized sidecar refused
    }

    // --- CR3-8: empty app_name / backup_dir fail closed at RUNTIME (not debug-only) ---
    void backupFailsClosedOnEmptyInputs() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "x");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        // Each empty input names ITS OWN field. Both return nullopt, so without the messages a
        // single guard covering only one of them would look like both were checked.
        QSignalSpy err(&mgr, &UserDataManager::operationError);
        QVERIFY(!mgr.backupAppData(QString(), {src}, backupDir.path(), cfg).has_value());
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QString());
        QCOMPARE(err.at(0).at(1).toString(), QStringLiteral("App name must not be empty"));

        err.clear();
        QVERIFY(!mgr.backupAppData(QStringLiteral("App"), {src}, QString(), cfg).has_value());
        QCOMPARE(err.count(), 1);
        QCOMPARE(err.at(0).at(0).toString(), QStringLiteral("App"));
        QCOMPARE(err.at(0).at(1).toString(), QStringLiteral("Backup directory must not be empty"));
    }
};

QTEST_MAIN(UserDataManagerTests)
#include "test_user_data_manager.moc"
