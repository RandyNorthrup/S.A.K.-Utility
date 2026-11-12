// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file program_integrity.h
/// @brief What S.A.K. checks about its own program folder before anything runs as administrator.
///
/// S.A.K. is portable: it runs as administrator straight from a USB stick, a data drive or a share
/// (owner, 2026-11-11: "it needs to run securely from the flash drive with nothing copied to the
/// user's machine"). Such a folder can be one another account writes to, and Windows loads DLLs
/// from the program's own folder first, so a DLL planted there would run as administrator.
///
/// Before every elevation (Run as Admin, and every start of the elevated helper) the unelevated
/// side verifies every program file the elevated process could load or run -- every .exe and .dll
/// in the program folder and below it, except data\ -- and holds each one open without
/// FILE_SHARE_WRITE or FILE_SHARE_DELETE, so a verified file cannot be changed, renamed or replaced
/// until the elevated process has taken its own hold. A file is accepted when:
///   (i)   its relative path and SHA-256 are in the manifest compiled into this build (the Qt, VC
///         runtime, vcpkg and bundled-tool files, whose bytes a release does not change), or
///   (ii)  this exe is signed and the file is signed by the same signer (security brief 4d: the
///         files a release signs), or
///   (iii) this exe is unsigned (a development or test build) and the file is one of the three
///         first-party programs S.A.K. starts, by name: the elevated helper and the two writers.
/// Anything else refuses the elevation and names the file. A signature that is present but broken
/// always refuses.
///
/// "Same signer" (sameSigner): both trusted on this PC -- the same leaf subject and the same EKU
/// OIDs (Azure Artifact Signing issues a new certificate daily for the same identity); otherwise
/// (an offline PC without the root yet) the same leaf certificate, byte for byte.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace sak::program_integrity {

/// What an Authenticode check of one file found.
enum class SignatureState : uint8_t {
    Unsigned,        ///< no signature at all
    Trusted,         ///< valid, and the chain is trusted on this PC
    UntrustedChain,  ///< the file's signature is intact, but this PC does not trust its root
    Invalid          ///< a signature is present and broken (the file changed after signing)
};

struct Signature {
    SignatureState state{SignatureState::Unsigned};
    QString subject;         ///< the signing certificate's subject
    QStringList ekus;        ///< its enhanced key usage OIDs, sorted
    QByteArray leaf_sha256;  ///< SHA-256 of the signing certificate itself
    QString detail;          ///< what WinVerifyTrust said, for a message
};

/// Check the Authenticode signature of the open file @p file_handle (WinVerifyTrust on that very
/// handle; no revocation check and no network). @p path names it in messages.
[[nodiscard]] Signature readSignature(void* file_handle, const QString& path);

/// Security brief 4d: whether @p candidate is signed by the same signer as @p reference. Two
/// unsigned files are not "the same signer"; the unsigned-build rule is separate.
[[nodiscard]] bool sameSigner(const Signature& reference, const Signature& candidate);

/// One file of the program folder, as the policy sees it.
struct FileFacts {
    QString relative;              ///< "Qt6Core.dll", "platforms\\qwindows.dll", "tools\\x\\y.exe"
    bool is_running_image{false};  ///< the very file this process was started from
    std::function<QByteArray()> sha256;    ///< the file's SHA-256, computed on demand
    std::function<Signature()> signature;  ///< its signature, read on demand
};

/// The policy (see the file comment). Empty when @p file is accepted against the running exe's
/// signature @p reference, otherwise why it is refused.
[[nodiscard]] QString refusalReason(const FileFacts& file, const Signature& reference);

/// The build manifest's SHA-256 for @p relative ('\\' or '/' separated, any case), if pinned.
[[nodiscard]] std::optional<QByteArray> pinnedSha256(const QString& relative);

/// Every relative path the build manifest pins.
[[nodiscard]] QStringList pinnedPaths();

/// The first-party programs an unsigned build may carry unsigned (rule iii).
[[nodiscard]] bool isFirstPartyProgram(const QString& relative);

/// Record which file this process was started from (volume and file index of
/// GetModuleFileNameW's path), as early in main() as possible. A later check that finds another
/// file at that path treats it as any other file, never as the running program.
void captureRunningImageIdentity();

/// Why an elevation was refused: the file and the reason.
struct Refusal {
    QString file;    ///< full path of the file that stopped it (empty when the check itself failed)
    QString reason;  ///< for the log
};

/// The program files, verified and held. Moving it keeps the hold; destroying it lets go.
class ProgramHold {
public:
    /// Verify and hold every .exe and .dll under @p program_dir except data\ (see the file
    /// comment). Fails on the first file the policy refuses, naming it.
    [[nodiscard]] static std::expected<ProgramHold, Refusal> verify(const QString& program_dir);

    ProgramHold();
    ProgramHold(ProgramHold&& other) noexcept;
    ProgramHold& operator=(ProgramHold&& other) noexcept;
    ProgramHold(const ProgramHold&) = delete;
    ProgramHold& operator=(const ProgramHold&) = delete;
    ~ProgramHold();

    /// The program folder's real path.
    [[nodiscard]] QString programDirectory() const;
    /// Real paths of every held file.
    [[nodiscard]] QStringList heldFiles() const;
    /// Whether @p path (a real path) is one of the held, verified files.
    [[nodiscard]] bool holds(const QString& path) const;
    [[nodiscard]] bool isEmpty() const;

private:
    struct State;
    std::unique_ptr<State> m_state;
};

/// The message a person sees when an elevation is refused (owner's wording).
[[nodiscard]] QString refusalMessage(const Refusal& refusal);

/// Log a refused elevation and hand refusalMessage() to the observer the GUI set, so the person
/// is told which file stopped it wherever in the program the elevation was asked for.
void reportRefusal(const Refusal& refusal);

/// Who is told about a refused elevation (the main window shows it). Empty: only the log.
void setRefusalObserver(std::function<void(const QString& message)> observer);

/// After startup in an elevated process: every module loaded from under @p program_dir must pass
/// the policy above, and every other module must come from %SystemRoot%, %ProgramFiles% or
/// %ProgramFiles(x86)% (read from Windows, not the environment). Returns the first module that is
/// neither. A module that broke the rule has already run its DllMain; the caller logs and exits.
[[nodiscard]] std::expected<void, QString> auditLoadedModules(const QString& program_dir);

/// Pure part of the audit: whether a module at @p module_path is allowed.
[[nodiscard]] bool moduleAllowed(const QString& module_path,
                                 const QString& program_dir,
                                 const std::function<bool(const QString&)>& verified,
                                 const QStringList& admin_roots);

}  // namespace sak::program_integrity
