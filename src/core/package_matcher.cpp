// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/package_matcher.h"

#include <QLatin1String>
#include <QRegularExpression>

namespace {

constexpr double kCaseInsensitiveExactConfidence = 0.95;
// A display name carries at most a few build-detail pieces; the bound only guards the loop.
constexpr int kMaxNormalizePasses = 8;
// The bundled tool's own package: reported by `choco list`, and not an application to reinstall.
constexpr QLatin1String kChocolateyOwnPackage{"chocolatey"};

struct KnownPackage {
    const char* app_name;
    const char* choco_package;
};

// Reviewed table of display names to package ids. Every id was confirmed on the community feed
// with `choco search <id> --exact`. Keys are written as the installers write DisplayName, and go
// through the same normalization as a scanned name when they are loaded.
constexpr KnownPackage kKnownPackages[] = {
    // Browsers
    {"Google Chrome", "googlechrome"},
    {"Mozilla Firefox", "firefox"},
    {"Mozilla Firefox ESR", "firefoxesr"},
    {"Microsoft Edge", "microsoft-edge"},
    {"Opera", "opera"},
    {"Opera Stable", "opera"},
    {"Brave", "brave"},
    {"Vivaldi", "vivaldi"},

    // Development tools
    {"Visual Studio Code", "vscode"},
    {"Microsoft Visual Studio Code", "vscode"},
    {"Git", "git"},
    {"GitHub Desktop", "github-desktop"},
    {"GitHub CLI", "gh"},
    {"GitKraken", "gitkraken"},
    {"Docker Desktop", "docker-desktop"},
    {"Node.js", "nodejs"},
    {"Python", "python"},
    {"Java", "javaruntime"},
    {"Java 8 Update", "javaruntime"},
    {"CMake", "cmake"},
    {"LLVM", "llvm"},
    {"Pandoc", "pandoc"},
    {"PowerShell", "powershell-core"},

    // Compression tools
    {"7-Zip", "7zip"},
    {"WinRAR", "winrar"},
    {"PeaZip", "peazip"},

    // Media
    {"VLC media player", "vlc"},
    {"iTunes", "itunes"},
    {"Spotify", "spotify"},
    {"Audacity", "audacity"},
    {"HandBrake", "handbrake"},
    {"MKVToolNix", "mkvtoolnix"},
    {"foobar2000", "foobar2000"},

    // Text editors
    {"Notepad++", "notepadplusplus"},
    {"Sublime Text", "sublimetext4"},
    {"Atom", "atom"},

    // Communication
    {"Discord", "discord"},
    {"Slack", "slack"},
    {"Zoom", "zoom"},
    {"Zoom Workplace", "zoom"},
    {"Microsoft Teams", "microsoft-teams"},
    {"Skype", "skype"},
    {"Mozilla Thunderbird", "thunderbird"},
    {"Telegram Desktop", "telegram"},
    {"Signal", "signal"},

    // Utilities
    {"PuTTY", "putty"},
    {"WinSCP", "winscp"},
    {"FileZilla", "filezilla"},
    {"FileZilla Client", "filezilla"},
    {"TeamViewer", "teamviewer"},
    {"AnyDesk", "anydesk"},
    {"RustDesk", "rustdesk"},
    {"Wireshark", "wireshark"},
    {"WinDirStat", "windirstat"},
    {"TightVNC", "tightvnc"},
    {"Revo Uninstaller", "revo-uninstaller"},
    {"CPUID CPU-Z", "cpu-z"},
    {"CPUID HWMonitor", "hwmonitor"},
    {"Angry IP Scanner", "angryip"},
    {"OpenVPN", "openvpn"},
    {"HxD Hex Editor", "hxd"},
    {"Everything", "everything"},
    {"Greenshot", "greenshot"},
    {"ShareX", "sharex"},
    {"WizTree", "wiztree"},
    {"TreeSize Free", "treesizefree"},
    {"CCleaner", "ccleaner"},
    {"Malwarebytes", "malwarebytes"},
    {"PowerISO", "poweriso"},

    // Office and productivity
    {"Adobe Acrobat", "adobereader"},
    {"Adobe Acrobat Reader", "adobereader"},
    {"Adobe Acrobat Reader DC", "adobereader"},
    {"LibreOffice", "libreoffice-fresh"},
    {"GIMP", "gimp"},
    {"Inkscape", "inkscape"},
    {"OBS Studio", "obs-studio"},
    {"VirtualBox", "virtualbox"},
    {"Oracle VirtualBox", "virtualbox"},
    {"Oracle VM VirtualBox", "virtualbox"},
    {"KeePass Password Safe", "keepass"},
    {"Blender", "blender"},
    {"paint.net", "paint.net"},
    {"Krita", "krita"},
    {"Dropbox", "dropbox"},
    {"Google Drive", "googledrive"},

    // Downloads and games
    {"qBittorrent", "qbittorrent"},
    {"Steam", "steam"},
    {"Epic Games Launcher", "epicgameslauncher"},
};

/// One trailing piece of a display name that identifies a build rather than the product. A
/// bracket group ("(x64 en-US)", "(User)"), an architecture ("-x64", " amd64"), a version
/// ("22.01", "version 0.5.1", "release 0.81", "2.7.5-I001"), or a packaging word. A word AFTER a
/// version stays: "Python 3.13.16 Documentation" is a component, not Python.
const QRegularExpression& trailingBuildDetail() {
    static const QRegularExpression expression(
        QStringLiteral(R"((?:\s*\([^()]*\))"
                       R"(|[\s-]+(?:x64|x86|amd64|arm64|64-bit|32-bit))"
                       R"(|\s+(?:version\s+|release\s+)?v?\d+(?:\.\d+)*(?:[-+][0-9A-Za-z.+-]+)?)"
                       R"(|\s+(?:for Windows|Desktop|Application)))"
                       R"($)"),
        QRegularExpression::CaseInsensitiveOption);
    return expression;
}

}  // namespace

namespace sak {

PackageMatcher::PackageMatcher() {
    initializeCommonMappings();
}

void PackageMatcher::initializeCommonMappings() {
    for (const auto& known : kKnownPackages) {
        addMapping(QLatin1String(known.app_name), QLatin1String(known.choco_package));
    }
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::findMatch(
    const AppScanner::AppInfo& app) const {
    return exactMatch(normalizeAppName(app.name));
}

void PackageMatcher::applyMatches(std::vector<AppScanner::AppInfo>& apps) const {
    for (auto& app : apps) {
        if (app.source == AppScanner::AppInfo::Source::Chocolatey) {
            if (app.choco_package.compare(kChocolateyOwnPackage, Qt::CaseInsensitive) == 0) {
                app.choco_package.clear();
                app.choco_available = false;
                continue;
            }
            app.match_confidence = AppScanner::AppInfo::Confidence::High;
            continue;
        }
        if (app.source == AppScanner::AppInfo::Source::AppX) {
            continue;
        }
        const auto match = findMatch(app);
        if (!match.has_value()) {
            continue;
        }
        app.choco_package = match->choco_package;
        app.choco_available = match->available;
        app.match_confidence = AppScanner::AppInfo::Confidence::High;
    }
}

std::optional<PackageMatcher::MatchResult> PackageMatcher::exactMatch(
    const QString& app_name) const {
    if (app_name.isEmpty()) {
        return std::nullopt;
    }

    // Check direct mapping
    const auto direct = m_exact_mappings.constFind(app_name);
    if (direct != m_exact_mappings.cend()) {
        return MatchResult{.choco_package = direct.value(),
                           .matched_name = app_name,
                           .confidence = 1.0,
                           .match_type = "exact",
                           .available = true,
                           .version = ""};
    }

    // Check case-insensitive
    for (auto it = m_exact_mappings.cbegin(); it != m_exact_mappings.cend(); ++it) {
        if (it.key().compare(app_name, Qt::CaseInsensitive) == 0) {
            return MatchResult{.choco_package = it.value(),
                               .matched_name = it.key(),
                               .confidence = kCaseInsensitiveExactConfidence,
                               .match_type = "exact",
                               .available = true,
                               .version = ""};
        }
    }

    return std::nullopt;
}

QString PackageMatcher::normalizeAppName(const QString& app_name) {
    QString normalized = app_name.trimmed();
    for (int pass = 0; pass < kMaxNormalizePasses; ++pass) {
        const auto previous_length = normalized.length();
        normalized.remove(trailingBuildDetail());
        normalized = normalized.trimmed();
        if (normalized.length() == previous_length) {
            break;
        }
    }
    return normalized;
}

void PackageMatcher::addMapping(const QString& app_name, const QString& choco_package) {
    const QString normalized = normalizeAppName(app_name);
    if (normalized.isEmpty() || choco_package.isEmpty()) {
        // Never store a blank mapping: it would surface as an empty suggestion and cannot map
        // anything. Fail closed.
        return;
    }
    m_exact_mappings[normalized] = choco_package;
}

void PackageMatcher::removeMapping(const QString& app_name) {
    const QString normalized = normalizeAppName(app_name);
    m_exact_mappings.remove(normalized);
}

bool PackageMatcher::hasMapping(const QString& app_name) const {
    const QString normalized = normalizeAppName(app_name);
    return m_exact_mappings.contains(normalized);
}

QString PackageMatcher::getMapping(const QString& app_name) const {
    const QString normalized = normalizeAppName(app_name);
    return m_exact_mappings.value(normalized, QString());
}

int PackageMatcher::getMappingCount() const {
    return static_cast<int>(m_exact_mappings.size());
}

}  // namespace sak
