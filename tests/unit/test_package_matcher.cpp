// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_package_matcher.cpp
/// @brief Unit tests for matching installed applications to Chocolatey packages

#include "sak/package_matcher.h"

#include <QtTest/QtTest>

#include <vector>

class PackageMatcherTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Mapping management
    void addMapping_isWhatFindMatchFinds();
    void getMappingCount_afterAdd();

    // findMatch
    void findMatch_exactMapping();
    void findMatch_caseInsensitive();
    void findMatch_noResult();
    void findMatch_realDisplayNames_data();
    void findMatch_realDisplayNames();
    void normalizeAppName_data();
    void normalizeAppName();

    // applyMatches
    void applyMatches_recordsTheTableMatchOnADesktopApp();
    void applyMatches_leavesAnUnknownAppUnmatched();
    void applyMatches_keepsTheIdChocolateyReported();
    void applyMatches_dropsChocolateysOwnPackage();
    void applyMatches_leavesStoreAppsUnmatched();

private:
    static sak::AppScanner::AppInfo makeAppInfo(const QString& name);
};

sak::AppScanner::AppInfo PackageMatcherTests::makeAppInfo(const QString& name) {
    sak::AppScanner::AppInfo info;
    info.name = name;
    return info;
}

// ============================================================================
// Mapping Management
// ============================================================================

void PackageMatcherTests::addMapping_isWhatFindMatchFinds() {
    sak::PackageMatcher matcher;
    QVERIFY(!matcher.findMatch(makeAppInfo("Test App")).has_value());
    matcher.addMapping("Test App", "test-app-choco");
    const auto result = matcher.findMatch(makeAppInfo("Test App"));
    QVERIFY(result.has_value());
    QCOMPARE(result->choco_package, QString("test-app-choco"));
}

void PackageMatcherTests::getMappingCount_afterAdd() {
    sak::PackageMatcher matcher;
    int initial = matcher.getMappingCount();

    matcher.addMapping("NewApp1", "new-app-1");
    matcher.addMapping("NewApp2", "new-app-2");

    QCOMPARE(matcher.getMappingCount(), initial + 2);
}

// ============================================================================
// findMatch Tests
// ============================================================================

void PackageMatcherTests::findMatch_exactMapping() {
    sak::PackageMatcher matcher;
    matcher.addMapping("My Test Application", "my-test-app");

    auto result = matcher.findMatch(makeAppInfo("My Test Application"));

    QVERIFY(result.has_value());
    QCOMPARE(result->choco_package, QString("my-test-app"));
    QCOMPARE(result->match_type, QString("exact"));
    QCOMPARE(result->matched_name, QString("My Test"));
    QVERIFY(result->available);
    // Direct exact-mapping hit returns confidence 1.0 (not the 0.95 case-insensitive path).
    QCOMPARE(result->confidence, 1.0);
}

void PackageMatcherTests::findMatch_caseInsensitive() {
    sak::PackageMatcher matcher;

    auto result = matcher.findMatch(makeAppInfo("Paint.NET"));

    QVERIFY(result.has_value());
    QCOMPARE(result->choco_package, QString("paint.net"));
    QCOMPARE(result->confidence, 0.95);
}

void PackageMatcherTests::findMatch_noResult() {
    sak::PackageMatcher matcher;
    QVERIFY(!matcher.findMatch(makeAppInfo("CompletelyUnknownApp_xyz_99999")).has_value());
}

void PackageMatcherTests::findMatch_realDisplayNames_data() {
    QTest::addColumn<QString>("display_name");
    QTest::addColumn<QString>("expected_package");

    // Taken from a real PC's Uninstall keys. An empty expectation means no match is correct.
    const auto row = [](const char* name, const char* package) {
        QTest::newRow(name) << QString::fromLatin1(name) << QString::fromLatin1(package);
    };
    row("7-Zip 22.01 (x64)", "7zip");
    row("Mozilla Firefox (x64 en-US)", "firefox");
    row("Mozilla Thunderbird (x64 en-US)", "thunderbird");
    row("Notepad++ (32-bit x86)", "notepadplusplus");
    row("Microsoft Visual Studio Code (User)", "vscode");
    row("PuTTY release 0.81 (64-bit)", "putty");
    row("Oracle VirtualBox 7.2.8", "virtualbox");
    row("OpenVPN 2.7.5-I001 amd64", "openvpn");
    row("PowerShell 7.6.9.0-x64", "powershell-core");
    row("Python 3.14.10 (64-bit)", "python");
    row("Adobe Acrobat (64-bit)", "adobereader");
    row("Docker Desktop", "docker-desktop");
    row("GitHub Desktop", "github-desktop");
    row("GitHub CLI", "gh");
    row("FileZilla 3.62.2", "filezilla");
    row("CPUID CPU-Z 2.09", "cpu-z");
    row("Google Chrome", "googlechrome");
    row("VLC media player", "vlc");
    row("Java 8 Update 381 (64-bit)", "javaruntime");

    // Components and look-alikes that must not take the product's package.
    row("Python 3.13.16 Documentation (64-bit)", "");
    row("Python 3.13.16 Core Interpreter (64-bit)", "");
    row("Python Launcher", "");
    row("Microsoft Edge WebView2 Runtime", "");
    row("Microsoft Teams Meeting Add-in for Microsoft Office", "");
    row("Git Extensions", "");
    row("Visual Studio Code Insiders", "");
    row("(x64)", "");
}

void PackageMatcherTests::findMatch_realDisplayNames() {
    QFETCH(QString, display_name);
    QFETCH(QString, expected_package);

    const sak::PackageMatcher matcher;
    const auto result = matcher.findMatch(makeAppInfo(display_name));

    if (expected_package.isEmpty()) {
        QVERIFY2(!result.has_value(), qPrintable(result ? result->choco_package : QString()));
        return;
    }
    QVERIFY(result.has_value());
    QCOMPARE(result->choco_package, expected_package);
}

void PackageMatcherTests::normalizeAppName_data() {
    QTest::addColumn<QString>("raw");
    QTest::addColumn<QString>("normalized");

    QTest::newRow("bracketed architecture") << "7-Zip 22.01 (x64)" << "7-Zip";
    QTest::newRow("hyphen in the product survives") << "7-Zip" << "7-Zip";
    QTest::newRow("bracketed arch and locale") << "Mozilla Firefox (x64 en-US)"
                                               << "Mozilla Firefox";
    QTest::newRow("version word") << "Tickr version 0.7.1" << "Tickr";
    QTest::newRow("release word") << "PuTTY release 0.81 (64-bit)" << "PuTTY";
    QTest::newRow("version with a suffix") << "OpenVPN 2.7.5-I001 amd64" << "OpenVPN";
    QTest::newRow("hyphen before arch") << "PowerShell 7-x64" << "PowerShell";
    QTest::newRow("packaging word") << "Telegram Desktop 5.0.1" << "Telegram";
    QTest::newRow("word after a version stays") << "Ableton Live 11 Lite" << "Ableton Live 11 Lite";
    QTest::newRow("trailing space")
        << "Magic The Gathering Online " << "Magic The Gathering Online";
    QTest::newRow("only build detail") << "(x64)" << "";
    QTest::newRow("empty") << "" << "";
}

void PackageMatcherTests::normalizeAppName() {
    QFETCH(QString, raw);
    QFETCH(QString, normalized);
    QCOMPARE(sak::PackageMatcher::normalizeAppName(raw), normalized);
}

// ============================================================================
// applyMatches
// ============================================================================

void PackageMatcherTests::applyMatches_recordsTheTableMatchOnADesktopApp() {
    std::vector<sak::AppScanner::AppInfo> apps{makeAppInfo("7-Zip 22.01 (x64)")};

    const sak::PackageMatcher matcher;
    matcher.applyMatches(apps);

    QCOMPARE(apps[0].choco_package, QString("7zip"));
    QVERIFY(apps[0].choco_available);
    QCOMPARE(apps[0].match_confidence, sak::AppScanner::AppInfo::Confidence::High);
}

void PackageMatcherTests::applyMatches_leavesAnUnknownAppUnmatched() {
    std::vector<sak::AppScanner::AppInfo> apps{makeAppInfo("Solar Fire v9")};

    const sak::PackageMatcher matcher;
    matcher.applyMatches(apps);

    QVERIFY(apps[0].choco_package.isEmpty());
    QVERIFY(!apps[0].choco_available);
    QCOMPARE(apps[0].match_confidence, sak::AppScanner::AppInfo::Confidence::Unknown);
}

void PackageMatcherTests::applyMatches_keepsTheIdChocolateyReported() {
    auto app = makeAppInfo("ripgrep");
    app.source = sak::AppScanner::AppInfo::Source::Chocolatey;
    app.choco_package = "ripgrep";
    app.choco_available = true;
    std::vector<sak::AppScanner::AppInfo> apps{app};

    const sak::PackageMatcher matcher;
    matcher.applyMatches(apps);

    QCOMPARE(apps[0].choco_package, QString("ripgrep"));
    QVERIFY(apps[0].choco_available);
    QCOMPARE(apps[0].match_confidence, sak::AppScanner::AppInfo::Confidence::High);
}

// `choco list` always reports Chocolatey itself. Offering it would "install" the bundled tool.
void PackageMatcherTests::applyMatches_dropsChocolateysOwnPackage() {
    auto app = makeAppInfo("chocolatey");
    app.source = sak::AppScanner::AppInfo::Source::Chocolatey;
    app.choco_package = "chocolatey";
    app.choco_available = true;
    std::vector<sak::AppScanner::AppInfo> apps{app};

    const sak::PackageMatcher matcher;
    matcher.applyMatches(apps);

    QVERIFY(apps[0].choco_package.isEmpty());
    QVERIFY(!apps[0].choco_available);
}

void PackageMatcherTests::applyMatches_leavesStoreAppsUnmatched() {
    // A Store package named exactly like a table entry still does not install through Chocolatey.
    auto app = makeAppInfo("Spotify");
    app.source = sak::AppScanner::AppInfo::Source::AppX;
    std::vector<sak::AppScanner::AppInfo> apps{app};

    const sak::PackageMatcher matcher;
    matcher.applyMatches(apps);

    QVERIFY(apps[0].choco_package.isEmpty());
}

QTEST_GUILESS_MAIN(PackageMatcherTests)
#include "test_package_matcher.moc"
