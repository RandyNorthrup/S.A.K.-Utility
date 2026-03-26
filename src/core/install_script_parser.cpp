// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file install_script_parser.cpp
/// @brief Parser implementation for Chocolatey install scripts

#include "sak/install_script_parser.h"

#include "sak/logger.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

namespace sak {

// ============================================================================
// Public API
// ============================================================================

ParsedInstallScript InstallScriptParser::parse(const QString& script_content) const {
    ParsedInstallScript result;
    result.original_script = script_content;

    if (script_content.trimmed().isEmpty()) {
        result.warnings.append("Empty script content");
        return result;
    }

    parseInstallChocolateyPackage(script_content, result);
    parseInstallChocolateyZipPackage(script_content, result);
    parseGetChocolateyWebFile(script_content, result);
    parseSplattingPattern(script_content, result);

    if (result.resources.isEmpty()) {
        result.warnings.append("No download URLs found in script");
    }

    sak::logInfo("[InstallScriptParser] Parsed script: {} resources, {} warnings",
                 static_cast<int>(result.resources.size()),
                 static_cast<int>(result.warnings.size()));

    return result;
}

ParsedInstallScript InstallScriptParser::parseFile(const QString& file_path) const {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        ParsedInstallScript result;
        result.warnings.append(QString("Cannot open file: %1").arg(file_path));
        sak::logWarning("[InstallScriptParser] Cannot open: {}", file_path.toStdString());
        return result;
    }

    QTextStream stream(&file);
    QString content = stream.readAll();
    return parse(content);
}

// ============================================================================
// Install-ChocolateyPackage
// ============================================================================

void InstallScriptParser::extractInstallPackageParams(const QString& call_text,
                                                      const QString& script,
                                                      DownloadResource& resource,
                                                      ParsedInstallScript& result) const {
    resource.url = resolveVariables(extractParameter(call_text, "url"), script);

    QString url64 = extractParameter(call_text, "url64bit");
    if (url64.isEmpty()) {
        url64 = extractParameter(call_text, "url64");
    }
    resource.url_64bit = resolveVariables(url64, script);

    resource.checksum = resolveVariables(extractParameter(call_text, "checksum"), script);
    resource.checksum_type = resolveVariables(extractParameter(call_text, "checksumType"), script);
    resource.file_name = resolveVariables(extractParameter(call_text, "fileName"), script);

    QString file_type = resolveVariables(extractParameter(call_text, "fileType"), script);
    if (!file_type.isEmpty()) {
        result.package_type = file_type;
    }

    QString silent = extractParameter(call_text, "silentArgs");
    if (!silent.isEmpty()) {
        result.silent_args = resolveVariables(silent, script);
    }
}

void InstallScriptParser::parseInstallChocolateyPackage(const QString& script,
                                                        ParsedInstallScript& result) const {
    // Match: Install-ChocolateyPackage followed by parameters (possibly multiline)
    static const QRegularExpression pattern(
        R"re(Install-ChocolateyPackage\b([^;]*?)(?=\n\s*\n|\n[^\s-]|$))re",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    auto matches = pattern.globalMatch(script);
    while (matches.hasNext()) {
        auto match = matches.next();
        QString call_text = match.captured(1);
        int pos = match.capturedStart();

        DownloadResource resource;
        resource.source_function = "Install-ChocolateyPackage";
        resource.line_number = lineNumberAt(script, pos);

        extractInstallPackageParams(call_text, script, resource, result);

        if (!resource.url.isEmpty() || !resource.url_64bit.isEmpty()) {
            result.resources.append(resource);
        }
    }
}

// ============================================================================
// Install-ChocolateyZipPackage
// ============================================================================

void InstallScriptParser::parseInstallChocolateyZipPackage(const QString& script,
                                                           ParsedInstallScript& result) const {
    static const QRegularExpression pattern(
        R"re(Install-ChocolateyZipPackage\b([^;]*?)(?=\n\s*\n|\n[^\s-]|$))re",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    auto matches = pattern.globalMatch(script);
    while (matches.hasNext()) {
        auto match = matches.next();
        QString call_text = match.captured(1);
        int pos = match.capturedStart();

        DownloadResource resource;
        resource.source_function = "Install-ChocolateyZipPackage";
        resource.line_number = lineNumberAt(script, pos);
        resource.url = resolveVariables(extractParameter(call_text, "url"), script);
        resource.url_64bit = resolveVariables(extractParameter(call_text, "url64bit"), script);
        resource.checksum = resolveVariables(extractParameter(call_text, "checksum"), script);
        resource.checksum_type = resolveVariables(extractParameter(call_text, "checksumType"),
                                                  script);

        result.package_type = "zip";

        if (!resource.url.isEmpty() || !resource.url_64bit.isEmpty()) {
            result.resources.append(resource);
        }
    }
}

// ============================================================================
// Get-ChocolateyWebFile
// ============================================================================

void InstallScriptParser::parseGetChocolateyWebFile(const QString& script,
                                                    ParsedInstallScript& result) const {
    static const QRegularExpression pattern(
        R"re(Get-ChocolateyWebFile\b([^;]*?)(?=\n\s*\n|\n[^\s-]|$))re",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);

    auto matches = pattern.globalMatch(script);
    while (matches.hasNext()) {
        auto match = matches.next();
        QString call_text = match.captured(1);
        int pos = match.capturedStart();

        DownloadResource resource;
        resource.source_function = "Get-ChocolateyWebFile";
        resource.line_number = lineNumberAt(script, pos);
        resource.url = resolveVariables(extractParameter(call_text, "url"), script);
        resource.url_64bit = resolveVariables(extractParameter(call_text, "url64bit"), script);
        resource.file_name = resolveVariables(extractParameter(call_text, "fileName"), script);
        resource.checksum = resolveVariables(extractParameter(call_text, "checksum"), script);
        resource.checksum_type = resolveVariables(extractParameter(call_text, "checksumType"),
                                                  script);

        if (!resource.url.isEmpty() || !resource.url_64bit.isEmpty()) {
            result.resources.append(resource);
        }
    }
}

// ============================================================================
// Splatting Pattern (@packageArgs)
// ============================================================================

void InstallScriptParser::parseSplattingPattern(const QString& script,
                                                ParsedInstallScript& result) const {
    // Match: $anyVariable = @{ ... } where closing } is on its own line.
    // Using \n\s*\} avoids stopping at } inside ${varName} mid-line.
    static const QRegularExpression pattern(R"re(\$(\w+)\s*=\s*@\{(.*?)\n\s*\})re",
                                            QRegularExpression::CaseInsensitiveOption |
                                                QRegularExpression::DotMatchesEverythingOption);

    auto matches = pattern.globalMatch(script);
    while (matches.hasNext()) {
        auto match = matches.next();
        QString block = match.captured(2);
        int pos = match.capturedStart();

        DownloadResource resource;
        resource.source_function = QString("splatting (@%1)").arg(match.captured(1));
        resource.line_number = lineNumberAt(script, pos);

        resource.url = resolveVariables(extractHashtableValue(block, "url"), script);
        QString url64 = extractHashtableValue(block, "url64bit");
        if (url64.isEmpty()) {
            url64 = extractHashtableValue(block, "url64");
        }
        resource.url_64bit = resolveVariables(url64, script);

        resource.checksum = resolveVariables(extractHashtableValue(block, "checksum"), script);
        resource.checksum_type = resolveVariables(extractHashtableValue(block, "checksumType"),
                                                  script);
        resource.file_name = resolveVariables(extractHashtableValue(block, "fileName"), script);

        QString file_type = extractHashtableValue(block, "fileType");
        if (!file_type.isEmpty()) {
            result.package_type = resolveVariables(file_type, script);
        }

        QString silent = extractHashtableValue(block, "silentArgs");
        if (!silent.isEmpty()) {
            result.silent_args = resolveVariables(silent, script);
        }

        if (resource.url.isEmpty() && resource.url_64bit.isEmpty()) {
            continue;
        }

        // Avoid duplicate if same URL was already found
        bool duplicate = false;
        for (const auto& existing : result.resources) {
            if (existing.url == resource.url && existing.url_64bit == resource.url_64bit) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            result.resources.append(resource);
            result.uses_splatting = true;
        }
    }
}

// ============================================================================
// Variable Resolution
// ============================================================================

QString InstallScriptParser::resolveVariables(const QString& value, const QString& script) const {
    if (value.isEmpty()) {
        return value;
    }

    // Match $variableName or ${variableName} references
    static const QRegularExpression var_pattern(R"(\$\{(\w+)\}|\$(\w+))");

    QString resolved = value;
    int iterations = 0;
    constexpr int kMaxResolutionDepth = 5;

    while (resolved.contains('$') && iterations < kMaxResolutionDepth) {
        auto match = var_pattern.match(resolved);
        if (!match.hasMatch()) {
            break;
        }

        // Group 1 = ${name}, group 2 = $name
        QString var_name = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);

        // Skip well-known PS variables
        if (var_name == "toolsDir" || var_name == "toolsPath" || var_name == "env" ||
            var_name == "PSScriptRoot" || var_name == "true" || var_name == "false" ||
            var_name == "null") {
            break;
        }

        // Look for assignment: $varName = 'value' or $varName = "value"
        QRegularExpression assign_pattern(
            QString(R"(\$%1\s*=\s*['"]([^'"]*?)['"])").arg(QRegularExpression::escape(var_name)));
        auto assign_match = assign_pattern.match(script);

        if (assign_match.hasMatch()) {
            resolved.replace(match.captured(0), assign_match.captured(1));
        } else {
            break;
        }

        iterations++;
    }

    // Clean up surrounding quotes
    if ((resolved.startsWith('\'') && resolved.endsWith('\'')) ||
        (resolved.startsWith('"') && resolved.endsWith('"'))) {
        resolved = resolved.mid(1, resolved.length() - 2);
    }

    return resolved;
}

// ============================================================================
// Parameter Extraction
// ============================================================================

QString InstallScriptParser::extractParameter(const QString& call_text,
                                              const QString& param_name) const {
    // Match: -paramName 'value' or -paramName "value" or -paramName $var
    QRegularExpression pattern(QString(R"(-%1\s+['\"']?([^'\"\s]+(?:\s[^'\"\s-]+)*)['\"']?)")
                                   .arg(QRegularExpression::escape(param_name)),
                               QRegularExpression::CaseInsensitiveOption);

    auto match = pattern.match(call_text);
    if (!match.hasMatch()) {
        return {};
    }

    QString value = match.captured(1).trimmed();

    // Handle quoted strings: -param 'some value'
    QRegularExpression quoted_pattern(
        QString(R"(-%1\s+['"]([^'"]*?)['"])").arg(QRegularExpression::escape(param_name)),
        QRegularExpression::CaseInsensitiveOption);

    auto quoted_match = quoted_pattern.match(call_text);
    if (quoted_match.hasMatch()) {
        return quoted_match.captured(1).trimmed();
    }

    return value;
}

QString InstallScriptParser::extractHashtableValue(const QString& block, const QString& key) const {
    // Quoted values first — handles URLs containing ${var} or special chars
    QRegularExpression quoted_pattern(
        QString(R"(['"]?%1['"]?\s*=\s*['"]([^'"]*?)['"])").arg(QRegularExpression::escape(key)),
        QRegularExpression::CaseInsensitiveOption);

    auto match = quoted_pattern.match(block);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }

    // Fallback: unquoted values (variable references like $varName)
    QRegularExpression unquoted_pattern(
        QString(R"(['"]?%1['"]?\s*=\s*([^\s;,\n]+))").arg(QRegularExpression::escape(key)),
        QRegularExpression::CaseInsensitiveOption);

    match = unquoted_pattern.match(block);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }

    return {};
}

int InstallScriptParser::lineNumberAt(const QString& script, int position) const {
    if (position < 0 || position > script.length()) {
        return 0;
    }
    return static_cast<int>(script.left(position).count('\n')) + 1;
}

}  // namespace sak
