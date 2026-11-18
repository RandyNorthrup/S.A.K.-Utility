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
#include <QProcess>
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
    void resolvedPathFollowsAJunctionAnywhere();
    void pathIsSameOrBelowNeedsASeparatorBoundary();
    void pathWithinRootRefusesAJunctionLeadingOut();
    void pathWithinRootAcceptsARootReachedThroughAJunction();
    void heldFileRefusesRenameDeleteAndReplace();
    void heldFileRefusesWriters();
    void heldFileWithinRootChecksTheHeldFile();
    void heldFileStillStartsAProgram();
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

// Lane SEC2 (2026-11-17): QFileInfo::canonicalFilePath does not resolve a junction in Qt 6, at the
// name or in a folder above it, so every containment check built on it let a junction through.
// resolvedPath goes through a handle and must name where the path really leads.
void NoFollowFsTests::resolvedPathFollowsAJunctionAnywhere() {
    const QString link = m_root->filePath(QStringLiteral("link"));
    QVERIFY(sak::test::createJunction(link, m_canary));
    const QString real_file = sak::no_follow::resolvedPath(QDir(m_canary).filePath(kCanaryName));
    QVERIFY(!real_file.isEmpty());
    // A junction in a folder above the name.
    const QString through = sak::no_follow::resolvedPath(QDir(link).filePath(kCanaryName));
    QVERIFY2(sak::no_follow::samePath(through, real_file), qPrintable(through));
    // A junction as the name itself.
    const QString at_name = sak::no_follow::resolvedPath(link);
    QVERIFY2(sak::no_follow::samePath(at_name, sak::no_follow::resolvedPath(m_canary)),
             qPrintable(at_name));
    // Nothing there: nothing to name.
    QVERIFY(sak::no_follow::resolvedPath(m_root->filePath(QStringLiteral("missing"))).isEmpty());
    QVERIFY(sak::no_follow::resolvedPath(QString()).isEmpty());
}

void NoFollowFsTests::pathIsSameOrBelowNeedsASeparatorBoundary() {
    using sak::no_follow::pathIsSameOrBelow;
    QVERIFY(
        pathIsSameOrBelow(QStringLiteral(R"(C:\App\tools\x.exe)"), QStringLiteral(R"(C:\App)")));
    QVERIFY(pathIsSameOrBelow(QStringLiteral("C:/App/tools"), QStringLiteral(R"(C:\App\)")));
    QVERIFY(pathIsSameOrBelow(QStringLiteral(R"(C:\App)"), QStringLiteral(R"(C:\App)")));
    QVERIFY(pathIsSameOrBelow(QStringLiteral(R"(c:\app\X)"), QStringLiteral(R"(C:\APP)")));
    QVERIFY(pathIsSameOrBelow(QStringLiteral(R"(D:\x)"), QStringLiteral(R"(D:\)")));
    QVERIFY(pathIsSameOrBelow(QStringLiteral(R"(\\server\share\d)"),
                              QStringLiteral(R"(\\server\share)")));
    // A sibling whose name starts with the root's is not inside it.
    QVERIFY(!pathIsSameOrBelow(QStringLiteral(R"(C:\AppX\x.exe)"), QStringLiteral(R"(C:\App)")));
    QVERIFY(!pathIsSameOrBelow(QStringLiteral(R"(C:\)"), QStringLiteral(R"(C:\App)")));
    QVERIFY(!pathIsSameOrBelow(QStringLiteral(R"(E:\x)"), QStringLiteral(R"(D:\)")));
    // Either side unknown: refuse.
    QVERIFY(!pathIsSameOrBelow(QString(), QStringLiteral(R"(C:\App)")));
    QVERIFY(!pathIsSameOrBelow(QStringLiteral(R"(C:\App\x)"), QString()));
}

void NoFollowFsTests::pathWithinRootRefusesAJunctionLeadingOut() {
    const QString root = m_root->filePath(QStringLiteral("root"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("plain.txt")), QByteArrayLiteral("in")));
    const QString out = QDir(root).filePath(QStringLiteral("out"));
    QVERIFY(sak::test::createJunction(out, m_canary));
    QVERIFY(sak::no_follow::pathWithinRoot(QDir(root).filePath(QStringLiteral("plain.txt")), root));
    QVERIFY2(!sak::no_follow::pathWithinRoot(QDir(out).filePath(kCanaryName), root),
             "a file under a junction that leads out of the root passed");
    QVERIFY2(!sak::no_follow::pathWithinRoot(out, root),
             "a junction that leads out of the root, named itself, passed");
    // The sibling the junction leads to is not inside the root either way.
    QVERIFY(!sak::no_follow::pathWithinRoot(m_canary, root));
    // A path that does not exist cannot be proved inside: refuse.
    QVERIFY(!sak::no_follow::pathWithinRoot(QDir(root).filePath(QStringLiteral("nope")), root));
}

// S.A.K. may legitimately be unpacked into a junctioned folder. A root reached through a junction
// holds its own real files, whichever spelling either side uses.
void NoFollowFsTests::pathWithinRootAcceptsARootReachedThroughAJunction() {
    const QString real = m_root->filePath(QStringLiteral("real"));
    QVERIFY(QDir().mkpath(QDir(real).filePath(QStringLiteral("tools"))));
    QVERIFY(
        writeFile(QDir(real).filePath(QStringLiteral("tools/tool.exe")), QByteArrayLiteral("x")));
    const QString alias = m_root->filePath(QStringLiteral("alias"));
    QVERIFY(sak::test::createJunction(alias, real));
    const QString through_alias = QDir(alias).filePath(QStringLiteral("tools/tool.exe"));
    QVERIFY2(sak::no_follow::pathWithinRoot(through_alias, alias),
             "a real file in a root reached through a junction was refused");
    QVERIFY(sak::no_follow::pathWithinRoot(through_alias, real));
    QVERIFY(sak::no_follow::pathWithinRoot(QDir(real).filePath(QStringLiteral("tools/tool.exe")),
                                           alias));
    QVERIFY(!sak::no_follow::pathWithinRoot(QDir(m_canary).filePath(kCanaryName), alias));
}

// Lane SEC2 round 2: a program is started from a file held by holdFile, so nothing can change
// between the check and the start. While it is held, these are refused; once it is let go, the
// same calls work, which proves the refusals came from the hold.
void NoFollowFsTests::heldFileRefusesRenameDeleteAndReplace() {
    const QString tools = m_root->filePath(QStringLiteral("app/tools"));
    QVERIFY(QDir().mkpath(tools));
    const QString tool = QDir(tools).filePath(QStringLiteral("tool.exe"));
    const QString other = QDir(tools).filePath(QStringLiteral("other.exe"));
    QVERIFY(writeFile(tool, QByteArrayLiteral("held")));
    QVERIFY(writeFile(other, QByteArrayLiteral("planted")));
    const auto wide = [](const QString& path) {
        return QDir::toNativeSeparators(path).toStdWString();
    };
    auto held = sak::no_follow::holdFile(tool);
    QVERIFY2(held, held ? "" : qPrintable(held.error()));

    QVERIFY2(MoveFileExW(wide(tool).c_str(), wide(tool + QStringLiteral(".moved")).c_str(), 0) == 0,
             "a held file was renamed");
    QVERIFY2(DeleteFileW(wide(tool).c_str()) == 0, "a held file was deleted");
    QVERIFY2(MoveFileExW(wide(other).c_str(), wide(tool).c_str(), MOVEFILE_REPLACE_EXISTING) == 0,
             "a held file was replaced by a rename over it");
    const QString renamed_folder = m_root->filePath(QStringLiteral("app/tools-moved"));
    QVERIFY2(MoveFileExW(wide(tools).c_str(), wide(renamed_folder).c_str(), 0) == 0,
             "the folder above a held file was renamed");
    QCOMPARE(readAll(tool), QByteArrayLiteral("held"));

    held->handle.reset();
    QVERIFY(MoveFileExW(wide(tools).c_str(), wide(renamed_folder).c_str(), 0) != 0);
}

void NoFollowFsTests::heldFileRefusesWriters() {
    const QString tool = m_root->filePath(QStringLiteral("tool.exe"));
    QVERIFY(writeFile(tool, QByteArrayLiteral("held")));
    auto held = sak::no_follow::holdFile(tool);
    QVERIFY2(held, held ? "" : qPrintable(held.error()));
    HANDLE writer = CreateFileW(QDir::toNativeSeparators(tool).toStdWString().c_str(),
                                GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    const DWORD error = GetLastError();
    if (writer != INVALID_HANDLE_VALUE) {
        CloseHandle(writer);
    }
    QVERIFY2(writer == INVALID_HANDLE_VALUE, "a held file was opened for writing");
    QCOMPARE(error, static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    QCOMPARE(readAll(tool), QByteArrayLiteral("held"));
}

void NoFollowFsTests::heldFileWithinRootChecksTheHeldFile() {
    const QString root = m_root->filePath(QStringLiteral("root"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("tool.exe")), QByteArrayLiteral("in")));
    QVERIFY(sak::test::createJunction(QDir(root).filePath(QStringLiteral("out")), m_canary));
    const auto inside =
        sak::no_follow::holdFileWithinRoot(QDir(root).filePath(QStringLiteral("tool.exe")), root);
    QVERIFY2(inside, inside ? "" : qPrintable(inside.error()));
    QVERIFY(sak::no_follow::samePath(inside->path,
                                     sak::no_follow::resolvedPath(
                                         QDir(root).filePath(QStringLiteral("tool.exe")))));
    const auto outside = sak::no_follow::holdFileWithinRoot(
        QDir(root).filePath(QStringLiteral("out/") + kCanaryName), root);
    QVERIFY2(!outside, "a file reached through a junction that leads out of the root was held");
    QVERIFY(!outside.error().isEmpty());
    // A root reached through a junction holds its own files.
    const QString alias = m_root->filePath(QStringLiteral("alias"));
    QVERIFY(sak::test::createJunction(alias, root));
    const auto through_alias =
        sak::no_follow::holdFileWithinRoot(QDir(alias).filePath(QStringLiteral("tool.exe")), alias);
    QVERIFY2(through_alias, through_alias ? "" : qPrintable(through_alias.error()));
    // A folder is not a program file.
    QVERIFY(!sak::no_follow::holdFile(root));
}

void NoFollowFsTests::heldFileStillStartsAProgram() {
    // A copy of cmd.exe stands in for a bundled tool: the hold must not stop Windows from starting
    // a program from the file, by the real path the hold names.
    const QString tool = m_root->filePath(QStringLiteral("held_tool.exe"));
    QVERIFY(QFile::copy(QDir(QString::fromLocal8Bit(qgetenv("SystemRoot")))
                            .filePath(QStringLiteral("System32/cmd.exe")),
                        tool));
    const auto held = sak::no_follow::holdFileWithinRoot(tool, m_root->path());
    QVERIFY2(held, held ? "" : qPrintable(held.error()));
    QProcess process;
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
    process.start(held->path,
                  {QStringLiteral("/d"), QStringLiteral("/c"), QStringLiteral("exit 7")});
    QVERIFY2(process.waitForStarted(), qPrintable(process.errorString()));
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 7);
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
