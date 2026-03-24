# Email & PST/OST Inspector Panel — Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 22, 2026  
**Status**: ✅ Implemented  
**Release**: v0.9.0.4

---

## 🎯 Executive Summary

The Email & PST/OST Inspector Panel provides enterprise-grade offline email forensics, data extraction, and email client profile management — all without requiring Microsoft Outlook or any email client to be installed. Technicians can open PST and OST files directly, browse the complete folder hierarchy, view emails/contacts/calendar/tasks/notes, export data in standard formats (EML, ICS, VCF, CSV), extract attachments in bulk, search across thousands of items, and inspect raw MAPI properties. Beyond PST/OST inspection, the panel includes email client profile backup/restore, Thunderbird MBOX import, and orphaned data-file discovery — making it the single tool a technician needs for any email-related task on a customer machine.

### Key Objectives

- **PST/OST File Browser** — Open and browse the complete folder hierarchy of Outlook PST and OST data files without Outlook installed
- **Item Viewer** — View emails, contacts, calendar items, tasks, and notes with rich detail panels
- **Export: Calendar → ICS / CSV** — Export calendar items to iCalendar (`.ics`) or CSV format
- **Export: Contacts → VCF / CSV** — Export contacts to vCard (`.vcf`) or CSV format
- **Export: Emails → EML / CSV** — Export messages as individual `.eml` files or to CSV summary
- **Attachment Extraction** — Extract attachments from any folder or individual message with bulk support
- **Full-Text Search** — Search across all items by subject, body, sender, date range, attachment name
- **MAPI Property Inspector** — View raw MAPI properties for any item (advanced forensic inspection)
- **Sortable Column List** — Sortable, filterable item list with configurable column visibility
- **Tabbed Detail View** — Detail panel with Content, Headers, Properties, and Attachments tabs
- **Email Client Profile Manager** — Backup/restore profiles for Outlook, Thunderbird, Windows Mail
- **Thunderbird MBOX Support** — Parse and browse Thunderbird mailbox (MBOX) files
- **Orphaned Data-File Discovery** — Scan all drives for orphaned PST/OST/MBOX files not linked to a profile
- **Report Generation** — Professional HTML/JSON summary of inspected files and extracted data

---

## 📊 Project Scope

### What is PST/OST Inspection?

**PST (Personal Storage Table)** and **OST (Offline Storage Table)** are the binary database formats Microsoft Outlook uses to store mailbox data locally. A PST file is a standalone archive; an OST file is an offline cache of a server-side mailbox (Exchange, Microsoft 365). Both share the same underlying MS-PST binary format documented in **[MS-PST]: Outlook Personal Folders (.pst) File Format** (Microsoft Open Specification).

**Technical Background**:
- PST files use a B-tree–based node database (NDB) layer, a lists/tables/properties (LTP) layer, and a messaging layer that maps MAPI properties onto the NDB.
- The file format has two variants: ANSI (legacy, 32-bit offsets, 2 GB limit) and Unicode (modern, 64-bit offsets, 50 GB limit).
- OST files are structurally identical to Unicode PST files, but are encrypted with a compressible-encryption transform that is reversible without credentials.
- All item types (messages, contacts, appointments, tasks, notes) are stored as MAPI property bags attached to folder nodes in the hierarchy.

**Why Offline Parsing Matters**:
- The customer's machine may not have Outlook installed (technician's laptop, post-migration, recovery scenario).
- Outlook locks its data files while running — offline parsing works on copies or while Outlook is closed.
- OST files cannot be opened in another Outlook profile — offline parsing is the only way to read them without the original account.
- Forensic inspection requires reading raw MAPI properties that Outlook's UI does not expose.

**Key Data Sources (Windows)**:
- **PST/OST files** — Direct binary parsing per MS-PST specification
- **MBOX files** — Plain-text concatenated messages (Thunderbird, many Linux clients)
- **Windows Registry** — `HKCU\Software\Microsoft\Office\<ver>\Outlook\Profiles` for profile discovery
- **Thunderbird profiles** — `%APPDATA%\Thunderbird\Profiles\` directory with `profiles.ini`
- **Windows Mail** — `%LOCALAPPDATA%\Comms\Unistore\data\` for modern Windows 10/11 Mail app
- **EML, ICS, VCF** — Standard RFC-based formats for interoperability

---

## 🎯 Use Cases

### 1. **Customer Migration — "I Need My Old Emails"**

**Scenario**: Customer migrated from on-premises Exchange to Microsoft 365. The old OST file is still on the hard drive but the Exchange server is decommissioned. Customer needs specific emails from the old OST.

**Workflow**:
1. Open Email & PST/OST Inspector Panel
2. Click **Open File** → browse to `C:\Users\<user>\AppData\Local\Microsoft\Outlook\<user>@company.com.ost`
3. Panel parses the OST file and displays the full folder hierarchy (Inbox, Sent Items, Calendar, etc.)
4. Browse to Inbox → sort by Date → find the emails from 2023
5. Select the required messages → **Export → EML** → save to USB drive
6. Also export the Calendar → **Export → ICS** for import into the new mailbox
7. Generate a summary report for the migration ticket

**Benefits**:
- No Outlook installation needed on the technician's laptop
- OST files readable without the original Exchange account
- Selective export — only the data the customer needs
- Standard formats (EML, ICS) import into any email client

---

### 2. **Email Forensics — "When Was This Sent?"**

**Scenario**: HR needs to confirm the exact send date and transport headers of an email for a compliance investigation. The sender's mailbox is on a PST archive.

**Workflow**:
1. Open the PST file in the Inspector
2. Search by subject keyword → find the email
3. Open the **Headers** tab → view all RFC 5322 transport headers (Received, Message-ID, DKIM, etc.)
4. Open the **MAPI Properties** tab → inspect `PR_MESSAGE_DELIVERY_TIME`, `PR_CLIENT_SUBMIT_TIME`, `PR_TRANSPORT_MESSAGE_HEADERS`
5. Export the raw EML file for legal chain-of-custody
6. Generate a professional HTML report documenting the findings

**Benefits**:
- Full RFC 5322 header inspection
- Raw MAPI property access for forensic-grade timestamps
- EML export preserves original message structure
- Report generation for compliance documentation

---

### 3. **Bulk Attachment Recovery — "I Need All My Invoices"**

**Scenario**: Accountant has 5 GB PST archive of emails from a vendor. Every email has a PDF invoice attachment. They need all PDFs extracted into a single folder.

**Workflow**:
1. Open the PST file
2. Right-click the "Invoices" folder → **Extract All Attachments**
3. Choose filter: `*.pdf` only
4. Choose destination folder
5. Panel extracts 1,247 PDF attachments with progress bar
6. Optional: preserve subfolder structure or flatten into one directory

**Benefits**:
- Bulk extraction across an entire folder tree
- File-type filtering (only get PDFs, or only get XLSX, etc.)
- Duplicate detection (same filename from different emails)
- Progress reporting and cancellation support

---

### 4. **Technician Profile Backup Before Reinstall**

**Scenario**: Customer's Windows installation needs a clean reinstall. Technician must back up all email client data before wiping the drive.

**Workflow**:
1. Open Email & PST/OST Inspector → **Profile Manager** tab
2. Panel auto-discovers:
   - Outlook 2021: 2 PST files (8 GB total), 1 OST file (12 GB)
   - Thunderbird: 3 profiles with MBOX/IMAP cache (4 GB)
   - Windows Mail: UWP data store (200 MB)
3. Select all → **Backup** → choose external drive
4. Panel copies data files, profile registry keys (exported as `.reg`), account settings
5. After reinstalling Windows:
   - Open SAK → **Profile Manager** → **Restore**
   - Import `.reg` keys, copy data files back to correct locations

**Benefits**:
- Discovers all email clients in one scan
- Backs up not just data files but also account configuration
- Restore path for each client documented in the backup manifest

---

### 5. **Orphaned PST/OST Discovery**

**Scenario**: A departing employee's machine has PST files scattered across multiple drives and folders. IT needs to find all of them for archival.

**Workflow**:
1. Open Email & PST/OST Inspector → **Orphaned Files** tab
2. Click **Scan All Drives**
3. Panel scans `C:\`, `D:\`, and any mounted volumes for `*.pst`, `*.ost`, `*.mbox`
4. Results: 14 files found — 8 are linked to a profile, 6 are orphaned
5. For each orphaned file: quick-preview the folder list and item count
6. Select orphaned files → **Copy to Archive** or **Open for Inspection**

**Benefits**:
- Finds data files that aren't linked to any email profile
- Shows which files ARE linked (so you don't duplicate effort)
- Handles multiple drives and network paths
- Quick preview without opening each file fully

---

### 6. **Thunderbird MBOX Import / Conversion**

**Scenario**: Customer is switching from Thunderbird to Outlook. They need their Thunderbird emails converted to a format Outlook can import.

**Workflow**:
1. Open Email & PST/OST Inspector
2. Click **Open File** → browse to Thunderbird profile MBOX file (e.g., `Inbox` with no extension)
3. Panel parses the MBOX file and displays messages
4. Select all → **Export → EML** → creates individual `.eml` files
5. Customer drags EML files into Outlook → imported

**Benefits**:
- Reads MBOX files from Thunderbird, Apple Mail, mutt, Pine, or any RFC 4155 client
- Converts to EML for universal import
- No Thunderbird installation required for parsing

---

### 7. **Contact and Calendar Extraction for CRM Import**

**Scenario**: A sales team's contacts and calendar data are trapped in a legacy PST archive. The data needs to be imported into a CRM system that accepts CSV.

**Workflow**:
1. Open the PST file
2. Navigate to "Contacts" folder → 3,400 contacts displayed
3. Sort by Company → filter by Company contains "Acme"
4. Select filtered contacts → **Export → CSV** with columns: Full Name, Email, Company, Phone, Title
5. Navigate to "Calendar" folder → **Export → CSV** with columns: Subject, Start, End, Location, Attendees
6. Import both CSVs into the CRM

**Benefits**:
- CSV export with configurable column selection
- VCF export for importing into phone/Google Contacts
- ICS export for importing into Google Calendar or Apple Calendar
- Column mapping matches common CRM import templates

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
EmailInspectorPanel (QWidget)
├─ EmailInspectorController (QObject)
│  ├─ State: Idle / Loading / Searching / Exporting
│  ├─ Manages: Parsers, search workers, export workers
│  └─ Aggregates: File metadata, item counts, export results
│
├─ PstParser (QObject) [Worker Thread]
│  ├─ NDB Layer: Node database B-tree traversal
│  │   ├─ Header parsing (magic bytes, CRC, ANSI vs Unicode detection)
│  │   ├─ Page map: AMap, PMap, FMap, FPMap
│  │   ├─ Node BTree (NBT) traversal
│  │   └─ Block BTree (BBT) traversal
│  ├─ LTP Layer: Property context and table context
│  │   ├─ Property Context (PC) — per-item property bags
│  │   ├─ Table Context (TC) — folder contents tables
│  │   └─ Heap-on-Node (HN) allocator for variable-size data
│  ├─ Messaging Layer: MAPI objects mapped onto NDB/LTP
│  │   ├─ Message Store — root object with display name, record key
│  │   ├─ Folder objects — hierarchy table, contents table, FAI table
│  │   ├─ Message objects — subject, body (plain + HTML + RTF), recipients, attachments
│  │   ├─ Attachment objects — filename, content bytes, embedded messages
│  │   ├─ Contact objects — name, email, phone, company, address, photo
│  │   ├─ Calendar objects — start/end, recurrence, attendees, location, reminders
│  │   ├─ Task objects — subject, due date, priority, status, percent complete
│  │   └─ Note objects — body, color, dimensions
│  ├─ Encryption: Compressible encryption (XOR-based, key-less, reversible)
│  ├─ CRC32 validation of pages and blocks
│  └─ Output: PstFolderTree, PstItemList, PstItemDetail
│
├─ MboxParser (QObject) [Worker Thread]
│  ├─ RFC 4155 MBOX parsing (From_ line delimited)
│  ├─ RFC 5322 message header parsing
│  ├─ MIME multipart parsing (RFC 2045–2049)
│  │   ├─ text/plain and text/html body extraction
│  │   ├─ Attachment extraction (Content-Disposition: attachment)
│  │   ├─ Inline image extraction (Content-ID)
│  │   ├─ Quoted-printable decoding (RFC 2045)
│  │   ├─ Base64 decoding (RFC 2045)
│  │   └─ Charset detection and conversion (UTF-8, ISO-8859-1, Windows-1252, etc.)
│  └─ Output: MboxMessageList, MboxMessageDetail
│
├─ EmailSearchWorker (QObject) [Worker Thread]
│  ├─ Full-text search across subject, body, sender, recipients
│  ├─ Date range filtering
│  ├─ Has-attachment filtering
│  ├─ Attachment filename filtering
│  ├─ Folder scope (search within folder or entire file)
│  ├─ MAPI property value search (advanced mode)
│  ├─ Case-insensitive with Unicode normalization
│  └─ Output: QVector<SearchHit> with context snippets
│
├─ EmailExportWorker (QObject) [Worker Thread]
│  ├─ Export emails → EML (RFC 5322 MIME message)
│  ├─ Export emails → CSV (subject, from, to, date, body-preview)
│  ├─ Export contacts → VCF (vCard 3.0, RFC 2426)
│  ├─ Export contacts → CSV (configurable columns)
│  ├─ Export calendar → ICS (iCalendar, RFC 5545)
│  ├─ Export calendar → CSV (configurable columns)
│  ├─ Export tasks → CSV
│  ├─ Export notes → plain text files
│  ├─ Batch attachment extraction with optional subfolder structure
│  ├─ Duplicate filename handling (append counter)
│  ├─ Progress reporting: items exported, bytes written
│  └─ Output: ExportResult (path, item_count, total_bytes, errors)
│
├─ EmailProfileManager (QObject) [Worker Thread]
│  ├─ Outlook profile discovery (registry scan)
│  │   ├─ HKCU\Software\Microsoft\Office\16.0\Outlook\Profiles
│  │   ├─ HKCU\Software\Microsoft\Office\15.0\Outlook\Profiles
│  │   ├─ HKCU\Software\Microsoft\Windows NT\CurrentVersion\Windows Messaging Subsystem\Profiles
│  │   └─ Data file paths (PST, OST) from profile entries
│  ├─ Thunderbird profile discovery
│  │   ├─ %APPDATA%\Thunderbird\profiles.ini parsing
│  │   ├─ Profile directory enumeration
│  │   └─ MBOX + SQLite (global-messages-db.sqlite) discovery
│  ├─ Windows Mail data discovery
│  │   ├─ %LOCALAPPDATA%\Comms\Unistore\data\
│  │   └─ Account configuration from Settings app data
│  ├─ Backup: Copy data files + export registry keys as .reg
│  ├─ Restore: Re-import .reg + copy data files to target paths
│  └─ Output: QVector<EmailClientProfile>
│
├─ OrphanedFileScanner (QObject) [Worker Thread]
│  ├─ Drive enumeration (fixed + removable)
│  ├─ Recursive scan for *.pst, *.ost, *.mbox, *.nsf
│  ├─ Cross-reference against discovered profiles
│  ├─ Quick header peek (file format validation + item count estimate)
│  ├─ Exclusion list (Windows\, $Recycle.Bin, etc.)
│  ├─ Progress: drives scanned, files found
│  └─ Output: QVector<OrphanedFileInfo>
│
└─ EmailReportGenerator (QObject)
   ├─ Aggregates file metadata, folder tree, item counts, export log
   ├─ Generates: HTML (printable), JSON (machine-readable), CSV (summary)
   ├─ Sections: File info, folder hierarchy, item statistics, export manifest
   └─ Technician/ticket fields for documentation
```

---

## 🛠️ Technical Specifications

### PST/OST Binary Parser

**Purpose**: Parse Microsoft PST and OST files per the [MS-PST] open specification, without requiring Outlook or any COM/MAPI library.

**Reference**: [MS-PST]: Outlook Personal Folders (.pst) File Format — Microsoft Open Specifications (publicly available, no license restrictions on implementation).

#### File Header

```cpp
// MS-PST §2.2.2.6 — PST file header
struct PstHeader {
    uint32_t magic;                // 0x4E444221 ("!BDN")
    uint32_t crc;                  // CRC32 of header fields
    uint16_t content_type;         // 0x534D ("SM") for PST
    uint16_t data_version;         // 14 = ANSI, 23 = Unicode (4K), 36 = Unicode
    uint16_t client_version;       // Outlook version that created the file
    uint8_t  platform_create;      // 0x01 = Windows
    uint8_t  platform_access;

    // Encoding (for OST compressible encryption)
    uint8_t  encryption_type;      // 0x00 = None, 0x01 = Compressible, 0x02 = High

    // Root structure pointers (Unicode PST)
    uint64_t root_nbt_page;        // File offset of Node BTree root page
    uint64_t root_bbt_page;        // File offset of Block BTree root page
    uint64_t root_amap_page;       // File offset of first AMap page

    // File size and allocation tracking
    uint64_t file_size;
    uint64_t free_space;
};
```

#### Data Structures

```cpp
// Represents a node in the PST internal hierarchy
struct PstNode {
    uint64_t node_id;              // NID — encodes type (folder, message, etc.)
    uint64_t data_bid;             // Block ID for this node's data
    uint64_t subnode_bid;          // Block ID for sub-node BTree (attachments, recipients)
    uint64_t parent_node_id;       // Parent NID in hierarchy
};

// Node ID type extraction
enum class PstNodeType : uint8_t {
    HeapNode         = 0x00,
    InternalNode     = 0x01,
    NormalFolder     = 0x02,
    SearchFolder     = 0x03,
    NormalMessage    = 0x04,
    Attachment       = 0x05,
    SearchUpdateQueue = 0x06,
    SearchCriteriaObj = 0x07,
    AssociatedMessage = 0x08,
    ContentsTable    = 0x0E,
    FaiContentsTable = 0x0F,
    HierarchyTable   = 0x0D,
    LtpContext       = 0x11,
    MessageStore     = 0x21,
    NameToIdMap      = 0x61,
};

// MAPI property tag: type (16-bit) + ID (16-bit)
struct MapiPropertyTag {
    uint16_t type;                 // PT_STRING8, PT_UNICODE, PT_LONG, PT_SYSTIME, etc.
    uint16_t id;                   // Property ID (e.g., 0x0037 = PR_SUBJECT)
};

// Commonly accessed MAPI property IDs
namespace MapiPropId {
    constexpr uint16_t kSubject                = 0x0037;
    constexpr uint16_t kSenderName             = 0x0C1A;
    constexpr uint16_t kSenderEmail            = 0x0C1F;
    constexpr uint16_t kDisplayTo              = 0x0E04;
    constexpr uint16_t kDisplayCc              = 0x0E03;
    constexpr uint16_t kMessageDeliveryTime    = 0x0E06;
    constexpr uint16_t kClientSubmitTime       = 0x0039;
    constexpr uint16_t kBody                   = 0x1000;
    constexpr uint16_t kHtmlBody               = 0x1013;
    constexpr uint16_t kRtfCompressed          = 0x1009;
    constexpr uint16_t kTransportHeaders       = 0x007D;
    constexpr uint16_t kMessageClass           = 0x001A;
    constexpr uint16_t kMessageFlags           = 0x0E07;
    constexpr uint16_t kMessageSize            = 0x0E08;
    constexpr uint16_t kHasAttachments         = 0x0E1B;
    constexpr uint16_t kImportance             = 0x0017;
    constexpr uint16_t kSensitivity            = 0x0036;

    // Attachment properties
    constexpr uint16_t kAttachFilename         = 0x3704;
    constexpr uint16_t kAttachLongFilename     = 0x3707;
    constexpr uint16_t kAttachData             = 0x3701;
    constexpr uint16_t kAttachSize             = 0x0E20;
    constexpr uint16_t kAttachMimeTag          = 0x370E;
    constexpr uint16_t kAttachContentId        = 0x3712;
    constexpr uint16_t kAttachMethod           = 0x3705;

    // Contact properties
    constexpr uint16_t kDisplayName            = 0x3001;
    constexpr uint16_t kEmailAddress           = 0x3003;
    constexpr uint16_t kCompanyName            = 0x3A16;
    constexpr uint16_t kBusinessPhone          = 0x3A08;
    constexpr uint16_t kMobilePhone            = 0x3A1C;
    constexpr uint16_t kHomePhone              = 0x3A09;
    constexpr uint16_t kJobTitle               = 0x3A17;
    constexpr uint16_t kGivenName              = 0x3A06;
    constexpr uint16_t kSurname                = 0x3A11;

    // Calendar properties
    constexpr uint16_t kStartDate              = 0x0060;
    constexpr uint16_t kEndDate                = 0x0061;
    constexpr uint16_t kLocation               = 0x8208;  // Named property
    constexpr uint16_t kRecurrencePattern      = 0x8232;  // Named property
    constexpr uint16_t kBusyStatus             = 0x8205;  // Named property
    constexpr uint16_t kAllDayEvent            = 0x8215;  // Named property

    // Task properties
    constexpr uint16_t kTaskDueDate            = 0x8105;  // Named property
    constexpr uint16_t kTaskStartDate          = 0x8104;  // Named property
    constexpr uint16_t kTaskStatus             = 0x8101;  // Named property
    constexpr uint16_t kTaskPercentComplete    = 0x8102;  // Named property
    constexpr uint16_t kTaskPriority           = 0x0026;

    // Note properties
    constexpr uint16_t kNoteColor              = 0x8B00;  // Named property
    constexpr uint16_t kNoteWidth              = 0x8B02;  // Named property
    constexpr uint16_t kNoteHeight             = 0x8B03;  // Named property
}

// MAPI property types
namespace MapiPropType {
    constexpr uint16_t kInt16       = 0x0002;
    constexpr uint16_t kInt32       = 0x0003;
    constexpr uint16_t kBoolean     = 0x000B;
    constexpr uint16_t kFloat64     = 0x0005;
    constexpr uint16_t kCurrency    = 0x0006;
    constexpr uint16_t kAppTime     = 0x0007;
    constexpr uint16_t kInt64       = 0x0014;
    constexpr uint16_t kString8     = 0x001E;
    constexpr uint16_t kUnicode     = 0x001F;
    constexpr uint16_t kSysTime     = 0x0040;
    constexpr uint16_t kGuid        = 0x0048;
    constexpr uint16_t kBinary      = 0x0102;
    constexpr uint16_t kMultiInt32  = 0x1003;
    constexpr uint16_t kMultiString = 0x101F;
    constexpr uint16_t kMultiBinary = 0x1102;
}
```

#### PST/OST Encryption

```cpp
// MS-PST §5.1 — Compressible Encryption
// OST files (and some PST files) use "compressible encryption" — a simple
// byte-level substitution cipher with no key. Two fixed 256-byte lookup tables
// (encrypt and decrypt) are defined in the spec. This is NOT security encryption;
// it exists only to prevent casual viewing of data in a hex editor.

constexpr std::array<uint8_t, 256> kDecryptTable = {
    // Full 256-byte decrypt table from MS-PST specification §5.1
    // Each byte of encrypted data is replaced by kDecryptTable[byte]
    0x47, 0xF1, 0xB4, 0xE6, 0x0B, 0x6A, 0x72, 0x48,
    0x85, 0x4E, 0x9E, 0xEB, 0xE2, 0xF8, 0x94, 0x53,
    // ... (full 256 entries populated from specification)
};

class PstDecryptor {
public:
    /// Decrypt a block of data in-place using compressible encryption
    static void decryptBlock(std::span<uint8_t> data) {
        for (auto& byte : data) {
            byte = kDecryptTable[byte];
        }
    }

    /// Determine encryption type from the file header
    static uint8_t detectEncryption(const PstHeader& header) {
        return header.encryption_type;
    }
};
```

#### Core Parser Class

```cpp
class PstParser : public QObject {
    Q_OBJECT

public:
    explicit PstParser(QObject* parent = nullptr);
    ~PstParser();

    /// Open a PST or OST file and parse the header + folder hierarchy
    void open(const QString& file_path);

    /// Close the currently opened file and release all resources
    void close();

    /// Get the folder tree (hierarchy only — items are loaded on demand)
    PstFolderTree folderTree() const;

    /// Load items (messages, contacts, etc.) for a specific folder
    void loadFolderItems(uint64_t folder_node_id, int offset, int limit);

    /// Load full detail for a specific item
    void loadItemDetail(uint64_t item_node_id);

    /// Load raw MAPI properties for a specific item
    void loadItemProperties(uint64_t item_node_id);

    /// Load attachment content bytes
    void loadAttachmentContent(uint64_t message_node_id, int attachment_index);

    void cancel();

Q_SIGNALS:
    void fileOpened(PstFileInfo file_info);
    void folderTreeLoaded(PstFolderTree tree);
    void folderItemsLoaded(uint64_t folder_id, QVector<PstItemSummary> items, int total_count);
    void itemDetailLoaded(PstItemDetail detail);
    void itemPropertiesLoaded(uint64_t item_id, QVector<MapiProperty> properties);
    void attachmentContentReady(uint64_t message_id, int index, QByteArray data, QString filename);
    void progressUpdated(int percent, QString status);
    void errorOccurred(QString error);

private:
    QFile m_file;
    PstHeader m_header;
    bool m_is_unicode{false};         // Unicode (64-bit) vs ANSI (32-bit)
    uint8_t m_encryption_type{0};

    // BTree caches
    QHash<uint64_t, PstNode> m_nbt_cache;     // Node BTree cache
    QHash<uint64_t, uint64_t> m_bbt_cache;    // Block BTree cache (BID → file offset)

    // NDB layer
    bool parseHeader();
    bool loadNodeBTree(uint64_t page_offset);
    bool loadBlockBTree(uint64_t page_offset);
    QByteArray readBlock(uint64_t bid);
    QByteArray readDataTree(uint64_t bid);   // Multi-block data trees (XBLOCKs)

    // LTP layer
    QVector<MapiProperty> readPropertyContext(uint64_t nid);
    QVector<QVector<MapiProperty>> readTableContext(uint64_t nid);
    QByteArray readHeapOnNode(uint64_t nid, uint32_t hn_id);

    // Messaging layer
    PstFolderTree buildFolderHierarchy(uint64_t root_nid);
    QVector<PstItemSummary> readContentsTable(uint64_t folder_nid, int offset, int limit);
    PstItemDetail readMessage(uint64_t message_nid);
    QVector<PstAttachmentInfo> readAttachments(uint64_t message_nid);
};
```

#### Shared Data Structures

```cpp
// File metadata after opening
struct PstFileInfo {
    QString file_path;
    QString display_name;            // Message store display name
    qint64 file_size_bytes;
    bool is_unicode;                 // Unicode (modern) vs ANSI (legacy)
    bool is_ost;                     // OST files have is_ost flag
    uint8_t encryption_type;         // 0=None, 1=Compressible, 2=High
    int total_folders;
    int total_items;                 // Estimated from internal counters
    QDateTime last_modified;
};

// Folder in the hierarchy tree
struct PstFolder {
    uint64_t node_id;
    uint64_t parent_node_id;
    QString display_name;
    int content_count;               // Number of items in this folder
    int unread_count;
    int subfolder_count;
    QString container_class;         // IPF.Note, IPF.Contact, IPF.Appointment, etc.
    QVector<PstFolder> children;     // Subfolders
};

using PstFolderTree = QVector<PstFolder>;

// Item types derived from PR_MESSAGE_CLASS
enum class EmailItemType {
    Email,          // IPM.Note
    Contact,        // IPM.Contact
    Calendar,       // IPM.Appointment
    Task,           // IPM.Task
    StickyNote,     // IPM.StickyNote
    JournalEntry,   // IPM.Activity
    DistList,       // IPM.DistList
    MeetingRequest, // IPM.Schedule.Meeting.Request
    Unknown
};

// Lightweight summary for list view (loaded from contents table)
struct PstItemSummary {
    uint64_t node_id;
    EmailItemType item_type;
    QString subject;
    QString sender_name;
    QString sender_email;
    QDateTime date;                  // Delivery time or start date
    qint64 size_bytes;
    bool has_attachments;
    bool is_read;
    int importance;                  // 0=Low, 1=Normal, 2=High
};

// Full detail for a single item (loaded on demand)
struct PstItemDetail {
    uint64_t node_id;
    EmailItemType item_type;

    // Common
    QString subject;
    QString sender_name;
    QString sender_email;
    QDateTime date;
    qint64 size_bytes;
    int importance;

    // Email-specific
    QString body_plain;              // Plain text body
    QString body_html;               // HTML body
    QByteArray body_rtf_compressed;  // Compressed RTF (MS-OXRTFCP)
    QString transport_headers;       // Full RFC 5322 headers
    QString display_to;
    QString display_cc;
    QString display_bcc;
    QString message_id;              // RFC 5322 Message-ID
    QString in_reply_to;

    // Attachments
    QVector<PstAttachmentInfo> attachments;

    // Contact-specific
    QString given_name;
    QString surname;
    QString company_name;
    QString job_title;
    QString email_address;
    QString business_phone;
    QString mobile_phone;
    QString home_phone;
    QString business_address;
    QString home_address;
    QByteArray contact_photo;        // JPEG/PNG bytes

    // Calendar-specific
    QDateTime start_time;
    QDateTime end_time;
    QString location;
    bool is_all_day;
    int busy_status;                 // 0=Free, 1=Tentative, 2=Busy, 3=OOF
    QString recurrence_description;
    QStringList attendees;

    // Task-specific
    QDateTime task_due_date;
    QDateTime task_start_date;
    int task_status;                 // 0=NotStarted, 1=InProgress, 2=Complete, 3=Waiting, 4=Deferred
    double task_percent_complete;    // 0.0 to 1.0

    // Note-specific
    int note_color;                  // 0=Blue, 1=Green, 2=Pink, 3=Yellow, 4=White
};

// Attachment metadata
struct PstAttachmentInfo {
    int index;                       // Attachment index within message
    QString filename;                // Display filename
    QString long_filename;           // Long filename (preferred)
    qint64 size_bytes;
    QString mime_type;
    QString content_id;              // For inline images
    int attach_method;               // 1=ByValue, 5=EmbeddedMessage, 6=OLE
    bool is_embedded_message;        // attach_method == 5
};

// Raw MAPI property for the property inspector
struct MapiProperty {
    uint16_t tag_id;
    uint16_t tag_type;
    QString property_name;           // Human-readable name (e.g., "PR_SUBJECT")
    QString display_value;           // Formatted value for display
    QByteArray raw_value;            // Raw bytes
};

// MBOX-parsed message (for Thunderbird support)
struct MboxMessage {
    int message_index;               // Sequential index in the MBOX file
    qint64 file_offset;             // Byte offset of "From " line in MBOX
    qint64 message_size;
    QString subject;
    QString from;
    QString to;
    QString cc;
    QDateTime date;
    bool has_attachments;
};

struct MboxMessageDetail {
    int message_index;
    QString subject;
    QString from;
    QString to;
    QString cc;
    QString bcc;
    QDateTime date;
    QString message_id;
    QString body_plain;
    QString body_html;
    QString raw_headers;
    QVector<PstAttachmentInfo> attachments;  // Reuse attachment struct
};

// Search result
struct EmailSearchHit {
    uint64_t item_node_id;
    EmailItemType item_type;
    QString subject;
    QString sender;
    QDateTime date;
    QString context_snippet;         // Surrounding text around the match
    QString match_field;             // "subject", "body", "sender", "attachment"
    QString folder_path;             // "Inbox/Projects/Phase2"
};

// Export result
struct EmailExportResult {
    QString export_path;
    QString export_format;           // "EML", "CSV", "ICS", "VCF", "Attachments"
    int items_exported;
    int items_failed;
    qint64 total_bytes;
    QStringList errors;
    QDateTime started;
    QDateTime finished;
};

// Email client profile (for backup/restore)
struct EmailClientProfile {
    enum class ClientType { Outlook, Thunderbird, WindowsMail, Other };
    ClientType client_type;
    QString client_name;             // "Microsoft Outlook 2021", "Thunderbird 115"
    QString client_version;
    QString profile_name;            // "Default Outlook Profile"
    QString profile_path;            // Registry or file path

    struct DataFile {
        QString path;
        QString type;                // "PST", "OST", "MBOX", "SQLite"
        qint64 size_bytes;
        bool is_linked;              // Currently linked to a profile
    };

    QVector<DataFile> data_files;
    qint64 total_size_bytes;
};

// Orphaned file info
struct OrphanedFileInfo {
    QString file_path;
    QString file_type;               // "PST", "OST", "MBOX"
    qint64 file_size_bytes;
    QDateTime last_modified;
    bool is_valid;                   // Header parsed successfully
    QString display_name;            // Message store name (for PST/OST)
    int estimated_item_count;
    bool is_linked_to_profile;       // Cross-referenced against discovered profiles
};
```

---

### MBOX Parser

**Purpose**: Parse MBOX files (Thunderbird, Apple Mail, Linux mail clients) per RFC 4155.

```cpp
class MboxParser : public QObject {
    Q_OBJECT

public:
    explicit MboxParser(QObject* parent = nullptr);

    void open(const QString& file_path);
    void close();

    /// Index all messages (scan for "From " lines) without loading bodies
    void indexMessages();

    /// Load summary list (headers only) for a range of messages
    void loadMessages(int offset, int limit);

    /// Load full detail for a specific message
    void loadMessageDetail(int message_index);

    void cancel();

Q_SIGNALS:
    void fileOpened(QString path, int estimated_count);
    void indexingComplete(int total_messages);
    void messagesLoaded(QVector<MboxMessage> messages, int total_count);
    void messageDetailLoaded(MboxMessageDetail detail);
    void progressUpdated(int percent, QString status);
    void errorOccurred(QString error);

private:
    QFile m_file;
    QVector<qint64> m_message_offsets;   // File offsets of each "From " line
    std::atomic<bool> m_cancelled{false};

    /// Scan file for all "From " line boundaries
    void buildMessageIndex();

    /// Parse RFC 5322 headers from raw bytes
    QMap<QString, QString> parseHeaders(const QByteArray& raw_headers);

    /// Parse MIME structure for body and attachments
    void parseMimeMessage(const QByteArray& raw_message, MboxMessageDetail& detail);

    /// Decode transfer encoding (base64, quoted-printable)
    QByteArray decodeTransferEncoding(const QByteArray& data, const QString& encoding);

    /// Decode character set to QString
    QString decodeCharset(const QByteArray& data, const QString& charset);
};
```

---

### Email Search Worker

**Purpose**: Full-text search across all items in an opened PST/OST/MBOX file.

```cpp
class EmailSearchWorker : public QObject {
    Q_OBJECT

public:
    struct SearchCriteria {
        QString query_text;              // Free-text search string
        bool search_subject{true};
        bool search_body{true};
        bool search_sender{true};
        bool search_recipients{false};
        bool search_attachment_names{false};
        bool case_sensitive{false};

        // Filters
        QDateTime date_from;             // Null = no lower bound
        QDateTime date_to;               // Null = no upper bound
        bool has_attachment_only{false};
        EmailItemType item_type_filter{EmailItemType::Unknown};  // Unknown = all types
        uint64_t folder_scope_id{0};     // 0 = search all folders

        // MAPI property search (advanced)
        uint16_t mapi_property_id{0};    // 0 = disabled
        QString mapi_property_value;
    };

    explicit EmailSearchWorker(QObject* parent = nullptr);

    void search(PstParser* parser, const SearchCriteria& criteria);
    void searchMbox(MboxParser* parser, const SearchCriteria& criteria);
    void cancel();

Q_SIGNALS:
    void searchHit(EmailSearchHit hit);
    void searchComplete(int total_hits, double elapsed_seconds);
    void progressUpdated(int items_searched, int total_items);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    bool matchesQuery(const QString& text, const QString& query, bool case_sensitive);
    QString extractContextSnippet(const QString& text, const QString& query, int context_chars);
};
```

---

### Email Export Worker

**Purpose**: Export items from PST/OST/MBOX files to standard formats.

```cpp
class EmailExportWorker : public QObject {
    Q_OBJECT

public:
    enum class ExportFormat {
        Eml,             // RFC 5322 MIME .eml files
        CsvEmails,       // CSV with email metadata + body preview
        Vcf,             // vCard 3.0 .vcf files
        CsvContacts,     // CSV with contact fields
        Ics,             // iCalendar .ics files
        CsvCalendar,     // CSV with calendar fields
        CsvTasks,        // CSV with task fields
        PlainTextNotes,  // Plain .txt files for sticky notes
        Attachments      // Extract attachment files
    };

    struct ExportConfig {
        ExportFormat format;
        QString output_path;             // Directory to export into
        QVector<uint64_t> item_ids;      // Specific items (empty = entire folder)
        uint64_t folder_id{0};           // Folder to export (0 = all selected)
        bool recurse_subfolders{false};

        // CSV options
        QStringList csv_columns;         // Column names to include
        QChar csv_delimiter{','};
        bool csv_include_header{true};
        QString csv_encoding{"UTF-8"};

        // Attachment options
        bool flatten_attachments{true};  // All into one folder vs. subfolder per message
        QString attachment_filter;       // Glob pattern, e.g., "*.pdf"
        bool skip_inline_images{true};   // Skip Content-ID referenced images

        // EML options
        bool eml_include_headers{true};

        // Naming
        bool prefix_with_date{true};     // "2024-01-15_Subject.eml"
    };

    explicit EmailExportWorker(QObject* parent = nullptr);

    void exportItems(PstParser* parser, const ExportConfig& config);
    void exportMboxItems(MboxParser* parser, const ExportConfig& config);
    void cancel();

Q_SIGNALS:
    void exportStarted(int total_items);
    void exportProgress(int items_done, int total_items, qint64 bytes_written);
    void exportComplete(EmailExportResult result);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    // Format writers
    bool writeEml(const PstItemDetail& item, const QString& output_dir, int index);
    bool writeVcf(const PstItemDetail& contact, const QString& output_dir, int index);
    bool writeIcs(const QVector<PstItemDetail>& events, const QString& output_path);
    bool writeCsv(const QVector<PstItemDetail>& items, const QString& output_path,
                  const QStringList& columns, QChar delimiter);
    bool extractAttachment(PstParser* parser, uint64_t msg_nid, int att_index,
                           const QString& output_dir);

    // EML generation per RFC 5322 / RFC 2045
    QByteArray buildEmlContent(const PstItemDetail& item);

    // vCard generation per RFC 2426
    QByteArray buildVcfContent(const PstItemDetail& contact);

    // iCalendar generation per RFC 5545
    QByteArray buildIcsContent(const QVector<PstItemDetail>& events);

    // Filename sanitization
    QString sanitizeFilename(const QString& name, int max_length);
    QString resolveFilenameConflict(const QString& dir, const QString& filename);
};
```

---

### Email Client Profile Manager

**Purpose**: Discover, backup, and restore email client profiles and data files.

```cpp
class EmailProfileManager : public QObject {
    Q_OBJECT

public:
    explicit EmailProfileManager(QObject* parent = nullptr);

    /// Discover all installed email clients and their profiles
    void discoverProfiles();

    /// Backup selected profiles to a target directory
    void backupProfiles(const QVector<int>& profile_indices, const QString& backup_path);

    /// Restore profiles from a backup manifest
    void restoreProfiles(const QString& backup_manifest_path);

    void cancel();

Q_SIGNALS:
    void profilesDiscovered(QVector<EmailClientProfile> profiles);
    void backupProgress(int files_done, int total_files, qint64 bytes_copied);
    void backupComplete(QString backup_path, int files_backed_up, qint64 total_bytes);
    void restoreComplete(int profiles_restored);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    // Discovery
    QVector<EmailClientProfile> discoverOutlookProfiles();
    QVector<EmailClientProfile> discoverThunderbirdProfiles();
    QVector<EmailClientProfile> discoverWindowsMailProfiles();

    // Backup
    bool exportRegistryKey(const QString& key_path, const QString& output_file);
    bool createBackupManifest(const QString& backup_path,
                              const QVector<EmailClientProfile>& profiles);

    // Restore
    bool importRegistryKey(const QString& reg_file);
};
```

---

### Orphaned File Scanner

**Purpose**: Scan all drives for PST/OST/MBOX files not linked to any email client profile.

```cpp
class OrphanedFileScanner : public QObject {
    Q_OBJECT

public:
    explicit OrphanedFileScanner(QObject* parent = nullptr);

    /// Scan specified drives (or all fixed drives if empty)
    void scan(const QStringList& drive_roots = {});

    /// Set the list of known profile-linked data files (for cross-referencing)
    void setLinkedFiles(const QSet<QString>& linked_paths);

    void cancel();

Q_SIGNALS:
    void fileFound(OrphanedFileInfo info);
    void scanProgress(QString current_path, int files_found);
    void scanComplete(QVector<OrphanedFileInfo> all_files);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};
    QSet<QString> m_linked_paths;

    /// Check file header to validate format and estimate item count
    OrphanedFileInfo peekFile(const QString& path);

    /// Excluded directories (Windows, $Recycle.Bin, etc.)
    bool isExcludedPath(const QString& path) const;
};
```

---

### Email Report Generator

**Purpose**: Generate professional HTML/JSON reports of inspection results.

```cpp
class EmailReportGenerator : public QObject {
    Q_OBJECT

public:
    struct ReportData {
        // Metadata
        QString technician_name;
        QString ticket_number;
        QString customer_name;
        QDateTime report_date;

        // File info
        PstFileInfo file_info;
        PstFolderTree folder_tree;

        // Statistics
        int total_emails{0};
        int total_contacts{0};
        int total_calendar_items{0};
        int total_tasks{0};
        int total_notes{0};
        int total_attachments{0};
        qint64 total_attachment_bytes{0};

        // Export log
        QVector<EmailExportResult> export_results;

        // Search log
        int searches_performed{0};
        int total_search_hits{0};

        // Profile info (if profile manager was used)
        QVector<EmailClientProfile> discovered_profiles;
        QVector<OrphanedFileInfo> orphaned_files;
    };

    explicit EmailReportGenerator(QObject* parent = nullptr);

    QString generateHtml(const ReportData& data);
    QByteArray generateJson(const ReportData& data);
    QString generateCsv(const ReportData& data);     // Summary CSV

Q_SIGNALS:
    void reportGenerated(QString output_path);
    void errorOccurred(QString error);
};
```

---

## 🖥️ UI Design

### Panel Layout

The Email & PST/OST Inspector Panel uses a horizontal split layout with a folder tree on the left and a content area on the right, consistent with familiar email client UIs.

```
┌──────────────────────────────────────────────────────────────────────────┐
│  Email & PST/OST Inspector                                              │
├──────────────────────────────────────────────────────────────────────────┤
│ [Open File ▼] [Close] │ Search: [________________] [🔍] [Advanced...] │
│ [Profile Manager] [Orphaned Files] [Export ▼] [Report ▼]               │
├─────────────────┬────────────────────────────────────────────────────────┤
│ 📁 Folder Tree  │  Item List (sortable columns)                        │
│                  │ ┌──────────────────────────────────────────────────┐  │
│ ▼ 📧 Mailbox    │ │ ☐ │ 📎│ Subject          │ From    │ Date      │  │
│   ▶ Inbox (142) │ │───┼───┼──────────────────┼─────────┼───────────│  │
│   ▶ Sent (89)   │ │ ☐ │ 📎│ RE: Q4 Budget    │ J.Smith │ 2024-12-1│  │
│   ▶ Drafts (3)  │ │ ☐ │   │ Meeting Tuesday  │ A.Jones │ 2024-12-1│  │
│   ▶ Deleted(51) │ │ ☑ │ 📎│ Invoice #4472    │ Vendor  │ 2024-12-1│  │
│   ▼ Projects    │ │ ☐ │   │ Welcome!         │ Admin   │ 2024-11-3│  │
│     ▶ Phase 1   │ │ ...                                             │  │
│     ▶ Phase 2   │ └──────────────────────────────────────────────────┘  │
│   ▶ Calendar    │                                                       │
│   ▶ Contacts    │  Detail Panel (tabbed)                                │
│   ▶ Tasks       │ ┌──────────────────────────────────────────────────┐  │
│   ▶ Notes       │ │ [Content] [Headers] [Properties] [Attachments]  │  │
│                  │ │                                                  │  │
│ ─────────────── │ │ From: J.Smith <jsmith@company.com>               │  │
│ File Info:       │ │ To: Team <team@company.com>                     │  │
│ budget_2024.pst │ │ Date: Dec 15, 2024 10:32 AM                     │  │
│ 2.4 GB, Unicode │ │ Subject: RE: Q4 Budget Review                   │  │
│ 3,247 items     │ │ ──────────────────────────────                   │  │
│ Encryption: None│ │ Hi Team,                                         │  │
│                  │ │                                                  │  │
│                  │ │ Please find the updated Q4 budget attached...    │  │
│                  │ │                                                  │  │
│                  │ │ 📎 Q4_Budget_Final.xlsx (245 KB) [Save] [Open]  │  │
│                  │ │ 📎 Notes.pdf (89 KB)             [Save] [Open]  │  │
│                  │ └──────────────────────────────────────────────────┘  │
├─────────────────┴────────────────────────────────────────────────────────┤
│ Status: Loaded budget_2024.pst — 3,247 items across 14 folders — Ready │
└──────────────────────────────────────────────────────────────────────────┘
```

### Tabs in the Main Panel

| Tab | Purpose |
|-----|---------|
| **Inspector** | Main PST/OST/MBOX browser with folder tree, item list, and detail panel |
| **Profile Manager** | Discover, backup, and restore email client profiles |
| **Orphaned Files** | Scan drives for PST/OST/MBOX files not linked to profiles |

### Detail Panel Tabs

| Tab | Content |
|-----|---------|
| **Content** | Rendered email body (HTML or plain text), contact card, calendar event details, or task/note view depending on item type |
| **Headers** | Full RFC 5322 transport headers (for emails) or raw MAPI header properties (for other types) |
| **Properties** | Sortable table of all MAPI properties: Tag ID, Tag Type, Property Name, Value — for advanced/forensic inspection |
| **Attachments** | List of attachments with filename, size, MIME type, and Save/Open/Save All buttons |

### Item List Columns (Configurable)

| Column | Email | Contact | Calendar | Task | Note |
|--------|-------|---------|----------|------|------|
| Subject | ✅ | — | ✅ | ✅ | ✅ |
| From / Name | ✅ Sender | ✅ Full Name | — | — | — |
| To | ✅ | — | — | — | — |
| Date | ✅ Received | — | ✅ Start | ✅ Due | ✅ Created |
| Size | ✅ | — | — | — | — |
| Has Attachments | ✅ (icon) | — | ✅ (icon) | — | — |
| Read/Unread | ✅ (bold) | — | — | — | — |
| Importance | ✅ (icon) | — | — | ✅ (icon) | — |
| Company | — | ✅ | — | — | — |
| Email Address | — | ✅ | — | — | — |
| Phone | — | ✅ | — | — | — |
| Location | — | — | ✅ | — | — |
| End Date | — | — | ✅ | — | — |
| Status | — | — | — | ✅ | — |
| % Complete | — | — | — | ✅ | — |
| Color | — | — | — | — | ✅ |

### Context Menu (Right-Click on Item)

```
┌────────────────────────────────┐
│ Open in Detail Panel           │
│ ───────────────────────────────│
│ Export as EML...               │  (emails only)
│ Export as VCF...               │  (contacts only)
│ Export as ICS...               │  (calendar only)
│ ───────────────────────────────│
│ Extract Attachments...         │  (items with attachments)
│ Save All Attachments...        │
│ ───────────────────────────────│
│ Copy Subject                   │
│ Copy Sender Email              │
│ Copy to Clipboard (Summary)    │
│ ───────────────────────────────│
│ View MAPI Properties           │
│ ───────────────────────────────│
│ Select All in Folder           │
│ Invert Selection               │
└────────────────────────────────┘
```

### Context Menu (Right-Click on Folder)

```
┌────────────────────────────────┐
│ Export Folder → EML...         │
│ Export Folder → CSV...         │
│ Export Folder → ICS...         │  (calendar folders)
│ Export Folder → VCF...         │  (contact folders)
│ ───────────────────────────────│
│ Extract All Attachments...     │
│ ───────────────────────────────│
│ Search in This Folder...       │
│ ───────────────────────────────│
│ Expand All Subfolders          │
│ Collapse All Subfolders        │
│ ───────────────────────────────│
│ Folder Properties              │
│ (item count, unread, class)    │
└────────────────────────────────┘
```

### Advanced Search Dialog

```
┌──────────────────────────────────────────────────────────┐
│  Advanced Search                                          │
├──────────────────────────────────────────────────────────┤
│                                                            │
│  Search text: [_________________________________]         │
│                                                            │
│  Search in:  [☑] Subject  [☑] Body  [☑] Sender           │
│              [☐] Recipients  [☐] Attachment names          │
│                                                            │
│  Date range: [__________] to [__________]                 │
│                                                            │
│  Item type:  (○) All  (○) Emails  (○) Contacts            │
│              (○) Calendar  (○) Tasks  (○) Notes            │
│                                                            │
│  [☐] Has attachments only                                 │
│                                                            │
│  Scope: (○) Entire file  (○) Current folder               │
│         (○) Current folder + subfolders                    │
│                                                            │
│  ──── Advanced (MAPI) ────                                │
│  Property ID: [0x____]  Value: [________________]         │
│                                                            │
│                           [Search]  [Cancel]               │
└──────────────────────────────────────────────────────────┘
```

### Profile Manager Tab

```
┌──────────────────────────────────────────────────────────────────────┐
│  Email Client Profiles                                               │
├──────────────────────────────────────────────────────────────────────┤
│                                                                       │
│ [Discover Profiles]  [Backup Selected]  [Restore from Backup...]     │
│                                                                       │
│ ┌────────────────────────────────────────────────────────────────┐   │
│ │ ☐ │ Client            │ Profile       │ Data Files │ Size     │   │
│ │───┼───────────────────┼───────────────┼────────────┼──────────│   │
│ │ ☑ │ 📧 Outlook 2021   │ Default       │ 3 files    │ 20.4 GB │   │
│ │   │   └─ archive.pst (8.2 GB)                                │   │
│ │   │   └─ user@co.com.ost (12.1 GB)                           │   │
│ │   │   └─ backup.pst (112 MB)                                 │   │
│ │ ☑ │ 🦊 Thunderbird 115 │ default-rel  │ 5 files    │ 3.8 GB  │   │
│ │   │   └─ Inbox (2.1 GB)                                      │   │
│ │   │   └─ Sent (890 MB)                                       │   │
│ │   │   └─ ...                                                  │   │
│ │ ☐ │ 📫 Windows Mail    │ (UWP Store)  │ 1 folder   │ 210 MB  │   │
│ └────────────────────────────────────────────────────────────────┘   │
│                                                                       │
│ Status: 3 email clients detected — 20 data files — 24.4 GB total    │
└──────────────────────────────────────────────────────────────────────┘
```

### Orphaned File Scanner Tab

```
┌──────────────────────────────────────────────────────────────────────┐
│  Orphaned Data File Scanner                                          │
├──────────────────────────────────────────────────────────────────────┤
│                                                                       │
│ Scan drives: [☑] C:  [☑] D:  [☐] E:  [☐] Network  [Scan]  [Stop]  │
│                                                                       │
│ ┌────────────────────────────────────────────────────────────────┐   │
│ │ Status │ Type │ Path                          │ Size   │ Items │   │
│ │────────┼──────┼───────────────────────────────┼────────┼───────│   │
│ │ 🔗     │ PST  │ C:\Users\...\archive.pst      │ 8.2 GB │ 12K  │   │
│ │ 🔗     │ OST  │ C:\Users\...\user@co.com.ost  │ 12 GB  │ 28K  │   │
│ │ ⚠️     │ PST  │ D:\Backup\old_emails.pst      │ 1.2 GB │ 4K   │   │
│ │ ⚠️     │ PST  │ D:\Desktop\vendor_archive.pst │ 890 MB │ 2K   │   │
│ │ ⚠️     │ MBOX │ C:\Users\...\Thunderbird\Inbox│ 2.1 GB │ 9K   │   │
│ │ ⚠️     │ PST  │ C:\Temp\migration_2023.pst    │ 3.4 GB │ 15K  │   │
│ └────────────────────────────────────────────────────────────────┘   │
│                                                                       │
│ 🔗 = Linked to profile (known)   ⚠️ = Orphaned (not in any profile) │
│                                                                       │
│ [Open Selected] [Copy to Archive...] [Generate Report]               │
│                                                                       │
│ Scanned: 142,847 folders on 2 drives — Found: 6 data files (4 orph) │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 📍 Constants & Configuration

```cpp
// email_constants.h

namespace sak::email {

// ── PST Parser Limits ──
constexpr int kMaxFolderDepth              = 50;       // Maximum nested folder depth
constexpr int kMaxItemsPerLoad             = 500;      // Items loaded per page in list view
constexpr int kMaxSearchResults            = 10000;    // Maximum search result count
constexpr int kMaxAttachmentSize           = 500 * 1024 * 1024;  // 500 MB per attachment
constexpr int kMaxFileSize                 = 50LL * 1024 * 1024 * 1024;  // 50 GB (PST format limit)
constexpr int kNodeBTreeCacheSize          = 50000;    // Max cached NDB nodes
constexpr int kBlockBTreeCacheSize         = 50000;    // Max cached BBT entries
constexpr int kPageSize                    = 512;      // NDB page size (bytes)
constexpr int kUnicodePageSize             = 4096;     // Unicode PST page size on 4K variant
constexpr int kMaxPropertyCount            = 5000;     // Max MAPI properties per item

// ── MBOX Parser Limits ──
constexpr int kMboxMaxMessageSize          = 100 * 1024 * 1024;  // 100 MB per message
constexpr int kMboxIndexBatchSize          = 10000;    // Messages indexed per progress update

// ── Export Limits ──
constexpr int kMaxExportBatchSize          = 50000;    // Max items per export operation
constexpr int kMaxFilenameLength           = 200;      // Max output filename characters
constexpr int kCsvMaxBodyPreviewChars      = 500;      // Body preview length in CSV export

// ── Search Limits ──
constexpr int kSearchContextSnippetChars   = 120;      // Characters of context around search match
constexpr int kSearchMaxConcurrent         = 1;        // Sequential search (disk I/O bound)

// ── Profile Manager ──
constexpr int kMaxProfileDiscoveryDepth    = 5;        // Registry key traversal depth
constexpr int kFileCopyBufferSize          = 4 * 1024 * 1024;  // 4 MB copy buffer

// ── Orphaned File Scanner ──
constexpr int kScanProgressIntervalMs      = 250;      // Progress update interval
constexpr int kScanMaxDepth                = 20;       // Max directory recursion depth
constexpr int kQuickPeekReadSize           = 1024;     // Bytes read for format validation

// ── UI Constants ──
constexpr int kFolderTreeMinWidth          = 200;
constexpr int kFolderTreeDefaultWidth      = 280;
constexpr int kItemListMinHeight           = 150;
constexpr int kDetailPanelMinHeight        = 200;
constexpr int kSearchDebounceMs            = 300;      // Delay before triggering search

// ── PST Magic Numbers ──
constexpr uint32_t kPstMagic               = 0x4E444221;  // "!BDN" (little-endian)
constexpr uint16_t kPstContentType         = 0x534D;      // "SM" for PST
constexpr uint16_t kOstContentType         = 0x4F53;      // "SO" for OST
constexpr uint16_t kAnsiVersion            = 14;          // ANSI PST format
constexpr uint16_t kUnicodeVersion         = 23;          // Unicode PST format
constexpr uint16_t kUnicode4kVersion       = 36;          // Unicode 4K-page PST

}  // namespace sak::email
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
email_inspector_panel.h              # Main UI panel (tabbed: Inspector, Profile Mgr, Orphaned)
email_inspector_controller.h         # Orchestrates parsers, searchers, exporters
email_types.h                        # All shared data structures (above)
email_constants.h                    # All named constants and limits
pst_parser.h                         # PST/OST binary parser
mbox_parser.h                        # MBOX/EML parser
email_search_worker.h                # Full-text search worker
email_export_worker.h                # Export to EML/CSV/VCF/ICS worker
email_profile_manager.h              # Profile backup/restore
orphaned_file_scanner.h              # Drive-wide data file discovery
email_report_generator.h             # HTML/JSON report generation
```

#### Implementation (`src/`)

```
gui/email_inspector_panel.cpp        # UI construction and layout
gui/email_inspector_panel_slots.cpp  # Signal/slot handlers
core/email_inspector_controller.cpp  # Controller logic
core/pst_parser.cpp                  # PST/OST file parsing
core/mbox_parser.cpp                 # MBOX file parsing
core/email_search_worker.cpp         # Search implementation
core/email_export_worker.cpp         # Export implementation
core/email_profile_manager.cpp       # Profile discovery, backup, restore
core/orphaned_file_scanner.cpp       # Orphaned file scanner
core/email_report_generator.cpp      # Report generation
```

#### Tests (`tests/`)

```
tests/unit/test_email_types.cpp               # Data structure defaults and validation
tests/unit/test_pst_parser.cpp                # PST parsing (with test fixture files)
tests/unit/test_mbox_parser.cpp               # MBOX parsing (with test fixture files)
tests/unit/test_email_search_worker.cpp       # Search query matching and filtering
tests/unit/test_email_export_worker.cpp       # Export format generation (EML, VCF, ICS, CSV)
tests/unit/test_email_profile_manager.cpp     # Profile discovery (mocked registry)
tests/unit/test_orphaned_file_scanner.cpp     # File scanning and format detection
tests/unit/test_email_report_generator.cpp    # Report output validation
```

#### Test Fixtures (`tests/fixtures/email/`)

```
tests/fixtures/email/tiny_unicode.pst         # Minimal valid Unicode PST (< 100 KB)
tests/fixtures/email/tiny_ansi.pst            # Minimal valid ANSI PST (< 100 KB)
tests/fixtures/email/sample.mbox              # Small MBOX with 5 messages
tests/fixtures/email/sample_multipart.eml     # MIME multipart test message
tests/fixtures/email/sample_attachment.eml    # Message with attachment
```

#### Resources

```
resources/email/
├─ report_template.html              # HTML report template
├─ mapi_property_names.json          # Property ID → human name mapping
└─ icons/
   ├─ email_inspector.svg
   ├─ pst_file.svg
   ├─ ost_file.svg
   ├─ mbox_file.svg
   ├─ folder_inbox.svg
   ├─ folder_sent.svg
   ├─ folder_calendar.svg
   ├─ folder_contacts.svg
   ├─ folder_tasks.svg
   ├─ folder_notes.svg
   ├─ item_email.svg
   ├─ item_contact.svg
   ├─ item_calendar.svg
   ├─ item_task.svg
   ├─ item_note.svg
   ├─ attachment.svg
   ├─ export.svg
   └─ search.svg
```

---

## 🔧 Third-Party Dependencies

### None Required for Core Parsing

The PST/OST parser is implemented from scratch using the publicly available [MS-PST] open specification. No third-party PST/MAPI library is needed.

| Dependency | Usage | License | Size | Notes |
|------------|-------|---------|------|-------|
| **None (MS-PST spec)** | PST/OST parsing | N/A | N/A | Implemented per open specification |
| **Qt6::Core** | File I/O, data structures | LGPL-3.0 | (existing) | Already in project |
| **Qt6::Widgets** | UI components | LGPL-3.0 | (existing) | Already in project |
| **Qt6::Concurrent** | Async operations | LGPL-3.0 | (existing) | Already in project |
| **ZLIB (vcpkg)** | Compressed RTF decompression | Zlib | (existing) | Already in project for benchmarks |

### Why Not Use libpff or readpst?

| Library | Issue |
|---------|-------|
| **libpff** (Joachim Metz) | LGPL-3.0 — acceptable, but adds a native C dependency that complicates the single-EXE portable deployment. The MS-PST spec is complete enough to parse from scratch. |
| **readpst** (libpst) | GPL-2.0 — incompatible with this project's AGPL-3.0 + commercial dual-license model. |
| **Aspose.Email** | Commercial license, expensive, .NET dependency. |
| **Custom MS-PST parser** | ✅ Zero dependencies, full control, spec is public, format is well-documented. |

The [MS-PST] open specification is ~200 pages and describes every byte of the format. Many open-source projects have successfully implemented PST parsers from this spec (libpff, java-libpst, pst-extractor for Node.js). Our C++23 implementation benefits from `std::span`, `std::expected`, structured bindings, and `<bit>` for endian-aware reads.

---

## 📋 Implementation Phases

### Phase 1: Data Structures & PST Header Parsing (Week 1–2)

**Goals**:
- Define all shared data structures in `email_types.h`
- Define all constants in `email_constants.h`
- Implement PST file header parsing and format detection (ANSI vs Unicode, encryption type)
- CRC32 validation of header

**Tasks**:
1. Create `email_types.h` with all structs above
2. Create `email_constants.h` with all named constants
3. Implement `PstParser::open()` — header parsing, magic validation, format detection
4. Implement `PstParser::close()` — clean resource release
5. Implement `PstDecryptor` — compressible encryption byte-substitution
6. Create test fixture: `tiny_unicode.pst` — hand-crafted minimal valid PST
7. Write `test_email_types.cpp` — struct defaults, enum conversions
8. Write `test_pst_parser.cpp` — header parsing tests (valid, invalid, ANSI, Unicode, encrypted)

**Acceptance Criteria**:
- ✅ Correctly identifies PST vs OST, ANSI vs Unicode
- ✅ CRC32 validation passes for valid files, fails for corrupted
- ✅ Encryption type correctly detected
- ✅ Rejects non-PST files with clear error message
- ✅ All tests pass

---

### Phase 2: NDB Layer — BTree Traversal (Week 3–4)

**Goals**:
- Implement Node BTree (NBT) and Block BTree (BBT) traversal
- Read pages, blocks, and multi-block data trees (XBLOCKs, XXBLOCKs)
- Handle both ANSI (32-bit) and Unicode (64-bit) offset formats

**Tasks**:
1. Implement `loadNodeBTree()` — recursive page traversal, populate `m_nbt_cache`
2. Implement `loadBlockBTree()` — recursive page traversal, populate `m_bbt_cache`
3. Implement `readBlock()` — read and optionally decrypt a single data block
4. Implement `readDataTree()` — follow XBLOCK/XXBLOCK chains for large data
5. Implement AMap page parsing for allocation tracking
6. Write tests for BTree traversal with fixture files

**Acceptance Criteria**:
- ✅ All nodes in fixture PST file discovered
- ✅ Handles ANSI and Unicode offset formats
- ✅ Multi-block data trees correctly reassembled
- ✅ Encrypted blocks correctly decrypted
- ✅ Invalid block references produce clear errors (not crashes)

---

### Phase 3: LTP Layer — Property Contexts & Table Contexts (Week 5–6)

**Goals**:
- Implement Heap-on-Node (HN) allocator for reading variable-size data
- Implement Property Context (PC) for reading item property bags
- Implement Table Context (TC) for reading folder contents tables

**Tasks**:
1. Implement `readHeapOnNode()` — HN page parsing, allocation table
2. Implement `readPropertyContext()` — BTH traversal, property extraction
3. Implement `readTableContext()` — column descriptors, row data, cell extraction
4. Property type interpretation (PT_UNICODE → QString, PT_SYSTIME → QDateTime, etc.)
5. Named property map resolution (NID 0x61 — Name-to-ID map)
6. Write tests for property reading with fixture files

**Acceptance Criteria**:
- ✅ Property values correctly extracted for all MAPI types
- ✅ Unicode strings decoded correctly
- ✅ SYSTIME (FILETIME) converted to QDateTime accurately
- ✅ Table context produces correct row-column data
- ✅ Named properties resolved to their numeric IDs

---

### Phase 4: Messaging Layer — Folders, Messages, Items (Week 7–9)

**Goals**:
- Implement folder hierarchy traversal
- Implement message reading (subject, body, headers, recipients)
- Implement attachment reading (metadata + content bytes)
- Implement contact, calendar, task, and note reading

**Tasks**:
1. Implement `buildFolderHierarchy()` — traverse hierarchy table from root
2. Implement `readContentsTable()` — paginated item list for a folder
3. Implement `readMessage()` — full message detail with all properties
4. Implement `readAttachments()` — enumerate + read attachment sub-nodes
5. Implement contact property extraction (name, email, phone, company, photo)
6. Implement calendar property extraction (start, end, recurrence, attendees, location)
7. Implement task and note property extraction
8. Implement compressed RTF decompression (MS-OXRTFCP algorithm, uses ZLIB)
9. Write comprehensive tests for each item type

**Acceptance Criteria**:
- ✅ Folder tree matches Outlook's view of the same PST
- ✅ All email fields correctly populated (subject, body, from, to, cc, date, headers)
- ✅ HTML body rendered correctly when available
- ✅ Attachments extractable with correct filenames and content
- ✅ Contacts have all fields (name, email, phone, company)
- ✅ Calendar events have start/end, location, recurrence description
- ✅ Tasks have due date, status, percent complete
- ✅ Compressed RTF correctly decompressed

---

### Phase 5: MBOX Parser & MIME Handling (Week 10–11)

**Goals**:
- Implement MBOX file indexing and message parsing
- Implement full MIME multipart parsing per RFC 2045–2049
- Handle character set conversion and transfer encoding

**Tasks**:
1. Implement `MboxParser::open()` and `buildMessageIndex()` — scan for "From " lines
2. Implement `loadMessages()` — parse headers for summary list
3. Implement `loadMessageDetail()` — full message with MIME parsing
4. Implement MIME multipart boundary parsing (nested parts)
5. Implement base64 and quoted-printable decoding
6. Implement charset detection and conversion (UTF-8, ISO-8859-*, Windows-1252, Shift_JIS, etc.)
7. Create test fixture: `sample.mbox` with 5 varied messages
8. Write tests for MBOX parsing, MIME decoding, charset handling

**Acceptance Criteria**:
- ✅ Correctly counts and indexes all messages in MBOX file
- ✅ Handles messages missing final newline
- ✅ Multi-part MIME correctly splits into body + attachments
- ✅ Base64 and quoted-printable decoded correctly
- ✅ Non-UTF-8 charsets converted without data loss
- ✅ Embedded messages (message/rfc822) parsed recursively

---

### Phase 6: Search Worker (Week 12)

**Goals**:
- Full-text search across all items in PST/OST/MBOX
- Date range filtering, attachment filtering, folder scoping
- Context snippet extraction for result display

**Tasks**:
1. Implement `EmailSearchWorker::search()` — iterate items, apply criteria
2. Implement `matchesQuery()` — case-insensitive substring/regex matching with Unicode normalization
3. Implement `extractContextSnippet()` — extract surrounding text for display
4. Date range filtering
5. Attachment name filtering
6. Folder scope limiting
7. MAPI property value search (advanced mode)
8. Progress reporting (items searched / total)
9. Write tests for search matching and filtering

**Acceptance Criteria**:
- ✅ Finds items matching query in subject, body, sender
- ✅ Date range correctly filters results
- ✅ Context snippets show relevant surrounding text
- ✅ Search across 10,000 items completes in < 10 seconds
- ✅ Cancellation stops search promptly
- ✅ All tests pass

---

### Phase 7: Export Worker — EML, VCF, ICS, CSV (Week 13–15)

**Goals**:
- Export emails to individual EML files (RFC 5322)
- Export contacts to VCF (vCard 3.0, RFC 2426)
- Export calendar to ICS (iCalendar, RFC 5545)
- Export any item type to CSV with configurable columns
- Bulk attachment extraction

**Tasks**:
1. Implement `writeEml()` — build RFC 5322 MIME message with headers + body + attachments
2. Implement `writeVcf()` — build vCard 3.0 with name, email, phone, company, photo
3. Implement `writeIcs()` — build iCalendar VCALENDAR with VEVENT components
4. Implement `writeCsv()` — configurable columns, proper escaping, UTF-8 BOM
5. Implement `extractAttachment()` — save attachment bytes to disk
6. Batch export with progress reporting
7. Filename sanitization and conflict resolution (append counter)
8. Date prefix for filename ordering
9. Write tests for each export format

**Acceptance Criteria**:
- ✅ EML files importable into Outlook, Thunderbird, and Windows Mail
- ✅ VCF files importable into Outlook, Google Contacts, and phone contact apps
- ✅ ICS files importable into Outlook, Google Calendar, and Apple Calendar
- ✅ CSV files open correctly in Excel with proper encoding (UTF-8 BOM)
- ✅ Attachments extracted with correct content (byte-for-byte match)
- ✅ Filename conflicts resolved without data loss
- ✅ All tests pass

---

### Phase 8: Email Client Profile Manager (Week 16–17)

**Goals**:
- Discover Outlook, Thunderbird, and Windows Mail profiles
- Backup data files + registry/config
- Restore profiles from backup

**Tasks**:
1. Implement `discoverOutlookProfiles()` — registry scan for all Outlook versions (14.0–16.0)
2. Implement `discoverThunderbirdProfiles()` — parse `profiles.ini`, enumerate profile dirs
3. Implement `discoverWindowsMailProfiles()` — enumerate UWP data store
4. Implement `backupProfiles()` — copy files + export `.reg` keys
5. Create backup manifest (JSON) with file paths, sizes, checksums
6. Implement `restoreProfiles()` — import `.reg` + copy files back
7. Handle locked files (Outlook running) — warn and skip or use VSS snapshot
8. Write tests (mocked registry for discovery, real file copy for backup)

**Acceptance Criteria**:
- ✅ Discovers Outlook 2016/2019/2021/365 profiles
- ✅ Discovers Thunderbird profiles with all MBOX + SQLite files
- ✅ Discovers Windows Mail data store
- ✅ Backup creates self-contained archive with manifest
- ✅ Restore correctly imports registry keys and copies files
- ✅ Handles locked files gracefully with clear user message

---

### Phase 9: Orphaned File Scanner (Week 18)

**Goals**:
- Scan drives for PST/OST/MBOX files
- Cross-reference against discovered profiles
- Quick header peek for validation

**Tasks**:
1. Implement drive enumeration (fixed + removable drives)
2. Implement recursive directory scan with exclusion list
3. Implement format validation via header peek (512 bytes)
4. Cross-reference found files against profile manager's linked files
5. Estimate item count from internal counters (PST header metadata)
6. Progress reporting (current directory, files found)
7. Write tests

**Acceptance Criteria**:
- ✅ Finds all PST/OST/MBOX files on scanned drives
- ✅ Correctly distinguishes linked vs orphaned files
- ✅ Excludes system directories ($Recycle.Bin, Windows, etc.)
- ✅ Quick peek validates format without full parse
- ✅ Cancellation stops scan promptly
- ✅ Handles permission-denied directories gracefully

---

### Phase 10: UI Panel Construction (Week 19–21)

**Goals**:
- Build the main Email Inspector panel with folder tree + item list + detail panel
- Build Profile Manager tab
- Build Orphaned Files tab
- Wire all signals/slots to controller

**Tasks**:
1. Create `EmailInspectorPanel` with tabbed layout (Inspector, Profile Manager, Orphaned Files)
2. Build folder tree widget with icons per folder type
3. Build item list with sortable columns (QSortFilterProxyModel)
4. Build detail panel with Content/Headers/Properties/Attachments tabs
5. Build toolbar (Open, Close, Search, Export, Report)
6. Build context menus for items and folders
7. Build Advanced Search dialog
8. Build Profile Manager tab with discover/backup/restore workflow
9. Build Orphaned Files tab with drive selection and scan results
10. Wire all UI events to controller → workers
11. Status bar with file info and operation progress

**Acceptance Criteria**:
- ✅ Panel loads and displays correctly in the main window tab bar
- ✅ Folder tree populates from parsed PST/OST/MBOX
- ✅ Item list sorts by any column
- ✅ Detail panel shows correct content for each item type
- ✅ Context menus trigger correct export/extraction operations
- ✅ All operations show progress and support cancellation
- ✅ Profile Manager discovers and displays all email clients
- ✅ Orphaned scanner results display correctly with linked/orphaned indicators

---

### Phase 11: Report Generation & Polish (Week 22–23)

**Goals**:
- Professional HTML/JSON/CSV report generation
- Final UI polish, keyboard shortcuts, accessibility
- Edge case handling and error recovery

**Tasks**:
1. Implement `EmailReportGenerator::generateHtml()` — styled report with all sections
2. Implement `EmailReportGenerator::generateJson()` — structured data output
3. Implement `EmailReportGenerator::generateCsv()` — summary spreadsheet
4. Add keyboard shortcuts (Ctrl+O open, Ctrl+F search, Ctrl+E export, etc.)
5. Add drag-and-drop PST/OST file opening
6. Handle corrupt PST gracefully (skip bad nodes, report errors, continue parsing)
7. Handle very large PST files (> 10 GB) with lazy loading and pagination
8. Memory management — ensure parsed items don't accumulate unbounded
9. Final accessibility pass (screen reader labels, tab order, focus indicators)
10. Write `test_email_report_generator.cpp`

**Acceptance Criteria**:
- ✅ HTML report is print-ready and professional
- ✅ JSON report valid and complete
- ✅ Corrupt PST files partially parseable (graceful degradation)
- ✅ 10+ GB PST files open without excessive memory usage
- ✅ All keyboard shortcuts work
- ✅ All tests pass

---

## ⌨️ Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `Ctrl+O` | Open PST/OST/MBOX file |
| `Ctrl+W` | Close current file |
| `Ctrl+F` | Focus search box |
| `Ctrl+Shift+F` | Open Advanced Search dialog |
| `Ctrl+E` | Export selected items |
| `Ctrl+Shift+E` | Extract attachments from selection |
| `Ctrl+A` | Select all items in current folder |
| `Ctrl+R` | Generate report |
| `F5` | Refresh current folder |
| `Enter` | Open selected item in detail panel |
| `Delete` | (no action — read-only inspector) |
| `Escape` | Cancel current operation / close dialog |

---

## 🔒 Security Considerations

### Read-Only Operations

The Email Inspector is strictly read-only. PST/OST files are opened with `QFile::ReadOnly` — the tool never modifies source data files. This is critical for forensic use cases where data integrity must be preserved.

### Sensitive Data Handling

- **Email content stays local** — no data is transmitted over the network.
- **Exported files go only to user-specified directories** — no temp file creation outside the export path.
- **Password-protected PST files** — the MS-PST "High Encryption" (encryption_type == 0x02) is not supported in v1.0. The tool will detect it and display a clear "Password-protected PST — not supported" message rather than producing corrupt output.
- **Clean memory release** — parsed email bodies and attachment bytes are released when the user navigates away. No sensitive data lingers in memory beyond its useful lifetime.

### File Validation

- **Magic number check** — reject files that don't start with `0x4E444221`
- **CRC32 validation** — verify header and page CRCs before trusting data
- **Size bounds checking** — all offsets validated against file size before seeking
- **Recursion depth limits** — folder tree traversal bounded by `kMaxFolderDepth`
- **Allocation limits** — no single allocation exceeds `kMaxAttachmentSize`

---

## 🧪 Testing Strategy

### Test Fixture Creation

Minimal PST test fixtures are created programmatically using a custom builder that writes the minimum valid NDB + LTP + Messaging structures. This avoids distributing real email data in the repository.

```cpp
// In test setup — create a minimal valid PST in a temp directory
class PstFixtureBuilder {
public:
    /// Create a minimal valid Unicode PST file with the given folder/item structure
    static bool createMinimalPst(const QString& output_path,
                                 const QVector<TestFolder>& folders);

    struct TestFolder {
        QString name;
        QVector<TestMessage> messages;
    };

    struct TestMessage {
        QString subject;
        QString from;
        QString body;
        QDateTime date;
    };
};
```

### Test Coverage Matrix

| Component | Happy Path | Error Cases | Edge Cases | Cancellation |
|-----------|-----------|-------------|------------|--------------|
| PstParser (header) | ✅ | ✅ Invalid magic, corrupt CRC | ✅ ANSI, Unicode, 4K-page | — |
| PstParser (NDB) | ✅ | ✅ Invalid offsets, circular refs | ✅ Single-page vs multi-level BTree | — |
| PstParser (LTP) | ✅ | ✅ Malformed property context | ✅ All MAPI property types | — |
| PstParser (Messaging) | ✅ | ✅ Missing properties, empty folders | ✅ Embedded messages, large attachments | ✅ |
| MboxParser | ✅ | ✅ Truncated file, missing "From " | ✅ Nested MIME, rare charsets | ✅ |
| EmailSearchWorker | ✅ | ✅ Empty query, no results | ✅ Unicode search terms, regex chars | ✅ |
| EmailExportWorker | ✅ | ✅ Read-only output dir, disk full | ✅ Filename conflicts, special chars | ✅ |
| EmailProfileManager | ✅ | ✅ No clients installed, locked files | ✅ Multiple Outlook versions | ✅ |
| OrphanedFileScanner | ✅ | ✅ Permission denied, invalid files | ✅ Network drives, large scans | ✅ |
| EmailReportGenerator | ✅ | ✅ Empty data, missing sections | ✅ Very large item counts | — |

---

## 📊 Performance Targets

| Operation | Target | Notes |
|-----------|--------|-------|
| Open 1 GB PST | < 3 seconds | Header + BTree load + folder tree only |
| Open 10 GB PST | < 10 seconds | Lazy loading — items loaded on demand |
| Load 500 items in folder | < 500 ms | Contents table read for one page |
| Load full message detail | < 200 ms | Property context + body + attachment metadata |
| Extract 1 MB attachment | < 100 ms | Block read + decrypt + write |
| Search 10,000 items | < 10 seconds | Sequential scan — disk I/O bound |
| Export 1,000 emails to EML | < 30 seconds | Parallel write with 4 MB buffers |
| Index 50,000-msg MBOX | < 15 seconds | Linear scan for "From " lines |
| Profile discovery | < 5 seconds | Registry + file system scan |
| Orphaned file scan (500K dirs) | < 60 seconds | Recursive scan with progress |

### Memory Budget

| Component | Budget | Strategy |
|-----------|--------|----------|
| NDB BTree cache | ≤ 50 MB | LRU eviction at `kNodeBTreeCacheSize` entries |
| Currently loaded items | ≤ 100 MB | Only `kMaxItemsPerLoad` items in memory at once |
| Single message detail | ≤ 50 MB | Largest reasonable email body + inline images |
| Attachment content | Stream to disk | Never buffer entire 500 MB attachment in RAM |
| MBOX message index | ≤ 20 MB | 8 bytes per message offset × 2.5M messages |
| Total panel footprint | ≤ 300 MB | Under any workload with bounded caches |

---

## 📚 Reference Documents

### Microsoft Open Specifications

- **[MS-PST]**: Outlook Personal Folders (.pst) File Format — Full binary format specification
  - https://learn.microsoft.com/en-us/openspecs/office_file_formats/ms-pst/
- **[MS-OXPROPS]**: Exchange Server Protocols Master Property List — All MAPI property definitions
  - https://learn.microsoft.com/en-us/openspecs/exchange_server_protocols/ms-oxprops/
- **[MS-OXRTFCP]**: Rich Text Format (RTF) Compression Algorithm — For compressed RTF body decompression
  - https://learn.microsoft.com/en-us/openspecs/exchange_server_protocols/ms-oxrtfcp/
- **[MS-OXCDATA]**: Data Structures — Shared data structures used across Exchange protocols
  - https://learn.microsoft.com/en-us/openspecs/exchange_server_protocols/ms-oxcdata/

### RFCs

- **RFC 5322**: Internet Message Format — EML file format
- **RFC 2045–2049**: MIME — Multipart messages, base64, quoted-printable, charsets
- **RFC 4155**: The application/mbox Media Type — MBOX file format
- **RFC 2426**: vCard MIME Directory Profile (vCard 3.0) — VCF contact format
- **RFC 5545**: Internet Calendaring and Scheduling (iCalendar) — ICS calendar format

### Existing Codebase References

- [include/sak/actions/outlook_backup_action.h](../include/sak/actions/outlook_backup_action.h) — Existing Outlook PST/OST file discovery and backup action (Quick Actions system)
- [src/actions/outlook_backup_action.cpp](../src/actions/outlook_backup_action.cpp) — Outlook backup implementation with file discovery logic
- [docs/NETWORK_DIAGNOSTIC_PANEL_PLAN.md](NETWORK_DIAGNOSTIC_PANEL_PLAN.md) — Reference architecture for panel implementation pattern

---

## 🔄 Integration with Existing Codebase

### Quick Action Synergy

The existing `OutlookBackupAction` already discovers and copies PST/OST files. The Email Inspector panel will reuse the same file discovery paths and extend them with full parsing capabilities. The backup action remains as a quick-operation shortcut; the panel provides deep inspection.

### Panel Registration

The Email Inspector panel will be registered in the main window's panel factory alongside existing panels:

```cpp
// In main_window.cpp panel creation
auto email_panel = new sak::EmailInspectorPanel(this);
addPanel("Email Inspector", email_panel, QIcon(":/icons/email_inspector.svg"));
```

### Logging

All operations use the existing `sak::logInfo`, `sak::logWarning`, `sak::logError` API:

```cpp
sak::logInfo("Opened PST file: {} ({} items, {} folders)",
             file_path.toStdString(), total_items, total_folders);
sak::logWarning("Corrupt node at NID {:#x} — skipping", node_id);
sak::logError("Failed to read block at offset {}: {}", offset, error.toStdString());
```

### Worker Threading

All parser and export workers follow the existing `WorkerBase` → `QThread` pattern with signal-only communication:

```cpp
auto thread = new QThread(this);
auto parser = new PstParser();
parser->moveToThread(thread);
connect(thread, &QThread::started, parser, [parser, path]() { parser->open(path); });
connect(parser, &PstParser::fileOpened, this, &EmailInspectorController::onFileOpened);
connect(parser, &PstParser::errorOccurred, this, &EmailInspectorController::onParserError);
thread->start();
```
