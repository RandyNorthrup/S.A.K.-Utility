// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file program_integrity.cpp
/// @brief Verifying and holding S.A.K.'s own program files before anything runs as administrator.

#include "sak/program_integrity.h"

#include "sak/logger.h"
#include "sak/no_follow_fs.h"

#include <QCryptographicHash>
#include <QDir>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// clang-format off
// Include order is load-bearing: windows.h first, then the crypto and trust headers that use its
// types (wintrust.h before softpub.h).
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <imagehlp.h>
#include <psapi.h>
#include <shlobj.h>
// clang-format on

namespace sak::program_integrity {

namespace {

/// One build-manifest entry: a relative path in the package (lower case, '\') and its SHA-256.
struct PinnedFile {
    std::wstring_view relative;
    std::string_view sha256_hex;
};

// Generated at configure time by cmake/program_manifest.cmake from the Qt, VC runtime, vcpkg and
// bundled-tool files this build ships.
#include "sak/program_manifest.inc"
// Generated when sak_script_types.dll is built (cmake/script_types_pin.cmake): that build's DLL.
#include "sak/script_types_pin.inc"

/// The configure-time manifest plus the script-types DLL this build made.
const std::vector<PinnedFile>& allPinnedFiles() {
    static const std::vector<PinnedFile> kFiles = [] {
        std::vector<PinnedFile> all(std::begin(kPinnedFiles), std::end(kPinnedFiles));
        all.push_back({L"sak_script_types.dll", kScriptTypesSha256});
        return all;
    }();
    return kFiles;
}

constexpr std::size_t kHashChunkBytes = 1U << 20U;  // 1 MiB reads while hashing
constexpr int kModuleListInitial = 256;
constexpr int kHexBase = 16;

const std::array<QString, 3> kFirstPartyPrograms = {
    QStringLiteral("sak_elevated_helper.exe"),
    QStringLiteral("sak_apfs_writer_cli.exe"),
    QStringLiteral("sak_hfs_writer_cli.exe"),
};

QString normalizedRelative(const QString& relative) {
    QString normal = relative.toLower();
    normal.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (normal.startsWith(QLatin1Char('\\'))) {
        normal.remove(0, 1);
    }
    return normal;
}

bool isProgramFileName(const QString& name) {
    return name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) ||
           name.endsWith(QLatin1String(".dll"), Qt::CaseInsensitive);
}

struct FileIdentity {
    DWORD volume{0};
    quint64 index{0};
    bool valid{false};
    friend bool operator==(const FileIdentity&, const FileIdentity&) = default;
};

FileIdentity identityOf(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(handle, &info) == 0) {
        return {};
    }
    constexpr int kHighShift = 32;
    return {.volume = info.dwVolumeSerialNumber,
            .index = (static_cast<quint64>(info.nFileIndexHigh) << kHighShift) | info.nFileIndexLow,
            .valid = true};
}

FileIdentity& runningImageIdentity() {
    static FileIdentity identity;
    return identity;
}

QString runningImagePath() {
    constexpr std::size_t kModulePathGrowth = 2;  // a longer path: try again with twice the room
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return QString::fromStdWString(buffer);
        }
        buffer.resize(buffer.size() * kModulePathGrowth);
    }
}

/// Open a program file to verify and hold it: read access, and others may only read, so nobody
/// can write, rename, replace or delete it while it is held. A link is opened as itself.
HANDLE openForHold(const QString& path) {
    return CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                       GENERIC_READ,
                       FILE_SHARE_READ,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                       nullptr);
}

QByteArray sha256OfHandle(HANDLE handle) {
    LARGE_INTEGER start{};
    if (SetFilePointerEx(handle, start, nullptr, FILE_BEGIN) == 0) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<char> buffer(kHashChunkBytes);
    for (;;) {
        DWORD read = 0;
        if (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) ==
            0) {
            return {};
        }
        if (read == 0) {
            return hash.result();
        }
        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(read)));
    }
}

bool hasEmbeddedCertificate(HANDLE handle) {
    DWORD count = 0;
    return ImageEnumerateCertificates(handle, CERT_SECTION_TYPE_ANY, &count, nullptr, 0) != 0 &&
           count > 0;
}

SignatureState stateFor(LONG status, HANDLE handle) {
    switch (status) {
    case ERROR_SUCCESS:
        return SignatureState::Trusted;
    case TRUST_E_NOSIGNATURE:
    case TRUST_E_SUBJECT_FORM_UNKNOWN:
    case TRUST_E_PROVIDER_UNKNOWN:
        // "No signature" for a file that carries a certificate table means one that cannot be read.
        return hasEmbeddedCertificate(handle) ? SignatureState::Invalid : SignatureState::Unsigned;
    case CERT_E_UNTRUSTEDROOT:
    case CERT_E_CHAINING:
    case CERT_E_UNTRUSTEDTESTROOT:
    case CERT_E_EXPIRED:
        return SignatureState::UntrustedChain;
    default:
        return SignatureState::Invalid;
    }
}

QString certificateSubject(PCCERT_CONTEXT certificate) {
    const DWORD length = CertNameToStrW(
        X509_ASN_ENCODING, &certificate->pCertInfo->Subject, CERT_X500_NAME_STR, nullptr, 0);
    if (length <= 1) {
        return {};
    }
    std::wstring text(length, L'\0');
    CertNameToStrW(X509_ASN_ENCODING,
                   &certificate->pCertInfo->Subject,
                   CERT_X500_NAME_STR,
                   text.data(),
                   length);
    text.resize(length - 1);
    return QString::fromStdWString(text);
}

QStringList certificateEkus(PCCERT_CONTEXT certificate) {
    DWORD size = 0;
    if (CertGetEnhancedKeyUsage(
            certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, nullptr, &size) == 0 ||
        size == 0) {
        return {};
    }
    std::vector<std::byte> buffer(size);
    auto* usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(buffer.data());
    if (CertGetEnhancedKeyUsage(certificate, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usage, &size) ==
        0) {
        return {};
    }
    QStringList ekus;
    for (DWORD i = 0; i < usage->cUsageIdentifier; ++i) {
        ekus.append(QString::fromLatin1(usage->rgpszUsageIdentifier[i]));
    }
    ekus.sort();
    return ekus;
}

/// The signing (leaf) certificate WinVerifyTrust's state data names, or null.
PCCERT_CONTEXT signerCertificate(HANDLE state_data) {
    CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(state_data);
    if (provider == nullptr) {
        return nullptr;
    }
    CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    if (signer == nullptr || signer->csCertChain == 0 || signer->pasCertChain == nullptr) {
        return nullptr;
    }
    return signer->pasCertChain[0].pCert;
}

void describeSigner(HANDLE state_data, Signature* signature) {
    const PCCERT_CONTEXT certificate = signerCertificate(state_data);
    if (certificate == nullptr) {
        signature->state = SignatureState::Invalid;
        signature->detail = QStringLiteral("no signing certificate");
        return;
    }
    signature->subject = certificateSubject(certificate);
    signature->ekus = certificateEkus(certificate);
    signature->leaf_sha256 = QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(certificate->pbCertEncoded),
                       static_cast<qsizetype>(certificate->cbCertEncoded)),
        QCryptographicHash::Sha256);
}

/// Whether @p path lies under @p root (both real paths, '\').
bool isUnder(const QString& path, const QString& root) {
    if (root.isEmpty()) {
        return false;
    }
    const QString prefix = root.endsWith(QLatin1Char('\\')) ? root : root + QLatin1Char('\\');
    return path.startsWith(prefix, Qt::CaseInsensitive);
}

QString knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    QString result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path))) {
        result = QString::fromWCharArray(path);
    }
    CoTaskMemFree(path);
    return result;
}

/// %SystemRoot%, %ProgramFiles% and %ProgramFiles(x86)%, read from Windows, not the environment.
QStringList adminRoots() {
    QStringList roots;
    for (const QString& root : {knownFolder(FOLDERID_Windows),
                                knownFolder(FOLDERID_ProgramFiles),
                                knownFolder(FOLDERID_ProgramFilesX86)}) {
        if (!root.isEmpty()) {
            roots.append(root);
        }
    }
    return roots;
}

/// The real path of the file at @p path, or empty.
QString realPathOf(const QString& path) {
    const no_follow::UniqueHandle handle(
        CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS,
                    nullptr));
    return handle.valid() ? no_follow::finalPathOfHandle(handle.get()) : QString();
}

}  // namespace

// ======================================================================
// Signatures and the policy
// ======================================================================

Signature readSignature(void* file_handle, const QString& path) {
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = wide.c_str();
    file.hFile = file_handle;  // verify these very bytes, not whatever the path names by then
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    // No network: a portable tool runs on PCs that are offline. Revocation is not checked.
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

    Signature signature;
    signature.state = stateFor(status, file_handle);
    signature.detail =
        QStringLiteral("WinVerifyTrust 0x%1").arg(static_cast<ULONG>(status), 0, kHexBase);
    if (signature.state == SignatureState::Trusted ||
        signature.state == SignatureState::UntrustedChain) {
        describeSigner(data.hWVTStateData, &signature);
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return signature;
}

bool sameSigner(const Signature& reference, const Signature& candidate) {
    const auto is_signed = [](const Signature& signature) {
        return signature.state == SignatureState::Trusted ||
               signature.state == SignatureState::UntrustedChain;
    };
    if (!is_signed(reference) || !is_signed(candidate)) {
        return false;
    }
    if (reference.state == SignatureState::Trusted && candidate.state == SignatureState::Trusted) {
        // A trusted chain vouches for the subject, and Azure renews the certificate daily for the
        // same identity, which its EKUs carry.
        return !reference.subject.isEmpty() && reference.subject == candidate.subject &&
               reference.ekus == candidate.ekus;
    }
    // Without a trusted chain only the very same certificate proves the same signer.
    return !reference.leaf_sha256.isEmpty() && reference.leaf_sha256 == candidate.leaf_sha256;
}

std::optional<QByteArray> pinnedSha256(const QString& relative) {
    const std::wstring wanted = normalizedRelative(relative).toStdWString();
    for (const PinnedFile& pinned : allPinnedFiles()) {
        if (pinned.relative == wanted) {
            return QByteArray::fromHex(QByteArray(
                pinned.sha256_hex.data(), static_cast<qsizetype>(pinned.sha256_hex.size())));
        }
    }
    return std::nullopt;
}

namespace {
/// Every hash the manifest has for @p relative (a name can come from two sources).
bool pinnedMatches(const QString& relative, const QByteArray& sha256) {
    const std::wstring wanted = normalizedRelative(relative).toStdWString();
    const QByteArray hex = sha256.toHex();
    return std::ranges::any_of(allPinnedFiles(), [&](const PinnedFile& pinned) {
        return pinned.relative == wanted &&
               QByteArray(pinned.sha256_hex.data(),
                          static_cast<qsizetype>(pinned.sha256_hex.size()))
                       .compare(hex, Qt::CaseInsensitive) == 0;
    });
}
}  // namespace

QStringList pinnedPaths() {
    QStringList paths;
    for (const PinnedFile& pinned : allPinnedFiles()) {
        paths.append(QString::fromWCharArray(pinned.relative.data(),
                                             static_cast<qsizetype>(pinned.relative.size())));
    }
    return paths;
}

bool isFirstPartyProgram(const QString& relative) {
    const QString normal = normalizedRelative(relative);
    return std::ranges::any_of(kFirstPartyPrograms,
                               [&normal](const QString& name) { return normal == name; });
}

namespace {

/// Rule (i): a file the release does not change, at its pinned path with its pinned bytes.
bool acceptedByPin(const FileFacts& file) {
    return pinnedSha256(file.relative).has_value() && file.sha256 &&
           pinnedMatches(file.relative, file.sha256());
}

/// Rule (iii): an unsigned build runs only its own three programs unsigned, by name.
bool acceptedAsUnsignedFirstParty(const FileFacts& file,
                                  const Signature& reference,
                                  const Signature& signature) {
    return reference.state == SignatureState::Unsigned &&
           signature.state == SignatureState::Unsigned && isFirstPartyProgram(file.relative);
}

}  // namespace

QString refusalReason(const FileFacts& file, const Signature& reference) {
    if (file.is_running_image || acceptedByPin(file)) {
        return {};
    }
    const Signature signature = file.signature ? file.signature() : Signature{};
    if (signature.state == SignatureState::Invalid) {
        return QStringLiteral("its signature is broken (%1)").arg(signature.detail);
    }
    if (sameSigner(reference, signature) ||
        acceptedAsUnsignedFirstParty(file, reference, signature)) {
        return {};
    }
    return QStringLiteral("it isn't part of S.A.K.");
}

void captureRunningImageIdentity() {
    const no_follow::UniqueHandle handle(
        CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(runningImagePath()).utf16()),
                    FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    0,
                    nullptr));
    if (handle.valid()) {
        runningImageIdentity() = identityOf(handle.get());
    }
}

QString refusalMessage(const Refusal& refusal) {
    if (refusal.file.isEmpty()) {
        return QStringLiteral(
                   "S.A.K. won't run as administrator: it could not check its own "
                   "program files (%1).")
            .arg(refusal.reason);
    }
    return QStringLiteral(
               "S.A.K. won't run as administrator while %1 is in its folder; it isn't "
               "part of S.A.K.")
        .arg(QDir::toNativeSeparators(refusal.file));
}

namespace {
std::mutex& observerMutex() {
    static std::mutex mutex;
    return mutex;
}
std::function<void(const QString&)>& refusalObserver() {
    static std::function<void(const QString&)> observer;
    return observer;
}
}  // namespace

void setRefusalObserver(std::function<void(const QString& message)> observer) {
    const std::lock_guard lock(observerMutex());
    refusalObserver() = std::move(observer);
}

void reportRefusal(const Refusal& refusal) {
    const QString message = refusalMessage(refusal);
    sak::logError("Elevation refused: {} ({}: {})",
                  message.toStdString(),
                  QDir::toNativeSeparators(refusal.file).toStdString(),
                  refusal.reason.toStdString());
    std::function<void(const QString&)> observer;
    {
        const std::lock_guard lock(observerMutex());
        observer = refusalObserver();
    }
    if (observer) {
        observer(message);
    }
}

// ======================================================================
// ProgramHold
// ======================================================================

struct ProgramHold::State {
    std::optional<no_follow::HeldDirectory> root;
    std::vector<no_follow::HeldDirectory> folders;
    std::vector<no_follow::UniqueHandle> files;
    QStringList file_paths;
    Signature reference;
};

namespace {

struct Walk {
    QString root;
    std::vector<no_follow::HeldDirectory>* folders{nullptr};
    std::vector<no_follow::UniqueHandle>* files{nullptr};
    QStringList* file_paths{nullptr};
    const Signature* reference{nullptr};
};

std::expected<void, Refusal> verifyFile(const Walk& walk, const QString& path) {
    no_follow::UniqueHandle handle(openForHold(path));
    if (!handle.valid()) {
        return std::unexpected(Refusal{
            .file = path,
            .reason = QStringLiteral("cannot hold it (Windows error %1)").arg(GetLastError())});
    }
    // A symbolic link is refused; another reparse point (a OneDrive placeholder, a deduplicated
    // file) is the file itself, read and checked like any other. FAT32 and exFAT answer this too.
    const auto kind = no_follow::entryKindOfHandle(handle.get());
    if (!kind) {
        return std::unexpected(Refusal{
            .file = path, .reason = QStringLiteral("cannot read it (%1)").arg(kind.error())});
    }
    if (*kind == no_follow::EntryKind::Link) {
        return std::unexpected(Refusal{.file = path, .reason = QStringLiteral("it is a link")});
    }
    const FileIdentity identity = identityOf(handle.get());
    HANDLE raw = handle.get();
    FileFacts facts{
        .relative = path.mid(walk.root.size() + 1),
        .is_running_image = runningImageIdentity().valid && identity == runningImageIdentity(),
        .sha256 = [raw]() { return sha256OfHandle(raw); },
        .signature = [raw, path]() { return readSignature(raw, path); },
    };
    if (const QString reason = refusalReason(facts, *walk.reference); !reason.isEmpty()) {
        return std::unexpected(Refusal{.file = path, .reason = reason});
    }
    walk.files->push_back(std::move(handle));
    walk.file_paths->append(path);
    return {};
}

std::expected<void, Refusal> walkFolder(const Walk& walk,
                                        const no_follow::HeldDirectory& folder,
                                        bool top) {
    const QDir listing(folder.path());
    const QFileInfoList entries = listing.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                        QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        const QString name = entry.fileName();
        if (top && name.compare(QLatin1String("data"), Qt::CaseInsensitive) == 0) {
            continue;  // the person's data, never loaded or run
        }
        if (entry.isDir() || entry.isJunction()) {
            auto child = folder.child(name,
                                      false);  // a link here refuses: nothing below it is ours
            if (!child) {
                return std::unexpected(
                    Refusal{.file = folder.filePath(name), .reason = child.error()});
            }
            if (auto inner = walkFolder(walk, *child, false); !inner) {
                return inner;
            }
            walk.folders->push_back(std::move(*child));
            continue;
        }
        if (isProgramFileName(name)) {
            if (auto verified = verifyFile(walk, folder.filePath(name)); !verified) {
                return verified;
            }
        }
    }
    return {};
}

/// A signature that vouches for nothing, and why.
Signature invalidSignature(const QString& detail) {
    Signature invalid;
    invalid.state = SignatureState::Invalid;
    invalid.detail = detail;
    return invalid;
}

Signature runningImageSignature() {
    const no_follow::UniqueHandle handle(openForHold(runningImagePath()));
    if (!handle.valid()) {
        return invalidSignature(QStringLiteral("cannot open the running program"));
    }
    if (!runningImageIdentity().valid) {
        // captureRunningImageIdentity() was never called: a program's own defect, never a file
        // in the folder (the step client did not call it, 2026-11-11).
        return invalidSignature(QStringLiteral(
            "S.A.K. did not record its own program file when it started; an internal error"));
    }
    if (identityOf(handle.get()) != runningImageIdentity()) {
        // Renamed away and replaced since this process started: nothing there can vouch for it.
        return invalidSignature(QStringLiteral("the program file changed since it started"));
    }
    return readSignature(handle.get(), runningImagePath());
}

}  // namespace

ProgramHold::ProgramHold() = default;
ProgramHold::ProgramHold(ProgramHold&& other) noexcept = default;
ProgramHold& ProgramHold::operator=(ProgramHold&& other) noexcept = default;
ProgramHold::~ProgramHold() = default;

std::expected<ProgramHold, Refusal> ProgramHold::verify(const QString& program_dir) {
    auto root = no_follow::HeldDirectory::openAnchor(program_dir);
    if (!root) {
        return std::unexpected(Refusal{.file = {}, .reason = root.error()});
    }
    ProgramHold hold;
    hold.m_state = std::make_unique<State>();
    State& state = *hold.m_state;
    state.reference = runningImageSignature();
    if (state.reference.state == SignatureState::Invalid) {
        // The check itself cannot run without the running program to compare with: said as
        // that, never as a file in the folder that is not part of S.A.K.
        return std::unexpected(Refusal{.file = {}, .reason = state.reference.detail});
    }
    const Walk walk{.root = root->path(),
                    .folders = &state.folders,
                    .files = &state.files,
                    .file_paths = &state.file_paths,
                    .reference = &state.reference};
    if (auto walked = walkFolder(walk, *root, true); !walked) {
        return std::unexpected(walked.error());
    }
    state.root.emplace(std::move(*root));
    return hold;
}

QString ProgramHold::programDirectory() const {
    return m_state && m_state->root ? m_state->root->path() : QString();
}

QStringList ProgramHold::heldFiles() const {
    return m_state ? m_state->file_paths : QStringList();
}

bool ProgramHold::holds(const QString& path) const {
    return m_state && std::ranges::any_of(m_state->file_paths, [&path](const QString& held) {
               return no_follow::samePath(held, path);
           });
}

bool ProgramHold::isEmpty() const {
    return !m_state || m_state->file_paths.isEmpty();
}

// ======================================================================
// The module audit
// ======================================================================

bool moduleAllowed(const QString& module_path,
                   const QString& program_dir,
                   const std::function<bool(const QString&)>& verified,
                   const QStringList& admin_roots) {
    if (isUnder(module_path, program_dir)) {
        return verified && verified(module_path);
    }
    return std::ranges::any_of(admin_roots, [&module_path](const QString& root) {
        return isUnder(module_path, root);
    });
}

namespace {

/// Whether the loaded module at @p path, under @p program_dir, passes the policy.
bool loadedModuleVerified(const QString& path,
                          const QString& program_dir,
                          const Signature& reference) {
    const no_follow::UniqueHandle handle(openForHold(path));
    if (!handle.valid()) {
        return false;
    }
    HANDLE raw = handle.get();
    const FileFacts facts{
        .relative = path.mid(program_dir.size() + 1),
        .is_running_image = runningImageIdentity().valid &&
                            identityOf(raw) == runningImageIdentity(),
        .sha256 = [raw]() { return sha256OfHandle(raw); },
        .signature = [raw, &path]() { return readSignature(raw, path); },
    };
    return refusalReason(facts, reference).isEmpty();
}

/// Real paths of every module loaded in this process.
std::expected<QStringList, QString> loadedModulePaths() {
    std::vector<HMODULE> modules(kModuleListInitial);
    DWORD needed = 0;
    for (;;) {
        const auto bytes = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
        if (EnumProcessModulesEx(
                GetCurrentProcess(), modules.data(), bytes, &needed, LIST_MODULES_ALL) == 0) {
            return std::unexpected(QStringLiteral("cannot list the loaded modules"));
        }
        modules.resize(needed / sizeof(HMODULE));
        if (needed <= bytes) {
            break;
        }
    }
    QStringList paths;
    for (HMODULE module : modules) {
        std::wstring name(MAX_PATH, L'\0');
        const DWORD length = GetModuleFileNameExW(
            GetCurrentProcess(), module, name.data(), static_cast<DWORD>(name.size()));
        if (length == 0) {
            continue;
        }
        name.resize(length);
        const QString real = realPathOf(QString::fromStdWString(name));
        paths.append(real.isEmpty() ? QString::fromStdWString(name) : real);
    }
    return paths;
}

}  // namespace

std::expected<void, QString> auditLoadedModules(const QString& program_dir) {
    const QString root = realPathOf(program_dir);
    const auto modules = loadedModulePaths();
    if (!modules) {
        return std::unexpected(modules.error());
    }
    const Signature reference = runningImageSignature();
    const QStringList roots = adminRoots();
    for (const QString& module : *modules) {
        const auto verified = [&](const QString& path) {
            return loadedModuleVerified(path, root, reference);
        };
        if (!moduleAllowed(module, root, verified, roots)) {
            return std::unexpected(module);
        }
    }
    return {};
}

}  // namespace sak::program_integrity
