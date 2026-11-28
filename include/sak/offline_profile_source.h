// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/user_profile_types.h"

#include <QByteArray>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <expected>
#include <functional>
#include <optional>

namespace sak {

/// @brief One elevated PowerShell run through the session's helper. Namespace-level so the
///        mount below can hold one; OfflineProfileSource aliases it as its own member type.
using OfflineElevatedScriptRunner =
    std::function<std::expected<QString, QString>(const QString& script, const QString& summary)>;

/**
 * @brief One profile folder found under <root>\\Users of an OFFLINE Windows installation.
 *
 * An entry is a decision, not just a discovery: @c included says whether this folder may go
 * into a backup, and @c skip_reason states plainly why it may not. Only a folder proven to
 * belong to a LOCAL account of that installation is included; a domain or Microsoft-account
 * profile, or one whose ownership could not be proven, is listed as skipped with its reason.
 */
struct OfflineProfileEntry {
    QString folder_name;   // The Users subfolder name; the username label v1 has
    QString profile_path;  // Native-backslash path under the volume root
    QString sid;           // SID from the offline SOFTWARE hive; empty when not readable
    bool sid_from_hive{false};
    bool included{true};
    QString skip_reason;             // Empty when included; never both
    qint64 estimated_size_bytes{0};  // Measured once at scan time; -1 when unknown
    QString hive_image_path;         // ProfileImagePath as the hive recorded it; may be empty
    /// The default backup folders with their sizes, measured once at scan time (off the GUI
    /// thread) for included profiles, so toggling a checkbox never walks the disk again.
    QVector<FolderSelection> folder_selections;
};

/**
 * @brief One volume carrying (or possibly carrying) an offline Windows installation.
 *
 * @c readable is false when the probe could not read the volume at all -- a locked
 * BitLocker volume is the common case -- and an unreadable volume is NEVER reported as
 * "no profiles" (fail closed: an empty success is the failure this whole file exists to
 * prevent).
 */
struct OfflineVolumeSource {
    QString volume_name;   // \\?\Volume{GUID}; mountvol needs a trailing backslash added
    QString root_path;     // Mounted path with trailing backslash; empty when unmounted
    QString display_name;  // Volume label, drive letter, or GUID tail: what a person picks
    bool windows_found{false};
    bool readable{true};
    QString unread_reason;  // Shown to the technician when readable is false
    bool no_media{false};   // ERROR_NOT_READY: an empty reader or drive; unlocking cannot help
    QString computer_name;  // SYSTEM hive when readable; else the display name, labeled
    QString machine_sid;    // SAM hive authority for local-account classification
    QString hive_error;     // Why the hives could not be read; empty when they were
    QVector<OfflineProfileEntry> profiles;
};

/// @brief Result of scanning every volume for an offline Windows installation.
/// @c enumeration_ok false means the volume list itself failed: a caller must refuse,
/// never read an empty @c volumes as "this PC has no offline systems".
struct OfflineSourceDiscovery {
    bool enumeration_ok{false};
    QString error;
    QVector<OfflineVolumeSource> volumes;
};

/**
 * @brief RAII mount of an unmounted offline volume into this run's own temp folder.
 *
 * The mount exists so a GUID-rooted offline tree becomes a directory the walk, the hive
 * loader and the backup engine can all use. Like OfflineRegistryHive, the destructor
 * releases and LOGS a failure it could not prove: a mount point left on the technician's
 * PC is visible damage, and release() is how a caller reports it instead.
 */
class OfflineVolumeMount {
public:
    /// @brief Mount @p volume_name under this run's temp folder. The runner is kept so
    ///        release() can take the mount down again through the same elevated channel.
    [[nodiscard]] static std::expected<OfflineVolumeMount, QString> acquire(
        const QString& volume_name, const OfflineElevatedScriptRunner& run_elevated);

    OfflineVolumeMount(OfflineVolumeMount&& other) noexcept;
    OfflineVolumeMount& operator=(OfflineVolumeMount&&) = delete;
    OfflineVolumeMount(const OfflineVolumeMount&) = delete;
    OfflineVolumeMount& operator=(const OfflineVolumeMount&) = delete;
    ~OfflineVolumeMount();

    /// @brief The directory the volume is mounted into (native path, no trailing slash).
    [[nodiscard]] const QString& directory() const { return m_directory; }

    /// @brief Take the mount down now. Idempotent: an already-released mount succeeds.
    ///        A failure names what Windows still holds and does NOT drop the directory.
    [[nodiscard]] std::expected<void, QString> release();

private:
    OfflineVolumeMount(QString directory, OfflineElevatedScriptRunner run_elevated)
        : m_directory(std::move(directory)), m_runner(std::move(run_elevated)) {}
    QString m_directory;
    OfflineElevatedScriptRunner m_runner;
};

/// @brief An offline volume that is ready to be read: located, readable, and -- when it
///        had no mount point -- mounted into this run's temp folder by @c mount.
struct OfflineVolumePrep {
    OfflineVolumeSource source;
    std::optional<OfflineVolumeMount> mount;  // engaged only when this call mounted it
};

/**
 * @brief RAII load of an offline registry hive file under HKLM with a unique throwaway key.
 *
 * The FIRST RegLoadKey in this tree (the plan's premise: nothing here ever read a hive).
 * The destructor unloads and VERIFIES the unload (the key must be gone); a failed unload
 * is reported through unload() before destruction so the caller can fail closed, and the
 * destructor logs what it could not remove -- an unloaded hive left in HKLM is visible
 * damage on the technician's own machine and never silent.
 */
class OfflineRegistryHive {
public:
    /// @brief Load @p hive_file_path (a hive file, e.g. the offline <root>\\Windows\\System32\\
    ///        config\\SOFTWARE) under HKLM. Requires elevation: SeRestorePrivilege, which
    /// RegLoadKey/RegUnloadKey need, and SeBackupPrivilege for KeyAccess::BackupSemantics
    /// reads. Without them the load fails with the OS reason and nothing is left behind.
    ///
    /// RegLoadAppKey (a process-private hive) was evaluated and not used: on Windows
    /// Server 2022 it refused a SAM, SYSTEM and SOFTWARE hive saved from a real
    /// installation with ERROR_BADDB (1009), while RegLoadKey loaded all three. The key
    /// name is random and unloaded (and verified gone) by unload() or the destructor.
    [[nodiscard]] static std::expected<OfflineRegistryHive, QString> load(
        const QString& hive_file_path);

    OfflineRegistryHive(OfflineRegistryHive&& other) noexcept;
    OfflineRegistryHive& operator=(OfflineRegistryHive&&) = delete;
    OfflineRegistryHive(const OfflineRegistryHive&) = delete;
    OfflineRegistryHive& operator=(const OfflineRegistryHive&) = delete;
    ~OfflineRegistryHive();

    /// @brief Unload now and verify the key is gone. Idempotent: an already-unloaded hive
    ///        returns success. A failure names the key Windows still holds.
    [[nodiscard]] std::expected<void, QString> unload();

    /// @brief How a key is opened. BackupSemantics opens with REG_OPTION_BACKUP_RESTORE, which
    ///        with SeBackupPrivilege reads a key whose DACL admits SYSTEM only (the SAM's
    ///        account keys); Normal is an ordinary KEY_READ open.
    enum class KeyAccess {
        Normal,
        BackupSemantics
    };

    [[nodiscard]] QString readString(const QString& sub_key, const QString& value_name) const;
    /// @brief A REG_DWORD value; nullopt when the key or value is missing or another type.
    [[nodiscard]] std::optional<quint32> readDword(const QString& sub_key,
                                                   const QString& value_name) const;
    [[nodiscard]] QByteArray readBinary(const QString& sub_key,
                                        const QString& value_name,
                                        KeyAccess access = KeyAccess::Normal) const;
    /// @brief Direct sub-key names under @p sub_key (e.g. the SIDs under ProfileList).
    ///        Empty for a missing parent -- callers distinguish with readable() semantics
    ///        by checking load success and the parent separately.
    [[nodiscard]] QStringList subKeyNames(const QString& sub_key) const;

private:
    explicit OfflineRegistryHive(std::wstring key_name) : m_key_name(std::move(key_name)) {}
    std::wstring m_key_name;
    bool m_loaded{false};
};

/// @brief One SOFTWARE ProfileList row (SID -> ProfileImagePath), as read from the hive.
struct OfflineHiveProfileRow {
    QString sid;
    QString profile_image_path;
};

/// @brief Everything the three offline hives give the scanner. A hive that failed to load
///        leaves its fields empty and names the reason in @c errors; the scanner then falls
///        back honestly (folder names only) rather than guessing.
struct OfflineHiveFacts {
    QVector<OfflineHiveProfileRow> profile_list;
    QString computer_name;
    QString machine_sid;
    QStringList errors;  // Per-hive reasons; empty when all three loaded
    bool profile_list_ok{false};
    bool machine_sid_ok{false};
};

/**
 * @brief Headless discovery of user profiles on offline Windows installations.
 *
 * Every Win32 touch point here is behind a pure seam so the unit tests can decide without
 * a disk: root normalization, the reserved-folder list, the SAM machine-SID parse, local vs
 * domain classification, recovery-key normalization and the unlock script text (which must
 * carry a placeholder for the staged key file, never the key). The live path shares DriveScanner's
 * proven indicator logic (hasWindowsIndicators / indicatorAbsentFor) so "is this an offline OS" is
 * one decision, not two that can drift.
 */
class OfflineProfileSource {
public:
    /// @brief Scan all volumes for offline Windows installations. @p is_elevated comes from
    ///        ElevationManager::isElevated() at the call site so the refusal is testable;
    ///        it reads every volume and every hive, so a GUI calls it off its own thread;
    ///        a non-elevated call returns the standard refusal without touching any disk.
    ///        The live system's own volume is always excluded -- rescuing the running system
    ///        is the live backup feature, and reading a live profile tree is the corruption
    ///        the existing filters exist to prevent.
    [[nodiscard]] static OfflineSourceDiscovery discover(bool is_elevated);

    /// @brief Re-discover ONE volume by name, after an unlock attempt changed it.
    ///        Returns the same fail-closed shape as discover().
    [[nodiscard]] static OfflineVolumeSource discoverVolume(const QString& volume_name,
                                                            bool is_elevated);

    /// @brief The standard refusal when the feature is asked without elevation: reading
    ///        another installation's hives needs administrator privileges.
    [[nodiscard]] static QString elevationRequiredMessage();

    /// @brief Runner that executes one PowerShell script elevated through the session's
    ///        helper and returns its stdout. Injected so tests never launch a process.
    using ElevatedScriptRunner = OfflineElevatedScriptRunner;

    /// @brief The default runner: ElevationBroker::sessionBroker() RunPowerShell.
    [[nodiscard]] static ElevatedScriptRunner sessionScriptRunner();

    /// @brief Unlock a locked encrypted offline volume with a technician-typed recovery key.
    ///        The key is normalized (48 digits, 8 groups) and staged by StagedScriptCredentials
    ///        as a held read-only file in this run's temp folder; only the PATH enters the
    ///        script -- the digits never appear in script text, argv or any log. The staged
    ///        file is zeroed and removed when the run is over. Returns the helper's stdout.
    [[nodiscard]] static std::expected<QString, QString> unlockEncryptedVolume(
        const QString& volume_name,
        const QString& typed_recovery_key,
        const ElevatedScriptRunner& run_elevated);

    /// @brief Mount an unmounted volume into a directory under this run's temp folder so a
    ///        GUID-rooted offline tree becomes a path Qt and the backup engine can walk.
    ///        Returns the directory it mounted into. The caller unmounts when done.
    [[nodiscard]] static std::expected<QString, QString> mountVolumeToRunDirectory(
        const QString& volume_name, const ElevatedScriptRunner& run_elevated);

    /// @brief Remove a mount directory created by mountVolumeToRunDirectory(). Best effort by
    ///        necessity (the mount may already be gone), but a failure is returned, never
    ///        swallowed: a directory mount left behind is visible damage to refuse silently.
    [[nodiscard]] static std::expected<void, QString> unmountRunDirectory(
        const QString& mount_directory, const ElevatedScriptRunner& run_elevated);

    /// @brief Locate the one volume @p requested_volume names inside a discovery result, by an
    ///        exact match only: the full volume GUID path (with or without its trailing
    ///        backslash), a mount path, a drive letter ("D", "D:" or "D:\" for a volume rooted
    ///        at D:\), or the display name. Never a partial match. Pure: no disk, so the
    ///        ambiguity and "none found" refusals the technician reads are unit-testable.
    /// @p caller_label names the feature in the refusal (the action id, or "profile rescue").
    [[nodiscard]] static std::expected<OfflineVolumeSource, QString> matchVolumeRequest(
        const OfflineSourceDiscovery& discovery,
        const QString& requested_volume,
        const QString& caller_label);

    /// @brief Everything a rescue run needs from one chosen volume: found, readable, holding a
    ///        Windows installation, and mounted into this run's temp folder when it had no
    ///        mount point. Only the requested volume's hives and profiles are read; the other
    ///        volumes are only listed (name, mount paths, label) to match the request. Every
    ///        refusal is fail-closed and names what was found instead; a mount this call created is
    ///        taken down again before it returns a refusal.
    /// @p is_elevated comes from ElevationManager at the call site, as in discover().
    /// @p run_elevated defaults to the session's helper; injected so a test can drive the
    ///        mount lifecycle without a process.
    [[nodiscard]] static std::expected<OfflineVolumePrep, QString> prepareVolume(
        const QString& requested_volume,
        const QString& caller_label,
        bool is_elevated,
        const ElevatedScriptRunner& run_elevated = sessionScriptRunner());

    /// @brief One rescuable folder as the backup pipeline wants it: the folder name is the
    ///        honest username v1 has, the SID is recorded only when the hive gave it, and the
    ///        folder selections come from the same static the live scanner uses. The size is
    ///        the one measured at scan time; there is no second walk.
    ///        Both the assistant action and the wizard page build their list here, so the two
    ///        cannot drift into two different rescues.
    [[nodiscard]] static UserProfile toUserProfile(const OfflineProfileEntry& entry);

    // ---- Pure decision seams (no Win32; unit-tested offscreen) ----------------

    /// @brief Display/probe form of a volume or mount path: native backslashes, exactly one
    ///        trailing backslash (so "<root>Windows" concatenates correctly).
    [[nodiscard]] static QString normalizeVolumeRoot(const QString& root);

    /// @brief True when @p candidate (mount path or GUID path) is the SAME volume as the
    ///        running system's root. Case-insensitive, trailing-slash-insensitive.
    [[nodiscard]] static bool isLiveSystemRoot(const QString& candidate, const QString& live_root);

    /// @brief The running system's own volume GUID path, no trailing backslash. Empty when
    ///        the query fails -- callers must then refuse, not assume "nothing is live".
    [[nodiscard]] static QString liveSystemVolumeName();

    /// @brief Users subfolders that are NOT user profiles on every Windows: the shared
    ///        Public folder and the Default/All Users/Default User fixtures.
    [[nodiscard]] static bool isReservedProfileFolderName(const QString& folder_name);

    /// @brief Parse the machine SID out of the SAM hive's SAM\\Domains\\Account "V" value:
    ///        the account domain's SID (revision 1, authority 5, four sub-authorities: 21 and
    ///        three machine values) is the last 24 bytes of V. Empty for anything malformed --
    ///        never a guess. Pinned by the elevated test against a SAM saved from the live
    ///        machine and that machine's own account-domain SID.
    [[nodiscard]] static QString machineSidFromSamAccountV(const QByteArray& account_v_value);

    /// @brief SYSTEM's control-set key for the REG_DWORD Select\\Default: 1 -> "ControlSet001".
    ///        Empty for 0 or anything above 999 -- never a guessed control set.
    [[nodiscard]] static QString controlSetKeyName(quint32 select_default);

    /// @brief True when @p profile_sid is under @p machine_sid (profile = machine + "-RID").
    ///        Both are compared uppercase; an empty either is false -- classification
    ///        without both facts is a skip, never an inclusion.
    [[nodiscard]] static bool sidBelongsToMachine(const QString& profile_sid,
                                                  const QString& machine_sid);

    /// @brief Decide one discovered folder: empty string = include, otherwise the skip reason.
    ///        Fail closed: a folder is included only when @p profile_list_read is true, the
    ///        ProfileList matched it to exactly one SID, and that SID is under @p machine_sid.
    ///        An unread ProfileList or SAM (empty @p machine_sid) skips it with the reason.
    [[nodiscard]] static QString profileSkipReason(const QString& folder_name,
                                                   const QString& sid,
                                                   const QString& machine_sid,
                                                   bool profile_list_read);

    /// @brief Normalize a typed BitLocker recovery key to "NNNNNN-NNNNNN-..." x 8 (48 digits).
    ///        Accepts spaces, dashes and stray punctuation between groups. An error names
    ///        what was wrong without echoing the digits.
    [[nodiscard]] static std::expected<QString, QString> normalizeRecoveryKey(const QString& typed);

    /// @brief The unlock script: GUID-path MountPoint and recoveryKeyFilePlaceholder() only,
    ///        which StagedScriptCredentials replaces with the held key file's path. Returns an
    ///        error when @p volume_name is not a volume GUID path.
    [[nodiscard]] static std::expected<QString, QString> bitLockerUnlockScript(
        const QString& volume_name);

    /// @brief The placeholder bitLockerUnlockScript() leaves for the staged key file's path.
    [[nodiscard]] static QString recoveryKeyFilePlaceholder();

    /// @brief True when @p volume_name is exactly a \\?\Volume{GUID} path as stored in
    ///        OfflineVolumeSource (no trailing backslash; mount and unlock append one).
    [[nodiscard]] static bool isVolumeGuidPath(const QString& volume_name);

    /// @brief True when @p directory is an absolute native path this run created under its
    ///        own temp folder -- unmount only accepts one of those, never an arbitrary path.
    [[nodiscard]] static bool isRunMountDirectory(const QString& directory);

    /// @brief Read the three offline hives under a located Windows tree: SOFTWARE (ProfileList),
    ///        SYSTEM (computer name) and SAM (machine SID). Each hive that cannot load leaves its
    ///        facts empty and names the reason in @c errors -- the scanner then skips every
    ///        folder whose local ownership that leaves unproven, never a guessed SID.
    /// @p windows_root is the located tree with trailing backslash ("<vol>\\Windows\\").
    ///        Requires elevation; every load reports the OS reason when it is missing.
    [[nodiscard]] static OfflineHiveFacts readOfflineHives(const QString& windows_root);

    /// @brief Enumerate <users_root> (native path, trailing backslash) into entries decided
    ///        against @p facts, exactly as discovery does for a located volume: reserved names
    ///        skipped, links refused without following, hives matched, sizes bounded. Public
    ///        so the unit tests decide on a temp tree without touching a real drive.
    /// @return Empty with an EMPTY error means genuinely no folders; an error means the walk
    ///         failed and the empty list must not be read as "no profiles".
    [[nodiscard]] static QVector<OfflineProfileEntry> scanUsersDirectory(
        const QString& users_root, const OfflineHiveFacts& facts, QString& error);
};

}  // namespace sak
