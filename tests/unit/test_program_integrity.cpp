// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_program_integrity.cpp
/// @brief Security brief 4c/4d: what an elevation accepts from the program folder.
///
/// The signed half of the policy (a release, where sak_utility.exe is signed) cannot be built on
/// a developer PC without a signing certificate, so sameSigner is tested on its own here, and the
/// signed package is proved on the VM (docs/RELEASE_READINESS.md, security section).

#include "sak/error_codes.h"
#include "sak/program_integrity.h"

#include "../support/junction_fixture.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>
#include <cstddef>

#include <windows.h>

namespace pi = sak::program_integrity;

namespace {

pi::Signature signedBy(const QString& subject,
                       const QStringList& ekus,
                       const QByteArray& leaf,
                       pi::SignatureState state = pi::SignatureState::Trusted) {
    return pi::Signature{
        .state = state, .subject = subject, .ekus = ekus, .leaf_sha256 = leaf, .detail = {}};
}

const QString kRandy = QStringLiteral("CN=Randy Northrup, O=Randy Northrup");
const QStringList kAzureEkus = {QStringLiteral("1.3.6.1.4.1.311.97.1"),
                                QStringLiteral("1.3.6.1.5.5.7.3.3")};

pi::FileFacts facts(const QString& relative,
                    const pi::Signature& signature,
                    const QByteArray& sha256 = QByteArray()) {
    return pi::FileFacts{.relative = relative,
                         .is_running_image = false,
                         .sha256 = [sha256]() { return sha256; },
                         .signature = [signature]() { return signature; }};
}

QString appFile(const QString& name) {
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool copyInto(const QString& source, const QString& folder, const QString& name) {
    return QDir().mkpath(folder) && QFile::copy(source, QDir(folder).filePath(name));
}

bool flipByteNearMiddle(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) {
        return false;
    }
    const qint64 at = file.size() / 2;
    file.seek(at);
    char byte = 0;
    file.getChar(&byte);
    file.seek(at);
    return file.putChar(static_cast<char>(byte ^ 0x5A));
}

}  // namespace

class ProgramIntegrityTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void uncapturedProgramRefusesAsItsOwnFault();
    void sameSignerNeedsTheSameIdentityOrTheSameCertificate();
    void policyAcceptsOnlyWhatTheRulesName();
    void signaturesOfRealFiles();
    void cleanFolderIsHeldAndAPlantedDllRefused();
    void tamperedPinnedFileIsRefused();
    void junctionInsideTheProgramFolderIsRefused();
    void heldFilesCannotBeChangedOrReplaced();
    void moduleRule();
    void moduleLoadedFromElsewhereFailsTheAudit();
    void everyShippedFileTheBuildCopiesIsPinned();
    void elevatedProgramsResolveImportsOnlyFromTheirFolderAndSystem32();
    void everyProgramThatCanElevateRecordsItself();
};

void ProgramIntegrityTests::initTestCase() {
    // Not captured here: uncapturedProgramRefusesAsItsOwnFault runs first and needs the state a
    // program that forgot to record itself is in. It captures when it is done.
}

// A program that never recorded its own file at start (the step client, 2026-11-11) vouches for
// nothing, so every elevation is refused; the refusal says that, an internal error, and never
// blames a file in the folder. Once recorded, an empty folder passes.
void ProgramIntegrityTests::uncapturedProgramRefusesAsItsOwnFault() {
    const QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const auto refused = pi::ProgramHold::verify(folder.path());
    QVERIFY(!refused);
    QVERIFY2(refused.error().file.isEmpty(), qPrintable(refused.error().file));
    QVERIFY2(refused.error().reason.contains(
                 QStringLiteral("did not record its own program file when it started")),
             qPrintable(refused.error().reason));
    const QString message = pi::refusalMessage(refused.error());
    QVERIFY2(message.contains(QStringLiteral("could not check its own program files")),
             qPrintable(message));
    QVERIFY2(!message.contains(QStringLiteral("isn't part of S.A.K.")), qPrintable(message));
    QVERIFY(QString::fromUtf8(sak::to_string(sak::error_code::program_check_failed))
                .contains(QStringLiteral("could not check its own program files")));

    pi::captureRunningImageIdentity();
    const auto held = pi::ProgramHold::verify(folder.path());
    QVERIFY2(held, qPrintable(held ? QString() : held.error().reason));
}

namespace {

/// The program source files that define an entry point (main, wWinMain or a QtTest main).
bool definesEntryPoint(const QString& text) {
    static const QRegularExpression entry(
        QStringLiteral(R"(^int (w)?main\(|^int WINAPI wWinMain|QTEST_(GUILESS_|APPLESS_)?MAIN\()"),
        QRegularExpression::MultilineOption);
    return entry.match(text).hasMatch();
}

QString readText(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

}  // namespace

namespace {

/// The certifier sources that compile program_integrity.cpp themselves (tests/CMakeLists.txt: an
/// add_executable or a set() of sources that lists it), by file name.
QStringList sourcesCompilingProgramIntegrity(const QDir& root) {
    QStringList compiles_integrity;
    const QString cmake = readText(root.filePath(QStringLiteral("tests/CMakeLists.txt")));
    static const QRegularExpression block(
        QStringLiteral(R"((?:add_executable|set)\(\w+([^)]*)\))"));
    static const QRegularExpression cpp(QStringLiteral(R"(([\w./-]+\.cpp))"));
    for (auto it = block.globalMatch(cmake); it.hasNext();) {
        const QString sources = it.next().captured(1);
        if (!sources.contains(QStringLiteral("program_integrity.cpp"))) {
            continue;
        }
        for (auto file = cpp.globalMatch(sources); file.hasNext();) {
            compiles_integrity.append(QFileInfo(file.next().captured(1)).fileName());
        }
    }
    return compiles_integrity;
}

/// Whether the program defined in @p path (source @p text) can reach an elevation: a unit test
/// that uses the broker, Run as Administrator or the hold; S.A.K. itself, the helper, or a
/// certifier that compiles program_integrity (the step client); or any program that uses them.
bool programCanElevate(const QString& path,
                       const QString& text,
                       bool unit_test,
                       const QStringList& compiles_integrity) {
    static const QStringList elevating = {QStringLiteral("ElevationBroker"),
                                          QStringLiteral("restartElevated"),
                                          QStringLiteral("launchElevated"),
                                          QStringLiteral("ProgramHold::verify")};
    const bool uses_elevation = std::ranges::any_of(elevating, [&text](const QString& name) {
        return text.contains(name);
    });
    if (unit_test) {
        return uses_elevation;
    }
    const QString name = QFileInfo(path).fileName();
    return uses_elevation || name == QStringLiteral("main.cpp") ||
           name == QStringLiteral("elevated_helper_main.cpp") || compiles_integrity.contains(name);
}

}  // namespace

// Guard: every program that can reach an elevation records its own file at start. Each program
// in src/ and tests/certification/ whose sources compile or link the elevation code, and each test
// that uses the broker, the manager or the hold directly, calls captureRunningImageIdentity().
void ProgramIntegrityTests::everyProgramThatCanElevateRecordsItself() {
    const QString repo = QFINDTESTDATA("../../CMakeLists.txt");
    QVERIFY2(!repo.isEmpty(), "cannot find the repository from the test's source");
    const QDir root = QFileInfo(repo).absoluteDir();
    const QStringList compiles_integrity = sourcesCompilingProgramIntegrity(root);
    QVERIFY2(compiles_integrity.contains(QStringLiteral("partition_manager_live_certifier.cpp")),
             qPrintable(compiles_integrity.join(QStringLiteral(", "))));
    QStringList missing;
    int programs = 0;
    for (const QString& folder : {QStringLiteral("src"),
                                  QStringLiteral("tests/certification"),
                                  QStringLiteral("tests/unit")}) {
        QDirIterator it(root.filePath(folder),
                        {QStringLiteral("*.cpp")},
                        QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString text = readText(path);
            if (!definesEntryPoint(text) ||
                !programCanElevate(
                    path, text, folder == QStringLiteral("tests/unit"), compiles_integrity)) {
                continue;
            }
            ++programs;
            if (!text.contains(QStringLiteral("captureRunningImageIdentity()"))) {
                missing.append(root.relativeFilePath(path));
            }
        }
    }
    QVERIFY2(programs >= 6, qPrintable(QString::number(programs)));
    QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral(", "))));
}

void ProgramIntegrityTests::sameSignerNeedsTheSameIdentityOrTheSameCertificate() {
    const QByteArray day1 = QByteArray(32, '\x01');
    const QByteArray day2 = QByteArray(32, '\x02');
    // Both trusted: the identity (subject and EKUs) decides, so a renewed certificate still
    // matches.
    QVERIFY(pi::sameSigner(signedBy(kRandy, kAzureEkus, day1), signedBy(kRandy, kAzureEkus, day2)));
    QVERIFY(!pi::sameSigner(signedBy(kRandy, kAzureEkus, day1),
                            signedBy(QStringLiteral("CN=Someone Else"), kAzureEkus, day1)));
    QVERIFY(!pi::sameSigner(signedBy(kRandy, kAzureEkus, day1),
                            signedBy(kRandy, {QStringLiteral("1.3.6.1.5.5.7.3.3")}, day1)));
    // A chain this PC does not trust: only the very same certificate.
    const auto offline = pi::SignatureState::UntrustedChain;
    QVERIFY(pi::sameSigner(signedBy(kRandy, kAzureEkus, day1, offline),
                           signedBy(kRandy, kAzureEkus, day1, offline)));
    QVERIFY(!pi::sameSigner(signedBy(kRandy, kAzureEkus, day1, offline),
                            signedBy(kRandy, kAzureEkus, day2, offline)));
    QVERIFY(pi::sameSigner(signedBy(kRandy, kAzureEkus, day1),
                           signedBy(kRandy, kAzureEkus, day1, offline)));
    // Unsigned and broken are never "the same signer".
    QVERIFY(!pi::sameSigner(pi::Signature{}, pi::Signature{}));
    QVERIFY(!pi::sameSigner(signedBy(kRandy, kAzureEkus, day1),
                            signedBy(kRandy, kAzureEkus, day1, pi::SignatureState::Invalid)));
}

void ProgramIntegrityTests::policyAcceptsOnlyWhatTheRulesName() {
    const pi::Signature unsigned_exe;
    const pi::Signature signed_exe = signedBy(kRandy, kAzureEkus, QByteArray(32, '\x07'));
    const pi::Signature foreign =
        signedBy(QStringLiteral("CN=Contoso"), kAzureEkus, QByteArray(32, '\x08'));
    const pi::Signature broken{.state = pi::SignatureState::Invalid,
                               .subject = {},
                               .ekus = {},
                               .leaf_sha256 = {},
                               .detail = QStringLiteral("bad digest")};

    // (i) pinned path with its hash.
    const QStringList pinned = pi::pinnedPaths();
    QVERIFY2(pinned.contains(QStringLiteral("qt6core.dll")), "the manifest lost Qt6Core.dll");
    const QByteArray core_hash =
        pi::pinnedSha256(QStringLiteral("Qt6Core.dll")).value_or(QByteArray());
    QVERIFY(pi::refusalReason(facts(QStringLiteral("Qt6Core.dll"), unsigned_exe, core_hash),
                              unsigned_exe)
                .isEmpty());
    QVERIFY(!pi::refusalReason(
                 facts(QStringLiteral("Qt6Core.dll"), unsigned_exe, QByteArray(32, '\x00')),
                 unsigned_exe)
                 .isEmpty());
    // (ii) same signer as a signed exe; never a different signer.
    QVERIFY(pi::refusalReason(facts(QStringLiteral("z.dll"), signed_exe), signed_exe).isEmpty());
    QVERIFY(!pi::refusalReason(facts(QStringLiteral("z.dll"), foreign), signed_exe).isEmpty());
    // (iii) an unsigned build: the three first-party programs by name, nothing else unsigned.
    QVERIFY(pi::refusalReason(facts(QStringLiteral("sak_elevated_helper.exe"), unsigned_exe),
                              unsigned_exe)
                .isEmpty());
    QVERIFY(!pi::refusalReason(facts(QStringLiteral("version.dll"), unsigned_exe), unsigned_exe)
                 .isEmpty());
    // A signed program never accepts an unsigned helper (security brief 4d).
    QVERIFY(!pi::refusalReason(facts(QStringLiteral("sak_elevated_helper.exe"), unsigned_exe),
                               signed_exe)
                 .isEmpty());
    // A broken signature refuses whatever the name, and says so (the other rules would refuse it
    // too, but as "it isn't part of S.A.K.", which sends a technician looking for the wrong file).
    const QString broken_reason =
        pi::refusalReason(facts(QStringLiteral("sak_elevated_helper.exe"), broken), unsigned_exe);
    QVERIFY2(broken_reason.contains(QStringLiteral("signature is broken (bad digest)")),
             qPrintable(broken_reason));
    QVERIFY2(pi::refusalReason(facts(QStringLiteral("z.dll"), broken), signed_exe)
                 .contains(QStringLiteral("signature is broken")),
             "a broken signature next to a signed program");
    // The running program itself.
    pi::FileFacts self = facts(QStringLiteral("sak_utility.exe"), unsigned_exe);
    self.is_running_image = true;
    QVERIFY(pi::refusalReason(self, unsigned_exe).isEmpty());
}

void ProgramIntegrityTests::signaturesOfRealFiles() {
    const auto read = [](const QString& path) {
        HANDLE handle =
            CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                        GENERIC_READ,
                        FILE_SHARE_READ,
                        nullptr,
                        OPEN_EXISTING,
                        0,
                        nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            return pi::Signature{.state = pi::SignatureState::Invalid,
                                 .subject = {},
                                 .ekus = {},
                                 .leaf_sha256 = {},
                                 .detail = QStringLiteral("open failed")};
        }
        const pi::Signature signature = pi::readSignature(handle, path);
        CloseHandle(handle);
        return signature;
    };
    QCOMPARE(read(QCoreApplication::applicationFilePath()).state, pi::SignatureState::Unsigned);
    const pi::Signature qt = read(appFile(QStringLiteral("Qt6Core.dll")));
    QVERIFY2(qt.state == pi::SignatureState::Trusted ||
                 qt.state == pi::SignatureState::UntrustedChain,
             qPrintable(qt.detail));
    QVERIFY2(qt.subject.contains(QStringLiteral("Qt Company"), Qt::CaseInsensitive),
             qPrintable(qt.subject));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tampered = dir.filePath(QStringLiteral("Qt6Core.dll"));
    QVERIFY(QFile::copy(appFile(QStringLiteral("Qt6Core.dll")), tampered));
    QVERIFY(flipByteNearMiddle(tampered));
    QCOMPARE(read(tampered).state, pi::SignatureState::Invalid);
}

void ProgramIntegrityTests::cleanFolderIsHeldAndAPlantedDllRefused() {
    QTemporaryDir program;
    QVERIFY(program.isValid());
    const QString root = program.path();
    QVERIFY(copyInto(
        QCoreApplication::applicationFilePath(), root, QStringLiteral("sak_elevated_helper.exe")));
    QVERIFY(copyInto(appFile(QStringLiteral("Qt6Core.dll")), root, QStringLiteral("Qt6Core.dll")));
    QVERIFY(copyInto(appFile(QStringLiteral("platforms/qwindows.dll")),
                     root + "/platforms",
                     QStringLiteral("qwindows.dll")));
    // data\ is the person's, never loaded: not checked.
    QVERIFY(copyInto(appFile(QStringLiteral("z.dll")), root + "/data", QStringLiteral("x.dll")));
    {
        const auto hold = pi::ProgramHold::verify(root);
        QVERIFY2(hold,
                 qPrintable(hold ? QString() : hold.error().file + ": " + hold.error().reason));
        QCOMPARE(hold->heldFiles().size(), 3);
    }
    // A planted DLL with a name the loader looks for refuses the elevation and is named.
    QVERIFY(copyInto(appFile(QStringLiteral("z.dll")), root, QStringLiteral("version.dll")));
    const auto refused = pi::ProgramHold::verify(root);
    QVERIFY(!refused);
    QVERIFY2(refused.error().file.endsWith(QStringLiteral("version.dll"), Qt::CaseInsensitive),
             qPrintable(refused.error().file));
    QVERIFY(pi::refusalMessage(refused.error()).contains(QStringLiteral("isn't part of S.A.K.")));
}

void ProgramIntegrityTests::tamperedPinnedFileIsRefused() {
    QTemporaryDir program;
    QVERIFY(program.isValid());
    QVERIFY(copyInto(
        appFile(QStringLiteral("Qt6Core.dll")), program.path(), QStringLiteral("Qt6Core.dll")));
    QVERIFY(flipByteNearMiddle(QDir(program.path()).filePath(QStringLiteral("Qt6Core.dll"))));
    const auto refused = pi::ProgramHold::verify(program.path());
    QVERIFY(!refused);
    QVERIFY(refused.error().file.endsWith(QStringLiteral("Qt6Core.dll"), Qt::CaseInsensitive));
}

void ProgramIntegrityTests::junctionInsideTheProgramFolderIsRefused() {
    QTemporaryDir program;
    QTemporaryDir elsewhere;
    QVERIFY(program.isValid() && elsewhere.isValid());
    QVERIFY(copyInto(appFile(QStringLiteral("platforms/qwindows.dll")),
                     elsewhere.path(),
                     QStringLiteral("qwindows.dll")));
    QVERIFY(sak::test::createJunction(QDir(program.path()).filePath(QStringLiteral("platforms")),
                                      elsewhere.path()));
    QVERIFY(!pi::ProgramHold::verify(program.path()));
}

void ProgramIntegrityTests::heldFilesCannotBeChangedOrReplaced() {
    QTemporaryDir program;
    QVERIFY(program.isValid());
    const QString helper = QDir(program.path()).filePath(QStringLiteral("sak_elevated_helper.exe"));
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), helper));
    {
        const auto hold = pi::ProgramHold::verify(program.path());
        QVERIFY(hold);
        QFile writer(helper);
        QVERIFY2(!writer.open(QIODevice::ReadWrite), "a held program file could be written");
        QVERIFY2(!QFile::rename(helper, helper + QStringLiteral(".old")),
                 "a held program file could be renamed away");
        QVERIFY(!QFile::remove(helper));
    }
    QVERIFY(QFile::rename(helper, helper + QStringLiteral(".old")));
}

void ProgramIntegrityTests::moduleRule() {
    const QString program = QStringLiteral("E:\\SAK");
    const QStringList roots = {QStringLiteral("C:\\Windows"), QStringLiteral("C:\\Program Files")};
    const auto none = [](const QString&) {
        return false;
    };
    const auto all = [](const QString&) {
        return true;
    };
    QVERIFY(pi::moduleAllowed(
        QStringLiteral("C:\\Windows\\System32\\kernel32.dll"), program, none, roots));
    QVERIFY(
        pi::moduleAllowed(QStringLiteral("C:\\Program Files\\AV\\hook.dll"), program, none, roots));
    QVERIFY(!pi::moduleAllowed(
        QStringLiteral("C:\\ProgramData\\Somewhere\\evil.dll"), program, all, roots));
    QVERIFY(!pi::moduleAllowed(QStringLiteral("E:\\SAK\\version.dll"), program, none, roots));
    QVERIFY(pi::moduleAllowed(QStringLiteral("E:\\SAK\\Qt6Core.dll"), program, all, roots));
    // A sibling folder that only shares the prefix is not inside the program folder.
    QVERIFY(!pi::moduleAllowed(QStringLiteral("E:\\SAK-other\\x.dll"), program, all, roots));
}

void ProgramIntegrityTests::moduleLoadedFromElsewhereFailsTheAudit() {
    const auto clean = pi::auditLoadedModules(QCoreApplication::applicationDirPath());
    QVERIFY2(clean, qPrintable(clean ? QString() : clean.error()));
    QTemporaryDir elsewhere;
    QVERIFY(elsewhere.isValid());
    const QString stray = QDir(elsewhere.path()).filePath(QStringLiteral("stray.dll"));
    QVERIFY(QFile::copy(appFile(QStringLiteral("z.dll")), stray));
    HMODULE module =
        LoadLibraryExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(stray).utf16()),
                       nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    QVERIFY(module != nullptr);
    const auto audit = pi::auditLoadedModules(QCoreApplication::applicationDirPath());
    FreeLibrary(module);
    QVERIFY(!audit);
    QVERIFY2(audit.error().endsWith(QStringLiteral("stray.dll"), Qt::CaseInsensitive),
             qPrintable(audit.error()));
}

// The manifest must cover every DLL the build puts beside the program that is not first-party:
// each DLL in the build output folder (Qt, MSVC runtime, vcpkg) and every Qt plugin. The build
// folder's windeployqt run also copies the Windows SDK's shader compilers, which the package leaves
// out (scripts/deploy_qt_runtime.ps1: --no-system-d3d-compiler, --no-system-dxc-compiler).
void ProgramIntegrityTests::everyShippedFileTheBuildCopiesIsPinned() {
    const QDir output(QCoreApplication::applicationDirPath());
    const QStringList not_shipped = {QStringLiteral("dxcompiler.dll"), QStringLiteral("dxil.dll")};
    QStringList missing;
    for (const QString& name : output.entryList({QStringLiteral("*.dll")}, QDir::Files)) {
        if (!not_shipped.contains(name, Qt::CaseInsensitive) && !pi::pinnedSha256(name)) {
            missing.append(name);
        }
    }
    for (const QString& folder : {QStringLiteral("platforms"),
                                  QStringLiteral("styles"),
                                  QStringLiteral("imageformats"),
                                  QStringLiteral("iconengines"),
                                  QStringLiteral("tls")}) {
        for (const QString& name :
             QDir(output.filePath(folder)).entryList({QStringLiteral("*.dll")}, QDir::Files)) {
            if (!pi::pinnedSha256(folder + QLatin1Char('/') + name)) {
                missing.append(folder + QLatin1Char('/') + name);
            }
        }
    }
    QVERIFY2(missing.isEmpty(), qPrintable(missing.join(QStringLiteral(", "))));
}

// /DEPENDENTLOADFLAG:0x1000 (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) in each program that runs as
// administrator: Windows resolves its imports from its own folder and System32, never from PATH or
// the current folder. Read from each built program's load configuration, without running it.
void ProgramIntegrityTests::elevatedProgramsResolveImportsOnlyFromTheirFolderAndSystem32() {
    const QDir dir(QCoreApplication::applicationDirPath());
    for (const char* name : {"sak_utility.exe",
                             "sak_elevated_helper.exe",
                             "sak_apfs_writer_cli.exe",
                             "sak_hfs_writer_cli.exe"}) {
        const QString path = QDir::toNativeSeparators(dir.filePath(QString::fromLatin1(name)));
        HMODULE image = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                       nullptr,
                                       LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
        QVERIFY2(image != nullptr, qPrintable(path));
        const auto guard = qScopeGuard([image] { FreeLibrary(image); });
        auto* base = reinterpret_cast<unsigned char*>(reinterpret_cast<ULONG_PTR>(image) &
                                                      ~static_cast<ULONG_PTR>(3));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY entry =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
        QVERIFY2(entry.VirtualAddress != 0, name);
        const auto* config =
            reinterpret_cast<const IMAGE_LOAD_CONFIG_DIRECTORY64*>(base + entry.VirtualAddress);
        QVERIFY2(config->Size >= offsetof(IMAGE_LOAD_CONFIG_DIRECTORY64, DependentLoadFlags) +
                                     sizeof(config->DependentLoadFlags),
                 name);
        QVERIFY2(config->DependentLoadFlags == LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, name);
    }
}

QTEST_GUILESS_MAIN(ProgramIntegrityTests)
#include "test_program_integrity.moc"
