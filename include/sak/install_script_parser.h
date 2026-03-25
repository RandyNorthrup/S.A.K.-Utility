// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file install_script_parser.h
/// @brief Parser for Chocolatey install scripts (chocolateyInstall.ps1)
///
/// Extracts download URLs, checksums, file names, and install arguments from
/// PowerShell install scripts used by Chocolatey packages. Supports all major
/// Chocolatey helper functions: Install-ChocolateyPackage,
/// Install-ChocolateyZipPackage, Get-ChocolateyWebFile, and variable/splatting
/// patterns.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace sak {

/// @brief A single downloadable resource extracted from an install script
struct DownloadResource {
    /// @brief Source URL for the binary
    QString url;
    /// @brief Secondary/fallback URL (url64bit parameter)
    QString url_64bit;
    /// @brief Expected checksum of the file
    QString checksum;
    /// @brief Checksum algorithm (sha256, md5, etc.)
    QString checksum_type;
    /// @brief Suggested filename for the download
    QString file_name;
    /// @brief The Chocolatey helper function that references this download
    QString source_function;
    /// @brief Line number in the script where the reference was found
    int line_number{0};
};

/// @brief Result of parsing a chocolateyInstall.ps1 script
struct ParsedInstallScript {
    /// @brief All discovered download resources
    QVector<DownloadResource> resources;
    /// @brief The original script content
    QString original_script;
    /// @brief Package arguments string (silentArgs, etc.)
    QString silent_args;
    /// @brief Package type (exe, msi, msu, zip, etc.)
    QString package_type;
    /// @brief Whether the script uses splatting (@packageArgs)
    bool uses_splatting{false};
    /// @brief Parsing warnings (non-fatal issues)
    QStringList warnings;
};

/// @brief Parser for chocolateyInstall.ps1 scripts
///
/// Extracts download URLs and related metadata from Chocolatey install scripts
/// by matching known PowerShell function patterns and variable assignments.
/// Does not execute the script — static analysis only.
class InstallScriptParser {
public:
    InstallScriptParser() = default;

    /// @brief Parse a chocolateyInstall.ps1 script
    /// @param script_content Full text content of the script
    /// @return Parsed result with extracted download resources
    [[nodiscard]] ParsedInstallScript parse(const QString& script_content) const;

    /// @brief Parse from a file path
    /// @param file_path Path to the chocolateyInstall.ps1 file
    /// @return Parsed result, or empty result with warnings if file unreadable
    [[nodiscard]] ParsedInstallScript parseFile(const QString& file_path) const;

private:
    /// @brief Extract resources from Install-ChocolateyPackage calls
    void parseInstallChocolateyPackage(const QString& script, ParsedInstallScript& result) const;

    /// @brief Extract parameters from a single Install-ChocolateyPackage call
    void extractInstallPackageParams(const QString& call_text,
                                     const QString& script,
                                     DownloadResource& resource,
                                     ParsedInstallScript& result) const;

    /// @brief Extract resources from Install-ChocolateyZipPackage calls
    void parseInstallChocolateyZipPackage(const QString& script, ParsedInstallScript& result) const;

    /// @brief Extract resources from Get-ChocolateyWebFile calls
    void parseGetChocolateyWebFile(const QString& script, ParsedInstallScript& result) const;

    /// @brief Extract resources from splatting patterns (@packageArgs)
    void parseSplattingPattern(const QString& script, ParsedInstallScript& result) const;

    /// @brief Resolve PowerShell variable references ($varName) in a value
    [[nodiscard]] QString resolveVariables(const QString& value, const QString& script) const;

    /// @brief Extract a named parameter value from a function call
    [[nodiscard]] QString extractParameter(const QString& call_text,
                                           const QString& param_name) const;

    /// @brief Extract a named parameter from a hashtable block
    [[nodiscard]] QString extractHashtableValue(const QString& block, const QString& key) const;

    /// @brief Find the line number for a position in the script
    [[nodiscard]] int lineNumberAt(const QString& script, int position) const;
};

}  // namespace sak
