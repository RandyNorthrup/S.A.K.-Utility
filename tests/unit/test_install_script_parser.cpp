// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_install_script_parser.cpp
/// @brief Unit tests for InstallScriptParser

#include "sak/install_script_parser.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestInstallScriptParser : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Construction
    void defaultConstruction();

    // Install-ChocolateyPackage parsing
    void parse_installChocolateyPackage_extractsUrl();
    void parse_installChocolateyPackage_extractsUrl64();
    void parse_installChocolateyPackage_extractsChecksum();
    void parse_installChocolateyPackage_extractsPackageType();

    // Install-ChocolateyZipPackage parsing
    void parse_installChocolateyZipPackage_extractsUrl();

    // Get-ChocolateyWebFile parsing
    void parse_getChocolateyWebFile_extractsUrl();

    // Splatting pattern parsing
    void parse_splatting_extractsResources();
    void parse_splatting_detectsFlag();

    // Variable resolution
    void parse_variableReference_resolved();

    // Edge cases
    void parse_emptyScript_returnsEmpty();
    void parse_noDownloads_returnsEmpty();
    void parse_multipleResources_allExtracted();

    // File parsing
    void parseFile_validFile_parsesContent();
    void parseFile_nonexistentFile_returnsWarning();

    // Silent args extraction
    void parse_silentArgs_extracted();
};

// ============================================================================
// Construction
// ============================================================================

void TestInstallScriptParser::defaultConstruction() {
    sak::InstallScriptParser parser;
    auto result = parser.parse(QString());
    QVERIFY(result.resources.isEmpty());
    QVERIFY(result.original_script.isEmpty());
}

// ============================================================================
// Install-ChocolateyPackage
// ============================================================================

void TestInstallScriptParser::parse_installChocolateyPackage_extractsUrl() {
    sak::InstallScriptParser parser;
    QString script = R"(
$packageName = 'testpkg'
Install-ChocolateyPackage -PackageName $packageName `
    -FileType 'exe' `
    -Url 'https://example.com/setup.exe' `
    -SilentArgs '/S'
)";
    auto result = parser.parse(script);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/setup.exe"));
}

void TestInstallScriptParser::parse_installChocolateyPackage_extractsUrl64() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/setup-x86.exe' `
    -Url64bit 'https://example.com/setup-x64.exe' `
    -SilentArgs '/S'
)";
    auto result = parser.parse(script);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/setup-x86.exe"));
    QCOMPARE(result.resources.first().url_64bit, QString("https://example.com/setup-x64.exe"));
}

void TestInstallScriptParser::parse_installChocolateyPackage_extractsChecksum() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'msi' `
    -Url 'https://example.com/setup.msi' `
    -Checksum 'abc123def456' `
    -ChecksumType 'sha256'
)";
    auto result = parser.parse(script);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().checksum, QString("abc123def456"));
    QCOMPARE(result.resources.first().checksum_type, QString("sha256"));
}

void TestInstallScriptParser::parse_installChocolateyPackage_extractsPackageType() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'msi' `
    -Url 'https://example.com/setup.msi'
)";
    auto result = parser.parse(script);
    QCOMPARE(result.package_type, QString("msi"));
}

// ============================================================================
// Install-ChocolateyZipPackage
// ============================================================================

void TestInstallScriptParser::parse_installChocolateyZipPackage_extractsUrl() {
    sak::InstallScriptParser parser;
    QString script = R"ps1(
Install-ChocolateyZipPackage -PackageName 'testpkg' `
    -Url 'https://example.com/archive.zip' `
    -UnzipLocation "$(Split-Path -parent $MyInvocation.MyCommand.Definition)"
)ps1";
    auto result = parser.parse(script);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/archive.zip"));
}

// ============================================================================
// Get-ChocolateyWebFile
// ============================================================================

void TestInstallScriptParser::parse_getChocolateyWebFile_extractsUrl() {
    sak::InstallScriptParser parser;
    QString script = R"(
Get-ChocolateyWebFile -PackageName 'testpkg' `
    -FileFullPath "$toolsDir\setup.exe" `
    -Url 'https://example.com/binary.exe'
)";
    auto result = parser.parse(script);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/binary.exe"));
}

// ============================================================================
// Splatting
// ============================================================================

void TestInstallScriptParser::parse_splatting_extractsResources() {
    sak::InstallScriptParser parser;
    QString script = R"(
$packageArgs = @{
    packageName    = 'testpkg'
    fileType       = 'exe'
    url            = 'https://example.com/setup.exe'
    url64bit       = 'https://example.com/setup-x64.exe'
    checksum       = 'aabbccdd'
    checksumType   = 'sha256'
    silentArgs     = '/VERYSILENT /NORESTART'
}
Install-ChocolateyPackage @packageArgs
)";
    auto result = parser.parse(script);
    QVERIFY(result.uses_splatting);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/setup.exe"));
    QCOMPARE(result.resources.first().url_64bit, QString("https://example.com/setup-x64.exe"));
}

void TestInstallScriptParser::parse_splatting_detectsFlag() {
    sak::InstallScriptParser parser;
    QString script = R"(
$packageArgs = @{
    packageName = 'pkg'
    url         = 'https://example.com/test.exe'
}
Install-ChocolateyPackage @packageArgs
)";
    auto result = parser.parse(script);
    QVERIFY(result.uses_splatting);
}

// ============================================================================
// Variable Resolution
// ============================================================================

void TestInstallScriptParser::parse_variableReference_resolved() {
    sak::InstallScriptParser parser;
    QString script = R"(
$downloadUrl = 'https://example.com/resolved.exe'
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url $downloadUrl
)";
    auto result = parser.parse(script);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/resolved.exe"));
}

// ============================================================================
// Edge Cases
// ============================================================================

void TestInstallScriptParser::parse_emptyScript_returnsEmpty() {
    sak::InstallScriptParser parser;
    auto result = parser.parse(QString());
    QVERIFY(result.resources.isEmpty());
}

void TestInstallScriptParser::parse_noDownloads_returnsEmpty() {
    sak::InstallScriptParser parser;
    QString script = R"(
# This script has no download URLs
Write-Host "Hello World"
$path = Get-Location
)";
    auto result = parser.parse(script);
    QVERIFY(result.resources.isEmpty());
}

void TestInstallScriptParser::parse_multipleResources_allExtracted() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'pkg1' `
    -FileType 'exe' `
    -Url 'https://example.com/installer1.exe'

Get-ChocolateyWebFile -PackageName 'pkg1' `
    -FileFullPath "$toolsDir\extra.dll" `
    -Url 'https://example.com/extra.dll'
)";
    auto result = parser.parse(script);
    QVERIFY(result.resources.size() >= 2);
}

// ============================================================================
// File Parsing
// ============================================================================

void TestInstallScriptParser::parseFile_validFile_parsesContent() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    QString file_path = temp_dir.path() + "/chocolateyInstall.ps1";
    QFile file(file_path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write(R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/fromfile.exe'
)");
    file.close();

    sak::InstallScriptParser parser;
    auto result = parser.parseFile(file_path);
    QVERIFY(!result.resources.isEmpty());
    QCOMPARE(result.resources.first().url, QString("https://example.com/fromfile.exe"));
}

void TestInstallScriptParser::parseFile_nonexistentFile_returnsWarning() {
    sak::InstallScriptParser parser;
    auto result = parser.parseFile("C:/nonexistent/path/script.ps1");
    QVERIFY(result.resources.isEmpty());
    QVERIFY(!result.warnings.isEmpty());
}

// ============================================================================
// Silent Args
// ============================================================================

void TestInstallScriptParser::parse_silentArgs_extracted() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/setup.exe' `
    -SilentArgs '/S /NORESTART'
)";
    auto result = parser.parse(script);
    QCOMPARE(result.silent_args, QString("/S /NORESTART"));
}

QTEST_GUILESS_MAIN(TestInstallScriptParser)
#include "test_install_script_parser.moc"
