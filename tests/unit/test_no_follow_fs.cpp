// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_no_follow_fs.cpp
/// @brief The no-follow file-system primitive: a junction never redirects a write or a delete.
///
/// Every case plants a junction that points at a canary folder holding one file, runs the
/// operation, and checks the canary is exactly as it was.

#include "sak/no_follow_fs.h"

#include "../support/junction_fixture.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdio>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <share.h>

namespace {

const QString kCanaryName = QStringLiteral("canary.txt");
const QByteArray kCanaryBytes = QByteArrayLiteral("the canary must survive");

QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// QVERIFY2 message for a std::expected<void, QString>.
template <typename Expected>
QByteArray why(const Expected& outcome) {
    return outcome ? QByteArray() : outcome.error().toUtf8();
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

}  // namespace

class NoFollowFsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void openRefusesAJunction();
    void openRefusesAFolderUnderAJunction();
    void openNamesTheRealFolderWhenRefused();
    void childRefusesAJunctionAndCreatesNothingThrough();
    void descendCreatesEveryComponent();
    void stagedFilePublishesUnderItsName();
    void stagedFileNoReplaceLeavesAnExistingFile();
    void stagedFileReplaceNameReplacesTheName();
    void stagedFileDiscardedLeavesNothing();
    void copyFileIntoCopiesAndRefusesAnExistingName();
    void moveIntoRefusesASourceUnderAJunction();
    void moveIntoMovesTheName();
    void moveIntoLandsInTheHeldDirectory();
    void removeTreeRemovesALinkNotItsTarget();
    void removeTreeOfAJunctionRemovesOnlyTheJunction();
    void removeTreeRemovesReadOnlyFiles();
    void removeTreeOfAMissingPathSucceeds();
    void createNewFileRefusesAnExistingName();
    void createNewFileDenyWriteKeepsOthersOut();
    void createUniqueChildIsFreshAndProtected();
    void entryKindReadsTheNameItself();
    void attributesAreReadOnEveryFileSystem();
    void heldReadOnlyFileOpensForReadersAndRefusesWriters();

private:
    std::unique_ptr<QTemporaryDir> m_root;
    QString m_canary;
};

void NoFollowFsTests::init() {
    m_root = std::make_unique<QTemporaryDir>();
    QVERIFY(m_root->isValid());
    m_canary = m_root->filePath(QStringLiteral("canary"));
    QVERIFY(sak::test::createCanary(m_canary, kCanaryName, kCanaryBytes));
}

void NoFollowFsTests::openRefusesAJunction() {
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    const auto held = sak::no_follow::HeldDirectory::open(link);
    QVERIFY(!held);
    QVERIFY2(held.error().contains(QStringLiteral("link")), qPrintable(held.error()));
}

void NoFollowFsTests::openRefusesAFolderUnderAJunction() {
    QVERIFY(QDir().mkpath(QDir(m_canary).filePath(QStringLiteral("inside"))));
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    QVERIFY(!sak::no_follow::HeldDirectory::open(link + QStringLiteral("/inside")));
}

void NoFollowFsTests::openNamesTheRealFolderWhenRefused() {
    QVERIFY(QDir().mkpath(QDir(m_canary).filePath(QStringLiteral("inside"))));
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    const auto held = sak::no_follow::HeldDirectory::open(link + QStringLiteral("/inside"));
    QVERIFY(!held);
    QVERIFY2(held.error().contains(QDir::toNativeSeparators(m_canary), Qt::CaseInsensitive),
             qPrintable(held.error()));
}

void NoFollowFsTests::childRefusesAJunctionAndCreatesNothingThrough() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY2(root, qPrintable(root ? QString() : root.error()));
    QVERIFY(sak::test::createJunction(m_root->filePath(QStringLiteral("link")), m_canary));
    QVERIFY(!root->child(QStringLiteral("link"), true));
    QVERIFY(!root->descend(QStringLiteral("link/made"), true));
    QVERIFY(!QFileInfo::exists(QDir(m_canary).filePath(QStringLiteral("made"))));
    QVERIFY(sak::test::canaryIntact(m_canary, kCanaryName, kCanaryBytes));
}

void NoFollowFsTests::descendCreatesEveryComponent() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const auto deep = root->descend(QStringLiteral("a/b\\c"), true);
    QVERIFY2(deep, qPrintable(deep ? QString() : deep.error()));
    QVERIFY(QFileInfo(m_root->filePath(QStringLiteral("a/b/c"))).isDir());
    QVERIFY(sak::no_follow::samePath(deep->path(), m_root->filePath(QStringLiteral("a/b/c"))));
    QVERIFY(!root->child(QStringLiteral(".."), true));
    QVERIFY(!root->child(QStringLiteral("x/y"), true));
}

void NoFollowFsTests::stagedFilePublishesUnderItsName() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    auto staged = sak::no_follow::StagedFile::create(*root, QStringLiteral("out.bin"));
    QVERIFY2(staged, qPrintable(staged ? QString() : staged.error()));
    QVERIFY(QFileInfo::exists(staged->stagingPath()));
    QCOMPARE(staged->device().write("payload"), 7);
    const auto published = staged->publish(sak::no_follow::Publish::NoReplace);
    QVERIFY2(published, why(published).constData());
    QCOMPARE(readAll(m_root->filePath(QStringLiteral("out.bin"))), QByteArray("payload"));
    QVERIFY(!QFileInfo::exists(staged->stagingPath()));
}

void NoFollowFsTests::stagedFileNoReplaceLeavesAnExistingFile() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const QString existing = m_root->filePath(QStringLiteral("out.bin"));
    QVERIFY(writeFile(existing, "original"));
    auto staged = sak::no_follow::StagedFile::create(*root, QStringLiteral("out.bin"));
    QVERIFY(staged);
    QCOMPARE(staged->device().write("new"), 3);
    QVERIFY(!staged->publish(sak::no_follow::Publish::NoReplace));
    QCOMPARE(readAll(existing), QByteArray("original"));
}

void NoFollowFsTests::stagedFileReplaceNameReplacesTheName() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const QString existing = m_root->filePath(QStringLiteral("out.bin"));
    QVERIFY(writeFile(existing, "original"));
    auto staged = sak::no_follow::StagedFile::create(*root, QStringLiteral("out.bin"));
    QVERIFY(staged);
    QCOMPARE(staged->device().write("new"), 3);
    const auto published = staged->publish(sak::no_follow::Publish::ReplaceName);
    QVERIFY2(published, why(published).constData());
    QCOMPARE(readAll(existing), QByteArray("new"));
}

void NoFollowFsTests::stagedFileDiscardedLeavesNothing() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    QString staging;
    {
        auto staged = sak::no_follow::StagedFile::create(*root, QStringLiteral("out.bin"));
        QVERIFY(staged);
        staging = staged->stagingPath();
        QCOMPARE(staged->device().write("never published"), 15);
    }
    QVERIFY(!QFileInfo::exists(staging));
    QVERIFY(!QFileInfo::exists(m_root->filePath(QStringLiteral("out.bin"))));
}

void NoFollowFsTests::copyFileIntoCopiesAndRefusesAnExistingName() {
    const auto target = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(target);
    const QString source = QDir(m_canary).filePath(kCanaryName);
    const auto copied = sak::no_follow::copyFileInto(
        source, *target, QStringLiteral("copy.txt"), sak::no_follow::Publish::NoReplace);
    QVERIFY2(copied, why(copied).constData());
    QCOMPARE(readAll(m_root->filePath(QStringLiteral("copy.txt"))), kCanaryBytes);
    QVERIFY(!sak::no_follow::copyFileInto(
        source, *target, QStringLiteral("copy.txt"), sak::no_follow::Publish::NoReplace));
    const QStringList left =
        QDir(m_root->path()).entryList({QStringLiteral("*.sakpart")}, QDir::Files | QDir::Hidden);
    QVERIFY2(left.isEmpty(), qPrintable(left.join(',')));
}

void NoFollowFsTests::moveIntoRefusesASourceUnderAJunction() {
    const auto target = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(target);
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    QVERIFY(!sak::no_follow::moveInto(QDir(link).filePath(kCanaryName),
                                      *target,
                                      QStringLiteral("moved.txt"),
                                      sak::no_follow::Publish::NoReplace));
    QVERIFY(sak::test::canaryIntact(m_canary, kCanaryName, kCanaryBytes));
}

void NoFollowFsTests::moveIntoMovesTheName() {
    const auto target = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(target);
    const QString source = m_root->filePath(QStringLiteral("source.txt"));
    QVERIFY(writeFile(source, "moving"));
    const auto moved = sak::no_follow::moveInto(
        source, *target, QStringLiteral("moved.txt"), sak::no_follow::Publish::NoReplace);
    QVERIFY2(moved, why(moved).constData());
    QVERIFY(!QFileInfo::exists(source));
    QCOMPARE(readAll(m_root->filePath(QStringLiteral("moved.txt"))), QByteArray("moving"));
}

void NoFollowFsTests::moveIntoLandsInTheHeldDirectory() {
    QVERIFY(QDir().mkpath(m_root->filePath(QStringLiteral("from"))));
    QVERIFY(QDir().mkpath(m_root->filePath(QStringLiteral("to"))));
    const auto target = sak::no_follow::HeldDirectory::open(m_root->filePath(QStringLiteral("to")));
    QVERIFY(target);
    const QString source = m_root->filePath(QStringLiteral("from/source.txt"));
    QVERIFY(writeFile(source, "moving"));
    const auto moved = sak::no_follow::moveInto(
        source, *target, QStringLiteral("moved.txt"), sak::no_follow::Publish::NoReplace);
    QVERIFY2(moved, why(moved).constData());
    QCOMPARE(readAll(m_root->filePath(QStringLiteral("to/moved.txt"))), QByteArray("moving"));
    QVERIFY(!QFileInfo::exists(m_root->filePath(QStringLiteral("from/moved.txt"))));
}

void NoFollowFsTests::removeTreeRemovesALinkNotItsTarget() {
    const QString tree = m_root->filePath(QStringLiteral("tree"));
    QVERIFY(QDir().mkpath(tree + QStringLiteral("/sub")));
    QVERIFY(writeFile(tree + QStringLiteral("/sub/file.txt"), "x"));
    QVERIFY(sak::test::createJunction(tree + QStringLiteral("/sub/escape"), m_canary));
    QVERIFY(sak::no_follow::removeTree(tree));
    QVERIFY(!QFileInfo::exists(tree));
    QVERIFY(sak::test::canaryIntact(m_canary, kCanaryName, kCanaryBytes));
}

void NoFollowFsTests::removeTreeOfAJunctionRemovesOnlyTheJunction() {
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    QVERIFY(sak::no_follow::removeTree(link));
    QVERIFY(sak::no_follow::entryKind(link) == sak::no_follow::EntryKind::Missing);
    QVERIFY(sak::test::canaryIntact(m_canary, kCanaryName, kCanaryBytes));
}

void NoFollowFsTests::removeTreeRemovesReadOnlyFiles() {
    const QString tree = m_root->filePath(QStringLiteral("tree"));
    QVERIFY(QDir().mkpath(tree));
    const QString file = tree + QStringLiteral("/locked.txt");
    QVERIFY(writeFile(file, "x"));
    QVERIFY(SetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file).utf16()),
                               FILE_ATTRIBUTE_READONLY) != 0);
    QVERIFY(sak::no_follow::removeTree(tree));
    QVERIFY(!QFileInfo::exists(tree));
}

void NoFollowFsTests::removeTreeOfAMissingPathSucceeds() {
    QVERIFY(sak::no_follow::removeTree(m_root->filePath(QStringLiteral("never-there"))));
}

void NoFollowFsTests::createNewFileRefusesAnExistingName() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    QVERIFY(sak::test::createJunction(m_root->filePath(QStringLiteral("planted.log")), m_canary));
    QVERIFY(!sak::no_follow::createNewFile(*root, QStringLiteral("planted.log"), true));
    QVERIFY(writeFile(m_root->filePath(QStringLiteral("existing.log")), "keep"));
    QVERIFY(!sak::no_follow::createNewFile(*root, QStringLiteral("existing.log"), true));
    QCOMPARE(readAll(m_root->filePath(QStringLiteral("existing.log"))), QByteArray("keep"));
    QVERIFY(sak::test::canaryIntact(m_canary, kCanaryName, kCanaryBytes));
}

void NoFollowFsTests::createNewFileDenyWriteKeepsOthersOut() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const auto handle = sak::no_follow::createNewFile(*root, QStringLiteral("held.log"), true);
    QVERIFY2(handle, qPrintable(handle ? QString() : handle.error()));
    QFile other(m_root->filePath(QStringLiteral("held.log")));
    QVERIFY(!other.open(QIODevice::WriteOnly | QIODevice::Append));
    QVERIFY(!QFile::remove(m_root->filePath(QStringLiteral("held.log"))));
    QFile reader(m_root->filePath(QStringLiteral("held.log")));
    QVERIFY(reader.open(QIODevice::ReadOnly));
}

void NoFollowFsTests::createUniqueChildIsFreshAndProtected() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const QString sddl = QStringLiteral("D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)");
    const auto first = root->createUniqueChild(QStringLiteral("run-"), sddl);
    QVERIFY2(first, qPrintable(first ? QString() : first.error()));
    const auto second = root->createUniqueChild(QStringLiteral("run-"), sddl);
    QVERIFY(second);
    QVERIFY(!sak::no_follow::samePath(first->path(), second->path()));
    QVERIFY(QFileInfo(first->path()).fileName().startsWith(QStringLiteral("run-")));
    if (!sak::no_follow::volumeKeepsAcls(root->handle())) {
        return;  // FAT and exFAT keep no DACL to check (and hold no links either)
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QCOMPARE(GetSecurityInfo(first->handle(),
                             SE_FILE_OBJECT,
                             DACL_SECURITY_INFORMATION,
                             nullptr,
                             nullptr,
                             nullptr,
                             nullptr,
                             &descriptor),
             static_cast<DWORD>(ERROR_SUCCESS));
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision) != 0);
    LocalFree(descriptor);
    QVERIFY((control & SE_DACL_PROTECTED) != 0);
}

void NoFollowFsTests::entryKindReadsTheNameItself() {
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    QVERIFY(sak::no_follow::entryKind(link) == sak::no_follow::EntryKind::Link);
    QVERIFY(sak::no_follow::entryKind(m_canary) == sak::no_follow::EntryKind::Directory);
    QVERIFY(sak::no_follow::entryKind(QDir(m_canary).filePath(kCanaryName)) ==
            sak::no_follow::EntryKind::File);
    QVERIFY(sak::no_follow::entryKind(m_root->filePath(QStringLiteral("none"))) ==
            sak::no_follow::EntryKind::Missing);
}

// Security brief 4a (owner, 2026-11-11): a script a tool reads by name (diskpart /s) is held
// read-only from the moment it is written until the tool is done. diskpart reads its script with
// the C runtime's _wfopen (its imports: _wfopen from msvcrt), which shares reading and writing, so
// it opens through the hold; any open for writing, and any delete, is refused, on NTFS and on FAT
// alike (the VM proof repeats this on a FAT32 volume).
void NoFollowFsTests::heldReadOnlyFileOpensForReadersAndRefusesWriters() {
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const QByteArray script = QByteArrayLiteral("list disk\r\n");
    auto held = sak::no_follow::writeHeldReadOnlyFile(*root, QStringLiteral("dp.txt"), script);
    QVERIFY2(held, why(held).constData());
    const QString path = m_root->filePath(QStringLiteral("dp.txt"));
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();

    // diskpart.exe reads /s through _wfopen, which is _wfsopen with _SH_DENYNO.
    FILE* as_diskpart = _wfsopen(wide.c_str(), L"r", _SH_DENYNO);
    QVERIFY2(as_diskpart != nullptr, "the reader diskpart uses could not open the held script");
    std::fclose(as_diskpart);
    HANDLE reader = CreateFileW(wide.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
    QVERIFY(reader != INVALID_HANDLE_VALUE);
    CloseHandle(reader);

    HANDLE writer = CreateFileW(wide.c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                0,
                                nullptr);
    const DWORD writer_error = GetLastError();
    QVERIFY(writer == INVALID_HANDLE_VALUE);
    QCOMPARE(writer_error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    QVERIFY(DeleteFileW(wide.c_str()) == 0);
    QCOMPARE(readAll(path), script);

    held->reset();
    QVERIFY(DeleteFileW(wide.c_str()) != 0);
}

// FAT32 and exFAT refuse FileAttributeTagInfo (measured on the VM, 2026-11-11), which made every
// held folder and every program-file check there fail ("Cannot read"), so S.A.K. would not run as
// administrator from a FAT32 or exFAT flash drive at all. There the attributes come from
// FileBasicInfo and the tag is 0; a reparse attribute with no tag still refuses. The VM runs this
// suite's file cases on a FAT32 and an exFAT volume (RELEASE_READINESS).
void NoFollowFsTests::attributesAreReadOnEveryFileSystem() {
    using sak::no_follow::AttributeAnswers;
    using sak::no_follow::attributesFromAnswers;
    constexpr unsigned long kJunction = IO_REPARSE_TAG_MOUNT_POINT;
    constexpr unsigned long kReparseFolder = FILE_ATTRIBUTE_REPARSE_POINT |
                                             FILE_ATTRIBUTE_DIRECTORY;

    // NTFS: the tag answer is used as it is, link or not.
    auto ntfs = attributesFromAnswers({.tag_attributes = kReparseFolder, .reparse_tag = kJunction});
    QVERIFY(ntfs);
    QCOMPARE(ntfs->first, kReparseFolder);
    QCOMPARE(ntfs->second, kJunction);
    // FAT32 / exFAT: no tag answer, the basic attributes, tag 0.
    auto fat = attributesFromAnswers({.basic_attributes = FILE_ATTRIBUTE_ARCHIVE});
    QVERIFY(fat);
    QCOMPARE(fat->first, static_cast<unsigned long>(FILE_ATTRIBUTE_ARCHIVE));
    QCOMPARE(fat->second, 0UL);
    // A reparse point whose tag cannot be read is never taken for an ordinary entry.
    QVERIFY(!attributesFromAnswers({.basic_attributes = kReparseFolder}));
    // Neither answer: refuse.
    QVERIFY(!attributesFromAnswers({}));

    // On this volume the handle answers directly.
    const auto root = sak::no_follow::HeldDirectory::open(m_root->path());
    QVERIFY(root);
    const auto kind = sak::no_follow::entryKindOfHandle(root->handle());
    QVERIFY2(kind, kind ? "" : qPrintable(kind.error()));
    QCOMPARE(*kind, sak::no_follow::EntryKind::Directory);
}

QTEST_GUILESS_MAIN(NoFollowFsTests)
#include "test_no_follow_fs.moc"
