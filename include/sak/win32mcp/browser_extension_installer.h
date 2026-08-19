// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QString>

namespace sak::win32mcp {

// Pinned identity of the S.A.K. browser-control extension. The RSA public key that
// yields this id is committed in browser/extension/manifest.json ("key") and is the
// signing key of browser/dist/sak_browser_control.crx, so the unpacked-load id, the
// CRX id, and this constant are provably identical (see the installer unit test).
inline constexpr char kBrowserExtensionId[] = "ofodhfbipljnhenjjjpbdaglkjdphoec";
inline constexpr char kBrowserExtensionVersion[] = "0.3.15";
inline constexpr char kBrowserExtensionCrxName[] = "sak_browser_control.crx";
inline constexpr char kNativeHostName[] = "com.sak.browsercontrol";

// Result of state(): whether the extension is set up for Chrome on this user.
enum class ExtensionInstallState {
    NotInstalled,  // neither the force-install policy entry nor the native host present
    Installed,     // both the force-install policy entry and the native host present
    Partial,       // exactly one of the two present (interrupted install/uninstall)
    Error,         // could not read the registry to decide
};

// Every path is injectable so unit tests exercise the real registry/file code against
// throwaway keys and temp dirs, never the user's live Chrome policy. Empty fields are
// filled with production defaults by the installer's constructor.
struct ExtensionInstallConfig {
    QString crx_path;            // signed CRX; default <appDir>/sak_browser_control.crx
                                 // then <appDir>/browser/sak_browser_control.crx
    QString data_dir;            // generated update.xml + host manifest; default %LOCALAPPDATA%\SAK
    QString host_exe_path;       // native host exe; default <appDir>/sak_win32_mcp.exe
    QString forcelist_key_path;  // HKCU subkey; default Chrome ExtensionInstallForcelist policy
    QString native_host_key_path;  // HKCU subkey; default Chrome NativeMessagingHosts\<host>
};

struct ExtensionInstallResult {
    bool ok = false;
    QString summary;  // one-line result for the model / status bar
    QString detail;   // paths touched or the failing step
};

// Headless, HKCU-only (no admin), reversible, idempotent setup of the browser-control
// extension: force-installs the signed CRX via Chrome enterprise policy and registers
// the native messaging host. Modern stable Chrome ignores --load-extension and blocks
// off-store sideload, so policy force-install is the only robust, version-independent
// path on customer machines.
class BrowserExtensionInstaller {
public:
    explicit BrowserExtensionInstaller(ExtensionInstallConfig config = {});

    // install(): write update.xml + native host manifest, then add our force-install
    //   policy entry (never disturbing other policies) and register the native host.
    // uninstall(): remove only our policy entry and our native host key; Chrome then
    //   auto-removes the extension. Leaves foreign policy entries untouched.
    ExtensionInstallResult install();
    ExtensionInstallResult uninstall() const;

    ExtensionInstallState state() const;
    QString stateString() const;  // "installed" | "not_installed" | "partial" | "error"
    bool crxPresent() const;

    const ExtensionInstallConfig& config() const { return config_; }

private:
    ExtensionInstallConfig config_;
};

}  // namespace sak::win32mcp
