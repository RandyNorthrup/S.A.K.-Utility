// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_iso_analyzer.cpp
/// @brief Unit tests for IsoAnalyzer classification helpers.

#include "sak/iso_analyzer.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QtTest/QtTest>

#include <algorithm>

using namespace sak;

namespace {
bool nameContainsAny(const QString& name, const QStringList& tokens) {
    return std::any_of(tokens.cbegin(), tokens.cend(), [&name](const QString& token) {
        return name.contains(token);
    });
}
}  // namespace

class IsoAnalyzerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void windowsFamilyIsInstallMedia();
    void windowsEditionsImplyInstallMedia();
    void linuxServerIsoIsNotWindows();
    void unknownOrEmptyIsNotWindows();
    void realIsosClassifyByContent();
};

// os_family == "Windows" (from the volume label / application ID) routes to the
// Windows install USB path.
void IsoAnalyzerTests::windowsFamilyIsInstallMedia() {
    IsoInfo info;
    info.os_family = QStringLiteral("Windows");
    QVERIFY(IsoAnalyzer::isWindowsInstallMedia(info));
}

// Editions parsed from install.wim/esd metadata are a Windows-media signal even
// if os_family were somehow unset.
void IsoAnalyzerTests::windowsEditionsImplyInstallMedia() {
    IsoInfo info;
    info.windows_editions = {QStringLiteral("Windows 11 Pro")};
    QVERIFY(IsoAnalyzer::isWindowsInstallMedia(info));
}

// P12-09: a Linux server ISO (family "Linux") must NOT be treated as Windows --
// the old filename heuristic matched "server" and mis-routed it.
void IsoAnalyzerTests::linuxServerIsoIsNotWindows() {
    IsoInfo info;
    info.os_family = QStringLiteral("Linux");
    info.distro_name = QStringLiteral("Ubuntu");
    info.volume_label = QStringLiteral("Ubuntu-Server 24.04.1 LTS amd64");
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

void IsoAnalyzerTests::unknownOrEmptyIsNotWindows() {
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(IsoInfo{}));
    IsoInfo unknown;
    unknown.os_family = QStringLiteral("Unknown");
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(unknown));
}

// Live end-to-end check against real images: set SAK_TEST_ISO_DIR to a folder of
// .iso files. Skipped in CI (env unset). Each Linux image must classify as
// not-Windows and each Windows image as Windows -- from content, not filename.
void IsoAnalyzerTests::realIsosClassifyByContent() {
    const QByteArray env = qgetenv("SAK_TEST_ISO_DIR");
    if (env.isEmpty()) {
        QSKIP("SAK_TEST_ISO_DIR not set; skipping real-ISO classification");
    }
    const QFileInfoList isos =
        QDir(QString::fromLocal8Bit(env)).entryInfoList({QStringLiteral("*.iso")}, QDir::Files);
    if (isos.isEmpty()) {
        QSKIP("no .iso files in SAK_TEST_ISO_DIR");
    }
    for (const QFileInfo& file : isos) {
        const IsoInfo info = IsoAnalyzer::analyze(file.absoluteFilePath());
        const bool is_windows = IsoAnalyzer::isWindowsInstallMedia(info);
        qInfo().noquote() << file.fileName() << "os_family=" << info.os_family
                          << "editions=" << info.windows_editions.size()
                          << "-> windows=" << is_windows;
        const QString lname = file.fileName().toLower();
        if (nameContainsAny(lname, {"arch", "ubuntu", "debian", "fedora", "mint", "linux"})) {
            QVERIFY2(!is_windows,
                     qPrintable("Linux ISO misclassified as Windows: " + file.fileName()));
        }
        if (nameContainsAny(lname, {"win11", "win10", "windows"})) {
            QVERIFY2(is_windows, qPrintable("Windows ISO not detected: " + file.fileName()));
        }
    }
}

QTEST_MAIN(IsoAnalyzerTests)
#include "test_iso_analyzer.moc"
