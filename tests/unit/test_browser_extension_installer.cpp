// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Exercises BrowserExtensionInstaller against a THROWAWAY HKCU subkey
// (Software\SAK_Test\ExtInstaller) and temp dirs -- never the user's real Chrome
// policy. Proves: force-install entry + native host written, foreign policy entries
// preserved, idempotency, uninstall removes only ours, fail-closed on a missing CRX,
// and that the pinned key/id/version in the committed manifest are self-consistent.

#include "sak/win32mcp/browser_extension_installer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QUrl>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace sak::win32mcp;

namespace {

#if defined(_WIN32)
const QString kTestBase = QStringLiteral("Software\\SAK_Test\\ExtInstaller");

std::wstring toW(const QString& s) {
    return s.toStdWString();
}

void nukeTestTree() {
    RegDeleteTreeW(HKEY_CURRENT_USER, toW(kTestBase).c_str());
}

// Read a REG_SZ ("" name -> default) from an HKCU subkey; empty QString if absent.
QString readReg(const QString& subkey, const QString& name) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, toW(subkey).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return {};
    }
    QString result;
    DWORD type = 0, bytes = 0;
    const std::wstring nameHolder = toW(name);
    const wchar_t* n = name.isEmpty() ? nullptr : nameHolder.c_str();
    if (RegQueryValueExW(h, n, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, L'\0');
        DWORD out = bytes;
        if (RegQueryValueExW(h, n, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &out) ==
            ERROR_SUCCESS) {
            result = QString::fromWCharArray(buf.data());
        }
    }
    RegCloseKey(h);
    return result;
}

// All (name -> data) string values under an HKCU subkey.
QMap<QString, QString> enumReg(const QString& subkey) {
    QMap<QString, QString> out;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, toW(subkey).c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) {
        return out;
    }
    for (DWORD i = 0;; ++i) {
        wchar_t nameBuf[512];
        DWORD nameLen = static_cast<DWORD>(std::size(nameBuf));
        LSTATUS s = RegEnumValueW(h, i, nameBuf, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (s != ERROR_SUCCESS && s != ERROR_MORE_DATA) {
            break;
        }
        const QString name = QString::fromWCharArray(nameBuf, static_cast<int>(nameLen));
        out.insert(name, readReg(subkey, name));
    }
    RegCloseKey(h);
    return out;
}

void seedReg(const QString& subkey, const QString& name, const QString& value) {
    HKEY h = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER,
                    toW(subkey).c_str(),
                    0,
                    nullptr,
                    REG_OPTION_NON_VOLATILE,
                    KEY_SET_VALUE,
                    nullptr,
                    &h,
                    nullptr);
    if (h) {
        const std::wstring w = toW(value);
        RegSetValueExW(h,
                       toW(name).c_str(),
                       0,
                       REG_SZ,
                       reinterpret_cast<const BYTE*>(w.c_str()),
                       static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
    }
}
#endif  // _WIN32

}  // namespace

class BrowserExtensionInstallerTest : public QObject {
    Q_OBJECT

#if defined(_WIN32)
    QString forcelistKey() const { return kTestBase + QStringLiteral("\\Forcelist"); }
    QString hostKey() const {
        return kTestBase + QStringLiteral("\\NativeHost\\") + QString::fromLatin1(kNativeHostName);
    }

    // A config wired entirely to throwaway locations.
    ExtensionInstallConfig testConfig(const QString& dataDir,
                                      const QString& crxPath,
                                      const QString& hostExe) const {
        ExtensionInstallConfig c;
        c.data_dir = dataDir;
        c.crx_path = crxPath;
        c.host_exe_path = hostExe;
        c.forcelist_key_path = forcelistKey();
        c.native_host_key_path = hostKey();
        return c;
    }

    static QString makeFile(const QString& path, const QByteArray& bytes) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            f.write(bytes);
            f.close();
        }
        return path;
    }
#endif

private slots:
    void init() {
#if defined(_WIN32)
        nukeTestTree();
#endif
    }
    void cleanup() {
#if defined(_WIN32)
        nukeTestTree();
#endif
    }

    // The committed manifest's public key must hash to the pinned id, and its version
    // must equal the pinned version (guards against silent drift of key/id/version).
    void pinnedIdentityMatchesManifest() {
        const QString manifestPath = QStringLiteral(SAK_REPO_ROOT) +
                                     QStringLiteral("/browser/extension/manifest.json");
        QFile f(manifestPath);
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(manifestPath));
        const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
        const QString keyB64 = obj.value(QStringLiteral("key")).toString();
        QVERIFY2(!keyB64.isEmpty(), "manifest.json is missing the pinned \"key\"");

        const QByteArray der = QByteArray::fromBase64(keyB64.toLatin1());
        const QByteArray hash = QCryptographicHash::hash(der, QCryptographicHash::Sha256);
        QString id;
        for (int i = 0; i < 16; ++i) {
            id.append(QChar('a' + ((static_cast<quint8>(hash[i]) >> 4) & 0xF)));
            id.append(QChar('a' + (static_cast<quint8>(hash[i]) & 0xF)));
        }
        QCOMPARE(id, QString::fromLatin1(kBrowserExtensionId));
        QCOMPARE(obj.value(QStringLiteral("version")).toString(),
                 QString::fromLatin1(kBrowserExtensionVersion));
    }

// moc does not define the compiler builtin _WIN32, so a _WIN32 guard here would hide
// these slots from moc and they would never run. SAK_WIN_REGISTRY_TESTS is a target
// compile definition (set on Windows in tests/CMakeLists.txt) that AUTOMOC forwards to
// moc, so the slots register on Windows and compile out elsewhere.
#if defined(SAK_WIN_REGISTRY_TESTS)
    void installWritesPolicyAndHost() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString crx = makeFile(dir.filePath("sak_browser_control.crx"), "CRX-BYTES");
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));

        const ExtensionInstallResult r = inst.install();
        QVERIFY2(r.ok, qPrintable(r.summary + " / " + r.detail));

        // Force-install policy: exactly one value, data "<id>;file:///.../update.xml".
        const auto values = enumReg(forcelistKey());
        QCOMPARE(values.size(), 1);
        const QString data = values.first();
        QCOMPARE(data,
                 QString::fromLatin1(kBrowserExtensionId) + QStringLiteral(";") +
                     QUrl::fromLocalFile(
                         QDir(dir.path()).filePath(QStringLiteral("browser_ext_update.xml")))
                         .toString(QUrl::FullyEncoded));

        // Native host default value -> the host manifest path.
        const QString hostManifest = readReg(hostKey(), QString());
        QCOMPARE(hostManifest,
                 QDir(dir.path())
                     .filePath(QString::fromLatin1(kNativeHostName) + QStringLiteral(".json")));

        // update.xml carries the CRX codebase + version.
        QFile ux(QDir(dir.path()).filePath("browser_ext_update.xml"));
        QVERIFY(ux.open(QIODevice::ReadOnly));
        const QString xml = QString::fromUtf8(ux.readAll());
        QVERIFY(xml.contains(QString::fromLatin1(kBrowserExtensionId)));
        QVERIFY(xml.contains(QString::fromLatin1(kBrowserExtensionVersion)));
        QVERIFY(xml.contains(QStringLiteral("codebase='") +
                             QUrl::fromLocalFile(crx).toString(QUrl::FullyEncoded) +
                             QStringLiteral("'")));

        // Host manifest allows exactly our extension origin.
        QFile hm(hostManifest);
        QVERIFY(hm.open(QIODevice::ReadOnly));
        const QJsonObject host = QJsonDocument::fromJson(hm.readAll()).object();
        QCOMPARE(host.value("allowed_origins").toArray().first().toString(),
                 QStringLiteral("chrome-extension://") + QString::fromLatin1(kBrowserExtensionId) +
                     QStringLiteral("/"));

        QCOMPARE(inst.stateString(), QStringLiteral("installed"));
    }

    void installFailsClosedWhenCrxMissing() {
        QTemporaryDir dir;
        const QString crx = dir.filePath("does_not_exist.crx");  // never created
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));

        const ExtensionInstallResult r = inst.install();
        QVERIFY(!r.ok);
        QCOMPARE(r.summary, QStringLiteral("Extension package missing"));
        // No policy entry written when the package is missing.
        QVERIFY(enumReg(forcelistKey()).isEmpty());
        QCOMPARE(inst.stateString(), QStringLiteral("not_installed"));
    }

    void installFailsClosedWhenCrxIsDirectory() {
        // F17: crxPresent() must require a regular FILE. A directory whose name matches the CRX
        // must NOT be accepted as the package (a force-install policy pointing at a directory is
        // invalid), so install() fails closed and writes no policy entry.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString crxDir = dir.filePath("sak_browser_control.crx");
        QVERIFY(QDir().mkpath(crxDir));  // a directory, not a file
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        BrowserExtensionInstaller inst(testConfig(dir.path(), crxDir, exe));

        QVERIFY(!inst.crxPresent());
        const ExtensionInstallResult r = inst.install();
        QVERIFY(!r.ok);
        QCOMPARE(r.summary, QStringLiteral("Extension package missing"));
        QVERIFY(enumReg(forcelistKey()).isEmpty());
    }

    void installPreservesForeignPolicyEntries() {
        QTemporaryDir dir;
        const QString crx = makeFile(dir.filePath("sak_browser_control.crx"), "CRX");
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        seedReg(forcelistKey(),
                "1",
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;https://clients2.google.com/x");

        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));
        QVERIFY(inst.install().ok);

        const auto values = enumReg(forcelistKey());
        QCOMPARE(values.size(), 2);
        QCOMPARE(values.value("1"),
                 QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;https://clients2.google.com/x"));
        QCOMPARE(values.value(QStringLiteral("2")),
                 QString::fromLatin1(kBrowserExtensionId) + QStringLiteral(";") +
                     QUrl::fromLocalFile(
                         QDir(dir.path()).filePath(QStringLiteral("browser_ext_update.xml")))
                         .toString(QUrl::FullyEncoded));
    }

    void installIsIdempotent() {
        QTemporaryDir dir;
        const QString crx = makeFile(dir.filePath("sak_browser_control.crx"), "CRX");
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));

        QVERIFY(inst.install().ok);
        QVERIFY(inst.install().ok);  // second install must not add a duplicate slot
        QCOMPARE(enumReg(forcelistKey()).size(), 1);
    }

    void uninstallRemovesOnlyOurs() {
        QTemporaryDir dir;
        const QString crx = makeFile(dir.filePath("sak_browser_control.crx"), "CRX");
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        seedReg(forcelistKey(), "1", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb;https://example/upd");
        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));
        QVERIFY(inst.install().ok);

        const ExtensionInstallResult r = inst.uninstall();
        QVERIFY(r.ok);

        const auto values = enumReg(forcelistKey());
        QCOMPARE(values.size(), 1);  // foreign entry survives
        QCOMPARE(values.value("1"),
                 QStringLiteral("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb;https://example/upd"));
        QVERIFY(readReg(hostKey(), QString()).isEmpty());  // native host key gone
        QCOMPARE(inst.stateString(), QStringLiteral("not_installed"));
    }

    void uninstallReportsFailureWhenNativeHostCannotBeRemoved() {
        // B13-03: a failed registry removal must surface as ok=false, not a silent success that
        // leaves the force-install policy / native host in place.
        QTemporaryDir dir;
        const QString crx = makeFile(dir.filePath("sak_browser_control.crx"), "CRX");
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));
        QVERIFY(inst.install().ok);
        // A subkey under the native host key makes RegDeleteKeyExW -- which cannot delete a key
        // that still has subkeys -- fail, so the host key remains after uninstall.
        seedReg(hostKey() + QStringLiteral("\\pinned"), QString(), QStringLiteral("x"));
        const ExtensionInstallResult r = inst.uninstall();
        QVERIFY(!r.ok);
        QCOMPARE(r.summary,
                 QStringLiteral("Browser extension uninstall incomplete: the force-install "
                                "policy or native host could not be fully removed"));
    }

    void statePartialWhenOnlyHostPresent() {
        QTemporaryDir dir;
        const QString crx = makeFile(dir.filePath("sak_browser_control.crx"), "CRX");
        const QString exe = makeFile(dir.filePath("sak_win32_mcp.exe"), "EXE");
        seedReg(hostKey(), QString(), "C:/somewhere/host.json");  // host only, no policy
        BrowserExtensionInstaller inst(testConfig(dir.path(), crx, exe));
        QCOMPARE(inst.stateString(), QStringLiteral("partial"));
    }
#endif  // SAK_WIN_REGISTRY_TESTS
};

QTEST_MAIN(BrowserExtensionInstallerTest)
#include "test_browser_extension_installer.moc"
