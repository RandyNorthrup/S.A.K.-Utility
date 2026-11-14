// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file no_follow_fs.h
/// @brief File-system operations that never follow a junction or symbolic link.
///
/// S.A.K. Utility runs from wherever it was unpacked (a USB stick, a data drive, a share) and can
/// run as administrator. A folder another account can write to -- the program's own data folder
/// on such a drive, or a folder the person picked as a destination -- can hold a junction or a
/// symbolic link that turns an administrator's write or delete into one aimed somewhere else, such
/// as System32. Every operation here names its target by a HANDLE that was opened without following
/// a link and then checked, and keeps that handle open so the target cannot be swapped:
///
///  - HeldDirectory: a directory opened without following a link, checked to be exactly the
///    directory named (it is not a link, and no folder above it redirected the open), and held
///    without FILE_SHARE_DELETE, so neither it nor any folder above it can be renamed, replaced or
///    removed while it is held (measured on NTFS: renaming an ancestor of a held directory is
///    refused with access denied).
///  - StagedFile: a new file written under a fresh random name inside a held directory with
///    CREATE_NEW (so a planted name is never opened), and published under its real name by a
///    rename relative to the held directory's handle. Replacing an existing name replaces that
///    name; a link there is replaced, never written through.
///  - removeTree: a recursive delete that removes a link itself and never what it points at.
///
/// "Link" means a name-surrogate reparse point (a junction, a directory or file symbolic link).
/// Other reparse points (OneDrive placeholders, deduplicated files) are ordinary files and folders
/// to these functions, because they do not redirect a path; the final-path check catches any that
/// did. FAT, FAT32 and exFAT volumes cannot hold reparse points at all.

#include <QFile>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

namespace sak::no_follow {

/// Owns one Win32 HANDLE and closes it. INVALID_HANDLE_VALUE is stored as "no handle".
class UniqueHandle {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(void* handle) noexcept;
    ~UniqueHandle();
    UniqueHandle(UniqueHandle&& other) noexcept;
    UniqueHandle& operator=(UniqueHandle&& other) noexcept;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] void* get() const noexcept { return m_handle; }
    [[nodiscard]] bool valid() const noexcept { return m_handle != nullptr; }
    /// Give up ownership without closing.
    [[nodiscard]] void* release() noexcept;
    void reset() noexcept;

private:
    void* m_handle{nullptr};
};

/// The path an open handle really names (GetFinalPathNameByHandleW, DOS form): "C:\dir\file", or
/// "\\server\share\dir" on a share. Empty when Windows cannot say.
[[nodiscard]] QString finalPathOfHandle(void* handle);

/// @p path made absolute, with long names and '\' separators (GetFullPathNameW, then
/// GetLongPathNameW when the path exists). Junctions in it are NOT resolved, so it differs from
/// finalPathOfHandle of the same file exactly when something on the way was redirected. Empty on
/// failure.
[[nodiscard]] QString comparablePath(const QString& path);

/// True when @p a and @p b name the same path (Windows compares names without case).
[[nodiscard]] bool samePath(const QString& a, const QString& b);

/// What one name is, read without following it.
enum class EntryKind : uint8_t {
    Missing,    ///< nothing has that name
    File,       ///< an ordinary file
    Directory,  ///< an ordinary directory
    Link,       ///< a junction or symbolic link (to a file or a directory)
    Other       ///< present but unreadable
};

/// What the name @p path itself is. Never follows a link at the last component; Windows still
/// resolves the folders above it, so pair this with a HeldDirectory for anything that matters.
[[nodiscard]] EntryKind entryKind(const QString& path);

/// What the open @p handle is: File, Directory or Link (never Missing). On every file system,
/// FAT32 and exFAT included (see attributesFromAnswers).
[[nodiscard]] std::expected<EntryKind, QString> entryKindOfHandle(void* handle);

/// Pure: a handle's attributes and reparse tag from the two answers Windows can give.
/// FileAttributeTagInfo (@p tag_attributes, @p reparse_tag) is what NTFS and ReFS answer; FAT32
/// and exFAT refuse it (measured on the VM, 2026-11-11: ERROR_INVALID_PARAMETER), and then
/// FileBasicInfo's @p basic_attributes are the attributes and the tag is 0, since those file
/// systems hold no reparse point. A reparse attribute with no tag to read refuses (std::nullopt).
struct AttributeAnswers {
    std::optional<unsigned long> tag_attributes;
    unsigned long reparse_tag{0};
    std::optional<unsigned long> basic_attributes;
};
[[nodiscard]] std::optional<std::pair<unsigned long, unsigned long>> attributesFromAnswers(
    const AttributeAnswers& answers);

/// True when the open @p handle's owner is Administrators or SYSTEM and its DACL is protected and
/// grants access only to those two: a folder an elevated S.A.K. made for itself, which no other
/// account can have planted or changed. False when that cannot be read.
[[nodiscard]] bool isProtectedForAdministrators(void* handle);

/// True when the volume holding @p path keeps ACLs (NTFS, ReFS); false for FAT, FAT32 and exFAT.
[[nodiscard]] bool volumeKeepsAcls(void* handle);

/// A directory held open: see the file comment.
class HeldDirectory {
public:
    /// Open @p path strictly: it must exist, be a directory and not a link, and no folder on the
    /// way may be a link either. A mapped network drive or a SUBST drive letter is fine: the check
    /// starts below the drive's own root. The error names the real folder when a link redirected
    /// it.
    [[nodiscard]] static std::expected<HeldDirectory, QString> open(const QString& path);

    /// Open the folder S.A.K. itself runs from, wherever links put it: what was launched is the
    /// trust anchor, and every folder opened below it with child() or descend() is checked
    /// strictly.
    [[nodiscard]] static std::expected<HeldDirectory, QString> openAnchor(const QString& path);

    /// The single entry @p name inside this directory, opened strictly. When it is missing and
    /// @p create is set, it is created first. A link there is refused, never followed or removed.
    [[nodiscard]] std::expected<HeldDirectory, QString> child(const QString& name,
                                                              bool create) const;

    /// child() for each '/' or '\' separated component of @p relative in turn.
    [[nodiscard]] std::expected<HeldDirectory, QString> descend(const QString& relative,
                                                                bool create) const;

    /// A new directory @p name inside this one, created with fail-if-exists (a name already there,
    /// a link included, refuses) and, on a volume that keeps ACLs, the protected DACL @p sddl.
    [[nodiscard]] std::expected<HeldDirectory, QString> createChild(const QString& name,
                                                                    const QString& sddl) const;

    /// A fresh directory inside this one, named @p prefix followed by 32 random hex digits, created
    /// with fail-if-exists. On a volume that keeps ACLs it gets the protected DACL @p sddl (pass an
    /// empty string to inherit), so nobody outside it can create a link or a file in it.
    [[nodiscard]] std::expected<HeldDirectory, QString> createUniqueChild(
        const QString& prefix, const QString& sddl) const;

    /// The directory's real path, '\' separated.
    [[nodiscard]] const QString& path() const noexcept { return m_path; }
    /// @p name inside this directory, '\' separated.
    [[nodiscard]] QString filePath(const QString& name) const;
    [[nodiscard]] void* handle() const noexcept { return m_handle.get(); }
    [[nodiscard]] bool isOpen() const noexcept { return m_handle.valid(); }

private:
    HeldDirectory(UniqueHandle handle, QString path);
    UniqueHandle m_handle;
    QString m_path;
};

/// What publish() does when the real name is already taken.
enum class Publish : uint8_t {
    NoReplace,   ///< fail, leaving what is there untouched
    ReplaceName  ///< replace that NAME; a link there is replaced, its target is never touched
};

/// A new file written under a fresh random name inside a held directory: see the file comment.
class StagedFile {
public:
    /// Stage @p name inside @p directory. The staging name is ".<name>.<32 random hex>.sakpart".
    [[nodiscard]] static std::expected<StagedFile, QString> create(const HeldDirectory& directory,
                                                                   const QString& name);
    StagedFile(StagedFile&& other) noexcept;
    StagedFile& operator=(StagedFile&& other) noexcept;
    StagedFile(const StagedFile&) = delete;
    StagedFile& operator=(const StagedFile&) = delete;
    /// Removes the staged file unless it was published.
    ~StagedFile();

    /// Open for writing over the staging file's own handle.
    [[nodiscard]] QFile& device() { return *m_file; }
    [[nodiscard]] const QString& stagingPath() const noexcept { return m_staging_path; }
    /// The path it is published at.
    [[nodiscard]] QString finalPath() const;

    /// Flush, then rename the staging file to its real name relative to the held directory.
    [[nodiscard]] std::expected<void, QString> publish(Publish mode);
    /// Delete the staging file now (by its handle). Safe to call twice.
    void discard() noexcept;

private:
    StagedFile() = default;
    std::unique_ptr<QFile> m_file;
    UniqueHandle m_directory;  ///< a duplicate of the held directory's handle
    QString m_directory_path;
    QString m_name;
    QString m_staging_path;
    bool m_done{false};
};

/// Copy @p source to @p name inside @p directory through a staged file (CopyFileW into the random
/// staging name, so attributes, times and alternate streams come along as they did with
/// QFile::copy, then a rename by handle). A link at @p source is followed for READING only.
[[nodiscard]] std::expected<void, QString> copyFileInto(const QString& source,
                                                        const HeldDirectory& directory,
                                                        const QString& name,
                                                        Publish mode);

/// Move the name @p source (a link there moves as a link) to @p name inside @p directory, by a
/// rename relative to the held directory's handle. Same volume only, like MoveFileExW without
/// MOVEFILE_COPY_ALLOWED.
[[nodiscard]] std::expected<void, QString> moveInto(const QString& source,
                                                    const HeldDirectory& directory,
                                                    const QString& name,
                                                    Publish mode);

/// copyFileInto() the folder of @p target (opened with HeldDirectory::open) under @p target's name.
[[nodiscard]] std::expected<void, QString> copyFileTo(const QString& source,
                                                      const QString& target,
                                                      Publish mode);

/// moveInto() the folder of @p target (opened with HeldDirectory::open) under @p target's name.
[[nodiscard]] std::expected<void, QString> moveTo(const QString& source,
                                                  const QString& target,
                                                  Publish mode);

/// Remove @p path and everything under it without following any link: a link (the top one or any
/// inside) is removed itself, and what it points at is never touched. Read-only files are removed
/// too. A missing @p path is success.
[[nodiscard]] std::expected<void, QString> removeTree(const QString& path);

/// A new, empty, ordinary file at @p name inside @p directory, created with CREATE_NEW without
/// following a link, for appending. @p deny_write leaves FILE_SHARE_WRITE out, so while it is open
/// nobody else can write it; it can still be read. The handle has GENERIC_WRITE (no DELETE).
[[nodiscard]] std::expected<UniqueHandle, QString> createNewFile(const HeldDirectory& directory,
                                                                 const QString& name,
                                                                 bool deny_write);

/// A new file @p name in @p directory holding exactly @p bytes, then held read-only (security brief
/// 4a, for a script a tool reads by name, such as diskpart /s). It is written through a CREATE_NEW
/// handle that shares reading only, closed, reopened for reading sharing reading only, and read
/// back to prove nobody changed it in the moment between. While the returned handle is open
/// nobody else can open the file to write, rename or delete it, even on FAT, where no ACL keeps
/// another account out; a reader that shares writing (diskpart opens its script with _wfopen,
/// which does) still can. Close the handle, then remove the file, once the reader is done.
[[nodiscard]] std::expected<UniqueHandle, QString> writeHeldReadOnlyFile(
    const HeldDirectory& directory, const QString& name, const QByteArray& bytes);

/// 32 random hex digits from the system's cryptographic generator.
[[nodiscard]] QString randomToken();

}  // namespace sak::no_follow
