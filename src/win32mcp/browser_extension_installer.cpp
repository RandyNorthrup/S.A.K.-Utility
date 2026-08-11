// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_extension_installer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <iterator>

#include <windows.h>
#endif

namespace sak::win32mcp {
namespace {

// Oversize the module-path buffer well past MAX_PATH so a long extended-length
// executable path still fits.
constexpr int kExePathBufferMultiplier = 4;
// Registry value names are capped at 16,383 characters by Win32; size the buffer
// one larger to hold the terminating NUL.
constexpr int kRegValueNameBufferChars = 16'384;

// Full path of THIS running executable. The installer runs both in the GUI app (where
// QCoreApplication is up) and in the headless MCP helper process (which has none), so on
// Windows it resolves the path from the OS directly rather than via QCoreApplication.
QString currentExeFilePath() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH * kExePathBufferMultiplier];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        return QString::fromWCharArray(buf, static_cast<int>(n));
    }
#endif
    return QCoreApplication::applicationFilePath();
}

// file:///C:/Program%20Files/... - percent-encoded so paths with spaces work as a
// Chrome file:// update_url / CRX codebase (QUrl handles the encoding + drive letter).
QString toFileUrl(const QString& absPath) {
    return QUrl::fromLocalFile(absPath).toString(QUrl::FullyEncoded);
}

QString defaultDataDir() {
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        return QDir(localAppData).filePath(QStringLiteral("SAK"));
    }
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(fallback.isEmpty() ? QDir::homePath() : fallback).filePath(QStringLiteral("SAK"));
}

QString appDir() {
    const QString exe = currentExeFilePath();
    if (!exe.isEmpty()) {
        return QFileInfo(exe).absolutePath();
    }
    const QString dir = QCoreApplication::applicationDirPath();
    return dir.isEmpty() ? QDir::currentPath() : dir;
}

QString resolveCrxPath(const QString& configured) {
    if (!configured.isEmpty()) {
        return configured;
    }
    const QDir base(appDir());
    const QString direct = base.filePath(QString::fromLatin1(kBrowserExtensionCrxName));
    if (QFileInfo::exists(direct)) {
        return direct;
    }
    const QString nested =
        base.filePath(QStringLiteral("browser/") + QString::fromLatin1(kBrowserExtensionCrxName));
    if (QFileInfo::exists(nested)) {
        return nested;
    }
    return direct;  // report the primary location even when absent (install fails closed)
}

QString updateXmlPath(const ExtensionInstallConfig& c) {
    return QDir(c.data_dir).filePath(QStringLiteral("browser_ext_update.xml"));
}

// The helpers below are referenced only from the Windows-only install/uninstall/state
// bodies; [[maybe_unused]] keeps the portable #else build (Clang/GCC -Wunused-function
// -Werror) green while they compile out of the non-Windows path.
[[maybe_unused]] QString hostManifestPath(const ExtensionInstallConfig& c) {
    return QDir(c.data_dir)
        .filePath(QString::fromLatin1(kNativeHostName) + QStringLiteral(".json"));
}

[[maybe_unused]] QString buildUpdateXml(const QString& crxPath) {
    // Omaha update manifest; a single force-installed app pointing at the local CRX.
    return QStringLiteral(
               "<?xml version='1.0' encoding='UTF-8'?>\n"
               "<gupdate xmlns='http://www.google.com/update2/response' protocol='2.0'>\n"
               "  <app appid='%1'>\n"
               "    <updatecheck codebase='%2' version='%3'/>\n"
               "  </app>\n"
               "</gupdate>\n")
        .arg(QString::fromLatin1(kBrowserExtensionId),
             toFileUrl(crxPath),
             QString::fromLatin1(kBrowserExtensionVersion));
}

[[maybe_unused]] QByteArray buildHostManifest(const QString& hostExePath) {
    QJsonObject obj;
    obj[QStringLiteral("name")] = QString::fromLatin1(kNativeHostName);
    obj[QStringLiteral("description")] =
        QStringLiteral("S.A.K. Utility browser-control native messaging host");
    obj[QStringLiteral("path")] = QDir::toNativeSeparators(hostExePath);
    obj[QStringLiteral("type")] = QStringLiteral("stdio");
    QJsonArray origins;
    origins.append(QStringLiteral("chrome-extension://") +
                   QString::fromLatin1(kBrowserExtensionId) + QStringLiteral("/"));
    obj[QStringLiteral("allowed_origins")] = origins;
    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

// Atomic + durable write: QSaveFile stages to a sibling temp file and only renames it
// over the target on commit() (which flushes + fsyncs first). So a policy is never left
// pointing at a half-written update.xml, and a failed re-install leaves the prior good
// file intact instead of a zeroed one. commit() returns false on any flush/rename error.
[[maybe_unused]] bool writeFileAtomic(const QString& path, const QByteArray& bytes, QString* err) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) {
            *err = QStringLiteral("cannot write ") + path + QStringLiteral(": ") + f.errorString();
        }
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        f.cancelWriting();
        if (err) {
            *err = QStringLiteral("short write ") + path;
        }
        return false;
    }
    if (!f.commit()) {
        if (err) {
            *err = QStringLiteral("could not finalize ") + path + QStringLiteral(": ") +
                   f.errorString();
        }
        return false;
    }
    return true;
}

// Value data Chrome expects in the ExtensionInstallForcelist policy: "<id>;<update_url>".
[[maybe_unused]] QString forcelistEntryData(const ExtensionInstallConfig& c) {
    return QString::fromLatin1(kBrowserExtensionId) + QStringLiteral(";") +
           toFileUrl(updateXmlPath(c));
}

[[maybe_unused]] bool isOurForcelistData(const QString& data) {
    return data.startsWith(QString::fromLatin1(kBrowserExtensionId) + QStringLiteral(";"));
}

}  // namespace

BrowserExtensionInstaller::BrowserExtensionInstaller(ExtensionInstallConfig config)
    : config_(std::move(config)) {
    if (config_.data_dir.isEmpty()) {
        config_.data_dir = defaultDataDir();
    }
    config_.crx_path = resolveCrxPath(config_.crx_path);
    if (config_.host_exe_path.isEmpty()) {
        // The app binary is itself the Chrome native messaging host (folded win32 MCP):
        // Chrome launches it with the extension origin, and main() runs the relay.
        config_.host_exe_path = currentExeFilePath();
    }
    if (config_.forcelist_key_path.isEmpty()) {
        config_.forcelist_key_path =
            QStringLiteral("Software\\Policies\\Google\\Chrome\\ExtensionInstallForcelist");
    }
    if (config_.native_host_key_path.isEmpty()) {
        config_.native_host_key_path =
            QStringLiteral("Software\\Google\\Chrome\\NativeMessagingHosts\\") +
            QString::fromLatin1(kNativeHostName);
    }
}

bool BrowserExtensionInstaller::crxPresent() const {
    // Require a regular FILE, not merely an existing path: a directory named like the CRX must
    // not be accepted as the package (a force-install policy pointing at a directory is invalid),
    // so install() fails closed rather than writing a policy that can never load.
    return QFileInfo(config_.crx_path).isFile();
}

}  // namespace sak::win32mcp

// ---------------------------------------------------------------------------
// Windows registry-backed implementation. On other platforms the operations are
// unsupported (Chrome policy lives in the Windows registry), so they fail cleanly.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <limits>
#include <vector>

#include <windows.h>

namespace sak::win32mcp {
namespace {

std::wstring toW(const QString& s) {
    return s.toStdWString();
}

// RAII HKEY.
class RegKey {
public:
    RegKey() = default;
    ~RegKey() {
        if (h_) {
            RegCloseKey(h_);
        }
    }
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    HKEY get() const { return h_; }
    HKEY* addr() { return &h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HKEY h_ = nullptr;
};

// Open (creating if needed) an HKCU subkey for read+write.
bool openOrCreate(const QString& subkey, RegKey& out) {
    return RegCreateKeyExW(HKEY_CURRENT_USER,
                           toW(subkey).c_str(),
                           0,
                           nullptr,
                           REG_OPTION_NON_VOLATILE,
                           KEY_READ | KEY_SET_VALUE,
                           nullptr,
                           out.addr(),
                           nullptr) == ERROR_SUCCESS;
}

// Open an HKCU subkey read-only, returning the raw status so callers can tell a genuine
// read failure (e.g. ERROR_ACCESS_DENIED from a policy-tamper ACL) from mere absence
// (ERROR_FILE_NOT_FOUND) -- conflating the two would report an installed extension as
// absent and let uninstall() claim success while the force-install policy survives.
LSTATUS openReadStatus(const QString& subkey, RegKey& out) {
    return RegOpenKeyExW(HKEY_CURRENT_USER, toW(subkey).c_str(), 0, KEY_READ, out.addr());
}

bool setString(HKEY key, const wchar_t* name, const QString& value) {
    const std::wstring w = toW(value);
    const size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    // Bound the size before the DWORD narrowing RegSetValueExW requires: a value whose byte
    // length exceeds DWORD would wrap and register a truncated string. Our own config strings
    // are tiny, so this can only trip on a corrupt input -- fail closed instead of truncating.
    if (bytes > (std::numeric_limits<DWORD>::max)()) {
        return false;
    }
    return RegSetValueExW(key,
                          name,
                          0,
                          REG_SZ,
                          reinterpret_cast<const BYTE*>(w.c_str()),
                          static_cast<DWORD>(bytes)) == ERROR_SUCCESS;
}

// Read one REG_SZ value ("" name -> default). Returns false if absent/not a string.
bool readString(HKEY key, const wchar_t* name, QString& out) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        return false;
    }
    std::vector<wchar_t> buf((bytes / sizeof(wchar_t)) + 1, L'\0');
    DWORD outBytes = bytes;
    if (RegQueryValueExW(
            key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &outBytes) !=
        ERROR_SUCCESS) {
        return false;
    }
    out = QString::fromWCharArray(buf.data());
    return true;
}

struct RegValue {
    QString name;
    QString data;
};

// Enumerate the string values under an already-open key. On any genuine enumeration or
// value-read error sets *ok=false (fail closed) and returns the partial list gathered so far;
// callers must refuse rather than act on a truncated view. *ok is true only when the walk ran
// clean to ERROR_NO_MORE_ITEMS with every REG_SZ value fully read.
std::vector<RegValue> enumStringValues(HKEY key, bool* ok) {
    std::vector<RegValue> values;
    *ok = true;
    DWORD index = 0;
    for (;;) {
        wchar_t nameBuf[kRegValueNameBufferChars];
        DWORD nameLen = static_cast<DWORD>(std::size(nameBuf));
        DWORD type = 0;
        DWORD dataBytes = 0;
        LSTATUS s =
            RegEnumValueW(key, index, nameBuf, &nameLen, nullptr, &type, nullptr, &dataBytes);
        if (s == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (s != ERROR_SUCCESS && s != ERROR_MORE_DATA) {
            *ok = false;  // genuine enumeration error: list is truncated, do not trust it
            break;
        }
        RegValue v;
        v.name = QString::fromWCharArray(nameBuf, static_cast<int>(nameLen));
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            QString data;
            if (!readString(key, v.name.isEmpty() ? nullptr : toW(v.name).c_str(), data)) {
                *ok = false;  // a value we must inspect could not be read: fail closed
                break;
            }
            v.data = data;
        }
        values.push_back(std::move(v));
        ++index;
    }
    return values;
}

// Lowest positive decimal name not already used (Chrome forcelist values are "1".."N").
QString firstFreeSlot(const std::vector<RegValue>& values) {
    int slot = 1;
    for (;;) {
        const QString candidate = QString::number(slot);
        bool taken = false;
        for (const auto& v : values) {
            if (v.name == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
        ++slot;
    }
}

// Add (or refresh) our single force-install policy value without disturbing other
// entries. On failure returns false and sets *failDetail to the offending location.
bool writeForcelistEntry(const ExtensionInstallConfig& c, QString* failDetail) {
    RegKey key;
    if (!openOrCreate(c.forcelist_key_path, key)) {
        *failDetail = c.forcelist_key_path;
        return false;
    }
    bool enumOk = false;
    const auto values = enumStringValues(key.get(), &enumOk);
    if (!enumOk) {
        // Cannot see the existing values: writing a slot blindly could clobber a foreign
        // policy entry or duplicate ours. Refuse rather than guess.
        *failDetail = c.forcelist_key_path + QStringLiteral(" (could not read existing values)");
        return false;
    }
    QString slot;
    for (const auto& v : values) {
        if (isOurForcelistData(v.data)) {
            slot = v.name;
            break;
        }
    }
    if (slot.isEmpty()) {
        slot = firstFreeSlot(values);
    }
    if (!setString(key.get(), toW(slot).c_str(), forcelistEntryData(c))) {
        *failDetail = c.forcelist_key_path + QStringLiteral(" value ") + slot;
        return false;
    }
    return true;
}

// Point Chrome's native host key at our generated manifest.
bool registerNativeHostKey(const ExtensionInstallConfig& c) {
    RegKey key;
    if (!openOrCreate(c.native_host_key_path, key)) {
        return false;
    }
    return setString(key.get(), nullptr, hostManifestPath(c));
}

// Registry presence tri-state: 1 present, 0 absent, -1 genuine read error.
int forcelistPresence(const ExtensionInstallConfig& c) {
    RegKey key;
    const LSTATUS s = openReadStatus(c.forcelist_key_path, key);
    if (s == ERROR_FILE_NOT_FOUND) {
        return 0;
    }
    if (s != ERROR_SUCCESS) {
        return -1;
    }
    bool enumOk = false;
    const auto values = enumStringValues(key.get(), &enumOk);
    if (!enumOk) {
        return -1;  // read error: never downgrade an unreadable key to "absent"
    }
    for (const auto& v : values) {
        if (isOurForcelistData(v.data)) {
            return 1;
        }
    }
    return 0;
}

int nativeHostPresence(const ExtensionInstallConfig& c) {
    RegKey key;
    const LSTATUS s = openReadStatus(c.native_host_key_path, key);
    if (s == ERROR_FILE_NOT_FOUND) {
        return 0;
    }
    if (s != ERROR_SUCCESS) {
        return -1;
    }
    QString def;
    return (readString(key.get(), nullptr, def) && !def.isEmpty()) ? 1 : 0;
}

// Outcome of trying to remove our force-install policy values from the forcelist key.
struct ForcelistRemoval {
    bool touched{false};     // at least one of our entries was deleted
    bool failed{false};      // an entry we own could not be deleted / key not writable
    bool read_error{false};  // the key exists but could not be READ (a genuine error)
};

// Remove ONLY our force-install entries, leaving foreign policy values intact. Fails (rather
// than reporting a silent success) if our entry cannot be removed or the key is unreadable/
// unwritable, so uninstall() never claims success while the force-install policy survives.
ForcelistRemoval removeOurForcelistEntries(const QString& forcelist_key_path) {
    ForcelistRemoval out;
    std::vector<RegValue> values;
    {
        RegKey key;
        const LSTATUS s = openReadStatus(forcelist_key_path, key);
        if (s == ERROR_FILE_NOT_FOUND) {
            return out;  // absent: nothing of ours to remove
        }
        if (s != ERROR_SUCCESS) {
            out.read_error = true;
            return out;
        }
        bool enumOk = false;
        values = enumStringValues(key.get(), &enumOk);
        if (!enumOk) {
            out.read_error = true;  // truncated view: refuse rather than claim a clean removal
            return out;
        }
    }
    RegKey wkey;
    if (!openOrCreate(forcelist_key_path, wkey)) {
        out.failed = true;  // our entries remain -- force-install policy stays in place
        return out;
    }
    for (const auto& v : values) {
        if (!isOurForcelistData(v.data)) {
            continue;
        }
        const LSTATUS del = RegDeleteValueW(wkey.get(),
                                            v.name.isEmpty() ? nullptr : toW(v.name).c_str());
        if (del == ERROR_SUCCESS) {
            out.touched = true;
        } else {
            out.failed = true;
        }
    }
    return out;
}

// Remove our native host key entirely (the leaf key is exclusively ours). Absence is fine
// (already gone); any other error means the key -- and thus the native host -- remains.
void removeNativeHostKey(const QString& native_host_key_path, bool* touched, bool* failed) {
    const LSTATUS del =
        RegDeleteKeyExW(HKEY_CURRENT_USER, toW(native_host_key_path).c_str(), KEY_WOW64_64KEY, 0);
    if (del == ERROR_SUCCESS) {
        *touched = true;
    } else if (del != ERROR_FILE_NOT_FOUND) {
        *failed = true;
    }
}

}  // namespace

ExtensionInstallResult BrowserExtensionInstaller::install() {
    ExtensionInstallResult r;
    if (!crxPresent()) {
        r.summary = QStringLiteral("Extension package missing");
        r.detail = QStringLiteral("signed CRX not found at ") + config_.crx_path;
        return r;  // fail closed: never write a policy that points at a nonexistent CRX
    }

    QString err;
    if (!writeFileAtomic(updateXmlPath(config_), buildUpdateXml(config_.crx_path).toUtf8(), &err)) {
        r.summary = QStringLiteral("Could not write update manifest");
        r.detail = err;
        return r;
    }
    if (!writeFileAtomic(
            hostManifestPath(config_), buildHostManifest(config_.host_exe_path), &err)) {
        r.summary = QStringLiteral("Could not write native host manifest");
        r.detail = err;
        return r;
    }

    // Native messaging host FIRST: point Chrome at our host manifest before writing any
    // force-install policy. If host registration fails we return here having written no
    // forcelist entry, so a force-install policy is never left stranded without its
    // native messaging host (fail closed).
    if (!registerNativeHostKey(config_)) {
        r.summary = QStringLiteral("Could not register native host key");
        r.detail = config_.native_host_key_path;
        return r;
    }

    // Force-install policy entry (idempotent: reuse our slot if already present).
    QString failDetail;
    if (!writeForcelistEntry(config_, &failDetail)) {
        r.summary = QStringLiteral("Could not write force-install policy entry");
        r.detail = failDetail;
        return r;
    }

    r.ok = true;
    r.summary = QStringLiteral("Browser extension installed");
    r.detail = QStringLiteral("id ") + QString::fromLatin1(kBrowserExtensionId) +
               QStringLiteral(
                   "; force-install policy + native host registered. Restart Chrome to "
                   "complete install.");
    return r;
}

// Not const by design: it is the semantic pair of the (mutating) install(), and both perform an
// external registry/filesystem mutation even though neither writes a data member.
// cppcheck-suppress functionConst
ExtensionInstallResult BrowserExtensionInstaller::uninstall() {
    ExtensionInstallResult r;
    const ForcelistRemoval fl = removeOurForcelistEntries(config_.forcelist_key_path);
    if (fl.read_error) {
        r.ok = false;
        r.summary = QStringLiteral("Could not read Chrome policy to uninstall");
        r.detail = config_.forcelist_key_path;
        return r;
    }
    bool touched = fl.touched;
    bool failed = fl.failed;
    removeNativeHostKey(config_.native_host_key_path, &touched, &failed);

    // Best-effort cleanup of generated files (harmless orphans once the registry policy is gone).
    QFile::remove(updateXmlPath(config_));
    QFile::remove(hostManifestPath(config_));

    r.ok = !failed;
    if (failed) {
        r.summary = QStringLiteral(
            "Browser extension uninstall incomplete: the force-install "
            "policy or native host could not be fully removed");
        r.detail = QStringLiteral("id ") + QString::fromLatin1(kBrowserExtensionId);
        return r;
    }
    r.summary = touched ? QStringLiteral("Browser extension uninstalled")
                        : QStringLiteral("Browser extension was not installed");
    r.detail = QStringLiteral("removed force-install policy entry + native host for id ") +
               QString::fromLatin1(kBrowserExtensionId);
    return r;
}

ExtensionInstallState BrowserExtensionInstaller::state() const {
    // A key that opens with anything other than success-or-absence is a genuine read error
    // (e.g. an ACL denying read); report Error instead of silently downgrading to
    // NotInstalled, which would mask a real force-install we simply could not see.
    const int forcelist = forcelistPresence(config_);
    const int host = nativeHostPresence(config_);
    if (forcelist < 0 || host < 0) {
        return ExtensionInstallState::Error;
    }
    if (forcelist == 1 && host == 1) {
        return ExtensionInstallState::Installed;
    }
    if (forcelist == 1 || host == 1) {
        return ExtensionInstallState::Partial;
    }
    return ExtensionInstallState::NotInstalled;
}

}  // namespace sak::win32mcp

#else   // !_WIN32

namespace sak::win32mcp {

ExtensionInstallResult BrowserExtensionInstaller::install() {
    return {false,
            QStringLiteral("Unsupported platform"),
            QStringLiteral("Chrome extension policy install is Windows-only")};
}
ExtensionInstallResult BrowserExtensionInstaller::uninstall() {
    return {false, QStringLiteral("Unsupported platform"), QString()};
}
ExtensionInstallState BrowserExtensionInstaller::state() const {
    return ExtensionInstallState::Error;
}

}  // namespace sak::win32mcp

#endif  // _WIN32

namespace sak::win32mcp {

QString BrowserExtensionInstaller::stateString() const {
    switch (state()) {
    case ExtensionInstallState::Installed:
        return QStringLiteral("installed");
    case ExtensionInstallState::NotInstalled:
        return QStringLiteral("not_installed");
    case ExtensionInstallState::Partial:
        return QStringLiteral("partial");
    case ExtensionInstallState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("error");
}

}  // namespace sak::win32mcp
