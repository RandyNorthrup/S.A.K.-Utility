// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_data_sources.h"

#include "sak/active_connections_monitor.h"
#include "sak/backup_destination_guard.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <array>
#include <span>

namespace sak {

namespace {

struct CommonSource {
    const char* name{};
    const char* category{};
    const char* relative_path{};
    std::span<const char* const> processes{};
    bool selected{true};
    std::span<const char* const> include_files{};
    std::span<const char* const> exclude_files{};
};

constexpr std::array kChrome{"chrome.exe"};
constexpr std::array kFirefox{"firefox.exe"};
constexpr std::array kEdge{"msedge.exe"};
constexpr std::array kBrave{"brave.exe"};
constexpr std::array kOpera{"opera.exe"};
constexpr std::array kVivaldi{"vivaldi.exe"};
constexpr std::array kThunderbird{"thunderbird.exe"};
constexpr std::array kOutlook{"outlook.exe", "olk.exe"};
constexpr std::array kVsCode{"code.exe"};
constexpr std::array kSublime{"sublime_text.exe"};
constexpr std::array kNotepadPlus{"notepad++.exe"};
constexpr std::array kDiscord{"discord.exe"};
constexpr std::array kTelegram{"telegram.exe"};
constexpr std::array kSignal{"signal.exe"};
constexpr std::array kSlack{"slack.exe"};
constexpr std::array kSteam{"steam.exe"};
constexpr std::array kEpic{"epicgameslauncher.exe"};
constexpr std::array kOneNote{"onenote.exe"};
constexpr std::array kStickyNotes{"microsoft.notes.exe"};
constexpr std::array kSpotify{"spotify.exe"};
constexpr std::array kVlc{"vlc.exe"};
constexpr std::array kObs{"obs64.exe"};
constexpr std::array kWinScp{"winscp.exe"};
constexpr std::array kFileZilla{"filezilla.exe"};
constexpr std::array kTerminal{"windowsterminal.exe"};

constexpr std::array kPstFiles{"*.pst"};

// Firefox and Thunderbird keep profiles.ini beside their Profiles folder. It names which profile to
// open, so the parent folder is the source: a restored Profiles folder without it would sit unused
// while the program opened a new, empty profile.
const std::array kCommonSources = std::to_array<CommonSource>({
    {"Chrome Profiles", "Browsers", "AppData/Local/Google/Chrome/User Data", kChrome},
    {"Firefox Profiles", "Browsers", "AppData/Roaming/Mozilla/Firefox", kFirefox},
    {"Edge Profiles", "Browsers", "AppData/Local/Microsoft/Edge/User Data", kEdge},
    {"Brave Profiles", "Browsers", "AppData/Local/BraveSoftware/Brave-Browser/User Data", kBrave},
    {"Opera Profiles", "Browsers", "AppData/Roaming/Opera Software/Opera Stable", kOpera},
    {"Vivaldi Profiles", "Browsers", "AppData/Local/Vivaldi/User Data", kVivaldi},

    {"Thunderbird Profiles", "Email", "AppData/Roaming/Thunderbird", kThunderbird},
    // One folder, two very different things. Older Outlook keeps PST archives here, and one may be
    // the only copy of a customer's mail. An OST or NST is a copy of what the server still has,
    // tied to this Outlook profile, and can run to tens of gigabytes: worth having only if someone
    // asks.
    {"Outlook Archives (PST)",
     "Email",
     "AppData/Local/Microsoft/Outlook",
     kOutlook,
     true,
     kPstFiles,
     {}},
    {"Outlook Cache (OST)",
     "Email",
     "AppData/Local/Microsoft/Outlook",
     kOutlook,
     false,
     {},
     kPstFiles},
    {"Outlook Signatures", "Email", "AppData/Roaming/Microsoft/Signatures", kOutlook},

    {"VS Code Settings", "Development", "AppData/Roaming/Code/User", kVsCode},
    {"VS Code Extensions", "Development", ".vscode/extensions", kVsCode},
    {"JetBrains Settings", "Development", "AppData/Roaming/JetBrains", {}},
    {"Sublime Text Settings", "Development", "AppData/Roaming/Sublime Text 3", kSublime},
    {"Notepad++ Settings", "Development", "AppData/Roaming/Notepad++", kNotepadPlus},

    {"Discord Data", "Communication", "AppData/Roaming/discord", kDiscord},
    {"Telegram Data", "Communication", "AppData/Roaming/Telegram Desktop", kTelegram},
    {"Signal Data", "Communication", "AppData/Roaming/Signal", kSignal},
    {"Slack Data", "Communication", "AppData/Roaming/Slack", kSlack},

    {"Steam Config", "Gaming", "AppData/Local/Steam", kSteam},
    {"Epic Games Settings", "Gaming", "AppData/Local/EpicGamesLauncher", kEpic},
    {"Minecraft Data", "Gaming", "AppData/Roaming/.minecraft", {}},

    {"OneNote Cache", "Productivity", "AppData/Local/Microsoft/OneNote", kOneNote},
    {"Sticky Notes",
     "Productivity",
     "AppData/Local/Packages/Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe/LocalState",
     kStickyNotes},

    {"Spotify Data", "Media", "AppData/Roaming/Spotify", kSpotify},
    {"VLC Settings", "Media", "AppData/Roaming/vlc", kVlc},
    {"OBS Studio", "Media", "AppData/Roaming/obs-studio", kObs},

    {"PuTTY Sessions", "Utilities", "AppData/Roaming/PuTTY", {}},
    {"WinSCP Settings", "Utilities", "AppData/Roaming/WinSCP", kWinScp},
    {"FileZilla Settings", "Utilities", "AppData/Roaming/FileZilla", kFileZilla},
    {"PowerShell Profile", "Utilities", "Documents/PowerShell", {}},
    {"Windows Terminal Settings",
     "Utilities",
     "AppData/Local/Packages/Microsoft.WindowsTerminal_8wekyb3d8bbwe/LocalState",
     kTerminal},

    {"SSH Keys", "System", ".ssh", {}, false},
    {"Git Config", "System", ".gitconfig", {}},
    {"npm Config", "System", "AppData/Roaming/npm", {}},
    {"pip Config", "System", "AppData/Roaming/pip", {}},
});

}  // namespace

QVector<AppDataSourceInfo> commonAppDataSources() {
    const auto toList = [](std::span<const char* const> names) {
        QStringList list;
        for (const char* name : names) {
            list.append(QString::fromLatin1(name));
        }
        return list;
    };
    QVector<AppDataSourceInfo> sources;
    sources.reserve(static_cast<qsizetype>(kCommonSources.size()));
    for (const CommonSource& common : kCommonSources) {
        AppDataSourceInfo info;
        info.name = QString::fromLatin1(common.name);
        info.category = QString::fromLatin1(common.category);
        info.relative_path = QString::fromLatin1(common.relative_path);
        info.selected = common.selected;
        info.include_patterns = toList(common.include_files);
        info.exclude_patterns = toList(common.exclude_files);
        sources.append(info);
    }
    return sources;
}

bool appDataPatternsTakeFile(const QString& file_name,
                             const QStringList& include_patterns,
                             const QStringList& exclude_patterns) {
    if (!include_patterns.isEmpty() && !QDir::match(include_patterns, file_name)) {
        return false;
    }
    return exclude_patterns.isEmpty() || !QDir::match(exclude_patterns, file_name);
}

bool backupFolderTakesFile(const FolderSelection& folder, const QString& file_name) {
    return folder.type != FolderType::ApplicationData ||
           appDataPatternsTakeFile(file_name, folder.include_patterns, folder.exclude_patterns);
}

QStringList appDataSourceProcessNames(const QString& relative_path) {
    QStringList names;
    for (const CommonSource& common : kCommonSources) {
        if (relative_path.compare(QLatin1String(common.relative_path), Qt::CaseInsensitive) != 0) {
            continue;
        }
        for (const char* process : common.processes) {
            names.append(QString::fromLatin1(process));
        }
    }
    names.removeDuplicates();  // Two rows can share a path, and so a program.
    return names;
}

QStringList runningProcessNames() {
    QSet<QString> names;
    const auto snapshot = ActiveConnectionsMonitor::snapshotProcessNames();
    for (const QString& name : snapshot) {
        names.insert(name.toLower());
    }
    return QStringList(names.cbegin(), names.cend());
}

QStringList appDataSourcesHeldOpen(const QVector<AppDataSourceInfo>& sources,
                                   const QStringList& running_processes) {
    QStringList held;
    for (const AppDataSourceInfo& source : sources) {
        if (!source.selected || held.contains(source.name)) {
            continue;
        }
        const QStringList processes = appDataSourceProcessNames(source.relative_path);
        const bool running = std::ranges::any_of(processes, [&](const QString& process) {
            return running_processes.contains(process, Qt::CaseInsensitive);
        });
        if (running) {
            held.append(source.name);
        }
    }
    return held;
}

AppDataMeasure measureAppDataSource(const AppDataSourceInfo& source,
                                    const QString& profile_path,
                                    const SmartFileFilter& filter,
                                    const std::atomic_bool* cancel) {
    AppDataMeasure measure;
    const QString absolute_path = profile_path + QLatin1Char('/') + source.relative_path;
    const QFileInfo info(absolute_path);
    if (!info.exists()) {
        return measure;
    }
    measure.exists = !source.splitsItsPath();
    if (!info.isDir()) {
        if (!filter.shouldExcludeFile(info, profile_path)) {
            measure.size_bytes = info.size();
            measure.file_count = 1;
        }
        return measure;
    }
    QDirIterator it(absolute_path,
                    QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if ((cancel != nullptr) && cancel->load()) {
            return measure;
        }
        it.next();
        const QFileInfo file = it.fileInfo();
        if (!appDataPatternsTakeFile(
                file.fileName(), source.include_patterns, source.exclude_patterns)) {
            continue;
        }
        // Before the filter: a PST over a size limit is still there, and the copy says it skipped
        // it.
        measure.exists = true;
        if (!filter.shouldExcludeFile(file, profile_path)) {
            measure.size_bytes += file.size();
            ++measure.file_count;
        }
    }
    return measure;
}

QVector<AppDataSourceInfo> scanAppDataSources(const QVector<UserProfile>& users,
                                              const SmartFilter& rules,
                                              const std::atomic_bool* cancel) {
    const SmartFileFilter filter(rules.forApplicationData());
    QVector<AppDataSourceInfo> found;
    for (AppDataSourceInfo source : commonAppDataSources()) {
        for (const UserProfile& user : users) {
            if ((cancel != nullptr) && cancel->load()) {
                return found;
            }
            if (!user.is_selected) {
                continue;
            }
            const AppDataMeasure measure =
                measureAppDataSource(source, user.profile_path, filter, cancel);
            source.exists = source.exists || measure.exists;
            source.size_bytes += measure.size_bytes;
        }
        if (source.exists) {
            found.append(source);
        }
    }
    return found;
}

namespace {

/// How one source appears among one user's stored folders.
void noteStoredCopies(const AppDataSourceInfo& source,
                      const BackupUserData& user,
                      bool& copied,
                      bool& inside_a_folder,
                      qint64& stored_bytes) {
    for (const FolderSelection& folder : user.backed_up_folders) {
        if (folder.type == FolderType::ApplicationData) {
            // Two rows can share a path, so the patterns say whose copy this is.
            if (backupPathKey(folder.relative_path) == backupPathKey(source.relative_path) &&
                folder.include_patterns == source.include_patterns &&
                folder.exclude_patterns == source.exclude_patterns) {
                copied = true;
                stored_bytes += folder.size_bytes;
            }
        } else if (folder.selected &&
                   backupPathIsWithin(source.relative_path, folder.relative_path)) {
            inside_a_folder = true;
        }
    }
}

}  // namespace

QVector<AppDataSourceInfo> appDataStoredInBackup(const BackupManifest& manifest) {
    QVector<AppDataSourceInfo> stored;
    for (AppDataSourceInfo source : manifest.app_data_sources) {
        bool copied = false;
        bool inside_a_folder = false;
        qint64 stored_bytes = 0;
        for (const BackupUserData& user : manifest.users) {
            noteStoredCopies(source, user, copied, inside_a_folder, stored_bytes);
        }
        if (!copied && !inside_a_folder) {
            continue;
        }
        if (copied) {
            source.size_bytes = stored_bytes;
        }
        stored.append(source);
    }
    return stored;
}

}  // namespace sak
