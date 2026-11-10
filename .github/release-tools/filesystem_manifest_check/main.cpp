// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file main.cpp
/// @brief Release (T3) check: the application's own filesystem tool manifest validation, run
/// against a packaged tree.
///
/// The application refuses a bundled filesystem tool whose bytes differ from the SHA-256 that
/// tools/filesystem/manifest.json pins, and release signing changes the bytes of every unsigned
/// tool it signs. This program compiles src/core/partition_file_system_tool_manifest.cpp, the
/// code the application runs, and asks it about a package, so a package whose tools the app would
/// refuse cannot be published. Built only by .github/workflows/release.yml; it never ships.

#include "sak/partition_file_system_tool_manifest.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

constexpr int kExitValid = 0;
constexpr int kExitRefused = 1;
constexpr int kExitUsage = 2;
constexpr qsizetype kExpectedArgumentCount = 2;

void printLine(std::FILE* stream, const QString& text) {
    const QByteArray bytes = text.toUtf8();
    std::fputs(bytes.constData(), stream);
    std::fputc('\n', stream);
}

}  // namespace

int main(int argc, char* argv[]) {
    const QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() != kExpectedArgumentCount) {
        printLine(stderr, QStringLiteral("usage: sak_filesystem_manifest_check <package root>"));
        return kExitUsage;
    }

    // The application reads the manifest beside sak_utility.exe and passes the manifest's own
    // folder as the tools root (resolveRuntimeManifestPath and its callers); do the same here.
    const QString manifestPath = sak::PartitionFileSystemToolManifest::defaultRuntimeManifestPath(
        QDir(arguments.at(1)).absolutePath());
    const QString toolsRoot = QFileInfo(manifestPath).absolutePath();

    const auto result = sak::PartitionFileSystemToolManifest::validateManifestFile(manifestPath,
                                                                                   toolsRoot);
    for (const auto& tool : result.tools) {
        // The tool runner's own question, asked once per tool the manifest approves.
        const auto required = sak::PartitionFileSystemToolManifest::validateRequiredTool(
            manifestPath, toolsRoot, tool.id);
        const QString verdict = required.ok ? QStringLiteral("OK") : QStringLiteral("REFUSED");
        printLine(stdout, QStringLiteral("%1 %2 (%3)").arg(verdict, tool.id, tool.relative_path));
    }
    for (const auto& error : result.errors) {
        printLine(stdout, QStringLiteral("ERROR %1").arg(error));
    }

    if (!result.ok || result.tools.isEmpty()) {
        printLine(stdout,
                  QStringLiteral("The application would refuse the packaged filesystem tools: %1")
                      .arg(manifestPath));
        return kExitRefused;
    }
    printLine(stdout,
              QStringLiteral("The application accepts all %1 packaged filesystem tools: %2")
                  .arg(result.tools.size())
                  .arg(manifestPath));
    return kExitValid;
}
