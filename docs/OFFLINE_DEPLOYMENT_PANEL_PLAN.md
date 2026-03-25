# Offline Deployment Package Manager — Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 24, 2026  
**Status**: Planning  
**Target Release**: TBD  

---

## 🎯 Executive Summary

The **Offline Deployment Package Manager** provides enterprise-grade offline software deployment capabilities directly within S.A.K. Utility. It enables technicians to build self-contained deployment packages on an internet-connected machine, then deploy that exact set of software to air-gapped, bandwidth-constrained, or field machines without any internet access. This panel replaces the need for Chocolatey for Business ($17,000+/year) by implementing our own package internalization engine that works with the free Chocolatey CLI already bundled with SAK Utility.

### The Problem

PC technicians and MSPs face these recurring pain points:

1. **Air-gapped environments** — Government, medical, financial, and industrial PCs often have no internet. Installing software requires manually hunting down offline installers from vendor websites, carrying USB drives, and hoping versions match.
2. **Bandwidth-constrained sites** — Rural offices, ships, job sites with satellite internet — downloading 500MB+ installers per machine across 20 workstations saturates the link for hours.
3. **Repeatable deployments** — Setting up a fleet of 50 identical workstations means downloading the same software 50 times, or manually building a USB drive with the right installers and hoping nothing is missed.
4. **Version drift** — Without a controlled package source, each machine may end up with slightly different software versions when technicians install "latest" at different times.
5. **Chocolatey's paid wall** — Chocolatey for Business ($17k+/year) offers Package Internalizer and CDN cache to solve these problems, but that price point is unreachable for independent technicians and small MSPs.

### The Solution

SAK Utility will build its **own package internalization engine** that:

- **Scans a PC** to discover all installed applications (Registry, AppX, Chocolatey)
- **Matches apps** to Chocolatey packages (using the existing `PackageMatcher`)
- **Downloads nupkg files** from the Chocolatey Community Repository (NuGet v2 API — free, no license required)
- **Parses install scripts** to find download URLs for the actual software binaries (EXE, MSI, ZIP)
- **Downloads the binaries** and embeds them in the package or stores alongside
- **Rewrites the install scripts** to use local paths instead of internet URLs
- **Repacks into self-contained nupkg files** using `choco pack` (free CLI)
- **Bundles everything** into a portable deployment package (folder or ZIP) with a manifest
- **Deploys offline** using SAK's bundled portable Chocolatey with `--source=<local_path>`

### Key Objectives

- [ ] **Package Internalization Engine** — Download, parse, internalize, and repack Chocolatey packages without a paid license
- [ ] **App Scanner Integration** — Scan current PC's installed apps and auto-match to Chocolatey packages for replication
- [ ] **Deployment Package Builder** — Create self-contained deployment packages (folder/ZIP/ISO) with manifest
- [ ] **Offline Installer** — Install from a deployment package on an air-gapped machine with zero internet
- [ ] **Direct Download Mode** — Download just the raw installers (EXE/MSI) without Chocolatey packaging for manual installation
- [ ] **Package Repository Browser** — Search, browse, and select Chocolatey packages with rich metadata
- [ ] **Version Pinning** — Lock specific versions for consistent fleet-wide deployments
- [ ] **Dependency Resolution** — Automatically include all package dependencies in the deployment package
- [ ] **Package Verification** — Checksum verification for all downloaded binaries
- [ ] **Deployment Reports** — Generate HTML/JSON reports of what was deployed and verification results
- [ ] **Curated Package Lists** — Save/load/share named collections (e.g., "Standard Office PC", "Developer Workstation")

---

## 📊 Chocolatey Paid vs Free — Research & Analysis

### What Chocolatey Business (C4B) Offers

Chocolatey for Business (~$17,000/year for organizations) provides these offline-related features:

| Feature | Edition | What It Does |
|---|---|---|
| **Package Internalizer** | C4B/MSP only | `choco download pkg --internalize` — Automatically downloads nupkg, finds all remote URLs in install scripts, downloads binaries, rewrites scripts to use local resources, and repacks the nupkg. Fully automated. |
| **CDN Download Cache** | Pro+ ($120/year) | Caches binary downloads on Chocolatey's private CDN so if the vendor site goes down, you can still install. Not offline, just resilient. |
| **Package Downloader** | Pro+ (Licensed) | `choco download pkg` — Downloads the nupkg file itself. Basic package copy without internalization. |
| **Package Builder** | C4B only | Auto-generates Chocolatey packages from EXE/MSI installers. Detects installer type and creates appropriate install script. |
| **Package Reducer** | Pro+ | Reduces disk space of installed packages by removing embedded binaries after install. |
| **Self-Service Anywhere** | C4B only | End-user self-service installs without admin rights. |
| **Central Management** | C4B only | Web dashboard for managing Chocolatey across all endpoints. |

### What Chocolatey Free (Open Source) Can Do

| Capability | Free? | Details |
|---|---|---|
| `choco install pkg --source=<local_folder>` | ✅ Yes | Install from a local folder containing .nupkg files — the core of offline install |
| `choco pack nuspec_file` | ✅ Yes | Build a .nupkg from a .nuspec + tools folder — the core of repackaging |
| `choco push pkg --source=<url>` | ✅ Yes | Push packages to a local repository |
| `choco search pkg` | ✅ Yes | Search the Chocolatey Community Repository |
| `choco list` | ✅ Yes | List locally installed Chocolatey packages |
| `choco install pkg` | ✅ Yes | Standard online install |
| Download .nupkg via HTTP | ✅ Yes | NuGet v2 API endpoint: `https://community.chocolatey.org/api/v2/package/{id}/{version}` — just an HTTP GET |
| `choco download pkg` | ❌ Licensed only | Package copy/download requires a paid license |
| `--internalize` flag | ❌ C4B only | Automatic internalization is C4B-exclusive |
| CDN cache | ❌ Pro+ only | Private CDN caching of downloads |

### Our Strategy: Build Our Own Internalization Engine

Since the free Chocolatey CLI **can** install from local sources and **can** pack packages from nuspec files, we only need to replicate the **Package Internalizer** logic ourselves. This is feasible because:

1. **nupkg = ZIP file** — A .nupkg is literally a ZIP archive. We can download and extract it with standard ZIP libraries (Qt's `QZipReader` or `minizip`).
2. **Install scripts are PowerShell** — `chocolateyInstall.ps1` contains the download URLs as string literals passed to `Install-ChocolateyPackage`, `Install-ChocolateyZipPackage`, `Get-ChocolateyWebFile`, etc. We can parse these with regex.
3. **NuGet v2 API is public** — The community repository exposes an OData v2 API for searching and downloading packages. No authentication required.
4. **`choco pack` is free** — Recompiling a modified package back to .nupkg is a free CLI command.
5. **`choco install --source=.` is free** — Installing from a local folder requires no license.

### Risk Analysis

| Risk | Mitigation |
|---|---|
| Some install scripts use complex PowerShell logic (variables, methods, conditionals) to construct URLs | Multi-level parser: regex for common patterns, PowerShell AST analysis for complex ones, manual URL override as fallback |
| Chocolatey package format could change | Version our parser, test against top 100 packages |
| Community repository may rate-limit downloads | Implement retry with backoff, download throttling, cache nupkg metadata |
| Large packages (>1GB) may be slow to internalize | Show progress, support resume, allow excluding large packages |
| Some software has redistribution restrictions | We download from the same URLs the original package uses — same legal standing as running the package normally. Include license compliance notice. |
| Chocolatey free CLI update breaks `--source` behavior | Pin Chocolatey version in bundled portable install, regression tests |

---

## 🎯 Use Cases

### 1. **"Clone This PC" for Fleet Deployment**

**Scenario**: MSP sets up a reference workstation with all required software. Need to replicate this exact configuration to 30 more workstations at a site with slow internet.

**Workflow**:
1. Open Offline Deployment Panel on the reference PC (internet-connected)
2. Click **"Scan This PC"** — SAK enumerates all installed applications
3. Panel shows all apps with Chocolatey package matches (confidence scores, versions)
4. Technician reviews matches, adjusts any incorrect mappings, deselects unwanted apps
5. Click **"Build Deployment Package"**
6. SAK downloads nupkg files → parses install scripts → downloads actual binaries → internalizes → repacks
7. Progress bar shows: "Internalizing 14/27 packages — Downloading Google Chrome (96.2 MB)..."
8. Output: `StandardOfficePC_2026-03-24/` folder (2.1 GB) with all internalized packages + manifest
9. Copy folder to USB drive or network share
10. On each target PC: Open SAK → Offline Deploy tab → Select package folder → **"Install All"**
11. SAK installs all 27 packages from local source — zero internet required
12. Generates deployment report: "27/27 installed successfully — 0 failures"

**Benefits**:
- One download cycle, unlimited installs
- Exact version consistency across all machines
- Full offline — works in server rooms, factories, ships, classified environments
- Deployment report for documentation/compliance

---

### 2. **Air-Gapped Government/Medical PC Setup**

**Scenario**: Hospital network has strict air-gap policy. New workstations need Chrome, Adobe Reader, 7-Zip, LibreOffice, and specialized medical software.

**Workflow**:
1. On an internet-connected staging PC, open SAK → Offline Deployment
2. Click **"New Package List"** → name it "Hospital_Workstation_Q1_2026"
3. Search and add packages: `googlechrome`, `adobereader`, `7zip`, `libreoffice-fresh`, custom packages
4. For each package, select version (latest or pinned) and review dependencies
5. Click **"Download & Internalize All"**
6. SAK builds the complete deployment package with all binaries embedded
7. Export as ZIP → copy to approved USB media via secure transfer protocol
8. On each air-gapped workstation: SAK → Offline Deploy → Load from USB → Install
9. Verification: SAK checksums every binary against the manifest before installing

**Benefits**:
- Complete chain of custody for software provenance
- Checksum verification prevents tampering
- Manifest documents exact versions for compliance audits
- No internet contact from air-gapped machines whatsoever

---

### 3. **"Download for Later" — Technician's Field Kit**

**Scenario**: Independent technician drives to clients' homes/offices. Wants to pre-download common software so house calls go faster.

**Workflow**:
1. Before heading out, open SAK → Offline Deployment
2. Load curated list: **"Common House Call Software"** (Chrome, Firefox, Malwarebytes, 7-Zip, VLC, etc.)
3. Click **"Build Field Kit"** — Downloads and internalizes ~15 common packages
4. Copy to USB drive alongside SAK Utility portable EXE
5. At client site: Plug in USB → Run SAK → Scan client's PC → See what's outdated
6. Select updates needed → Install from USB (offline packages)
7. For apps not in the field kit, use normal online install (fallback)

**Benefits**:
- House call prep takes 5 minutes instead of hoping client has good internet
- Works even when client's WiFi is broken (the reason for the call!)
- Always has the latest versions pre-cached
- Can update the field kit weekly on a schedule

---

### 4. **Bandwidth-Constrained Branch Office (20 PCs)**

**Scenario**: Remote office with satellite internet (2 Mbps). Need to deploy Microsoft Edge, Teams, Office viewer, and 10 other apps to 20 new PCs.

**Workflow**:
1. At HQ: Build deployment package for "Branch_Office_Standard" (total ~4 GB)
2. Ship on USB or use slow overnight transfer to site NAS
3. On-site tech: Open SAK on each workstation → Offline Deploy → Point to NAS share `\\NAS\deploy_packages\`
4. Install all packages from NAS — LAN speed (Gigabit) instead of satellite
5. Takes ~2 minutes per PC instead of ~4 hours per PC over satellite

**Benefits**:
- Download once at HQ, deploy N times at branch
- Uses LAN bandwidth, not WAN
- NAS share acts as local package repository

---

### 5. **Direct Download Mode — Raw Installers Without Chocolatey**

**Scenario**: Technician needs offline installers for a non-Chocolatey machine (maybe it can't run Chocolatey, or the client doesn't want it).

**Workflow**:
1. Open SAK → Offline Deployment → **"Direct Download"** tab
2. Search for software: "Firefox", "VLC", "Notepad++"
3. SAK fetches Chocolatey package metadata → extracts download URLs → shows: "Firefox Setup 124.0.exe (98 MB) from mozilla.org"
4. Select desired apps → Click **"Download Installers"**
5. SAK downloads the actual EXE/MSI files into a clean folder
6. Result: `Offline_Installers/` folder with: `Firefox Setup 124.0.exe`, `vlc-3.0.20-win64.exe`, `npp.8.6.4.Installer.x64.exe`
7. Carry on USB → double-click installers on target machine — no Chocolatey needed

**Benefits**:
- Even simpler than full deployment packages — just the raw installers
- Works on machines where Chocolatey isn't desired
- Technician doesn't have to hunt vendor websites for download links
- SAK does the URL parsing from Chocolatey package metadata

---

### 6. **Update an Existing Deployment Package**

**Scenario**: The "Standard Office PC" deployment package was built 2 months ago. Need to update it with latest versions.

**Workflow**:
1. Open SAK → Offline Deployment → Load existing package manifest
2. SAK compares each package version against Chocolatey Community Repository
3. Shows: "5 packages have updates available" with version diff (e.g., Chrome 122 → 124)
4. Click **"Update Selected"** — re-downloads only the changed packages
5. Re-internalizes updated packages, leaves unchanged ones alone (incremental update)
6. Updated deployment package is ready

**Benefits**:
- Incremental updates save bandwidth — only re-downloads what changed
- Maintains the curated package list across updates
- Version history preserved in manifest

---

### 7. **Enterprise Software Catalog with Approval Workflow**

**Scenario**: IT department maintains an approved software catalog. Only approved software can be deployed to company PCs.

**Workflow**:
1. IT admin builds a curated package list: "Approved_Enterprise_Software_2026"
2. List includes specific versions locked (e.g., Chrome 122.0.6261.112 — not latest)
3. Exports list as JSON manifest → distributes to field technicians
4. Technicians load the approved manifest → Build deployment package → Deploy
5. Deployment report confirms exact versions match the approved catalog

**Benefits**:
- Central control over what versions are deployed
- Technicians can't accidentally deploy unapproved software
- Audit trail via deployment reports
- JSON manifest is human-readable and version-controllable (Git)

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
OfflineDeploymentPanel (QWidget)
├─ OfflineDeploymentController (QObject)
│  ├─ State: Idle / Scanning / Downloading / Internalizing / Deploying
│  ├─ Manages: All workers and background operations
│  └─ Aggregates: Results for reports
│
├─ PackageInternalizationEngine (QObject) [Core]
│  ├─ NupkgDownloader — Downloads .nupkg via NuGet v2 API
│  ├─ NupkgExtractor — Unzips nupkg, reads nuspec metadata
│  ├─ InstallScriptParser — Parses chocolateyInstall.ps1 for URLs
│  │   ├─ Pattern: Install-ChocolateyPackage -Url '<URL>'
│  │   ├─ Pattern: Install-ChocolateyZipPackage -Url '<URL>'
│  │   ├─ Pattern: Get-ChocolateyWebFile -Url '<URL>'
│  │   ├─ Pattern: Install-ChocolateyInstallPackage -File '<embedded>'
│  │   ├─ Pattern: Variable assignments ($url = '...')
│  │   └─ Fallback: Regex for any URL-like strings (https?://...)
│  ├─ BinaryDownloader — Downloads EXE/MSI/ZIP binaries with resume/retry
│  ├─ ScriptRewriter — Modifies install script to use local paths
│  ├─ NupkgRepacker — Runs `choco pack` to rebuild .nupkg
│  └─ ChecksumVerifier — SHA256 verification of all downloads
│
├─ DeploymentPackageBuilder (QObject) [Worker Thread]
│  ├─ Resolves dependencies (transitive closure)
│  ├─ Downloads and internalizes all packages in queue
│  ├─ Generates package manifest (JSON)
│  ├─ Calculates total size and estimated install time
│  ├─ Exports: Folder / ZIP / ISO (via mkisofs if available)
│  └─ Progress: Per-package and per-binary granularity
│
├─ OfflineInstaller (QObject) [Worker Thread]
│  ├─ Reads deployment package manifest
│  ├─ Verifies checksums before install
│  ├─ Runs `choco install pkg --source=<local_path> -y`
│  ├─ Verifies installation via Registry/AppX scan
│  ├─ Handles: Install order, dependency order, retry logic
│  └─ Generates deployment report
│
├─ DirectDownloader (QObject) [Worker Thread]
│  ├─ Extracts binary URLs from Chocolatey package metadata
│  ├─ Downloads raw EXE/MSI/ZIP files (no Chocolatey packaging)
│  ├─ Organizes into named folders
│  └─ Generates file manifest with checksums
│
├─ PackageListManager (QObject) [Core]
│  ├─ Create / Save / Load / Export / Import curated package lists
│  ├─ Format: JSON with metadata (name, description, date, author)
│  ├─ Version pinning per package
│  ├─ Merge / diff two package lists
│  └─ Built-in presets: "Office PC", "Developer Workstation", "Kiosk", etc.
│
├─ CommunityRepositoryBrowser (QObject)
│  ├─ Search Chocolatey Community Repository (NuGet v2 OData API)
│  ├─ Package detail: description, version history, dependencies, download count
│  ├─ Category filtering
│  ├─ Approved-only filter
│  └─ Caches search results locally
│
└─ DeploymentReportGenerator (QObject)
   ├─ Generates: HTML (printable), JSON (machine-readable), CSV
   ├─ Includes: Package name, version, source, checksum, install status
   ├─ Machine info: hostname, OS version, timestamp
   └─ Summary statistics: total/success/failed/skipped
```

### Relationship to Existing SAK Components

| New Component | Reuses From |
|---|---|
| App scanning (installed apps) | `AppScanner` — `scanRegistry()`, `scanAppX()`, `scanChocolatey()` |
| App → Chocolatey matching | `PackageMatcher` — `findMatchesParallel()` with confidence scoring |
| Chocolatey CLI operations | `ChocolateyManager` — `installPackage()`, `getChocoPath()`, `verifyIntegrity()` |
| Background threading | `WorkerBase` pattern / `QtConcurrent::run()` pattern from `AppInstallationWorker` |
| Install queue + progress | `MigrationReport` + `MigrationJob` pattern from `AppInstallationPanel` |
| Package list persistence | `MigrationReport::exportToJson()` / `importFromJson()` pattern |
| Log output | `LogToggleSwitch` + `DetachableLogWindow` |
| Settings | `ConfigManager` |
| UI theme | `style_constants.h`, `layout_constants.h`, Windows 11 theme |
| Mapping database | `package_mappings.json` (42 hardcoded app→choco mappings) |

---

## 🛠️ Technical Specifications

### NuGet v2 API Integration

The Chocolatey Community Repository is a NuGet v2 OData feed. All access is unauthenticated HTTP.

**Endpoints**:
```
Base URL: https://community.chocolatey.org/api/v2/

Search:
  GET /Search()?$filter=IsLatestVersion&searchTerm='<query>'&targetFramework=''&includePrerelease=false
  Returns: OData XML with package metadata

Package Metadata:
  GET /Packages(Id='<package_id>',Version='<version>')
  Returns: OData XML entry with all metadata

Download Package:
  GET /package/<package_id>/<version>
  Returns: .nupkg file (application/zip)

All Versions:
  GET /FindPackagesById()?id='<package_id>'
  Returns: OData XML feed with all versions
```

**Data Structure — Package Metadata**:
```cpp
struct ChocoPackageMetadata {
    QString package_id;            // "googlechrome"
    QString version;               // "124.0.6367.91"
    QString title;                 // "Google Chrome"
    QString description;
    QString authors;
    QString project_url;           // Vendor website
    QString icon_url;
    QStringList tags;
    QStringList dependencies;      // List of {id, version_range} pairs
    int download_count;
    qint64 package_size_bytes;     // Size of .nupkg
    QString published;             // ISO 8601 datetime
    bool is_approved;
    QString checksum;              // Package hash
    QString license_url;
    QString release_notes;
};
```

**Implementation — NuGet v2 API Client**:
```cpp
class NuGetApiClient : public QObject {
    Q_OBJECT
public:
    explicit NuGetApiClient(QObject* parent = nullptr);

    // Search packages
    void searchPackages(const QString& query, int max_results = 30);

    // Get specific package metadata
    void getPackageMetadata(const QString& package_id,
                            const QString& version = QString());

    // Get all versions of a package
    void getPackageVersions(const QString& package_id);

    // Download .nupkg to local path
    void downloadNupkg(const QString& package_id,
                       const QString& version,
                       const QString& output_dir);

    // Get package dependencies (recursive)
    void resolveDependencies(const QString& package_id,
                             const QString& version);

    void cancel();

Q_SIGNALS:
    void searchComplete(QVector<ChocoPackageMetadata> results);
    void metadataReady(ChocoPackageMetadata metadata);
    void versionsReady(QString package_id, QStringList versions);
    void downloadProgress(QString package_id, qint64 received, qint64 total);
    void downloadComplete(QString package_id, QString local_path);
    void dependenciesResolved(QVector<ChocoPackageMetadata> dependency_tree);
    void errorOccurred(QString context, QString error_message);

private:
    QNetworkAccessManager* m_network_manager;
    std::atomic<bool> m_cancelled{false};

    static constexpr auto kBaseUrl =
        "https://community.chocolatey.org/api/v2/";
    static constexpr int kRequestTimeoutMs = 30000;
    static constexpr int kMaxRetries = 3;

    ChocoPackageMetadata parseODataEntry(const QDomElement& entry);
    QVector<ChocoPackageMetadata> parseODataFeed(const QByteArray& xml);
};
```

---

### Package Internalization Engine

The core innovation — replicating C4B's Package Internalizer using custom code + free Chocolatey CLI.

**Internalization Process (per package)**:

```
1. Download .nupkg from NuGet v2 API
         ↓
2. Extract as ZIP → temp directory
         ↓
3. Read *.nuspec for metadata + dependencies
         ↓
4. Read tools/chocolateyInstall.ps1
         ↓
5. Parse install script for download URLs
   ├─ Install-ChocolateyPackage → -Url, -Url64bit
   ├─ Install-ChocolateyZipPackage → -Url, -Url64bit
   ├─ Get-ChocolateyWebFile → -Url, -Url64bit
   ├─ Variable assignments → $url = '...', $url64 = '...'
   └─ Fallback → any https?://...\.(?:exe|msi|zip|7z) pattern
         ↓
6. Download each binary (with retry, resume, progress)
         ↓
7. Verify checksums (if specified in script: -Checksum, -Checksum64)
         ↓
8. Embed binaries in tools/ folder of extracted package
         ↓
9. Rewrite install script:
   ├─ Replace URL with: "$toolsDir\<filename>"
   ├─ Replace Install-ChocolateyPackage → Install-ChocolateyInstallPackage
   ├─ For ZipPackage: point -Url to local file
   └─ Add -UseOriginalLocation if needed
         ↓
10. Delete _rels/, package/, [Content_Types].xml (NuGet packaging artifacts)
         ↓
11. Run `choco pack <nuspec>` → produces internalized .nupkg
         ↓
12. Store in deployment package directory
```

**Data Structures**:
```cpp
struct ParsedInstallScript {
    struct DownloadResource {
        QString variable_name;       // "$url" or "$url64"
        QString original_url;        // "https://dl.google.com/chrome/..."
        QString filename;            // "ChromeStandalone64.msi"
        QString checksum;            // SHA256 from script (if available)
        QString checksum_type;       // "sha256" / "md5"
        qint64 expected_size;        // -1 if unknown
        bool is_64bit;               // true if this is the 64-bit variant
    };

    enum class InstallerType {
        ChocolateyPackage,           // Install-ChocolateyPackage (downloads + installs)
        ChocolateyZipPackage,        // Install-ChocolateyZipPackage (downloads + extracts)
        ChocolateyInstallPackage,    // Install-ChocolateyInstallPackage (local file)
        ChocolateyWebFile,           // Get-ChocolateyWebFile (download only)
        EmbeddedInstaller,           // Already has binary in the package
        NoDownload,                  // Pure config/meta package
        Unparseable                  // Could not determine — needs manual URL
    };

    InstallerType installer_type;
    QVector<DownloadResource> resources;
    QString raw_script;              // Original script content
    bool fully_parsed;               // true if all URLs were found
    QStringList parse_warnings;      // Issues found during parsing
};

struct InternalizationResult {
    QString package_id;
    QString version;
    bool success;
    QString internalized_nupkg_path;
    qint64 total_download_bytes;     // Total binary downloads
    double download_time_seconds;
    QStringList downloaded_files;
    QString error_message;
    QStringList warnings;
};
```

**Implementation — Install Script Parser**:
```cpp
class InstallScriptParser : public QObject {
    Q_OBJECT
public:
    explicit InstallScriptParser(QObject* parent = nullptr);

    /// Parse a chocolateyInstall.ps1 and extract all download URLs
    ParsedInstallScript parse(const QString& script_content);

    /// Parse with manual URL overrides for unparseable scripts
    ParsedInstallScript parse(const QString& script_content,
                              const QMap<QString, QString>& url_overrides);

private:
    // Pattern matchers in priority order
    QVector<ParsedInstallScript::DownloadResource>
        parseChocolateyPackageCall(const QString& script);

    QVector<ParsedInstallScript::DownloadResource>
        parseChocolateyZipPackageCall(const QString& script);

    QVector<ParsedInstallScript::DownloadResource>
        parseGetChocolateyWebFile(const QString& script);

    QVector<ParsedInstallScript::DownloadResource>
        parseVariableAssignments(const QString& script);

    QVector<ParsedInstallScript::DownloadResource>
        parseFallbackUrls(const QString& script);

    // Extract parameter value from PowerShell function call
    // Handles: -Url 'value', -Url "value", -Url $variable
    QString extractParameter(const QString& call, const QString& param_name);

    // Resolve $variable references in URLs
    QString resolveVariables(const QString& url, const QMap<QString, QString>& vars);

    // Extract checksum parameters
    QPair<QString, QString> extractChecksum(const QString& call,
                                             bool is_64bit);
};
```

**Key Regex Patterns for Script Parsing**:
```cpp
// Install-ChocolateyPackage with named or positional parameters
// Matches: Install-ChocolateyPackage 'name' 'type' 'url' 'url64'
// Matches: Install-ChocolateyPackage -PackageName 'name' -Url 'url'
static const QRegularExpression kInstallChocolateyPackageRe(
    R"(Install-ChocolateyPackage\s+)"
    R"((?:-\w+\s+)?)"                          // optional param name
    R"(['"]([^'"]+)['"])"                       // package name
    R"(\s+(?:-\w+\s+)?['"]([^'"]+)['"])"       // installer type
    R"(\s+(?:-\w+\s+)?['"]([^'"]+)['"])"       // url
    R"((?:\s+(?:-\w+\s+)?['"]([^'"]+)['"])?)", // url64 (optional)
    QRegularExpression::CaseInsensitiveOption |
    QRegularExpression::MultilineOption);

// Named parameter extraction (more reliable)
// Matches: -Url 'https://...' or -Url "https://..."
static const QRegularExpression kNamedUrlParamRe(
    R"(-Url(?:64(?:bit)?)?[\s=]+['"]([^'"]+)['"])",
    QRegularExpression::CaseInsensitiveOption);

// Variable assignment: $url = 'https://...'
static const QRegularExpression kVariableUrlRe(
    R"(\$(\w*url\w*)\s*=\s*['"]([^'"]+)['"])",
    QRegularExpression::CaseInsensitiveOption);

// Checksum parameter: -Checksum 'abc123' or -Checksum64 'def456'
static const QRegularExpression kChecksumParamRe(
    R"(-Checksum(?:64)?[\s=]+['"]([^'"]+)['"])",
    QRegularExpression::CaseInsensitiveOption);

// Checksum type: -ChecksumType 'sha256'
static const QRegularExpression kChecksumTypeRe(
    R"(-ChecksumType(?:64)?[\s=]+['"]([^'"]+)['"])",
    QRegularExpression::CaseInsensitiveOption);

// Fallback: any URL that looks like a binary download
static const QRegularExpression kFallbackUrlRe(
    R"((https?://[^\s'"<>]+\.(?:exe|msi|msix|zip|7z|gz|tar|dmg|pkg)))",
    QRegularExpression::CaseInsensitiveOption);
```

**Implementation — Script Rewriter**:
```cpp
class ScriptRewriter : public QObject {
    Q_OBJECT
public:
    explicit ScriptRewriter(QObject* parent = nullptr);

    struct RewriteResult {
        QString rewritten_script;
        bool success;
        QStringList changes_made;    // Human-readable list of modifications
        QString error_message;
    };

    /// Rewrite install script to use local embedded resources
    RewriteResult rewrite(
        const QString& original_script,
        const QVector<ParsedInstallScript::DownloadResource>& resources,
        const QMap<QString, QString>& local_file_paths);

private:
    // Replace Install-ChocolateyPackage → Install-ChocolateyInstallPackage
    // with -File pointing to local binary
    QString rewriteChocolateyPackageCall(
        const QString& script,
        const QMap<QString, QString>& url_to_local_path);

    // Replace URL in Install-ChocolateyZipPackage with local path
    QString rewriteZipPackageCall(
        const QString& script,
        const QMap<QString, QString>& url_to_local_path);

    // Replace URLs in variable assignments
    QString rewriteVariableAssignments(
        const QString& script,
        const QMap<QString, QString>& url_to_local_path);

    // Add $toolsDir at top if not present
    QString ensureToolsDirVariable(const QString& script);
};
```

---

### Binary Downloader

**Purpose**: Robust HTTP downloader for software binaries with resume, retry, and verification.

```cpp
class BinaryDownloader : public QObject {
    Q_OBJECT
public:
    explicit BinaryDownloader(QNetworkAccessManager* nam,
                              QObject* parent = nullptr);

    struct DownloadRequest {
        QString url;
        QString output_path;
        QString expected_checksum;       // Empty if unknown
        QString checksum_type;           // "sha256" (default), "md5"
        qint64 expected_size;            // -1 if unknown
        bool allow_resume;               // Resume partial downloads
    };

    struct DownloadResult {
        bool success;
        QString local_path;
        qint64 bytes_downloaded;
        double elapsed_seconds;
        QString actual_checksum;         // SHA256 of downloaded file
        bool checksum_verified;          // true if matched expected
        QString error_message;
        int http_status_code;
    };

    /// Download a single binary
    void download(const DownloadRequest& request);

    /// Download multiple binaries (sequential with global progress)
    void downloadBatch(const QVector<DownloadRequest>& requests);

    void cancel();

Q_SIGNALS:
    void progress(QString url, qint64 received, qint64 total);
    void downloadComplete(DownloadResult result);
    void batchProgress(int completed, int total, qint64 total_bytes);
    void batchComplete(QVector<DownloadResult> results);
    void errorOccurred(QString url, QString error);

private:
    QNetworkAccessManager* m_network_manager;
    std::atomic<bool> m_cancelled{false};

    static constexpr int kMaxRetries = 3;
    static constexpr int kRetryDelayBaseMs = 2000;
    static constexpr int kDownloadTimeoutMs = 300000; // 5 minutes
    static constexpr qint64 kMaxBinarySize = 4LL * 1024 * 1024 * 1024; // 4 GB

    DownloadResult downloadWithRetry(const DownloadRequest& request);
    QString computeChecksum(const QString& file_path,
                            const QString& algorithm);
};
```

---

### Deployment Package Format

A deployment package is a self-contained directory (or ZIP) with this structure:

```
DeploymentPackage_<name>_<date>/
├─ manifest.json                    — Package list, versions, checksums, metadata
├─ packages/                        — Internalized .nupkg files
│  ├─ googlechrome.124.0.6367.91.nupkg
│  ├─ 7zip.24.05.nupkg
│  ├─ adobereader.2024.002.20759.nupkg
│  ├─ firefox.124.0.nupkg
│  ├─ chocolatey-core.extension.1.4.0.nupkg  (dependency)
│  └─ ...
├─ installers/                      — Raw installer binaries (Direct Download mode only)
│  ├─ ChromeStandalone64.msi
│  ├─ Firefox Setup 124.0.exe
│  └─ ...
├─ logs/                            — Build and deployment logs
│  └─ build_2026-03-24T14-30-00.log
└─ README.txt                       — Human-readable deployment instructions
```

**Manifest Format**:
```json
{
    "manifest_version": "1.0",
    "package_name": "Standard Office PC",
    "description": "Standard software set for office workstations",
    "created_at": "2026-03-24T14:30:00Z",
    "created_by": "SAK Utility v0.9.0",
    "source_machine": "TECH-PC-001",
    "source_os": "Windows 11 Pro 23H2",
    "total_packages": 15,
    "total_size_bytes": 2147483648,
    "packages": [
        {
            "package_id": "googlechrome",
            "version": "124.0.6367.91",
            "title": "Google Chrome",
            "nupkg_file": "packages/googlechrome.124.0.6367.91.nupkg",
            "nupkg_checksum_sha256": "a1b2c3d4...",
            "internalized": true,
            "embedded_binaries": [
                {
                    "filename": "ChromeStandalone64.msi",
                    "original_url": "https://dl.google.com/dl/chrome/install/googlechromestandaloneenterprise64.msi",
                    "size_bytes": 100925440,
                    "checksum_sha256": "e5f6a7b8..."
                }
            ],
            "dependencies": ["chocolatey-core.extension"],
            "install_order": 2,
            "version_pinned": false,
            "tags": ["browser", "web"],
            "approved": true
        }
    ],
    "dependency_packages": [
        {
            "package_id": "chocolatey-core.extension",
            "version": "1.4.0",
            "nupkg_file": "packages/chocolatey-core.extension.1.4.0.nupkg",
            "nupkg_checksum_sha256": "f9e8d7c6...",
            "is_dependency_only": true,
            "install_order": 1
        }
    ],
    "install_order": [
        "chocolatey-core.extension",
        "googlechrome",
        "7zip",
        "adobereader",
        "firefox"
    ]
}
```

---

### Offline Installer

**Purpose**: Install packages from a deployment package on an air-gapped machine.

```cpp
class OfflineInstaller : public QObject {
    Q_OBJECT
public:
    explicit OfflineInstaller(ChocolateyManager* choco_manager,
                              QObject* parent = nullptr);

    struct InstallConfig {
        QString package_dir;             // Path to packages/ folder
        bool verify_checksums;           // Verify before install (default: true)
        bool skip_dependencies;          // Skip deps if already installed
        bool force_reinstall;            // Reinstall even if present
        int timeout_per_package_sec;     // Timeout per install (default: 600)
        int max_retries;                 // Retries per package (default: 2)
    };

    struct InstallResult {
        QString package_id;
        QString version;
        bool success;
        bool was_already_installed;
        bool checksum_verified;
        double install_time_seconds;
        QString error_message;
    };

    /// Load and validate a deployment package manifest
    std::expected<QJsonDocument, QString>
        loadManifest(const QString& manifest_path);

    /// Verify all checksums in deployment package
    void verifyPackageIntegrity(const QString& package_dir);

    /// Install all packages from manifest
    void installFromManifest(const QString& manifest_path,
                             const InstallConfig& config);

    /// Install specific packages from deployment package
    void installSelected(const QString& package_dir,
                         const QStringList& package_ids,
                         const InstallConfig& config);

    void cancel();

Q_SIGNALS:
    void verifyProgress(int checked, int total, QString current_package);
    void verifyComplete(int passed, int failed, QStringList failed_packages);
    void installStarted(int total_packages);
    void packageInstallStarted(QString package_id, int current, int total);
    void packageInstallComplete(InstallResult result);
    void installComplete(QVector<InstallResult> results);
    void errorOccurred(QString context, QString error);

private:
    ChocolateyManager* m_choco_manager;
    std::atomic<bool> m_cancelled{false};

    // Install using: choco install <pkg> --source=<local_dir> -y --no-progress
    InstallResult installSinglePackage(
        const QString& package_id,
        const QString& version,
        const QString& source_dir,
        const InstallConfig& config);

    bool verifyNupkgChecksum(const QString& nupkg_path,
                              const QString& expected_sha256);

    QStringList resolveInstallOrder(const QJsonDocument& manifest);
};
```

---

### Deployment Package Builder

**Purpose**: Orchestrate the entire build process — scan, match, download, internalize, package.

```cpp
class DeploymentPackageBuilder : public QObject {
    Q_OBJECT
public:
    explicit DeploymentPackageBuilder(
        NuGetApiClient* api_client,
        PackageInternalizationEngine* internalization_engine,
        PackageMatcher* package_matcher,
        QObject* parent = nullptr);

    struct BuildConfig {
        QString package_name;            // "Standard Office PC"
        QString description;
        QString output_directory;        // Where to save the package
        bool internalize_packages;       // true = full offline, false = nupkg only
        bool include_raw_installers;     // Also save EXE/MSI alongside nupkg
        bool include_dependencies;       // Resolve and include all deps
        bool compress_to_zip;            // ZIP the final package
        int max_concurrent_downloads;    // Parallel download limit (default: 3)
    };

    struct BuildProgress {
        int total_packages;
        int completed_packages;
        int failed_packages;
        QString current_package;
        QString current_phase;           // "Downloading", "Internalizing", "Packing"
        qint64 total_bytes_downloaded;
        double elapsed_seconds;
    };

    /// Build from a list of explicit package IDs + versions
    void buildFromPackageList(
        const QVector<QPair<QString, QString>>& packages, // {id, version}
        const BuildConfig& config);

    /// Build from app scan results (scan PC → match → build)
    void buildFromScanResults(
        const std::vector<AppScanner::AppInfo>& apps,
        const std::vector<PackageMatcher::MatchResult>& matches,
        const BuildConfig& config);

    /// Update an existing deployment package (incremental)
    void updateExistingPackage(const QString& manifest_path,
                                const BuildConfig& config);

    void cancel();

Q_SIGNALS:
    void buildStarted(int total_packages);
    void buildProgress(BuildProgress progress);
    void packageInternalized(InternalizationResult result);
    void buildComplete(QString manifest_path, int success, int failed);
    void buildFailed(QString error_message);

private:
    NuGetApiClient* m_api_client;
    PackageInternalizationEngine* m_engine;
    PackageMatcher* m_matcher;
    std::atomic<bool> m_cancelled{false};

    void generateManifest(const QString& output_dir,
                           const BuildConfig& config,
                           const QVector<InternalizationResult>& results);
    void generateReadme(const QString& output_dir,
                         const BuildConfig& config);
};
```

---

### Curated Package Lists (Presets)

```cpp
class PackageListManager : public QObject {
    Q_OBJECT
public:
    explicit PackageListManager(QObject* parent = nullptr);

    struct PackageEntry {
        QString package_id;
        QString version;                 // Empty = latest
        bool version_pinned;             // Lock to this exact version
        QString category;                // "Browser", "Utility", "Development"
        QString notes;                   // Technician notes
    };

    struct PackageList {
        QString name;
        QString description;
        QString author;
        QDateTime created_at;
        QDateTime updated_at;
        QVector<PackageEntry> packages;
    };

    // CRUD operations
    void createList(const PackageList& list);
    void saveList(const PackageList& list, const QString& file_path);
    PackageList loadList(const QString& file_path);
    void deleteList(const QString& file_path);

    // Built-in presets
    static PackageList presetOfficePC();
    static PackageList presetDeveloperWorkstation();
    static PackageList presetKiosk();
    static PackageList presetSchoolLab();
    static PackageList presetRemoteWorker();

    // Operations
    PackageList mergeWithScanResults(
        const PackageList& base_list,
        const std::vector<AppScanner::AppInfo>& scan_results,
        const std::vector<PackageMatcher::MatchResult>& matches);

    PackageList diffPackageLists(const PackageList& list_a,
                                  const PackageList& list_b);

Q_SIGNALS:
    void listLoaded(PackageList list);
    void listSaved(QString file_path);
    void errorOccurred(QString error);
};
```

**Built-in Preset Example — "Standard Office PC"**:
```cpp
PackageList PackageListManager::presetOfficePC() {
    return PackageList{
        .name = "Standard Office PC",
        .description = "Common software for a typical office workstation",
        .author = "SAK Utility Built-in",
        .created_at = QDateTime::currentDateTimeUtc(),
        .updated_at = QDateTime::currentDateTimeUtc(),
        .packages = {
            {"googlechrome", "", false, "Browser", ""},
            {"firefox", "", false, "Browser", ""},
            {"adobereader", "", false, "Productivity", ""},
            {"7zip", "", false, "Utility", ""},
            {"vlc", "", false, "Media", ""},
            {"notepadplusplus", "", false, "Utility", ""},
            {"greenshot", "", false, "Utility", "Screenshot tool"},
            {"foxitreader", "", false, "Productivity", "Alternative PDF reader"},
            {"libreoffice-fresh", "", false, "Productivity", ""},
            {"treesizefree", "", false, "Utility", "Disk usage analyzer"},
            {"everything", "", false, "Utility", "Fast file search"},
            {"windirstat", "", false, "Utility", "Disk usage visualizer"},
            {"putty", "", false, "Networking", "SSH client"},
            {"winscp", "", false, "Networking", "SFTP client"},
        }
    };
}
```

---

## 🖥️ UI Design

### Panel Layout

```
┌──────────────────────────────────────────────────────────────────────┐
│ Offline Deployment Package Manager                            [≡] [↗]│
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────────┐ │
│  │ [📥 Build Package] [📤 Deploy Offline] [⬇ Direct Download]    │ │
│  │ [📋 Package Lists] [🔍 Browse Repository]                      │ │
│  └─────────────────────────────────────────────────────────────────┘ │
│                                                                      │
│  ═══════════════════════════════════════════════════════════════════  │
│                                                                      │
│  BUILD PACKAGE TAB:                                                  │
│  ┌───────────────────────────────────────────────────────────────┐   │
│  │ Source:  ○ Scan This PC   ○ Load Package List   ○ Manual Add  │   │
│  │                                                               │   │
│  │ [Scan Now]  Package List: [Standard Office PC    ▼] [Load]    │   │
│  └───────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌──┬────────────────────┬───────────┬──────────┬────────┬───────┐  │
│  │☑ │ Application        │ Choco Pkg │ Version  │ Match  │ Size  │  │
│  ├──┼────────────────────┼───────────┼──────────┼────────┼───────┤  │
│  │☑ │ Google Chrome       │ googlechr │ 124.0... │ ██████ │ 96 MB │  │
│  │☑ │ 7-Zip               │ 7zip      │ 24.05    │ ██████ │ 2 MB  │  │
│  │☑ │ Adobe Reader         │ adoberead │ 2024.002 │ █████░ │ 262MB │  │
│  │☐ │ Visual Studio Code   │ vscode    │ 1.87.2   │ ██████ │ 95 MB │  │
│  │☑ │ Firefox              │ firefox   │ 124.0    │ ██████ │ 56 MB │  │
│  │⚠ │ Custom App X         │ (none)    │   —      │ ░░░░░░ │  —    │  │
│  └──┴────────────────────┴───────────┴──────────┴────────┴───────┘  │
│                                                                      │
│  Selected: 12 packages  │  Total est. size: 1.8 GB  │ Deps: +3      │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ Package Name: [Standard Office PC______________]               │  │
│  │ Output:       [C:\DeployPackages\    ] [Browse]                │  │
│  │ Options: ☑ Internalize (full offline)  ☑ Include dependencies  │  │
│  │          ☐ Include raw installers       ☐ Compress to ZIP      │  │
│  │                                                               │  │
│  │                          [▶ Build Deployment Package]          │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ Progress: [████████████████░░░░░░░░░░░░░░] 54%                │  │
│  │ Internalizing: firefox (5 of 12)                               │  │
│  │ Downloading: Firefox Setup 124.0.exe — 34.2 / 56.0 MB         │  │
│  │ Speed: 12.4 MB/s  │  ETA: ~2 min                              │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  ═══════════════════════════════════════════════════════════════════  │
│  [Log ▾]                                                             │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ 14:30:01 [INFO] Scanning installed applications...             │  │
│  │ 14:30:03 [INFO] Found 47 installed applications                │  │
│  │ 14:30:04 [INFO] Matched 38/47 to Chocolatey packages          │  │
│  │ 14:30:10 [INFO] Downloading googlechrome.124.0.6367.91.nupkg  │  │
│  │ 14:30:12 [INFO] Parsing install script for googlechrome...     │  │
│  │ 14:30:12 [INFO] Found URL: ChromeStandalone64.msi (96 MB)     │  │
│  │ 14:30:45 [INFO] Binary downloaded, checksum verified ✓         │  │
│  │ 14:30:46 [INFO] Script rewritten, repacking...                 │  │
│  │ 14:30:47 [INFO] ✓ googlechrome internalized (96.2 MB)         │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

### Deploy Offline Tab

```
┌──────────────────────────────────────────────────────────────────────┐
│  DEPLOY OFFLINE TAB:                                                 │
│  ┌───────────────────────────────────────────────────────────────┐   │
│  │ Package Source: [C:\USB\DeployPackage_Office\     ] [Browse]   │   │
│  │                                                 [Load Manifest]│   │
│  └───────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ Package: "Standard Office PC" (15 packages, 2.1 GB)            │  │
│  │ Built: 2026-03-24  Source: TECH-PC-001  By: SAK v0.9.0        │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  ┌──┬────────────────────┬──────────┬──────────┬──────────────────┐ │
│  │☑ │ Package            │ Version  │ Status   │ Action           │ │
│  ├──┼────────────────────┼──────────┼──────────┼──────────────────┤ │
│  │☑ │ Google Chrome       │ 124.0... │ Not inst │ Will install     │ │
│  │☑ │ 7-Zip               │ 24.05    │ Older    │ Will upgrade     │ │
│  │☐ │ Adobe Reader         │ 2024.002 │ Current  │ Already current  │ │
│  │☑ │ Firefox              │ 124.0    │ Not inst │ Will install     │ │
│  └──┴────────────────────┴──────────┴──────────┴──────────────────┘ │
│                                                                      │
│  ☑ Verify checksums before install   ☑ Skip already-current          │
│  ☐ Force reinstall all                                               │
│                                                                      │
│  [✓ Verify Package Integrity]  [▶ Install Selected]                  │
│                                                                      │
│  ┌────────────────────────────────────────────────────────────────┐  │
│  │ Verification: 15/15 packages passed checksum ✓                 │  │
│  │ Installing: 12 packages (3 skipped — already current)          │  │
│  │ Progress: [██████████████████████░░░░░░░░] 73%                │  │
│  │ Installing: Firefox 124.0 (9 of 12)...                         │  │
│  └────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  [📄 Generate Deployment Report]                                     │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 📦 Chocolatey Paid vs Our Implementation — Feature Comparison

This section details exactly what C4B's paid features do and how we replicate or improve upon them.

### Package Internalizer (C4B — $17k+/year)

**What C4B does**: `choco download pkg --internalize` downloads the nupkg, uses PowerShell AST to find all `Install-ChocolateyPackage` / `Get-ChocolateyWebFile` calls, downloads the referenced binaries, edits the scripts to point locally, and repacks.

**What SAK does (our free implementation)**:

| C4B Feature | SAK Implementation |
|---|---|
| Parse install scripts | `InstallScriptParser` — regex-based multi-pattern parser with variable resolution |
| Download binaries | `BinaryDownloader` — HTTP with resume, retry, checksum verification |
| Rewrite scripts | `ScriptRewriter` — URL replacement, function call rewriting |
| Repack nupkg | `choco pack` (free CLI) via `ChocolateyManager` |
| Dependency resolution | `NuGetApiClient::resolveDependencies()` — recursive NuGet v2 API walk |
| `--internalize-all-urls` | `parseFallbackUrls()` — regex for any binary URL in script |
| `--resources-location` | Not needed — we always embed in the package or keep alongside |
| `--append-use-original-location` | `ScriptRewriter` adds flag when appropriate |

**Where SAK improves over C4B**:
- **Visual progress** — C4B is CLI-only; SAK shows per-package, per-binary progress with speed/ETA
- **Batch management** — SAK's GUI lets you curate package lists visually, not via CLI flags
- **PC Scan integration** — C4B doesn't scan a PC and match apps; you have to know the package names
- **Deployment manifest** — C4B produces loose nupkg files; SAK creates a manifest with checksums, metadata, install order
- **Direct Download mode** — C4B has no equivalent; SAK can extract just the raw EXE/MSI for non-Chocolatey use
- **Integrated with SAK's existing App Installation** — Deploy packages flow right into SAK's batch installer

### CDN Download Cache (Pro+ — $120/year)

**What Pro+ does**: Caches binary downloads on Chocolatey's private CDN. If the vendor download URL goes down, you can still install because the CDN has a copy.

**SAK equivalent**: Our internalization approach is **better** than CDN cache because:
- CDN cache still requires internet at install time — just from a different server
- Our internalized packages work with **zero internet** — binaries are embedded
- CDN cache only works for packages the CDN has cached — our approach works for any package

### Package Builder (C4B)

**What C4B does**: Point it at an EXE/MSI and it auto-creates a Chocolatey package.

**SAK equivalent** (future enhancement): Could add a "Create Package from Installer" feature that:
1. Takes an EXE/MSI file
2. Detects installer type (InnoSetup, NSIS, MSI, InstallShield)
3. Generates nuspec + chocolateyInstall.ps1 with appropriate silent flags
4. Packs into nupkg

This is a natural extension but **not in scope for v1.0** of this feature.

---

## 🧪 Testing Strategy

### Unit Tests

```
tests/unit/test_install_script_parser.cpp
tests/unit/test_script_rewriter.cpp
tests/unit/test_nuget_api_client.cpp
tests/unit/test_deployment_manifest.cpp
tests/unit/test_package_list_manager.cpp
tests/unit/test_offline_installer.cpp
tests/unit/test_binary_downloader.cpp
tests/unit/test_checksum_verifier.cpp
```

**Test Categories**:

| Test File | What It Tests |
|---|---|
| `test_install_script_parser` | Parse `Install-ChocolateyPackage` calls, variable URLs, positional params, named params, 64-bit URLs, checksums, Install-ChocolateyZipPackage, Get-ChocolateyWebFile, fallback URL detection, scripts with no downloads, scripts with complex variable construction |
| `test_script_rewriter` | URL replacement in scripts, function call rewriting (Install-ChocolateyPackage → Install-ChocolateyInstallPackage), variable replacement, $toolsDir injection, multi-URL scripts, idempotent rewrites |
| `test_nuget_api_client` | OData XML parsing (search results, single entry, version list), URL construction, error handling (404, timeout, malformed XML), pagination |
| `test_deployment_manifest` | JSON serialization/deserialization, install order resolution, checksum format, backward compatibility, edge cases (empty manifest, single package) |
| `test_package_list_manager` | Create/save/load/delete lists, preset generation, merge with scan results, diff two lists, JSON format validation |
| `test_offline_installer` | Manifest loading, checksum verification, install command construction, skip-already-installed logic, dependency ordering, cancellation |
| `test_binary_downloader` | Download with checksum verification, resume partial download, retry on failure, timeout handling, cancellation |
| `test_checksum_verifier` | SHA256 computation, MD5 computation, file not found, large file handling |

### Install Script Parser — Critical Test Cases

The parser must handle the real-world diversity of Chocolatey package scripts:

```cpp
class TestInstallScriptParser : public QObject {
    Q_OBJECT
private Q_SLOTS:

    // --- Happy path ---
    void testSimpleInstallChocolateyPackage();
    // Install-ChocolateyPackage 'chrome' 'msi' 'https://example.com/setup.msi'

    void testNamedParameters();
    // Install-ChocolateyPackage -PackageName 'chrome'
    //   -FileType 'msi' -Url 'https://example.com/setup.msi'
    //   -Url64bit 'https://example.com/setup64.msi'

    void testVariableUrls();
    // $url = 'https://example.com/setup.exe'
    // $url64 = 'https://example.com/setup64.exe'
    // Install-ChocolateyPackage ... -Url $url -Url64bit $url64

    void testZipPackage();
    // Install-ChocolateyZipPackage 'name' 'https://example.com/archive.zip' $toolsDir

    void testGetChocolateyWebFile();
    // Get-ChocolateyWebFile -FileName 'setup.exe' -Url 'https://...'

    void testWithChecksums();
    // -Checksum 'abc123' -ChecksumType 'sha256'
    // -Checksum64 'def456' -ChecksumType64 'sha256'

    void testMultipleDownloadsInOneScript();
    // Some packages download multiple files (e.g., a main installer + a plugin)

    // --- Edge cases ---
    void testEmbeddedPackageNoUrls();
    // Package already has the installer embedded — no URLs to find

    void testMetaPackageNoScript();
    // Package with no chocolateyInstall.ps1 (dependency-only meta package)

    void testSplatting();
    // $packageArgs = @{ Url = 'https://...' ; Url64bit = 'https://...' }
    // Install-ChocolateyPackage @packageArgs

    void testHereStringUrls();
    // $url = @"
    // https://example.com/setup.exe
    // "@

    void testConditionalUrls();
    // if ($env:PROCESSOR_ARCHITECTURE -eq 'AMD64') { $url = '...' } else { $url = '...' }

    void testUrlsWithVariableInterpolation();
    // $version = '1.2.3'
    // $url = "https://example.com/app-${version}-setup.exe"

    void testInstallChocolateyInstallPackage();
    // Already using local file — nothing to internalize

    // --- Error cases ---
    void testEmptyScript();
    void testScriptWithNoChocolateyFunctions();
    void testMalformedPowerShell();
    void testUrlsThatReturn404();
};
```

### Integration Tests

```
tests/integration/test_package_internalization_e2e.cpp
tests/integration/test_offline_deploy_e2e.cpp
```

| Test | What It Validates |
|---|---|
| End-to-end internalization | Download a known small package (e.g., `notepadplusplus.commandline`), internalize it, verify the nupkg contains the binary, install from local source |
| Offline deploy cycle | Build a deployment package → verify manifest → install on same machine from local source → verify installation |

---

## 📁 New Files

### Headers (`include/sak/`)
```
nuget_api_client.h
install_script_parser.h
script_rewriter.h
binary_downloader.h
package_internalization_engine.h
deployment_package_builder.h
offline_installer.h
package_list_manager.h
deployment_report_generator.h
offline_deployment_panel.h
offline_deployment_controller.h
offline_deployment_constants.h
```

### Sources (`src/`)
```
src/core/nuget_api_client.cpp
src/core/install_script_parser.cpp
src/core/script_rewriter.cpp
src/core/binary_downloader.cpp
src/core/package_internalization_engine.cpp
src/core/deployment_package_builder.cpp
src/core/offline_installer.cpp
src/core/package_list_manager.cpp
src/core/deployment_report_generator.cpp
src/gui/offline_deployment_panel.cpp
src/gui/offline_deployment_panel_table.cpp
src/gui/offline_deployment_panel_actions.cpp
src/gui/offline_deployment_controller.cpp
```

### Tests (`tests/`)
```
tests/unit/test_install_script_parser.cpp
tests/unit/test_script_rewriter.cpp
tests/unit/test_nuget_api_client.cpp
tests/unit/test_deployment_manifest.cpp
tests/unit/test_package_list_manager.cpp
tests/unit/test_offline_installer.cpp
tests/unit/test_binary_downloader.cpp
tests/unit/test_checksum_verifier.cpp
tests/integration/test_package_internalization_e2e.cpp
tests/integration/test_offline_deploy_e2e.cpp
```

### Resources
```
resources/presets/office_pc.json
resources/presets/developer_workstation.json
resources/presets/kiosk.json
resources/presets/school_lab.json
resources/presets/remote_worker.json
```

---

## 🔒 Security Considerations

### Download Integrity
- All binary downloads verified via SHA256 checksum (when available from package metadata)
- All nupkg downloads verified via NuGet package hash
- Manifest stores checksums for every file — verification before offline install
- Support for HTTPS-only download policy (reject HTTP URLs)

### Script Execution Safety
- SAK **never directly executes** the chocolateyInstall.ps1 scripts — that's Chocolatey's job
- SAK only **parses** scripts to extract URLs — no eval, no PowerShell invocation during internalization
- Rewritten scripts are reviewed by the `ScriptRewriter` for correctness before packing

### Distribution Rights
- SAK downloads software from the same URLs the Chocolatey package authors specified
- No redistribution occurs — downloads are for the technician's own use
- Include license compliance notice in deployment README
- Do not host a public mirror — packages are for offline use only

### Input Validation
- Validate all NuGet API responses (OData XML) against expected schema
- Sanitize package IDs and versions (alphanumeric + dots + hyphens only)
- Validate file paths to prevent directory traversal
- Maximum binary size limit (configurable, default 4 GB)
- Timeout on all HTTP operations

---

## 🗓️ Implementation Phases

### Phase 1: Core Engine (Foundation)
- [ ] `NuGetApiClient` — Search, metadata, download nupkg
- [ ] `InstallScriptParser` — Parse chocolateyInstall.ps1 for URLs
- [ ] `BinaryDownloader` — HTTP download with retry/checksum
- [ ] `ScriptRewriter` — Rewrite URLs to local paths
- [ ] `PackageInternalizationEngine` — Orchestrate the full internalization pipeline
- [ ] Unit tests for all parser patterns
- [ ] Integration test: internalize 1 known package end-to-end

### Phase 2: Package Management
- [ ] `DeploymentPackageBuilder` — Build complete deployment packages
- [ ] `PackageListManager` — Curated lists, presets, save/load
- [ ] Manifest format (JSON) with checksums and install order
- [ ] Dependency resolution (transitive)
- [ ] Incremental updates (update existing package)
- [ ] Unit tests for manifest, lists, deps

### Phase 3: Offline Deployment
- [ ] `OfflineInstaller` — Install from deployment package
- [ ] Checksum verification before install
- [ ] Install order resolution (dependencies first)
- [ ] Deployment report generation (HTML/JSON)
- [ ] Skip already-installed packages
- [ ] Integration test: full build → verify → deploy cycle

### Phase 4: GUI Panel
- [ ] `OfflineDeploymentPanel` — Tab-based UI (Build / Deploy / Direct Download / Lists / Browse)
- [ ] `OfflineDeploymentController` — State management, signal routing
- [ ] PC scan integration (AppScanner + PackageMatcher)
- [ ] Progress display with per-package/per-binary granularity
- [ ] Log output with `LogToggleSwitch`

### Phase 5: Direct Download & Polish
- [ ] Direct Download mode (raw EXE/MSI without Chocolatey packaging)
- [ ] Repository browser with search/filter
- [ ] ZIP export for deployment packages
- [ ] Built-in preset package lists
- [ ] Enterprise features: version pinning UI, approval workflow JSON export
- [ ] Error recovery: resume failed builds, skip individual failures

---

## 📐 Constants

```cpp
// offline_deployment_constants.h

namespace sak::offline {

// NuGet API
constexpr auto kNuGetBaseUrl = "https://community.chocolatey.org/api/v2/";
constexpr int kApiRequestTimeoutMs = 30000;
constexpr int kApiMaxRetries = 3;
constexpr int kApiRetryDelayBaseMs = 2000;
constexpr int kSearchMaxResults = 50;

// Downloads
constexpr int kDownloadTimeoutMs = 300000;      // 5 minutes per binary
constexpr int kDownloadMaxRetries = 3;
constexpr int kDownloadRetryDelayBaseMs = 3000;
constexpr int kMaxConcurrentDownloads = 3;
constexpr qint64 kMaxBinarySizeBytes = 4LL * 1024 * 1024 * 1024; // 4 GB
constexpr int kDownloadBufferSize = 65536;       // 64 KB read buffer

// Internalization
constexpr int kMaxPackagesPerBuild = 200;
constexpr int kPackTimeoutMs = 60000;            // choco pack timeout
constexpr int kInstallTimeoutPerPackageMs = 600000; // 10 min per package install

// Checksums
constexpr auto kDefaultChecksumAlgorithm = "sha256";

// Deployment manifest
constexpr auto kManifestVersion = "1.0";
constexpr auto kManifestFilename = "manifest.json";
constexpr auto kPackagesSubdir = "packages";
constexpr auto kInstallersSubdir = "installers";
constexpr auto kLogsSubdir = "logs";
constexpr auto kReadmeFilename = "README.txt";

// UI
constexpr int kProgressUpdateIntervalMs = 250;
constexpr int kScanTimeoutMs = 60000;

// Package list presets
constexpr int kMaxPackageListEntries = 500;
constexpr int kMaxPackageListNameLength = 100;

} // namespace sak::offline
```

---

## 🔄 Comparison with Alternatives

### Why Not Just Use Chocolatey Business?

| Factor | Chocolatey C4B | SAK Utility |
|---|---|---|
| **Cost** | ~$17,000/year (organization) | Free (included with SAK) |
| **License** | Per-machine or named user | Portable, no licensing |
| **UI** | CLI only (unless you buy Chocolatey GUI Licensed) | Full GUI with progress, previews, reports |
| **PC Scan → Package Build** | Not available | Scan apps → auto-match → build package |
| **Deployment Manifest** | None — just loose .nupkg files | JSON manifest with checksums, metadata, install order |
| **Direct Download** | Not available | Extract raw EXE/MSI for non-Chocolatey use |
| **Curated Lists** | Manual CLI package list | Visual package list editor with presets |
| **Portable** | Requires Chocolatey installation or portable setup | Single EXE with bundled Chocolatey |
| **Verification** | Basic checksum in package | Full manifest checksum verification before deploy |

### Why Not Use winget?

- winget has no offline/internalization story as of March 2026
- winget's package repository is less mature than Chocolatey's 12,000+ packages
- winget has no portable mode — requires Windows Package Manager service
- winget doesn't support custom local repositories (file share as source)
- SAK already has deep Chocolatey integration (ChocolateyManager, PackageMatcher, etc.)

### Why Not Download Installers Manually from Vendor Sites?

- Time: Finding the correct download page for 20+ apps takes 30+ minutes
- Errors: Wrong architecture (x86 vs x64), wrong version, wrong edition
- Consistency: Each technician downloads different versions at different times
- No automation: Manual double-clicking each installer, answering prompts
- No verification: No checksums, no install validation
- No reporting: No record of what was installed or what version

SAK's approach automates all of this with checksums, manifests, and reports.

---

## 🎯 Success Metrics

| Metric | Target |
|---|---|
| Packages successfully internalized | ≥90% of top 100 Chocolatey packages |
| Install script parsing accuracy | ≥95% URL extraction rate |
| Offline install success rate | ≥98% (for internalized packages) |
| Build time for 20-package deployment | <10 minutes on 100 Mbps connection |
| Offline deploy time for 20 packages | <5 minutes on local SSD |
| Checksum verification coverage | 100% of downloaded binaries (when checksum available) |

---

## 📚 References

- [Chocolatey Package Internalizer (C4B)](https://docs.chocolatey.org/en-us/features/package-internalizer/) — The paid feature we're replicating
- [Manual Package Internalization Guide](https://docs.chocolatey.org/en-us/guides/create/recompile-packages/) — Step-by-step manual process we're automating
- [Automate Package Internalizer (C4B)](https://docs.chocolatey.org/en-us/guides/organizations/automate-package-internalization/) — Enterprise automation workflow
- [Chocolatey CDN Download Cache (Pro+)](https://docs.chocolatey.org/en-us/features/private-cdn/) — CDN caching feature (we surpass this with full offline)
- [Host Packages Internally](https://docs.chocolatey.org/en-us/features/host-packages/) — Local repository hosting options
- [NuGet v2 OData API](https://learn.microsoft.com/en-us/nuget/api/package-base-address-resource) — API for downloading packages
- [Chocolatey `choco download` Command](https://docs.chocolatey.org/en-us/choco/commands/download/) — Licensed-only download command reference
- [Chocolatey Organizational Deployment Guide](https://docs.chocolatey.org/en-us/guides/organizations/organizational-deployment-guide/) — Enterprise deployment architecture
- SAK Existing: `ChocolateyManager`, `AppScanner`, `PackageMatcher`, `MigrationReport`, `AppInstallationWorker`
