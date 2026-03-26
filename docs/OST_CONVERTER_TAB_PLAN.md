# OST Converter Tab â€” Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 25, 2026  
**Status**: ðŸ“‹ Planned  
**Parent Panel**: Email Tools (EmailInspectorPanel)  
**Tab Position**: Top-level tab alongside the existing Email Inspector view

---

## ðŸŽ¯ Executive Summary

The OST Converter tab adds bulk, multi-threaded OST/PST file conversion capabilities
to the Email Tools panel. Technicians can convert complete Outlook OST and PST files
into industry-standard formats â€” PST, EML, MSG, MBOX, DBX â€” with full preservation
of folder hierarchy, email metadata, read/unread status, and rich text formatting. The
converter also supports direct migration to cloud email platforms (Office 365, Exchange
Server, Gmail, Yahoo) via IMAP upload, recovery of deleted and corrupt items from
damaged OST files, and intelligent splitting of large resultant PST files into
manageable parts (2 GB, 5 GB, 10 GB). All parsing leverages the existing
in-tree `PstParser` engine â€” no Outlook installation required.

### Key Objectives

- **Multi-Threaded Bulk Conversion** â€” Process multiple OST/PST files concurrently with configurable thread count for maximum throughput on multi-core machines
- **Complete Format Support** â€” Convert to PST, EML, MSG, MBOX, DBX, HTML, and PDF formats with 100% data fidelity
- **Cloud Migration (IMAP Upload)** â€” Upload converted mailbox directly to Office 365, Exchange Server, Gmail, Yahoo, or any IMAP-capable server
- **Deleted Item Recovery** â€” Scan the OST file's "Recoverable Items" and orphaned nodes to recover soft-deleted and hard-deleted messages
- **Corrupt File Repair** â€” Detect and skip corrupt blocks during parsing; salvage maximum data from damaged OST files with detailed error reporting
- **Folder & Metadata Preservation** â€” Maintain original folder tree, read/unread status, importance flags, categories, sent/received timestamps, and all MAPI properties
- **Rich Content Preservation** â€” Preserve HTML bodies, embedded images, inline attachments, RTF formatting, and plain-text fallbacks
- **PST Splitting** â€” Split large output PST files into smaller volumes (2 GB, 5 GB, 10 GB, or custom size) for compatibility with legacy Outlook versions and easier transport
- **Selective Conversion** â€” Convert entire mailbox, specific folders, date ranges, or filtered subsets
- **Detailed Logging & Reports** â€” Per-item conversion status, error log, and professional summary report

---

## ðŸ“Š Project Scope

### What is OST Conversion?

**OST (Offline Storage Table)** files are local caches of Exchange/Microsoft 365
mailboxes. Unlike PST files, OST files are tightly bound to their originating Outlook
profile and cannot be opened by any other Outlook instance or email client. When a
user's Exchange server is decommissioned, their account is migrated, or their Outlook
profile is deleted, the OST file becomes an orphan â€” a potentially valuable mailbox
archive that is inaccessible through normal means.

**OST Conversion** is the process of reading the binary OST file (which uses the same
MS-PST format internally), extracting all mailbox data, and writing it into a portable,
universally accessible format. This is a critical operation for:

- **Migration**: Moving mailboxes between email platforms
- **Recovery**: Salvaging data from damaged or orphaned OST files
- **Archival**: Creating portable backups in standard formats
- **Compliance**: Extracting specific emails for legal or audit requirements
- **Decommissioning**: Extracting data before decommissioning Exchange servers

**Technical Background**:

OST and PST files share the same three-layer binary format (NDB â†’ LTP â†’ Messaging)
documented in [MS-PST]. The key differences are:

| Aspect | PST | OST |
|--------|-----|-----|
| Content type header | `0x4D53` ("SM") | `0x534F` ("SO") |
| Encryption | Usually none or compressible | Usually compressible |
| Portability | Can be opened by any Outlook | Bound to one profile |
| Data version | 14 (ANSI), 23 (Unicode) | 23 (Unicode), 36 (Unicode4K) |
| Block alignment | 64 bytes (ANSI/Unicode) | 512 bytes (4K) |
| Compression | None (ANSI/Unicode) | zlib on some blocks (4K) |
| Recoverable Items | Rarely present | Contains soft/hard-deleted items |

**Conversion Pipeline**:

```
Input OST/PST â†’ PstParser (NDB/LTP/Messaging)
    â†’ Folder Tree Enumeration
    â†’ Per-Message Item Loading (multi-threaded)
    â†’ Format Writer (PST/EML/MSG/MBOX/DBX/IMAP)
    â†’ Output File(s) + Conversion Report
```

**Key Data Sources**:
- **Existing PstParser** â€” In-tree binary parser handles NDB/LTP/Messaging layers for both PST and OST
- **Recoverable Items Folder** â€” NID `0x0301` in the hierarchy contains soft-deleted items
- **Orphaned Nodes** â€” Nodes not reachable from the folder hierarchy may contain hard-deleted items
- **MAPI Properties** â€” All 300+ property types preserved through conversion

---

## ðŸŽ¯ Use Cases

### 1. **Exchange Server Decommission â€” Bulk OST Extraction**

**Scenario**: Company is migrating from on-premises Exchange 2016 to Microsoft 365. The
Exchange server is being decommissioned. 50 users have local OST files that contain
emails not yet synced to the cloud.

**Workflow**:
1. Open Email Tools â†’ OST Converter tab
2. Click **Add Files** â†’ browse to each user's `AppData\Local\Microsoft\Outlook\` directory
3. All 50 OST files appear in the conversion queue with size and folder count
4. Select output format: **PST** (for import into Outlook/365)
5. Enable **Split PST at 5 GB** (for easier upload to 365)
6. Set thread count to 4 (match CPU cores)
7. Click **Convert All** â†’ multi-threaded conversion begins
8. Progress dashboard shows per-file status, items converted, ETA
9. After completion: 50 PST files (some split into parts) ready for import

**Benefits**:
- Batch processing â€” 50 files queued and converted unattended
- Multi-threaded â€” 4Ã— faster than sequential conversion
- PST splitting prevents "file too large" errors during Microsoft 365 import
- No Outlook or Exchange connectivity needed

---

### 2. **Orphaned OST Recovery â€” Profile Deleted**

**Scenario**: A user accidentally deleted their Outlook profile. The OST file
still exists on disk but Outlook refuses to open it ("The file is not an Outlook
data file"). The user needs their sent emails from the last 3 months.

**Workflow**:
1. Open Email Tools â†’ OST Converter tab
2. Click **Add Files** â†’ browse to the orphaned OST file
3. Enable **Recover Deleted Items** checkbox
4. Enable **Date Filter**: last 3 months only
5. Select folder filter: **Sent Items** only
6. Select output format: **EML** (for easy drag-and-drop into any email client)
7. Click **Convert** â†’ converter extracts all Sent Items from the last 3 months
8. 347 EML files exported to the output directory
9. User drags EML files into their new Outlook profile's Sent Items folder

**Benefits**:
- Recovers data from orphaned OST files that Outlook cannot open
- Date and folder filtering reduces conversion time and output size
- EML format is universally compatible with all email clients
- Deleted item recovery finds messages removed from the Sent Items folder

---

### 3. **Cross-Platform Migration â€” Outlook to Thunderbird**

**Scenario**: Customer is switching from Microsoft Outlook to Mozilla Thunderbird.
They have a 12 GB PST archive and want all emails migrated.

**Workflow**:
1. Open Email Tools â†’ OST Converter tab
2. Click **Add Files** â†’ select the 12 GB PST file
3. Select output format: **MBOX** (Thunderbird's native format)
4. Enable **Preserve Folder Structure** (creates one MBOX file per folder)
5. Click **Convert** â†’ multi-threaded conversion processes all folders
6. Output: a directory tree of MBOX files mirroring the PST folder hierarchy
7. Copy the MBOX directory into Thunderbird's profile â†’ emails appear

**Benefits**:
- Direct PST-to-MBOX conversion without intermediate steps
- Folder structure preserved â€” Inbox, Sent, Drafts, custom folders all maintained
- Thunderbird reads MBOX natively; no import plugin needed
- 12 GB file converted in under 10 minutes with multi-threading

---

### 4. **Cloud Migration â€” Upload to Gmail**

**Scenario**: Small business owner is moving from Outlook desktop to Gmail. They
want all historical emails from their OST file uploaded to Gmail.

**Workflow**:
1. Open Email Tools â†’ OST Converter tab
2. Click **Add Files** â†’ select the OST file
3. Select output target: **IMAP Upload**
4. Enter IMAP settings: `imap.gmail.com`, port 993, SSL
5. Authenticate with Gmail App Password
6. Map PST folders to Gmail labels (Inbox â†’ INBOX, Sent â†’ [Gmail]/Sent Mail)
7. Click **Upload** â†’ converter reads items and uploads via IMAP APPEND
8. Progress shows: items uploaded, bandwidth used, errors (e.g., size limit)
9. After completion: all emails appear in Gmail with correct folders/labels

**Benefits**:
- Direct OST-to-Gmail migration without intermediate file conversion
- Folder-to-label mapping gives user control over organization
- IMAP APPEND preserves email dates, read/unread status, and flags
- Works with any IMAP server (Gmail, Yahoo, Fastmail, self-hosted)

---

### 5. **Damaged OST Repair â€” Corrupt Data Recovery**

**Scenario**: Customer's laptop crashed during an Outlook sync. The OST file is
8 GB but Outlook says "Errors have been detected in the file" and the built-in
repair tool (scanpst.exe) cannot fix it.

**Workflow**:
1. Open Email Tools â†’ OST Converter tab
2. Click **Add Files** â†’ select the corrupt OST file
3. Converter detects corruption during header/BBT parse â†’ shows warning
4. Enable **Recovery Mode** (attempts to read all accessible blocks, skips corrupt ones)
5. Select output format: **PST** (to create a clean, repaired copy)
6. Click **Convert** â†’ converter processes the file, logging each corrupt block
7. Results: 7,842 of 8,100 items recovered (97%), 258 items in corrupt blocks
8. Recovery report shows which folders/items were affected
9. Clean PST file opens normally in Outlook

**Benefits**:
- Recovers data that scanpst.exe cannot fix
- Skip-and-log approach maximizes data recovery
- Per-item error tracking shows exactly what was lost
- Output PST is structurally clean â€” no inherited corruption

---

### 6. **Legal Hold â€” Selective Export with Metadata**

**Scenario**: Legal department needs all emails between two specific parties during
a 6-month window, with full headers and MAPI properties preserved.

**Workflow**:
1. Open Email Tools â†’ OST Converter tab
2. Click **Add Files** â†’ select the custodian's PST/OST file
3. Configure filters:
   - Date range: Jan 1 - Jun 30, 2025
   - Sender/recipient filter: `alice@company.com` AND `bob@vendor.com`
4. Select output format: **MSG** (preserves all MAPI properties)
5. Enable **Include Properties Manifest** (CSV with all MAPI property values per item)
6. Click **Convert** â†’ filtered conversion finds 127 matching messages
7. Output: 127 MSG files + `properties_manifest.csv` + `conversion_report.html`

**Benefits**:
- MSG format preserves complete MAPI property set for forensic use
- Date and sender filtering narrows scope to legally relevant items
- Properties manifest provides spreadsheet-ready metadata for legal review
- Conversion report documents chain of custody (source file, hashes, timestamps)

---

## ðŸ—ï¸ Architecture Overview

### Component Hierarchy

```
EmailInspectorPanel (existing â€” becomes top-level tab container)
â”‚
â”œâ”€ Tab 0: Email Inspector (existing EmailInspectorPanel content)
â”‚  â”œâ”€ Ribbon toolbar, folder tree, item list, detail panel
â”‚  â””â”€ (unchanged â€” all existing functionality preserved)
â”‚
â””â”€ Tab 1: OST Converter (NEW)
   â”‚
   â”œâ”€ OstConverterWidget (QWidget) [Main UI]
   â”‚  â”œâ”€ File Queue: Table showing added OST/PST files with metadata
   â”‚  â”œâ”€ Output Settings: Format selector, destination, split options
   â”‚  â”œâ”€ Filter Panel: Date range, folder selection, sender/recipient
   â”‚  â”œâ”€ Recovery Options: Deleted items, corrupt block handling
   â”‚  â”œâ”€ IMAP Settings: Server, port, SSL, credentials, folder mapping
   â”‚  â”œâ”€ Progress Dashboard: Per-file progress bars, ETA, item counts
   â”‚  â””â”€ Convert / Cancel buttons
   â”‚
   â”œâ”€ OstConverterController (QObject) [Orchestration]
   â”‚  â”œâ”€ Manages the conversion queue
   â”‚  â”œâ”€ Creates and manages worker threads
   â”‚  â”œâ”€ Aggregates progress from all workers
   â”‚  â”œâ”€ Generates conversion report
   â”‚  â””â”€ Signals: conversionStarted, fileProgress, fileComplete,
   â”‚              allComplete, errorOccurred
   â”‚
   â”œâ”€ OstConversionWorker (QObject) [Worker Thread â€” one per file]
   â”‚  â”œâ”€ Owns a PstParser instance for reading the source file
   â”‚  â”œâ”€ Iterates folder tree â†’ loads items â†’ writes to format writer
   â”‚  â”œâ”€ Handles deleted item recovery (Recoverable Items folder scan)
   â”‚  â”œâ”€ Handles corrupt block skip-and-log
   â”‚  â”œâ”€ Reports per-item progress
   â”‚  â””â”€ Supports cancellation via atomic flag
   â”‚
   â”œâ”€ Format Writers (one per output format)
   â”‚  â”œâ”€ PstWriter â€” Creates a new PST file (NDB/LTP/Messaging layers)
   â”‚  â”‚   â””â”€ PstSplitter â€” Monitors output size, rotates to new volume
   â”‚  â”œâ”€ EmlWriter â€” Writes RFC 5322 .eml files (reuses EmailExportWorker logic)
   â”‚  â”œâ”€ MsgWriter â€” Writes MS-OXMSG compound files (.msg)
   â”‚  â”œâ”€ MboxWriter â€” Writes Unix mstrstrstrbox format (one per folder)
   â”‚  â”œâ”€ DbxWriter â€” Writes Outlook Express DBX format
   â”‚  â”œâ”€ HtmlWriter â€” Writes HTML pages with embedded images
   â”‚  â””â”€ PdfWriter â€” Writes PDF via QTextDocument â†’ QPdfWriter
   â”‚
   â”œâ”€ ImapUploader (QObject) [Worker Thread]
   â”‚  â”œâ”€ Connects to IMAP server via QSslSocket
   â”‚  â”œâ”€ Authenticates (PLAIN, LOGIN, XOAUTH2 for Gmail/365)
   â”‚  â”œâ”€ Creates folder hierarchy on server
   â”‚  â”œâ”€ Uploads messages via IMAP APPEND with flags and dates
   â”‚  â””â”€ Reports per-message progress
   â”‚
   â”œâ”€ DeletedItemScanner (utility)
   â”‚  â”œâ”€ Scans Recoverable Items folder (NID 0x0301)
   â”‚  â”œâ”€ Walks orphaned NBT nodes not in hierarchy
   â”‚  â”œâ”€ Attempts to read each orphaned node as a message
   â”‚  â””â”€ Returns recovered items as PstItemDetail vector
   â”‚
   â””â”€ ConversionReportGenerator (utility)
      â”œâ”€ Tracks per-file, per-folder, per-item status
      â”œâ”€ Generates HTML report with statistics
      â”œâ”€ Generates CSV properties manifest (for MSG exports)
      â””â”€ Includes source file checksums (SHA-256) for chain of custody
```

### Integration with EmailInspectorPanel

The OST Converter tab requires promoting `EmailInspectorPanel` from a single
widget to a top-level tab container. The existing email inspector content becomes
"Tab 0: Email Inspector" and the new converter becomes "Tab 1: OST Converter":

```cpp
// In main_window.cpp â€” createEmailToolSection() (modified)
// The EmailInspectorPanel itself becomes a tab container.
// Internally, setupUi() wraps existing content in Tab 0.
// OstConverterWidget is added as Tab 1.

// EmailInspectorPanel::setupUi() modification:
m_top_tabs = new QTabWidget(this);
m_top_tabs->addTab(createInspectorContent(), tr("Email Inspector"));
m_top_tabs->addTab(createOstConverterTab(), tr("OST Converter"));
```

Alternatively, `main_window.cpp` can wrap both as separate panels inside a
`QTabWidget` at the "Email Tools" level â€” similar to how Benchmark & Diagnostics
hosts multiple tabs. This avoids modifying `EmailInspectorPanel` internals:

```cpp
// Option B (preferred â€” cleaner separation):
auto* email_tabs = new QTabWidget(email_wrapper);
email_tabs->addTab(m_email_inspector_panel.get(), tr("Email Inspector"));
email_tabs->addTab(m_ost_converter_widget.get(), tr("OST Converter"));
```

The `OstConverterWidget` is self-contained: it creates its own `PstParser` instance
per conversion job, owns its worker threads, and manages its own UI state. The only
integration points are:

1. **statusMessage** signal â†’ MainWindow status bar
2. **progressUpdate** signal â†’ MainWindow progress bar
3. **logOutput** signal â†’ MainWindow log panel

---

## ðŸ› ï¸ Technical Specifications

### Data Structures

```cpp
// ============================================================================
// OST Converter Types (include/sak/ost_converter_types.h)
// ============================================================================

/// @brief Output format for OST conversion
enum class OstOutputFormat {
    Pst,         ///< Microsoft PST file (MS-PST format)
    Eml,         ///< RFC 5322 MIME .eml files (one per message)
    Msg,         ///< MS-OXMSG compound files (one per message)
    Mbox,        ///< Unix mbox format (one file per folder)
    Dbx,         ///< Outlook Express DBX format
    Html,        ///< HTML pages with embedded images
    Pdf,         ///< PDF via QTextDocument/QPdfWriter
    ImapUpload   ///< Direct IMAP upload (not a file format)
};

/// @brief Recovery mode for damaged files
enum class RecoveryMode {
    Normal,          ///< Standard parsing â€” stop on critical errors
    SkipCorrupt,     ///< Skip corrupt blocks, log errors, continue
    DeepRecovery     ///< Scan all NBT nodes including orphaned ones
};

/// @brief PST split size preset
enum class PstSplitSize {
    NoSplit,     ///< Single file (no splitting)
    Split2Gb,    ///< 2 GB per volume (ANSI PST compat)
    Split5Gb,    ///< 5 GB per volume (recommended)
    Split10Gb,   ///< 10 GB per volume
    Custom       ///< User-specified size in MB
};

/// @brief IMAP authentication method
enum class ImapAuthMethod {
    Plain,       ///< PLAIN SASL mechanism
    Login,       ///< LOGIN command
    XOAuth2      ///< XOAUTH2 for Gmail / Microsoft 365
};

/// @brief IMAP server connection settings
struct ImapServerConfig {
    QString host;                  ///< e.g., "imap.gmail.com"
    uint16_t port = 993;          ///< Default IMAP SSL port
    bool use_ssl = true;
    ImapAuthMethod auth_method = ImapAuthMethod::Plain;
    QString username;
    QString password;              ///< App password or OAuth token
    int timeout_seconds = 30;
    int max_retries = 3;
};

/// @brief Folder mapping for IMAP upload
struct ImapFolderMapping {
    QString source_folder;         ///< PST folder path: "Inbox/Projects"
    QString target_folder;         ///< IMAP folder: "INBOX.Projects"
    bool skip = false;             ///< True to exclude this folder
};

/// @brief A single file in the conversion queue
struct OstConversionJob {
    QString source_path;           ///< Full path to OST/PST file
    QString display_name;          ///< Filename for display
    qint64 file_size_bytes = 0;
    bool is_ost = false;
    int estimated_items = 0;       ///< From PstFileInfo
    int estimated_folders = 0;

    // Conversion state
    enum class Status {
        Queued,
        Parsing,
        Converting,
        Uploading,    ///< IMAP upload phase
        Complete,
        Failed,
        Cancelled
    };
    Status status = Status::Queued;

    // Progress
    int items_processed = 0;
    int items_total = 0;
    int items_recovered = 0;       ///< Deleted items found
    int items_failed = 0;
    qint64 bytes_written = 0;
    QString current_folder;        ///< Currently processing folder
    QString error_message;
};

/// @brief Global conversion configuration
struct OstConversionConfig {
    // Output
    OstOutputFormat format = OstOutputFormat::Pst;
    QString output_directory;

    // Threading
    int max_threads = 2;           ///< Concurrent file conversions

    // Filtering
    QDateTime date_from;           ///< Null = no lower bound
    QDateTime date_to;             ///< Null = no upper bound
    QStringList folder_include;    ///< Empty = all folders
    QStringList folder_exclude;    ///< Folders to skip
    QString sender_filter;         ///< Sender email contains
    QString recipient_filter;      ///< Recipient email contains

    // Recovery
    RecoveryMode recovery_mode = RecoveryMode::Normal;
    bool recover_deleted_items = false;

    // PST output options
    PstSplitSize split_size = PstSplitSize::NoSplit;
    qint64 custom_split_mb = 5120; ///< Custom split size in MB

    // EML/MSG options
    bool prefix_filename_with_date = true;
    bool preserve_folder_structure = true;

    // MBOX options
    bool one_mbox_per_folder = true;

    // IMAP upload options
    ImapServerConfig imap_config;
    QVector<ImapFolderMapping> folder_mappings;

    // Manifest / reporting
    bool generate_properties_manifest = false;
    bool generate_html_report = true;
    bool include_source_checksums = true;
};

/// @brief Result of a single file conversion
struct OstConversionResult {
    QString source_path;
    QString output_path;
    OstOutputFormat format;
    int items_converted = 0;
    int items_failed = 0;
    int items_recovered = 0;       ///< Deleted items recovered
    int folders_processed = 0;
    qint64 bytes_written = 0;
    int pst_volumes_created = 0;   ///< For split PST (1 if no split)
    QStringList errors;
    QDateTime started;
    QDateTime finished;
    QString source_sha256;         ///< SHA-256 of source file
};

/// @brief Aggregate result of the entire conversion batch
struct OstConversionBatchResult {
    int files_total = 0;
    int files_succeeded = 0;
    int files_failed = 0;
    int total_items_converted = 0;
    int total_items_recovered = 0;
    qint64 total_bytes_written = 0;
    QVector<OstConversionResult> file_results;
    QDateTime batch_started;
    QDateTime batch_finished;
};
```

### Multi-Threaded Conversion Architecture

```cpp
/// @brief Controller managing the conversion pipeline
class OstConverterController : public QObject {
    Q_OBJECT

public:
    explicit OstConverterController(QObject* parent = nullptr);

    /// Add a file to the conversion queue
    void addFile(const QString& path);

    /// Remove a file from the queue (before conversion starts)
    void removeFile(int index);

    /// Clear the entire queue
    void clearQueue();

    /// Start the conversion batch
    void startConversion(const OstConversionConfig& config);

    /// Cancel all in-progress conversions
    void cancelAll();

    /// Get the current queue
    [[nodiscard]] const QVector<OstConversionJob>& queue() const;

Q_SIGNALS:
    void fileAdded(int index, sak::OstConversionJob job);
    void fileRemoved(int index);
    void queueCleared();

    // Conversion progress
    void conversionStarted(int total_files);
    void fileConversionStarted(int file_index);
    void fileProgress(int file_index, int items_done, int items_total,
                      QString current_folder);
    void fileConversionComplete(int file_index, sak::OstConversionResult result);
    void allConversionsComplete(sak::OstConversionBatchResult result);

    // Errors
    void errorOccurred(int file_index, QString message);
    void warningOccurred(int file_index, QString message);

    // Status
    void statusMessage(QString message, int timeout_ms);

private:
    QVector<sak::OstConversionJob> m_queue;
    QVector<QThread*> m_worker_threads;
    QVector<OstConversionWorker*> m_workers;
    sak::OstConversionConfig m_config;
    std::atomic<bool> m_cancelled{false};
    int m_active_workers = 0;
    sak::OstConversionBatchResult m_batch_result;
};
```

### Format Writers

#### PST Writer (MS-PST Format)

Writing a valid PST file requires constructing all three layers from scratch:

```cpp
/// @brief Creates a new PST file from extracted mailbox data
class PstWriter {
public:
    explicit PstWriter(const QString& output_path);

    /// Initialize a new Unicode PST file with empty BTrees
    [[nodiscard]] std::expected<void, sak::error_code> create();

    /// Set the message store display name
    void setDisplayName(const QString& name);

    /// Create a folder in the hierarchy
    [[nodiscard]] std::expected<uint64_t, sak::error_code> createFolder(
        uint64_t parent_nid,
        const QString& name,
        const QString& container_class);

    /// Write a message into a folder
    [[nodiscard]] std::expected<void, sak::error_code> writeMessage(
        uint64_t folder_nid,
        const sak::PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data);

    /// Finalize and close the PST file (write final BTrees, header CRC)
    [[nodiscard]] std::expected<void, sak::error_code> finalize();

    /// Current file size in bytes
    [[nodiscard]] qint64 currentSize() const;

private:
    QFile m_file;
    uint64_t m_next_nid = 0x100;  ///< Next available NID
    uint64_t m_next_bid = 0x100;  ///< Next available BID
    // ... NDB/LTP/Messaging construction internals
};
```

**PST Write Format Specification**:

The PST writer creates a Unicode PST (wVer=23, page size 512) for maximum
compatibility with all Outlook versions from 2003 onward. The file structure:

1. **Header** (564 bytes): Magic, version, encryption=None, root pointers
2. **NDB Layer**: Node BTree and Block BTree written as balanced B-trees
3. **LTP Layer**: Property Contexts (PCs) for each folder/message as Heap-on-Node
4. **Messaging Layer**: Folder hierarchy (contents tables, hierarchy tables),
   message nodes with MAPI properties, attachment sub-nodes

**Reference**: [MS-PST] Â§2.6 (NDB Layer Constraints), Â§2.4 (LTP Layer),
Â§2.5 (Messaging Layer)

#### PST Splitter

```cpp
/// @brief Wraps PstWriter to split output across multiple volumes
class PstSplitter {
public:
    PstSplitter(const QString& base_path, qint64 max_size_bytes);

    /// Write a message â€” automatically rotates to next volume if needed
    [[nodiscard]] std::expected<void, sak::error_code> writeMessage(
        uint64_t folder_nid,
        const sak::PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data);

    /// Finalize all open volumes
    [[nodiscard]] std::expected<void, sak::error_code> finalizeAll();

    /// Number of volumes created so far
    [[nodiscard]] int volumeCount() const;

private:
    /// Create a new volume file (base_part01.pst, base_part02.pst, ...)
    [[nodiscard]] std::expected<void, sak::error_code> rotateVolume();

    QString m_base_path;
    qint64 m_max_size;
    int m_volume_index = 0;
    std::unique_ptr<PstWriter> m_current_writer;
    QHash<uint64_t, uint64_t> m_folder_nid_map;  ///< Recreate folders in new volume
};
```

**Split naming convention**: `mailbox_part01.pst`, `mailbox_part02.pst`, etc.

When a volume reaches the size threshold, the splitter:
1. Finalizes the current volume
2. Creates a new PST file with the next part number
3. Recreates the folder hierarchy in the new volume
4. Continues writing messages into the new volume

#### EML Writer

```cpp
/// @brief Writes RFC 5322 MIME .eml files
///
/// Reuses the existing buildEmlContent() logic from EmailExportWorker,
/// extended with inline image and multi-part MIME support.
class EmlWriter {
public:
    explicit EmlWriter(const QString& output_dir);

    /// Write a single message as an EML file
    [[nodiscard]] std::expected<QString, sak::error_code> writeMessage(
        const sak::PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& subfolder_path);

private:
    QString m_output_dir;
    QHash<QString, int> m_filename_counters;  ///< Conflict resolution
};
```

Produces RFC 5322â€“compliant MIME messages with:
- `From`, `To`, `Cc`, `Bcc`, `Subject`, `Date`, `Message-ID` headers
- `Content-Type: multipart/mixed` with text/html and text/plain parts
- Attachment MIME parts with `Content-Disposition: attachment`
- Inline images as `Content-Disposition: inline` with `Content-ID`

#### MSG Writer (MS-OXMSG Compound File)

```cpp
/// @brief Writes Microsoft MSG files (MAPI property containers)
///
/// MSG files are OLE2 compound files containing MAPI property streams.
/// This preserves all MAPI properties for forensic-grade export.
class MsgWriter {
public:
    explicit MsgWriter(const QString& output_dir);

    /// Write a single message as a .msg compound file
    [[nodiscard]] std::expected<QString, sak::error_code> writeMessage(
        const sak::PstItemDetail& item,
        const QVector<sak::MapiProperty>& all_properties,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& subfolder_path);

private:
    QString m_output_dir;
    QHash<QString, int> m_filename_counters;

    /// Write a MAPI property to an OLE2 stream
    void writePropertyStream(QDataStream& stream,
                             const sak::MapiProperty& prop);

    /// Create OLE2 compound file structure
    [[nodiscard]] std::expected<void, sak::error_code> createCompoundFile(
        const QString& path, const QVector<sak::MapiProperty>& props,
        const QVector<QPair<QString, QByteArray>>& attachments);
};
```

MSG files use the OLE2 Compound Binary File format ([MS-CFB]) with MAPI property
streams ([MS-OXMSG]). Each .msg file contains:
- `__properties_version1.0` stream â€” fixed-length MAPI properties
- `__substg1.0_<TAG>` streams â€” variable-length property values (subject, body, etc.)
- `__attach_version1.0_#<N>` storages â€” one per attachment
- `__recip_version1.0_#<N>` storages â€” one per recipient

#### MBOX Writer

```cpp
/// @brief Writes Unix mbox files (one file per folder or one combined)
class MboxWriter {
public:
    explicit MboxWriter(const QString& output_dir, bool one_per_folder);

    /// Write a message into the appropriate mbox file
    [[nodiscard]] std::expected<void, sak::error_code> writeMessage(
        const sak::PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& folder_path);

    /// Finalize â€” close all open file handles
    void finalize();

private:
    QString m_output_dir;
    bool m_one_per_folder;
    QHash<QString, QFile*> m_open_files;

    /// Format a message as an mbox entry (From_ line + RFC 5322 content)
    [[nodiscard]] QByteArray formatMboxEntry(
        const sak::PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachments);
};
```

MBOX format: Each message is preceded by a `From ` line (sender + timestamp),
followed by the RFC 5822 message content. Messages are separated by blank lines.
The `From ` delimiter within message bodies is escaped with `>From `.

#### DBX Writer

```cpp
/// @brief Writes Outlook Express DBX files
///
/// DBX is a legacy format used by Outlook Express 5/6. While rare,
/// some migration scenarios still require it for legacy systems.
class DbxWriter {
public:
    explicit DbxWriter(const QString& output_dir);

    /// Write a message into the appropriate DBX file
    [[nodiscard]] std::expected<void, sak::error_code> writeMessage(
        const sak::PstItemDetail& item,
        const QVector<QPair<QString, QByteArray>>& attachment_data,
        const QString& folder_path);

    void finalize();

private:
    QString m_output_dir;
    /// DBX file header: magic 0xCFAD12FE + folder metadata
    void writeDbxHeader(QFile& file, const QString& folder_name);
};
```

#### IMAP Uploader

```cpp
/// @brief Uploads converted messages to an IMAP server
///
/// Uses Qt's QSslSocket for IMAP protocol communication.
/// Supports PLAIN, LOGIN, and XOAUTH2 authentication.
class ImapUploader : public QObject {
    Q_OBJECT

public:
    explicit ImapUploader(QObject* parent = nullptr);

    /// Start upload of all items to the configured IMAP server
    void upload(const sak::ImapServerConfig& config,
                const QVector<sak::ImapFolderMapping>& mappings,
                PstParser* parser);

    /// Cancel the upload
    void cancel();

Q_SIGNALS:
    void uploadStarted(int total_items);
    void uploadProgress(int items_done, int total_items, qint64 bytes_sent);
    void folderCreated(QString folder_name);
    void uploadComplete(int items_uploaded, int items_failed);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};
    QSslSocket* m_socket{nullptr};

    /// Send an IMAP command and wait for response
    [[nodiscard]] std::expected<QString, sak::error_code> sendCommand(
        const QString& command, int timeout_ms = 30000);

    /// Authenticate with the server
    [[nodiscard]] std::expected<void, sak::error_code> authenticate(
        const sak::ImapServerConfig& config);

    /// Create a folder on the server (IMAP CREATE)
    [[nodiscard]] std::expected<void, sak::error_code> createFolder(
        const QString& folder_path);

    /// Upload a single message (IMAP APPEND with flags and date)
    [[nodiscard]] std::expected<void, sak::error_code> appendMessage(
        const QString& folder,
        const QByteArray& eml_content,
        const QStringList& flags,
        const QDateTime& internal_date);

    /// Map PST flags to IMAP flags (\Seen, \Flagged, \Answered, etc.)
    [[nodiscard]] static QStringList mapFlags(const sak::PstItemDetail& item);
};
```

**IMAP Upload Protocol**:

```
C: A001 LOGIN username password
S: A001 OK LOGIN completed
C: A002 CREATE "Inbox/Projects"
S: A002 OK CREATE completed
C: A003 APPEND "Inbox/Projects" (\Seen) "25-Mar-2026 10:30:00 +0000" {4567}
C: <literal EML content, 4567 bytes>
S: A003 OK APPEND completed
```

Flags mapping:
| PST Property | IMAP Flag |
|---|---|
| `PR_MESSAGE_FLAGS & MSGFLAG_READ` | `\Seen` |
| `PR_IMPORTANCE == 2` | `\Flagged` |
| `PR_LAST_VERB_EXECUTED == 102/103` | `\Answered` |
| `PR_MESSAGE_FLAGS & MSGFLAG_UNSENT` | `\Draft` |

### Deleted Item Recovery

```cpp
/// @brief Scans for recoverable and orphaned items in an OST file
class DeletedItemScanner {
public:
    explicit DeletedItemScanner(PstParser* parser);

    /// Scan the Recoverable Items folder hierarchy
    [[nodiscard]] QVector<sak::PstItemDetail> scanRecoverableItems();

    /// Scan NBT for orphaned message nodes not in any folder hierarchy
    [[nodiscard]] QVector<sak::PstItemDetail> scanOrphanedNodes();

    /// Combined: scan both sources
    [[nodiscard]] QVector<sak::PstItemDetail> recoverAll();

Q_SIGNALS:
    void recoveryProgress(int items_found, int nodes_scanned);

private:
    PstParser* m_parser;  ///< Non-owning â€” caller manages lifetime

    /// Test if an NID is reachable from the folder hierarchy
    [[nodiscard]] bool isNodeInHierarchy(uint64_t nid) const;

    /// Attempt to read an orphaned NID as a message
    [[nodiscard]] std::optional<sak::PstItemDetail> tryReadOrphanedNode(
        uint64_t nid);
};
```

**Recoverable Items Folder** (NID `0x0301`):
- Contains soft-deleted items (user deleted + still in retention)
- Sub-folders: Deletions, Purges, Versions, DiscoveryHolds
- Items are structurally identical to normal messages

**Orphaned Node Scanning**:
1. Build a set of all NIDs reachable from the folder hierarchy
2. Walk the entire NBT cache
3. For each NID with type `NormalMessage` (0x04) not in the reachable set:
   - Attempt `readMessage(nid)` â†’ if successful, it's a recoverable item
   - Log if the node is corrupt or unreadable

### Conversion Report Generator

```cpp
/// @brief Generates professional HTML/CSV conversion reports
class ConversionReportGenerator {
public:
    /// Generate HTML report for a conversion batch
    [[nodiscard]] static QString generateHtmlReport(
        const sak::OstConversionBatchResult& batch);

    /// Generate CSV properties manifest for MSG exports
    [[nodiscard]] static QString generateCsvManifest(
        const QVector<sak::PstItemDetail>& items,
        const QVector<QVector<sak::MapiProperty>>& all_properties);
};
```

**HTML Report Contents**:
- Source file summary (path, size, SHA-256 hash, item count)
- Per-folder conversion statistics
- Error log (corrupt blocks, failed items, warnings)
- Recovered items summary
- Output file listing (paths, sizes)
- Timing statistics (duration, items/second throughput)

---

## ðŸŽ¨ User Interface Design

### OST Converter Tab Layout

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚  Email Tools                                                              â”‚
â”‚  [Email Inspector] [OST Converter]                           â† tab bar  â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚                                                                          â”‚
â”‚  â”Œâ”€â”€ ðŸ“‚ SOURCE FILES â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  [+ Add Files]  [+ Add Folder]  [âœ• Remove]  [Clear All]        â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   â”‚    â”‚
â”‚  â”‚  â”‚ File                 | Size    | Items  | Status         â”‚   â”‚    â”‚
â”‚  â”‚  â”‚â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ john@company.com.ost | 4.2 GB  | ~12400 | â³ Queued     â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ archive-2024.pst     | 1.8 GB  | ~5200  | â³ Queued     â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ mary.smith.ost       | 8.1 GB  | ~24000 | â³ Queued     â”‚   â”‚    â”‚
â”‚  â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜   â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜    â”‚
â”‚                                                                          â”‚
â”‚  â”Œâ”€â”€ âš™ï¸ OUTPUT SETTINGS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  Format: [PST â–¼]  Destination: [C:\Output\________] [Browse]    â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  â˜ Split PST files:  [5 GB â–¼]  (2 GB / 5 GB / 10 GB / Custom) â”‚    â”‚
â”‚  â”‚  â˜‘ Preserve folder structure                                    â”‚    â”‚
â”‚  â”‚  â˜‘ Prefix filenames with date  (EML/MSG only)                   â”‚    â”‚
â”‚  â”‚  Threads: [2 â–²â–¼]  (1â€“8, default = CPU cores / 2)              â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜    â”‚
â”‚                                                                          â”‚
â”‚  â”Œâ”€â”€ ðŸ” FILTERS (optional) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  Date range: [__________] to [__________]    (calendar pickers) â”‚    â”‚
â”‚  â”‚  Folders:    [Include: ____________]  [Exclude: ____________]   â”‚    â”‚
â”‚  â”‚  Sender:     [_______________________]                          â”‚    â”‚
â”‚  â”‚  Recipient:  [_______________________]                          â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜    â”‚
â”‚                                                                          â”‚
â”‚  â”Œâ”€â”€ ðŸ”§ RECOVERY OPTIONS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  â˜ Recover deleted items (scan Recoverable Items folder)        â”‚    â”‚
â”‚  â”‚  â˜ Deep recovery (scan orphaned nodes â€” slow, thorough)         â”‚    â”‚
â”‚  â”‚  â˜ Skip corrupt blocks (continue on errors, log skipped items)  â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜    â”‚
â”‚                                                                          â”‚
â”‚  â”Œâ”€â”€ â˜ï¸ IMAP UPLOAD (when format = IMAP) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  Server: [imap.gmail.com____]  Port: [993]  â˜‘ SSL              â”‚    â”‚
â”‚  â”‚  Auth:   [PLAIN â–¼]                                              â”‚    â”‚
â”‚  â”‚  User:   [user@gmail.com____]  Password: [â€¢â€¢â€¢â€¢â€¢â€¢â€¢â€¢] [ðŸ‘]        â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  Folder Mapping:                                                â”‚    â”‚
â”‚  â”‚  â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”   â”‚    â”‚
â”‚  â”‚  â”‚ Source (PST)           | Target (IMAP)        | Skip    â”‚   â”‚    â”‚
â”‚  â”‚  â”‚â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€  â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ Inbox                  | INBOX                | â˜       â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ Sent Items             | [Gmail]/Sent Mail    | â˜       â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ Drafts                 | [Gmail]/Drafts       | â˜       â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ Deleted Items          | [Gmail]/Trash        | â˜‘       â”‚   â”‚    â”‚
â”‚  â”‚  â”‚ Custom Folder 1        | Custom Folder 1      | â˜       â”‚   â”‚    â”‚
â”‚  â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜   â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  [Test Connection]                                              â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜    â”‚
â”‚                                                                          â”‚
â”‚  â”Œâ”€â”€ ðŸ“Š PROGRESS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  Overall: [â–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘] 52%   ETA: ~4 min     â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  File 1: john@company.com.ost                                   â”‚    â”‚
â”‚  â”‚   [â–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆ] 100%  âœ… 12,400 items     â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  File 2: archive-2024.pst                                       â”‚    â”‚
â”‚  â”‚   [â–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–ˆâ–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘â–‘] 32%   ðŸ“ Sent Items        â”‚    â”‚
â”‚  â”‚   Items: 1,664 / 5,200  |  Written: 423 MB  |  Recovered: 12   â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â”‚  File 3: mary.smith.ost                                         â”‚    â”‚
â”‚  â”‚   â³ Queued                                                     â”‚    â”‚
â”‚  â”‚                                                                  â”‚    â”‚
â”‚  â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜    â”‚
â”‚                                                                          â”‚
â”‚  [â–¶ Convert All]  [â¹ Cancel]  [ðŸ“„ View Report]                        â”‚
â”‚                                                                          â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

### IMAP Section Visibility

The IMAP Upload section is only visible when `OstOutputFormat::ImapUpload` is
selected in the format dropdown. All other format options hide this section.

### Progress Dashboard Behavior

- **Overall progress bar**: weighted by total item count across all files
- **Per-file progress**: shows current folder name, items done/total, bytes written
- **Recovery count**: shows deleted items found (if recovery enabled)
- **ETA**: calculated from elapsed time Ã— (remaining / done)
- **Status icons**: â³ Queued, ðŸ”„ Converting, âœ… Complete, âŒ Failed, â›” Cancelled

---

## ðŸ“‚ File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
ost_converter_types.h              # All enums and data structures above
ost_converter_widget.h             # Main tab widget UI
ost_converter_controller.h         # Conversion orchestration + thread management
ost_conversion_worker.h            # Per-file conversion worker (threaded)
pst_writer.h                       # PST file writer (NDB/LTP/Messaging)
pst_splitter.h                     # PST volume splitting wrapper
eml_writer.h                       # RFC 5322 EML file writer
msg_writer.h                       # MS-OXMSG compound file writer
mbox_writer.h                      # Unix mbox file writer
dbx_writer.h                       # Outlook Express DBX writer
html_email_writer.h                # HTML page writer for emails
pdf_email_writer.h                 # PDF writer via QTextDocument
imap_uploader.h                    # IMAP protocol upload client
deleted_item_scanner.h             # Recoverable/orphaned item recovery
conversion_report_generator.h      # HTML/CSV report generation
ost_converter_constants.h          # Named constants for the converter
```

#### Implementation (`src/`)

```
gui/ost_converter_widget.cpp       # Tab UI: file queue, settings, progress
core/ost_converter_controller.cpp  # Thread pool management, queue processing
core/ost_conversion_worker.cpp     # Per-file conversion pipeline
core/pst_writer.cpp                # PST file creation (NDB/LTP/Messaging)
core/pst_splitter.cpp              # Volume splitting logic
core/eml_writer.cpp                # EML file generation
core/msg_writer.cpp                # MSG compound file generation
core/mbox_writer.cpp               # MBOX file generation
core/dbx_writer.cpp                # DBX file generation
core/html_email_writer.cpp         # HTML email rendering
core/pdf_email_writer.cpp          # PDF email rendering
core/imap_uploader.cpp             # IMAP client + APPEND upload
core/deleted_item_scanner.cpp      # Recovery scanning
core/conversion_report_generator.cpp # Report generation
```

#### Tests (`tests/unit/`)

```
test_ost_converter_types.cpp       # Type invariants and defaults
test_pst_writer.cpp                # PST creation, folder/message write, round-trip
test_pst_splitter.cpp              # Volume rotation, size thresholds
test_eml_writer.cpp                # RFC 5322 compliance, MIME structure
test_msg_writer.cpp                # OLE2/MAPI property stream validation
test_mbox_writer.cpp               # From_ line escaping, folder-per-file
test_imap_uploader.cpp             # Command/response parsing, flag mapping
test_deleted_item_scanner.cpp      # Recoverable Items scan, orphan detection
test_conversion_report_generator.cpp # HTML report structure validation
test_ost_converter_controller.cpp  # Queue management, multi-thread orchestration
```

---

## ðŸ”§ Third-Party Dependencies

| Component | Engine | Source | License | Purpose |
|-----------|--------|--------|---------|---------|
| PST Read | PstParser (in-tree) | In-tree | N/A | Read OST/PST files |
| PST Write | PstWriter (in-tree) | In-tree | N/A | Create output PST files |
| EML/MBOX | In-tree writers | In-tree | N/A | RFC 5322 generation |
| MSG (OLE2) | In-tree writer | In-tree | N/A | Compound file creation |
| PDF Output | QPdfWriter | Qt 6.5.3 | LGPL-3.0 | PDF rendering |
| HTML Render | QTextDocument | Qt 6.5.3 | LGPL-3.0 | HTML-to-PDF pipeline |
| IMAP Client | QSslSocket | Qt 6.5.3 (Network) | LGPL-3.0 | IMAP protocol |
| SSL/TLS | OpenSSL (via Qt) | Qt platform | Apache-2.0 | Secure IMAP connections |
| Hashing | QCryptographicHash | Qt 6.5.3 | LGPL-3.0 | SHA-256 checksums |
| Threading | QThread | Qt 6.5.3 | LGPL-3.0 | Worker threads |
| Concurrent | QtConcurrent | Qt 6.5.3 | LGPL-3.0 | Thread pool management |

> **No new external dependencies.** The entire converter is built on the existing
> PstParser, Qt libraries (already linked), and new in-tree format writers. The
> MSG writer implements the OLE2 compound file format directly â€” no COM dependency.

---

## ðŸ”§ Implementation Phases

### Phase 1: Core Pipeline + PST Output (4 weeks)

**Goal**: End-to-end OST â†’ PST conversion with correct data fidelity.

**Tasks**:
1. Create `ost_converter_types.h` with all data structures
2. Create `ost_converter_constants.h` with named constants
3. Implement `PstWriter` â€” NDB layer (header, NBT, BBT), LTP layer (PC, TC),
   Messaging layer (folder hierarchy, messages, attachments)
4. Implement `PstSplitter` wrapping PstWriter with volume rotation
5. Implement `OstConversionWorker` â€” single-threaded per-file conversion pipeline:
   open source â†’ enumerate folders â†’ iterate items â†’ write to PstWriter
6. Implement `OstConverterController` â€” queue management, single-worker execution
7. Create `OstConverterWidget` â€” minimal UI: file list, format selector (PST only),
   destination, Convert/Cancel buttons, progress bar
8. Add tab to EmailInspectorPanel or main_window.cpp
9. Write `test_pst_writer.cpp` â€” round-trip: write PST â†’ read back with PstParser
10. Write `test_ost_converter_controller.cpp` â€” queue and single-file conversion

**Acceptance Criteria**:
- [ ] Convert a test OST file to PST
- [ ] Open the output PST in the Email Inspector â†’ all folders/items visible
- [ ] PST splitting at 2 GB produces correct multi-volume output
- [ ] All unit tests pass

### Phase 2: Additional Format Writers (3 weeks)

**Goal**: EML, MSG, MBOX, DBX, HTML, PDF output formats.

**Tasks**:
1. Implement `EmlWriter` â€” extend existing `buildEmlContent()` logic
2. Implement `MsgWriter` â€” OLE2 compound file with MAPI property streams
3. Implement `MboxWriter` â€” folder-per-file mbox with From_ escaping
4. Implement `DbxWriter` â€” legacy Outlook Express format
5. Implement `HtmlEmailWriter` â€” styled HTML pages with embedded images
6. Implement `PdfEmailWriter` â€” QTextDocument â†’ QPdfWriter pipeline
7. Update `OstConverterWidget` format dropdown with all formats
8. Write tests for each writer

**Acceptance Criteria**:
- [ ] EML files importable in Outlook, Thunderbird, and Apple Mail
- [ ] MSG files open correctly in Outlook
- [ ] MBOX files importable in Thunderbird
- [ ] HTML output renders email bodies with images
- [ ] PDF output preserves formatting and embedded images

### Phase 3: Multi-Threading + Recovery (3 weeks)

**Goal**: Concurrent file conversion and deleted item recovery.

**Tasks**:
1. Upgrade `OstConverterController` to manage N worker threads via QThread
2. Implement thread-safe progress aggregation (atomic counters, queued signals)
3. Implement `DeletedItemScanner` â€” Recoverable Items folder + orphaned nodes
4. Add recovery options to UI (checkboxes, recovery mode selector)
5. Implement `RecoveryMode::SkipCorrupt` in `OstConversionWorker`
6. Implement `RecoveryMode::DeepRecovery` with full NBT walk
7. Update progress dashboard for multi-file parallel conversion
8. Write tests for deleted item scanning and multi-thread orchestration

**Acceptance Criteria**:
- [ ] 4 files converting simultaneously on a 4-thread config
- [ ] Deleted items recovered from Recoverable Items folder
- [ ] Orphaned nodes recovered in DeepRecovery mode
- [ ] Corrupt blocks skipped with error logging (no crash)

### Phase 4: IMAP Upload (2 weeks)

**Goal**: Direct upload to IMAP servers (Gmail, 365, Exchange, Yahoo).

**Tasks**:
1. Implement `ImapUploader` â€” QSslSocket IMAP client
2. Implement PLAIN and LOGIN authentication
3. Implement XOAUTH2 for Gmail and Microsoft 365
4. Implement folder creation (IMAP CREATE)
5. Implement message upload (IMAP APPEND with flags and date)
6. Implement folder mapping UI (source PST folder â†’ target IMAP folder)
7. Add "Test Connection" button
8. Write tests for IMAP command/response parsing

**Acceptance Criteria**:
- [ ] Connect to Gmail via IMAP with App Password
- [ ] Create folders on the server matching PST hierarchy
- [ ] Upload messages with correct flags (read/unread, importance)
- [ ] Upload messages with correct internal date
- [ ] Cancel upload mid-operation without server-side corruption

### Phase 5: Filters, Report, Polish (2 weeks)

**Goal**: Filtering, reporting, and production polish.

**Tasks**:
1. Implement date range filter in `OstConversionWorker`
2. Implement folder include/exclude filter
3. Implement sender/recipient text filter
4. Implement `ConversionReportGenerator` â€” HTML report + CSV manifest
5. Add "View Report" button that opens the HTML report in default browser
6. Add SHA-256 checksums of source files to the report
7. Implement QSettings persistence for last-used settings
8. Polish: keyboard shortcuts, tooltips, error messages
9. Test on various OST files (Unicode, Unicode4K, encrypted, corrupt)
10. Write integration test with a sample OST file

**Acceptance Criteria**:
- [ ] Date filter correctly limits output to specified range
- [ ] Folder filter includes/excludes correct folders
- [ ] Sender filter matches emails by sender address
- [ ] HTML report contains complete statistics and error log
- [ ] CSV manifest lists all MAPI properties per exported item

---

## ðŸ“‹ CMakeLists.txt Changes

### New Source Files

```cmake
# In the main SAK_SOURCES list, add:

# OST Converter Tab
src/gui/ost_converter_widget.cpp
src/core/ost_converter_controller.cpp
src/core/ost_conversion_worker.cpp
src/core/pst_writer.cpp
src/core/pst_splitter.cpp
src/core/eml_writer.cpp
src/core/msg_writer.cpp
src/core/mbox_writer.cpp
src/core/dbx_writer.cpp
src/core/html_email_writer.cpp
src/core/pdf_email_writer.cpp
src/core/imap_uploader.cpp
src/core/deleted_item_scanner.cpp
src/core/conversion_report_generator.cpp
```

### New Header Files

```cmake
# In the main SAK_HEADERS list, add:

# OST Converter Tab
include/sak/ost_converter_types.h
include/sak/ost_converter_widget.h
include/sak/ost_converter_controller.h
include/sak/ost_conversion_worker.h
include/sak/pst_writer.h
include/sak/pst_splitter.h
include/sak/eml_writer.h
include/sak/msg_writer.h
include/sak/mbox_writer.h
include/sak/dbx_writer.h
include/sak/html_email_writer.h
include/sak/pdf_email_writer.h
include/sak/imap_uploader.h
include/sak/deleted_item_scanner.h
include/sak/conversion_report_generator.h
include/sak/ost_converter_constants.h
```

### New Test Files

```cmake
# In the test section, add:

sak_add_test(test_ost_converter_types     tests/unit/test_ost_converter_types.cpp)
sak_add_test(test_pst_writer              tests/unit/test_pst_writer.cpp)
sak_add_test(test_pst_splitter            tests/unit/test_pst_splitter.cpp)
sak_add_test(test_eml_writer              tests/unit/test_eml_writer.cpp)
sak_add_test(test_msg_writer              tests/unit/test_msg_writer.cpp)
sak_add_test(test_mbox_writer             tests/unit/test_mbox_writer.cpp)
sak_add_test(test_imap_uploader           tests/unit/test_imap_uploader.cpp)
sak_add_test(test_deleted_item_scanner    tests/unit/test_deleted_item_scanner.cpp)
sak_add_test(test_conversion_report_gen   tests/unit/test_conversion_report_generator.cpp)
sak_add_test(test_ost_converter_controller tests/unit/test_ost_converter_controller.cpp)
```

### Link Dependencies

No new library dependencies. Uses:
- **Qt Core** (QThread, QProcess, QFile, QSettings, QCryptographicHash) â€” already linked
- **Qt Widgets** (UI components) â€” already linked
- **Qt Network** (QSslSocket for IMAP) â€” already linked
- **Qt Concurrent** (thread pool) â€” already linked

---

## ðŸ“‹ Configuration & Settings

### QSettings Keys

```
OstConverter/lastOutputDir          = QString     # Last used output directory
OstConverter/lastFormat             = int         # Last selected OstOutputFormat
OstConverter/maxThreads             = int         # Thread count (default: cores/2)
OstConverter/splitSize              = int         # PstSplitSize enum value
OstConverter/customSplitMb          = int         # Custom split size in MB
OstConverter/preserveFolderStructure = bool       # Default: true
OstConverter/prefixWithDate         = bool        # Default: true
OstConverter/recoverDeleted         = bool        # Default: false
OstConverter/skipCorrupt            = bool        # Default: false
OstConverter/generateReport         = bool        # Default: true
OstConverter/includeChecksums       = bool        # Default: true

# IMAP (non-sensitive â€” password is NOT persisted)
OstConverter/imapHost               = QString
OstConverter/imapPort               = int
OstConverter/imapUseSsl             = bool
OstConverter/imapAuthMethod         = int
OstConverter/imapUsername            = QString
```

### Constants

```cpp
// In include/sak/ost_converter_constants.h

namespace sak::ost_converter {

/// Thread limits
constexpr int kMinThreads = 1;
constexpr int kMaxThreads = 8;
constexpr int kDefaultThreadMultiplier = 2;  ///< cores / multiplier

/// PST split sizes in bytes
constexpr qint64 kSplit2Gb  = 2LL * 1024 * 1024 * 1024;  ///< 2,147,483,648
constexpr qint64 kSplit5Gb  = 5LL * 1024 * 1024 * 1024;  ///< 5,368,709,120
constexpr qint64 kSplit10Gb = 10LL * 1024 * 1024 * 1024;  ///< 10,737,418,240
constexpr qint64 kMinCustomSplitMb = 500;   ///< Minimum custom split: 500 MB
constexpr qint64 kMaxCustomSplitMb = 50000; ///< Maximum custom split: 50 GB

/// IMAP protocol
constexpr int kImapDefaultPort = 993;
constexpr int kImapDefaultTimeoutMs = 30000;
constexpr int kImapMaxRetries = 3;
constexpr int kImapMaxMessageSizeBytes = 25 * 1024 * 1024;  ///< 25 MB (Gmail limit)

/// Progress reporting
constexpr int kProgressUpdateIntervalMs = 250;  ///< Min interval between UI updates
constexpr int kBatchProgressSmoothing = 5;      ///< ETA smoothing window (samples)

/// File queue
constexpr int kMaxQueuedFiles = 100;  ///< Maximum files in conversion queue
constexpr qint64 kMaxSourceFileSizeBytes = 50LL * 1024 * 1024 * 1024;  ///< 50 GB

/// Report
constexpr int kMaxErrorLogEntries = 10000;  ///< Limit error log size

/// OLE2 Compound File (MSG writer)
constexpr int kOle2SectorSize = 512;
constexpr int kOle2MiniSectorSize = 64;
constexpr int kOle2MiniStreamCutoff = 4096;

/// DBX format
constexpr uint32_t kDbxMagic = 0xCFAD12FE;
constexpr int kDbxHeaderSize = 0x24BC;

} // namespace sak::ost_converter
```

---

## ðŸ§ª Testing Strategy

### Unit Tests

**test_ost_converter_types.cpp**:
- All structs are default-constructible
- All structs are copy-constructible (for signal/slot)
- Enum values cover all expected formats
- OstConversionJob::Status transitions are valid

**test_pst_writer.cpp** (critical â€” most complex component):
- Create empty PST â†’ valid header, empty BTrees
- Create folder â†’ folder appears in hierarchy
- Write message with subject/body/date â†’ read back matches
- Write message with attachments â†’ attachments readable
- Write message with HTML body + embedded images â†’ preserved
- Round-trip: write 100 messages â†’ read with PstParser â†’ compare
- Unicode string handling (CJK, emoji, RTL)
- Large message body (>32 KB, multi-block)
- Empty folder handling
- Nested folder hierarchy (5 levels deep)

**test_pst_splitter.cpp**:
- Split at 2 GB creates multiple volumes
- Folder hierarchy duplicated in each volume
- Messages distributed across volumes correctly
- Volume naming: `base_part01.pst`, `base_part02.pst`
- Single message larger than split size â†’ written to its own volume

**test_eml_writer.cpp**:
- RFC 5322 Date header format
- MIME multipart/mixed structure
- Attachment Content-Disposition
- Inline image Content-ID
- UTF-8 subject encoding (RFC 2047)
- Filename sanitization
- Folder structure creation

**test_msg_writer.cpp**:
- OLE2 compound file header valid
- Property stream contains expected MAPI tags
- Variable-length property streams (subject, body)
- Attachment storage structure
- Recipient storage structure
- Empty message (no body, no attachments)

**test_mbox_writer.cpp**:
- From_ line format: `From sender@example.com Thu Mar 25 10:30:00 2026`
- From_ escaping in message body: `>From`
- Blank line separator between messages
- Folder-per-file output structure
- Combined single-file output
- Non-ASCII sender handling

**test_imap_uploader.cpp**:
- IMAP command formatting
- Response parsing (OK, NO, BAD, tagged, untagged)
- Flag mapping: read â†’ `\Seen`, importance â†’ `\Flagged`
- APPEND command with literal size
- Date formatting for IMAP internal date
- XOAUTH2 token formatting
- Error handling: connection refused, auth failure, command failure

**test_deleted_item_scanner.cpp**:
- Recoverable Items folder scan (mock PstParser)
- Orphaned node detection (nodes not in hierarchy)
- Corrupt node handling (return empty, log error)
- Combined recovery deduplicates results

**test_conversion_report_generator.cpp**:
- HTML report contains source file info
- HTML report contains per-folder statistics
- HTML report contains error log
- HTML report has valid HTML structure
- CSV manifest contains expected columns
- CSV manifest correctly escapes commas/quotes

**test_ost_converter_controller.cpp**:
- Add file to queue â†’ signal emitted
- Remove file from queue â†’ signal emitted
- Clear queue â†’ all files removed
- Start conversion â†’ workers created
- Cancel conversion â†’ workers stopped
- Queue maximum enforced (100 files)

### Integration Tests

**test_ost_converter_integration.cpp** (manual, requires sample files):
- Convert sample OST â†’ PST â†’ verify round-trip in Email Inspector
- Convert sample PST â†’ EML â†’ verify EML content and structure
- Convert sample PST â†’ MBOX â†’ verify MBOX structure
- Multi-file batch conversion (3 files, 2 threads)
- Recovery mode on a deliberately corrupted test file
- PST splitting with small threshold (1 MB) for testing

### Manual Test Matrix

| Test Case | Input | Output | Verification |
|-----------|-------|--------|-------------|
| OST â†’ PST | Normal OST file | Single PST | Open in Outlook, verify folders/items |
| OST â†’ PST split | Large OST (>5 GB) | Multiple PSTs | All parts open, combined content matches |
| OST â†’ EML | OST with 100 items | 100 EML files | Open in Thunderbird, verify headers |
| OST â†’ MSG | OST with attachments | MSG files | Open in Outlook, verify MAPI props |
| PST â†’ MBOX | PST archive | MBOX files | Import in Thunderbird, verify content |
| OST â†’ IMAP | OST file | Gmail inbox | Verify emails + flags in Gmail web |
| Corrupt OST | Damaged file | PST + report | Maximum recovery, error report accurate |
| Date filter | 1 year of emails | Filtered output | Only date-range items in output |
| Folder filter | Multi-folder PST | Inbox only | Only Inbox contents in output |
| Deleted recovery | OST with deletions | Recovered items | Deleted items appear in output |

---

## ðŸš§ Limitations & Challenges

### Technical Limitations

**PST Writer Complexity**:
- âš ï¸ Writing a valid PST file requires constructing NDB/LTP/Messaging layers from scratch
- âš ï¸ B-tree balancing for large mailboxes (>50,000 items) needs careful implementation
- âš ï¸ MAPI property encoding varies by type (PT_UNICODE, PT_BINARY, PT_SYSTIME, etc.)
- **Mitigation**: Start with a simplified writer that produces valid but unoptimized PST files. Optimize B-tree layout in a later pass. Verify with PstParser round-trip testing.

**MSG OLE2 Format**:
- âš ï¸ OLE2 compound file format is complex (FAT sectors, directory entries, mini-stream)
- âš ï¸ No Qt or vcpkg library for OLE2 writing â€” must implement from [MS-CFB] specification
- **Mitigation**: Implement a minimal OLE2 writer that handles the subset needed for MSG files. The compound file only needs property streams and attachment storages.

**IMAP Upload Reliability**:
- âš ï¸ IMAP APPEND is not idempotent â€” duplicate uploads create duplicate messages
- âš ï¸ Gmail has a 25 MB per-message size limit
- âš ï¸ Some IMAP servers throttle rapid APPEND commands
- âš ï¸ XOAUTH2 tokens expire and need refresh
- **Mitigation**: Track uploaded message IDs (Message-ID header) to detect duplicates. Skip oversized messages with warning. Implement rate limiting with configurable delay. For XOAUTH2, prompt for new token on expiry.

**Deleted Item Recovery Accuracy**:
- âš ï¸ Hard-deleted items may have partially overwritten blocks
- âš ï¸ Orphaned nodes may be from a previous mailbox sync, not the current user
- âš ï¸ Recovery scanning reads every node in the NBT â€” slow for large files
- **Mitigation**: Deep recovery is opt-in only. Log recovered item metadata for user review. Show clear warning that recovered items may include stale/irrelevant data.

**DBX Format (Legacy)**:
- âš ï¸ DBX format documentation is incomplete (reverse-engineered)
- âš ï¸ Outlook Express is discontinued â€” very few migration targets
- **Mitigation**: Implement basic DBX support as a best-effort feature. Prioritize PST/EML/MBOX/MSG which cover 99% of use cases.

**Unicode4K Block Compression**:
- âš ï¸ OST files with 4K pages may have zlib-compressed blocks
- âœ… PstParser already handles decompression (verified in repo memory)
- **Mitigation**: No additional work needed â€” existing parser handles this transparently.

### Workarounds

**Memory Pressure with Large Files**:
```cpp
// Process one folder at a time, not entire mailbox in memory
for (const auto& folder : folder_tree) {
    auto items = parser.readFolderItems(folder.node_id, 0, folder.content_count);
    for (const auto& item : items.value()) {
        auto detail = parser.readItemDetail(item.node_id);
        writer.writeMessage(folder_nid, detail.value(), attachments);
        // Detail goes out of scope â†’ memory freed
    }
}
```

**Thread-Safe Progress Reporting**:
```cpp
// Workers emit signals on their own thread; UI updates on main thread
// Use Qt::QueuedConnection (default for cross-thread signals)
connect(worker, &OstConversionWorker::itemProgress,
        this, &OstConverterController::onWorkerProgress);
        // Qt automatically queues this across thread boundaries
```

---

## ðŸŽ¯ Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| PST round-trip fidelity | 100% (all items readable after convert) | Critical |
| EML RFC 5322 compliance | Importable in Outlook + Thunderbird + Apple Mail | Critical |
| MSG MAPI property preservation | All properties from source present in output | High |
| MBOX import success | 100% import rate in Thunderbird | High |
| Conversion throughput | > 500 items/second per thread (simple messages) | High |
| Multi-thread scaling | Near-linear scaling up to 4 threads | Medium |
| PST split accuracy | All items present across volumes, no duplicates | Critical |
| IMAP upload success | > 99% per-message success rate (excluding size limits) | High |
| Deleted item recovery | > 90% of soft-deleted items recovered | High |
| Corrupt file recovery | > 80% of items in accessible blocks recovered | Medium |
| Memory usage | < 500 MB for a 10 GB OST file | High |
| Report generation | < 5 seconds for 50,000-item batch | Medium |

---

## ðŸ”’ Security Considerations

### Credential Handling (IMAP)
- IMAP passwords are **never persisted to disk** â€” only held in memory during upload
- Password field uses `QLineEdit::Password` echo mode
- XOAUTH2 tokens are obtained via the platform's OAuth flow, not stored by SAK
- SSL/TLS is required by default; plain-text IMAP is disabled
- Certificate validation via Qt's default CA bundle; self-signed certs require user confirmation

### Source File Integrity
- Source OST/PST files are opened in **read-only mode** (QIODevice::ReadOnly)
- The converter never modifies the source file
- SHA-256 checksums are computed before conversion for chain-of-custody verification
- Checksums are included in the conversion report

### Output File Safety
- Output directory must exist and be writable before conversion starts
- Filenames are sanitized to remove path traversal characters (`../`, `\`, etc.)
- Output files are written with standard permissions (no world-writable bits)
- Temporary files are cleaned up on cancellation or failure

### OLE2 Compound File Safety (MSG Writer)
- MSG files are created from known-good MAPI properties extracted by PstParser
- No user-supplied data is used for OLE2 directory entry names
- Sector allocation is bounds-checked to prevent buffer overflows

### IMAP Protocol Safety
- IMAP literal data uses the `{size}` synchronization mechanism â€” no injection possible
- Folder names are escaped per IMAP RFC 3501 Â§ 5.1 (modified UTF-7)
- Connection timeouts prevent hanging on unresponsive servers
- Maximum message size enforced before APPEND attempt

---

## ðŸ’¡ Future Enhancements (Post-v1.0)

### v1.1 - Advanced Features
- **Incremental Conversion**: Track which items were already converted; only convert new/changed items on re-run
- **Deduplication**: Detect and skip duplicate messages across multiple source files (by Message-ID)
- **Calendar/Contact-Only Export**: Convert only calendar items (ICS) or contacts (VCF) without full mailbox conversion
- **Encryption Support**: Handle PST files with high encryption (NDB_CRYPT_CYCLIC / Permutative)
- **ANSI PST Support**: Read legacy ANSI PST files (wVer=14, 32-bit offsets)

### v1.2 - Enterprise Features
- **Batch Scheduling**: Queue conversion jobs with start time (overnight batch runs)
- **PowerShell Script Generation**: Generate a PowerShell script that imports the output PSTs into Microsoft 365 using `Import-MailboxData`
- **Exchange Web Services (EWS)**: Direct upload to on-premises Exchange via EWS SOAP API
- **Microsoft Graph API**: Upload to Microsoft 365 via Graph API (REST, no IMAP)
- **Conversion Templates**: Save/load conversion configurations for repeated migrations
- **Remote File Support**: Convert OST files from network shares with UNC paths

---

## ðŸ“š Resources

### Microsoft Open Specifications
- [[MS-PST]: Outlook Personal Folders File Format](https://learn.microsoft.com/openspecs/office_file_formats/ms-pst/)
- [[MS-OXMSG]: Outlook Item Message File Format](https://learn.microsoft.com/openspecs/exchange_server_protocols/ms-oxmsg/)
- [[MS-CFB]: Compound Binary File Format](https://learn.microsoft.com/openspecs/windows_protocols/ms-cfb/)
- [[MS-OXRTFCP]: Rich Text Format Compression](https://learn.microsoft.com/openspecs/exchange_server_protocols/ms-oxrtfcp/)
- [[MS-OXPROPS]: Exchange Server Protocols Master Property List](https://learn.microsoft.com/openspecs/exchange_server_protocols/ms-oxprops/)

### RFC Standards
- [RFC 5322: Internet Message Format](https://tools.ietf.org/html/rfc5322)
- [RFC 2045-2049: MIME](https://tools.ietf.org/html/rfc2045)
- [RFC 2047: MIME Header Encoding](https://tools.ietf.org/html/rfc2047)
- [RFC 6350: vCard 4.0](https://tools.ietf.org/html/rfc6350)
- [RFC 5545: iCalendar](https://tools.ietf.org/html/rfc5545)
- [RFC 3501: IMAP4rev1](https://tools.ietf.org/html/rfc3501)
- [RFC 4616: PLAIN SASL Mechanism](https://tools.ietf.org/html/rfc4616)
- [RFC 7628: SASL XOAUTH2](https://tools.ietf.org/html/rfc7628)

### Format References
- [Unix mbox format](https://en.wikipedia.org/wiki/Mbox)
- [DBX file format (reverse-engineered)](http://www.langenberg.nl/dbxformat.html)
- [OLE2 Compound File Visualization](https://github.com/richardlehane/mscfb)

### Existing Codebase References
- `include/sak/pst_parser.h` â€” NDB/LTP/Messaging reader (reuse for all input)
- `include/sak/email_export_worker.h` â€” EML/VCF/ICS/CSV writers (extend for EML)
- `include/sak/email_types.h` â€” All data structures (PstItemDetail, PstFolder, etc.)
- `include/sak/email_constants.h` â€” MAPI property IDs, node types, format constants
- `src/core/pst_parser.cpp` â€” 4,000+ lines of verified PST/OST parsing logic

---

## ðŸ“ž Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](../CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: March 25, 2026  
**Author**: Randy Northrup  
**Status**: ðŸ“‹ Planned â€” Ready for Implementation
