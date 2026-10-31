# UUPMediaConverter S.A.K. Build Notes

The UUP-to-ISO step runs `UUPMediaConverter.exe desktop-convert` from this folder
(`src/core/uup_iso_builder.cpp`). Every file here is pinned by SHA-256 in
`scripts/bundle_uup_tools.ps1`, which also rejects any file it does not pin.

| File | What it is |
|---|---|
| `UUPMediaConverter.exe` | OSTooling/UUPMediaCreator `UUPMediaConverter` (reports 3.1.9.2), upstream `main` at `885f2e3603d515918773c904e778f331bb239d1b` (2026-04-10, "project: Upgrade to .NET 10") plus `UUPMediaCreator-885f2e36-sak.patch`, published self-contained single-file for win-x64 with .NET SDK 10.0.403 (runtime 10.0.14) |
| `libwim-15.dll` | wimlib, loaded by the converter's `WimLibImaging` through ManagedWimLib 2.6.0 (see "wimlib" below) |
| `CDImage\cdimage.exe` | ISO writer the converter starts from `CDImage\` beside itself; upstream's copy, see `THIRD_PARTY_LICENSES.md` |
| `Settings\Settings.ini` | setup-media file list and edition names; upstream's file at the pinned commit (git blob `65dff311`, includes PR #194) |
| `UUPMediaCreator-885f2e36-sak.patch` | the S.A.K. patch, below |
| `wimlib\` | wimlib 1.14.5 source tarball, its signature, and wimlib's license files |

## The patch

`UUPMediaCreator-885f2e36-sak.patch` changes two files:

- `src/UnifiedUpdatePlatform.Media.Creator/MediaCreator.cs`: `HandleEditionPlan` always calls
  `BaseEditionBuilder.CreateBaseEdition`, never `CreateBaseEditionWithAppXs`. Upstream takes the
  AppX path only when it runs elevated, and S.A.K. always runs it elevated. Offline DISM AppX
  provisioning fails when the host servicing stack does not match the target build, and that
  failure failed the whole edition. The resulting media is what upstream builds when it is not
  elevated: no Store apps provisioned into the image.
- `src/Applications/UUPMediaConverter/DesktopConvert.cs`: `desktop-convert` returns 1 when the
  conversion reported an Error phase. Upstream logs the error and still exits 0, and S.A.K.
  treats a non-zero exit as failure (`UupIsoBuilder::onConverterFinished`).

The earlier patch (on upstream 71a24c3) also turned an AppX provisioning failure in
`BaseEditionBuilder.cs` into a warning. With the first change that code never runs, so it is
not carried.

## Rebuilding

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_uup_media_converter.ps1 [-Install]
```

The script verifies the patch's SHA-256, clones `https://github.com/OSTooling/UUPMediaCreator.git`
at the pinned commit with an LF working tree, applies the patch, writes a `global.json` pinning
SDK 10.0.403 with roll-forward disabled and a `NuGet.config` naming only nuget.org, restores
into an isolated package folder, and stops unless every downloaded package matches the SHA-512
pinned in the script (20 packages, including the 10.0.14 runtime packs). It then runs

```
dotnet publish src/Applications/UUPMediaConverter/UUPMediaConverter.csproj -c Release -r win-x64
  --self-contained -p:PublishSingleFile=true -p:ContinuousIntegrationBuild=true --no-restore
```

at BelowNormal priority with two MSBuild nodes, compares the published `libwim-15.dll`,
`CDImage\cdimage.exe` and `Settings\Settings.ini` with the shipped copies, and prints the
executable's SHA-256 next to the pinned one. `-Install` copies only `UUPMediaConverter.exe`.
The publish also emits `.pdb` files and two `.dll.config` files; neither ships.

The build is deterministic. On 2026-10-31 two runs in different work directories produced the
same `UUPMediaConverter.exe` (SHA-256
`D5C609FEA3A3F90AF5D119990E46CA7F5725726A8EFC690C8AE47DB63EFE8165`, 75,419,234 bytes). It
depends on cloning from the upstream URL above: Source Link records the origin URL in each
project's PDB, and the PDB's ID is stamped into the assembly.

The SDK is not downloaded by the script. If `dotnet --list-sdks` has no 10.0.403, install it
per user without elevation and pass its `dotnet.exe`:

```powershell
.\dotnet-install.ps1 -Version 10.0.403 -InstallDir <dir> -NoPath
powershell -ExecutionPolicy Bypass -File scripts/build_uup_media_converter.ps1 -Dotnet <dir>\dotnet.exe
```

Microsoft's release metadata
(`https://builds.dotnet.microsoft.com/dotnet/release-metadata/10.0/releases.json`) gives
`dotnet-sdk-10.0.403-win-x64.zip` the SHA-512
`7a114c318fa7f1d9822991b534b2814d0e78386ceda816b371f5141acefe741d87ccb606ede06cadac8f9ac5070e32b708a67bbd1619ff5e5a7858a27874ef48`.
.NET 10 is LTS, supported to 2028-11-14. The previous bundle ran on .NET 8.0.23, whose support
ends 2026-11-10.

## wimlib

`libwim-15.dll` is wimlib 1.14.5, wimlib's own Windows build: the DLL from
`https://wimlib.net/downloads/wimlib-1.14.5-windows-x86_64-bin.zip` (released 2026-01-30). wimlib
publishes no signature or checksum for its Windows zips; the zip's SHA-256
`2F446D6FA3866582175F1A22A7BE198EEEE0AEC7ABA5B4E04AD25C99EAE2D265` matches the one in Scoop's
`wimlib` manifest, and the DLL's SHA-256 is
`BA853EE1E3FC5F5798581F02E8E066BA07A0A2375F0BF444FE981431FD508495`. It imports Windows and UCRT
DLLs only, as the build it replaces did. The publish emits the wimlib 1.14.4 that ManagedWimLib
2.6.0 carries (`592431EF...`, built by the ManagedWimLib project, not by wimlib); that copy is
not shipped. 1.14.5 is the latest wimlib release; its changes are a progress-message fix in
`wimlib-imagex`, build fixes, an NTFS-3G capture change (not used on Windows), and the library
license relaxed from LGPLv3+ to LGPLv2.1+.

`wimlib\` beside it holds what the LGPL asks to go with the library: the wimlib 1.14.5 source
(`wimlib-1.14.5.tar.gz`, SHA-256
`84221A3ABD5B91228F15F8E6065C335A336237B5738197B75BF419EEA561A194`) with Eric Biggers' detached
signature (`wimlib-1.14.5.tar.gz.sig`; good signature from subkey
`9A72FB1435CED45F89ED9843F35C2978BE928E2B` of primary key
`EB397C1FF130EC7D01ED644502C7A73D84B0447C`, the key published on his GitHub account), and the
license files from the Windows zip.

The swap was proved, not assumed. A harness built against the converter's own
`UnifiedUpdatePlatform.Services.Imaging` project (the `WimLibImaging` class the converter uses
for every WIM operation) and ManagedWimLib 2.6.0 (the same `ManagedWimLib.dll` and
`Joveler.DynLoader.dll` bytes the converter's single-file bundle contains), with
`libwim-15.dll` beside the executable as in this folder, ran against a 3 MB LZX test WIM
captured by `wimlib-imagex`:

- the process loaded `libwim-15.dll` `BA853EE1...` from the executable's folder and wimlib
  reported version 1.14.5;
- `GetWIMInformation`, `ExportImage` (to a new LZMS WIM), `AddFileToImage` (in-place update and
  overwrite), `EnumerateFiles` and `ExtractFileFromImage` all succeeded, both extracted files
  matched their sources byte for byte, and `wimlib-imagex verify` passed on the written WIM;
- the same run against the 1.14.4 DLL passed identically, and with no `libwim-15.dll` present
  the harness failed in `WimLibImaging.InitNativeLibrary` (it cannot pass without the DLL).

## Proof (2026-10-31)

- From a folder holding only the four run-time files (`UUPMediaConverter.exe`, `libwim-15.dll`,
  `CDImage\cdimage.exe`, `Settings\Settings.ini`; no `.dll.config`), `--help` prints the usage
  text (exit 1, CommandLineParser's help exit, as before), and `desktop-convert` on a missing and
  on an empty UUP folder logs the fatal composition-database error and exits 1. The same
  executable built without the patch exits 0 on both, which is the failure the second patch
  change exists to prevent.
- CTest `test_uup_conversion_pipeline` (existence, no fork, help, missing and empty UUP folder)
  passes against this bundle.
- Not yet proven: an elevated conversion of a real UUP set. It needs Administrator rights and
  several GB of UUP files and is a Windows 11 VM smoke item.
