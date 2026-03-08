# Advanced Search Panel - Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 2, 2026  
**Status**: ✅ Complete  
**Completed in**: v0.8.5  
**Source Reference**: [RandyNorthrup/advanced_search](https://github.com/RandyNorthrup/advanced_search) (v0.5.1, MIT License)

---

## 🎯 Executive Summary

The Advanced Search Panel provides enterprise-grade grep-style file content searching directly within S.A.K. Utility. It enables technicians to rapidly search across directory trees for text patterns, regex expressions, file metadata, image EXIF/GPS data, archive contents, and binary/hex patterns — all within a unified three-panel interface (File Explorer | Results Tree | Preview Pane). This panel ports the functionality of the standalone [Advanced Search Tool](https://github.com/RandyNorthrup/advanced_search) (Python/PySide6) to native C++23/Qt 6.5 for seamless integration with the existing SAK architecture.

### Key Objectives
- ✅ **Text Content Search** - Grep-style pattern matching across directory trees with configurable context lines
- ✅ **Full Regex Support** - Regular expression search with 8 built-in pattern presets and custom pattern management
- ✅ **Image Metadata Search** - EXIF, GPS, and PNG metadata search for JPG, PNG, TIFF, GIF, BMP, WebP files
- ✅ **File Metadata Search** - Property extraction from PDF, Office (DOCX/XLSX/PPTX), audio/video, SQLite, XML, JSON, CSV, RTF, OpenDocument, eBooks, screenwriting formats
- ✅ **Archive Content Search** - Search inside ZIP and EPUB files without extraction
- ✅ **Binary/Hex Search** - Hex pattern matching in binary files with offset display
- ✅ **Three-Panel UI** - File Explorer (lazy-loading tree) | Results Tree (sortable, 8 modes) | Preview Pane (highlighted matches with navigation)
- ✅ **Search History** - Autocomplete-enabled history with persistent storage
- ✅ **Regex Pattern Library** - 8 built-in patterns (email, URL, IPv4, phone, dates, numbers, hex, identifiers) plus custom user patterns
- ✅ **Performance Optimized** - Worker thread search, LRU file cache, batch UI updates, configurable limits

---

## 📊 Project Scope

### What is Advanced Search?

**Advanced Search** is a comprehensive file content search tool that goes beyond simple filename matching. It searches *inside* files for text patterns, regex expressions, and metadata — similar to `grep` on Linux but with a full graphical interface, metadata awareness, and preview capabilities.

**Search Workflow**:
1. Technician selects a directory (or specific file) in the File Explorer
2. Enters a search pattern (text, regex, or selects from pattern library)
3. Configures options (case sensitive, whole word, context lines, file extensions, search mode)
4. Clicks Search — results populate in real-time in the Results Tree
5. Clicks any result to see the full file in the Preview Pane with all matches highlighted
6. Navigates between matches with Previous/Next buttons (yellow = all matches, orange = current)

**Search Modes**:
- **Text Content Search** (default) — Search file contents for literal text or regex patterns
- **Image Metadata Search** — Search EXIF/GPS/PNG metadata fields in image files
- **File Metadata Search** — Search document properties (author, title, dates) in PDF, Office, audio/video, etc.
- **Archive Search** — Search inside ZIP/EPUB files without extraction
- **Binary/Hex Search** — Search binary files for hex patterns, showing byte offsets

---

## 🎯 Use Cases

### 1. **Finding Configuration Values Across a Project**
**Scenario**: Technician needs to find all references to a specific API key or configuration value across a codebase.

**Workflow**:
1. Open Advanced Search Panel
2. Select project root directory in File Explorer
3. Enter the API key pattern in search box
4. Set file extensions to `.json,.yaml,.env,.ini,.config`
5. Click Search
6. Results tree shows every file containing the pattern with match count
7. Click any result to see the exact line with highlighted match in preview
8. Double-click to open the file in the default editor at that line

**Benefits**:
- Finds all occurrences across hundreds of files in seconds
- File extension filtering narrows results to config files only
- Preview pane shows exact context without opening each file

---

### 2. **Finding Photos by Location or Camera**
**Scenario**: Customer needs to find all photos taken with a specific camera or at a GPS location.

**Workflow**:
1. Open Advanced Search Panel
2. Select the photos directory
3. Enable "Image Metadata" checkbox
4. Search for "Canon" (camera make) or "GPS" (geotagged photos)
5. Results show every image with matching EXIF metadata
6. Preview pane displays all metadata fields with matches highlighted

**Benefits**:
- No need for specialized EXIF viewer software
- Searches all image formats (JPG, PNG, TIFF, GIF, BMP, WebP)
- GPS data extraction shows where photos were taken

---

### 3. **Auditing Document Authorship**
**Scenario**: Legal team needs to identify all documents authored by a former employee.

**Workflow**:
1. Open Advanced Search Panel
2. Select the file server / documents share
3. Enable "File Metadata" checkbox
4. Search for the employee's name
5. Results show PDFs, Word docs, Excel files, PowerPoint presentations with that author
6. Sort by "Date Modified (Newest)" to see most recent first

**Benefits**:
- Searches PDF metadata, Word properties, Excel properties, OpenDocument metadata
- Author, title, subject, keywords, creation date — all searchable
- Handles DOCX, XLSX, PPTX, ODS, ODT, ODP, RTF, EPUB

---

### 4. **Searching Inside ZIP Archives**
**Scenario**: Developer needs to find a specific error message in log archives (compressed as ZIP).

**Workflow**:
1. Open Advanced Search Panel
2. Select the logs archive directory
3. Enable "Archive Search" checkbox
4. Enter the error message pattern
5. Results show matches inside ZIP files as `archive.zip/internal/logfile.txt`
6. Preview pane shows the matching content from inside the archive

**Benefits**:
- No need to extract archives to search them
- Works with ZIP and EPUB formats
- Shows archive-internal path in results

---

### 5. **Finding Patterns with Regex Library**
**Scenario**: Security audit requires finding all email addresses, IP addresses, and URLs in a codebase.

**Workflow**:
1. Open Advanced Search Panel
2. Select the source directory
3. Click "Regex Patterns" dropdown
4. Check: Email addresses, URLs, IPv4 addresses
5. Search box auto-populates with combined regex pattern
6. Click Search — finds all occurrences of any of the three patterns
7. Sort by "Match Count (High-Low)" to find files with most sensitive data

**Benefits**:
- Pre-built regex patterns eliminate trial-and-error
- Multiple patterns combine with OR (`|`) automatically
- Custom patterns can be saved for reuse

---

### 6. **Binary File Forensics**
**Scenario**: Investigating a suspicious file for embedded text or known hex signatures.

**Workflow**:
1. Open Advanced Search Panel
2. Select the suspicious file in File Explorer
3. Enable "Binary/hex" checkbox
4. Search for a known hex pattern or text string
5. Results show byte offsets with hex dump context

**Benefits**:
- Searches binary content that normal text search would miss
- Shows hex dump with surrounding bytes for context
- Useful for forensics, malware analysis, file format investigation

---

## 🏗️ Architecture Overview

### Implementation Strategy: Pure C++ Port

The Advanced Search Tool (Python/PySide6) will be ported to native C++23/Qt 6.5 to match the SAK Utility's architecture. This approach ensures:

- **Consistent architecture** with all other SAK panels
- **No Python runtime dependency** (SAK is a single portable executable)
- **Better performance** for large directory tree searches
- **TigerStyle compliance** from the start
- **Native Qt integration** with existing `WorkerBase`, `ConfigManager`, signal/slot patterns

### Python → C++ Dependency Mapping

| Python Library | C++ Replacement | Purpose |
|---|---|---|
| `PySide6` (Qt6 bindings) | Qt 6.5 (native C++) | UI framework — direct mapping |
| `Pillow` (PIL) | Qt 6.5 `QImage` + custom EXIF parser | Image metadata (EXIF, GPS, PNG info) |
| `PyPDF2` | Custom lightweight PDF metadata parser | PDF document properties |
| `python-docx` | `QIODevice` + `QuaZip` (ZIP) + XML parsing | DOCX metadata extraction |
| `openpyxl` | `QIODevice` + `QuaZip` (ZIP) + XML parsing | XLSX metadata extraction |
| `mutagen` | `taglib` (C library) or custom parser | Audio/video tag extraction |
| `os.walk` / `os.scandir` | `QDirIterator` with lazy loading | Directory traversal |
| `re` (Python regex) | `QRegularExpression` (PCRE2) | Regex pattern matching |
| `json` | `QJsonDocument` / `QJsonObject` | JSON file metadata + settings persistence |
| `csv` | Custom CSV header parser (trivial) | CSV column metadata |
| `xml.etree.ElementTree` | `QXmlStreamReader` | XML / OPF / ODF metadata parsing |
| `zipfile` | `QuaZip` library (or `minizip` via vcpkg) | ZIP/EPUB/DOCX/XLSX archive access |
| `sqlite3` | `QSqlDatabase` + `QSqlQuery` (Qt SQL) | SQLite database schema inspection |

### Component Hierarchy

```
AdvancedSearchPanel (QWidget) — Main UI panel
├─ AdvancedSearchController (QObject)
│  ├─ State: Idle / Searching / Cancelled
│  ├─ Manages: SearchWorker thread
│  ├─ Aggregates: Search results, sort/filter
│  └─ Persists: Search history, preferences, custom patterns
│
├─ SearchWorker (WorkerBase) [Worker Thread]
│  ├─ Directory traversal via QDirIterator
│  ├─ Text content search (line-by-line with context)
│  ├─ Regex compilation via QRegularExpression
│  ├─ File extension filtering
│  ├─ Exclusion pattern matching (.git, node_modules, etc.)
│  ├─ Max results limit / max file size limit
│  ├─ Cancellation via WorkerBase::checkStop()
│  └─ Output: QVector<SearchMatch>
│
├─ MetadataExtractor (QObject) [Worker Thread]
│  ├─ ImageMetadataExtractor
│  │  ├─ EXIF tag parsing (custom parser using QDataStream)
│  │  ├─ GPS coordinate extraction
│  │  ├─ PNG text chunk extraction
│  │  └─ Output: QMap<QString, QString>
│  │
│  ├─ FileMetadataExtractor
│  │  ├─ PDF: Custom binary parser for /Info dictionary
│  │  ├─ DOCX/XLSX/PPTX: QuaZip → XML → core properties
│  │  ├─ ODS/ODT/ODP: QuaZip → meta.xml → Dublin Core
│  │  ├─ EPUB: QuaZip → content.opf → Dublin Core
│  │  ├─ Audio/Video: taglib or custom ID3/MP4/FLAC parser
│  │  ├─ SQLite: QSqlDatabase → schema + table info
│  │  ├─ CSV: Header row extraction
│  │  ├─ JSON: Top-level structure analysis
│  │  ├─ XML: Root tag, namespace, attributes
│  │  ├─ RTF: Header metadata extraction
│  │  ├─ Screenwriting: FDX (XML), Fountain (text), Celtx (ZIP)
│  │  └─ Output: QMap<QString, QString>
│  │
│  └─ ArchiveSearcher
│     ├─ ZIP text content search (QuaZip)
│     ├─ EPUB text content search (QuaZip)
│     ├─ File size limit enforcement per archive member
│     └─ Output: QVector<SearchMatch>
│
├─ BinarySearcher (QObject) [Worker Thread]
│  ├─ Binary file reading via QFile
│  ├─ Text pattern matching in binary content
│  ├─ Hex pattern matching
│  ├─ Byte offset calculation
│  ├─ Hex dump context generation (16 bytes before/after)
│  └─ Output: QVector<SearchMatch>
│
└─ RegexPatternLibrary (QObject)
   ├─ 8 built-in patterns (email, URL, IPv4, phone, dates, numbers, hex, identifiers)
   ├─ Custom user patterns (persistent JSON storage)
   ├─ Pattern combination (OR-joined regex)
   └─ Pattern validation
```

---

## 🛠️ Technical Specifications

### Core Data Structures

```cpp
/// @brief Represents a single search match in a file
struct SearchMatch {
    QString filePath;           ///< Full path to the file (or archive_path/internal_path)
    int lineNumber;             ///< 1-based line number (or byte offset for binary)
    QString lineContent;        ///< Content of the matching line
    int matchStart;             ///< Character offset of match start within line
    int matchEnd;               ///< Character offset of match end within line
    QStringList contextBefore;  ///< Lines before the match (0-10)
    QStringList contextAfter;   ///< Lines after the match (0-10)
};

/// @brief Search configuration options
struct SearchConfig {
    QString rootPath;           ///< Directory or file to search
    QString pattern;            ///< Search pattern (text or regex)
    
    // Search mode flags
    bool caseSensitive = false;
    bool useRegex = false;
    bool wholeWord = false;
    bool searchImageMetadata = false;   ///< EXIF/GPS metadata search
    bool searchFileMetadata = false;    ///< PDF/Office/audio/video metadata
    bool searchInArchives = false;      ///< Search inside ZIP/EPUB
    bool hexSearch = false;             ///< Binary/hex search mode
    
    // Filtering
    QStringList fileExtensions;         ///< Empty = all files
    QStringList excludePatterns = {     ///< Exclusion regex patterns
        R"(\.git)", R"(\.svn)", R"(__pycache__)", R"(node_modules)",
        R"(\.pyc$)", R"(\.exe$)", R"(\.dll$)", R"(\.so$)", R"(\.bin$)"
    };
    
    // Limits
    int contextLines = 2;              ///< Lines of context (0-10)
    int maxResults = 0;                ///< 0 = unlimited
    qint64 maxSearchFileSize = 50LL * 1024 * 1024;  ///< 50 MB default
    int networkTimeoutSec = 5;         ///< UNC path timeout
};

/// @brief Regex pattern preset definition
struct RegexPatternInfo {
    QString key;                ///< Unique identifier (e.g., "emails")
    QString label;              ///< Display label (e.g., "Email addresses")
    QString pattern;            ///< Regex pattern string
    bool enabled = false;       ///< Currently active in combined search
};

/// @brief Search preferences (persisted)
struct SearchPreferences {
    int maxResults = 0;         ///< 0 = unlimited
    int maxPreviewFileSizeMb = 10;
    int maxSearchFileSizeMb = 50;
    int maxCacheSize = 50;      ///< LRU file cache entries
};
```

### Search Worker

**Purpose**: Execute directory-recursive file content search on a background thread using `WorkerBase`.

```cpp
class AdvancedSearchWorker : public WorkerBase {
    Q_OBJECT
public:
    explicit AdvancedSearchWorker(const SearchConfig& config, QObject* parent = nullptr);
    
    ~AdvancedSearchWorker() override = default;
    
    // Disable copy/move (inherited from WorkerBase)
    AdvancedSearchWorker(const AdvancedSearchWorker&) = delete;
    AdvancedSearchWorker& operator=(const AdvancedSearchWorker&) = delete;
    AdvancedSearchWorker(AdvancedSearchWorker&&) = delete;
    AdvancedSearchWorker& operator=(AdvancedSearchWorker&&) = delete;

Q_SIGNALS:
    /// @brief Emitted with accumulated results (batch updates for performance)
    void resultsReady(QVector<SearchMatch> matches);
    
    /// @brief Emitted per-file as search progresses
    void fileSearched(const QString& filePath, int matchCount);

protected:
    /// @brief Main search execution — runs on worker thread
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    SearchConfig m_config;
    
    /// @brief Check if path should be excluded
    [[nodiscard]] bool isExcluded(const QString& path) const;
    
    /// @brief Search a single file for the pattern
    [[nodiscard]] QVector<SearchMatch> searchFile(const QString& filePath, 
                                                    const QRegularExpression& regex) const;
    
    /// @brief Search file text content line-by-line
    [[nodiscard]] QVector<SearchMatch> searchTextContent(const QString& filePath,
                                                          const QRegularExpression& regex) const;
    
    /// @brief Search image metadata (EXIF, GPS, PNG)
    [[nodiscard]] QVector<SearchMatch> searchImageMetadata(const QString& filePath,
                                                            const QRegularExpression& regex) const;
    
    /// @brief Search file metadata (PDF, Office, audio, video, etc.)
    [[nodiscard]] QVector<SearchMatch> searchFileMetadata(const QString& filePath,
                                                           const QRegularExpression& regex) const;
    
    /// @brief Search inside archive files (ZIP, EPUB)
    [[nodiscard]] QVector<SearchMatch> searchArchive(const QString& filePath,
                                                      const QRegularExpression& regex) const;
    
    /// @brief Search binary file for hex/text patterns
    [[nodiscard]] QVector<SearchMatch> searchBinary(const QString& filePath,
                                                     const QRegularExpression& regex) const;
    
    /// @brief Check if path is a UNC network path
    [[nodiscard]] bool isNetworkPath(const QString& path) const;
    
    /// @brief Check network path accessibility with timeout
    [[nodiscard]] bool checkNetworkPathAccessible(const QString& path) const;
    
    /// @brief Compile the search regex from config
    [[nodiscard]] std::expected<QRegularExpression, QString> compileRegex() const;
    
    // File type classification
    static const QSet<QString> kImageExtensions;
    static const QSet<QString> kFileMetadataExtensions;
    static const QSet<QString> kArchiveExtensions;
};
```

**Execute Implementation Sketch**:
```cpp
auto AdvancedSearchWorker::execute() -> std::expected<void, sak::error_code> {
    auto regexResult = compileRegex();
    if (!regexResult) {
        return std::unexpected(sak::error_code::invalid_parameter);
    }
    const auto& regex = regexResult.value();
    
    QVector<SearchMatch> allMatches;
    
    // Check network accessibility
    if (isNetworkPath(m_config.rootPath)) {
        if (!checkNetworkPathAccessible(m_config.rootPath)) {
            return std::unexpected(sak::error_code::network_error);
        }
    }
    
    // Single file search
    if (QFileInfo(m_config.rootPath).isFile()) {
        if (!isExcluded(m_config.rootPath)) {
            auto matches = searchFile(m_config.rootPath, regex);
            allMatches.append(matches);
        }
        emit resultsReady(allMatches);
        return {};
    }
    
    // Directory recursive search
    QDirIterator it(m_config.rootPath, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    
    int batchCount = 0;
    constexpr int kBatchSize = 50;  // Emit results every 50 files
    QVector<SearchMatch> batchMatches;
    
    while (it.hasNext()) {
        if (checkStop()) {
            return {};  // Cancelled — WorkerBase handles cancelled signal
        }
        
        const QString filePath = it.next();
        
        // Skip excluded paths
        if (isExcluded(filePath)) continue;
        
        // Check file extension filter
        if (!m_config.fileExtensions.isEmpty()) {
            const QString ext = QFileInfo(filePath).suffix().toLower();
            bool matched = false;
            for (const auto& filter : m_config.fileExtensions) {
                if (filter.startsWith('.') && filePath.endsWith(filter, Qt::CaseInsensitive)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) continue;
        }
        
        // Search the file
        auto matches = searchFile(filePath, regex);
        if (!matches.isEmpty()) {
            emit fileSearched(filePath, matches.size());
            batchMatches.append(matches);
            allMatches.append(matches);
        }
        
        // Check max results limit
        if (m_config.maxResults > 0 && allMatches.size() >= m_config.maxResults) {
            break;
        }
        
        // Batch emit for UI responsiveness
        if (++batchCount % kBatchSize == 0 && !batchMatches.isEmpty()) {
            emit resultsReady(batchMatches);
            batchMatches.clear();
        }
    }
    
    // Emit remaining batch
    if (!batchMatches.isEmpty()) {
        emit resultsReady(batchMatches);
    }
    
    return {};
}
```

---

### Metadata Extractors

#### Image Metadata Extractor (EXIF/GPS/PNG)

**Purpose**: Extract EXIF, GPS, and PNG text metadata from image files without Pillow.

**Approach**: Custom EXIF parser using `QDataStream` to read TIFF IFD entries. Qt's `QImage` handles image decoding but doesn't expose EXIF. A lightweight custom parser (~300 lines) reads:

- **EXIF IFD0**: Camera make, model, orientation, date/time, software
- **EXIF SubIFD**: Exposure time, f-number, ISO, focal length, flash, lens model
- **GPS IFD**: Latitude, longitude, altitude, timestamp, datum
- **PNG text chunks**: tEXt, iTXt, zTXt chunks containing metadata

```cpp
class ImageMetadataExtractor {
public:
    /// @brief Extract all metadata from an image file
    /// @param filePath Path to image file
    /// @return Map of key-value metadata pairs
    [[nodiscard]] static QMap<QString, QString> extract(const QString& filePath);
    
private:
    /// @brief Parse EXIF data from JPEG APP1 marker
    [[nodiscard]] static QMap<QString, QString> parseExif(QIODevice& device);
    
    /// @brief Parse TIFF IFD (Image File Directory) entries
    [[nodiscard]] static QMap<QString, QString> parseIFD(QDataStream& stream, 
                                                          quint32 ifdOffset,
                                                          bool bigEndian);
    
    /// @brief Parse GPS IFD sub-directory
    [[nodiscard]] static QMap<QString, QString> parseGpsIFD(QDataStream& stream,
                                                             quint32 ifdOffset,
                                                             bool bigEndian);
    
    /// @brief Parse PNG text chunks
    [[nodiscard]] static QMap<QString, QString> parsePngChunks(QIODevice& device);
    
    /// @brief Convert EXIF tag ID to human-readable name
    [[nodiscard]] static QString exifTagName(quint16 tagId);
    
    /// @brief Convert GPS tag ID to human-readable name
    [[nodiscard]] static QString gpsTagName(quint16 tagId);
    
    /// @brief Format EXIF rational value (numerator/denominator)
    [[nodiscard]] static QString formatRational(quint32 num, quint32 den);
    
    // EXIF tag constants
    static constexpr quint16 kTagMake = 0x010F;
    static constexpr quint16 kTagModel = 0x0110;
    static constexpr quint16 kTagDateTime = 0x0132;
    static constexpr quint16 kTagExifIFD = 0x8769;
    static constexpr quint16 kTagGpsIFD = 0x8825;
    // ... (complete tag table)
};
```

#### File Metadata Extractor

**Purpose**: Extract metadata from PDF, Office, audio/video, database, and structured data files.

```cpp
class FileMetadataExtractor {
public:
    /// @brief Extract metadata from any supported file type
    [[nodiscard]] static QMap<QString, QString> extract(const QString& filePath);
    
    /// @brief Check if file type is supported for metadata extraction
    [[nodiscard]] static bool isSupported(const QString& extension);
    
private:
    // Document formats
    [[nodiscard]] static QMap<QString, QString> extractPdfMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractDocxMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractXlsxMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractPptxMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractOdfMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractRtfMetadata(const QString& filePath);
    
    // eBook / archive formats
    [[nodiscard]] static QMap<QString, QString> extractEpubMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractZipMetadata(const QString& filePath);
    
    // Structured data
    [[nodiscard]] static QMap<QString, QString> extractCsvMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractJsonMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractXmlMetadata(const QString& filePath);
    
    // Database
    [[nodiscard]] static QMap<QString, QString> extractSqliteMetadata(const QString& filePath);
    
    // Audio/Video (using taglib or custom parser)
    [[nodiscard]] static QMap<QString, QString> extractAudioMetadata(const QString& filePath);
    
    // Screenwriting formats
    [[nodiscard]] static QMap<QString, QString> extractFdxMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractFountainMetadata(const QString& filePath);
    [[nodiscard]] static QMap<QString, QString> extractCeltxMetadata(const QString& filePath);
};
```

**Office Document Metadata Extraction (DOCX/XLSX/PPTX)**:

All modern Office formats are ZIP archives containing XML files. Metadata lives in `docProps/core.xml` (Dublin Core) and `docProps/app.xml` (application properties).

```cpp
QMap<QString, QString> FileMetadataExtractor::extractDocxMetadata(const QString& filePath) {
    QMap<QString, QString> metadata;
    
    QuaZip zip(filePath);
    if (!zip.open(QuaZip::mdUnzip)) {
        return metadata;
    }
    
    // Read core properties (docProps/core.xml)
    if (zip.setCurrentFile("docProps/core.xml")) {
        QuaZipFile file(&zip);
        if (file.open(QIODevice::ReadOnly)) {
            QXmlStreamReader xml(&file);
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement()) {
                    const QString tag = xml.name().toString();
                    if (tag == "title" || tag == "creator" || tag == "subject" ||
                        tag == "description" || tag == "keywords" || 
                        tag == "category" || tag == "created" || tag == "modified") {
                        const QString value = xml.readElementText().left(200);
                        if (!value.isEmpty()) {
                            // Capitalize first letter for display
                            metadata[tag.at(0).toUpper() + tag.mid(1)] = value;
                        }
                    }
                }
            }
            file.close();
        }
    }
    
    // Count paragraphs/sheets from content
    if (zip.setCurrentFile("word/document.xml")) {
        QuaZipFile file(&zip);
        if (file.open(QIODevice::ReadOnly)) {
            QXmlStreamReader xml(&file);
            int paragraphCount = 0;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == u"p") {
                    ++paragraphCount;
                }
            }
            metadata["Paragraphs"] = QString::number(paragraphCount);
            file.close();
        }
    }
    
    zip.close();
    return metadata;
}
```

**PDF Metadata Extraction** (lightweight binary parser — no PyPDF2 dependency):

```cpp
QMap<QString, QString> FileMetadataExtractor::extractPdfMetadata(const QString& filePath) {
    QMap<QString, QString> metadata;
    
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return metadata;
    
    // PDF metadata is in the /Info dictionary
    // Read last 1KB to find xref table and trailer
    const qint64 readSize = qMin(file.size(), qint64(4096));
    file.seek(file.size() - readSize);
    QByteArray tail = file.read(readSize);
    
    // Find "startxref" to locate cross-reference table
    // Then find /Info reference in trailer dictionary
    // Follow object reference to /Info dictionary
    // Extract /Title, /Author, /Subject, /Keywords, /Creator, /Producer, /CreationDate
    
    // Also count pages by finding /Type /Page entries
    file.seek(0);
    QByteArray content = file.readAll();
    
    // Simple regex-based metadata extraction from PDF text
    static const QRegularExpression infoPattern(R"(/(\w+)\s*\(([^)]*)\))");
    auto it = infoPattern.globalMatch(content);
    while (it.hasNext()) {
        auto match = it.next();
        const QString key = match.captured(1);
        const QString value = match.captured(2).left(200);
        if (key == "Title" || key == "Author" || key == "Subject" ||
            key == "Keywords" || key == "Creator" || key == "Producer" ||
            key == "CreationDate" || key == "ModDate") {
            metadata[QString("PDF_%1").arg(key)] = value;
        }
    }
    
    // Count pages
    const int pageCount = content.count("/Type /Page") - content.count("/Type /Pages");
    if (pageCount > 0) {
        metadata["PDF_Pages"] = QString::number(pageCount);
    }
    
    file.close();
    return metadata;
}
```

**SQLite Metadata Extraction**:

```cpp
QMap<QString, QString> FileMetadataExtractor::extractSqliteMetadata(const QString& filePath) {
    QMap<QString, QString> metadata;
    
    // Use a unique connection name to avoid conflicts
    const QString connName = QString("sak_advsearch_%1").arg(quintptr(QThread::currentThread()));
    
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(filePath);
        db.setConnectOptions("QSQLITE_OPEN_READONLY");
        
        if (!db.open()) return metadata;
        
        QSqlQuery query(db);
        
        // Get table names
        query.exec("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name");
        QStringList tables;
        while (query.next()) {
            tables.append(query.value(0).toString());
        }
        metadata["Tables"] = tables.join(", ");
        metadata["Table Count"] = QString::number(tables.size());
        
        // Schema info for first 5 tables
        for (int i = 0; i < qMin(tables.size(), 5); ++i) {
            query.exec(QString("PRAGMA table_info(%1)").arg(tables[i]));
            QStringList columns;
            while (query.next()) {
                columns.append(query.value(1).toString());
            }
            metadata[QString("Table_%1_Columns").arg(tables[i])] = columns.join(", ");
            
            query.exec(QString("SELECT COUNT(*) FROM %1").arg(tables[i]));
            if (query.next()) {
                metadata[QString("Table_%1_Rows").arg(tables[i])] = query.value(0).toString();
            }
        }
        
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
    
    return metadata;
}
```

---

### Regex Pattern Library

**Purpose**: Provide 8 built-in regex pattern presets and support user-defined custom patterns.

```cpp
class RegexPatternLibrary : public QObject {
    Q_OBJECT
public:
    explicit RegexPatternLibrary(QObject* parent = nullptr);
    
    /// @brief Get all built-in patterns
    [[nodiscard]] QVector<RegexPatternInfo> builtinPatterns() const;
    
    /// @brief Get all custom user patterns
    [[nodiscard]] QVector<RegexPatternInfo> customPatterns() const;
    
    /// @brief Add a custom pattern
    void addCustomPattern(const QString& name, const QString& label, const QString& pattern);
    
    /// @brief Remove a custom pattern by key
    void removeCustomPattern(const QString& key);
    
    /// @brief Update a custom pattern
    void updateCustomPattern(const QString& key, const QString& label, const QString& pattern);
    
    /// @brief Set enabled state for a pattern (builtin or custom)
    void setPatternEnabled(const QString& key, bool enabled);
    
    /// @brief Get combined regex string for all enabled patterns
    [[nodiscard]] QString combinedPattern() const;
    
    /// @brief Get count of active patterns
    [[nodiscard]] int activeCount() const;
    
    /// @brief Clear all enabled patterns
    void clearAll();
    
    /// @brief Load custom patterns from persistent storage
    void loadCustomPatterns();
    
    /// @brief Save custom patterns to persistent storage
    void saveCustomPatterns();
    
Q_SIGNALS:
    void patternsChanged();
    
private:
    QVector<RegexPatternInfo> m_builtinPatterns;
    QVector<RegexPatternInfo> m_customPatterns;
    QString m_storageFile;
    
    void initBuiltinPatterns();
};
```

**Built-in Pattern Definitions**:
```cpp
void RegexPatternLibrary::initBuiltinPatterns() {
    m_builtinPatterns = {
        {"emails",   "Email addresses",    R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)"},
        {"urls",     "URLs (http/https)",   R"(https?://[^\s]+)"},
        {"ipv4",     "IPv4 addresses",      R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)"},
        {"phone",    "Phone numbers",       R"(\b(?:\+?1[-.]?)?(?:\(?[0-9]{3}\)?[-.]?)?[0-9]{3}[-.]?[0-9]{4}\b)"},
        {"dates",    "Dates (various)",     R"(\b\d{1,4}[-/.]\d{1,2}[-/.]\d{1,4}\b)"},
        {"numbers",  "Numbers",             R"(\b\d+\b)"},
        {"hex",      "Hex values",          R"(\b0x[0-9A-Fa-f]+\b|#[0-9A-Fa-f]{6}\b)"},
        {"words",    "Words/identifiers",   R"(\b[A-Za-z_]\w*\b)"},
    };
}
```

---

### Advanced Search Controller

**Purpose**: Orchestrate search operations, manage state, and persist settings.

```cpp
class AdvancedSearchController : public QObject {
    Q_OBJECT
public:
    enum class State {
        Idle,
        Searching,
        Cancelled
    };
    
    explicit AdvancedSearchController(QObject* parent = nullptr);
    ~AdvancedSearchController() override;
    
    // Search operations
    void startSearch(const SearchConfig& config);
    void cancelSearch();
    
    // State
    [[nodiscard]] State currentState() const;
    
    // Search history
    void addToHistory(const QString& pattern);
    void clearHistory();
    [[nodiscard]] QStringList searchHistory() const;
    
    // Preferences
    void setPreferences(const SearchPreferences& prefs);
    [[nodiscard]] SearchPreferences preferences() const;
    void loadPreferences();
    void savePreferences();
    
    // Regex pattern library
    [[nodiscard]] RegexPatternLibrary* patternLibrary() const;
    
Q_SIGNALS:
    void stateChanged(State newState);
    void searchStarted(const QString& pattern);
    void resultsReceived(QVector<SearchMatch> matches);
    void fileSearched(const QString& filePath, int matchCount);
    void searchFinished(int totalMatches, int totalFiles);
    void searchFailed(const QString& error);
    void searchCancelled();
    
    void statusMessage(const QString& message, int timeout);
    void progressUpdate(int current, int maximum);
    
private:
    State m_state = State::Idle;
    std::unique_ptr<AdvancedSearchWorker> m_worker;
    std::unique_ptr<RegexPatternLibrary> m_patternLibrary;
    
    QStringList m_searchHistory;
    SearchPreferences m_preferences;
    
    static constexpr int kMaxHistorySize = 50;
};
```

---

## 🎨 User Interface Design

### Advanced Search Panel Layout

The panel uses a three-panel horizontal splitter layout matching the original Python application.

```
┌─────────────────────────────────────────────────────────────────────┐
│ Advanced Search Panel                                               │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─── SEARCH BAR ──────────────────────────────────────────────┐   │
│  │                                                               │  │
│  │  Search for: [_______________________] [Search] [Stop]       │  │
│  │                                                               │  │
│  │  Context: [2▼] [Regex Patterns▼] ☐ Case sensitive            │  │
│  │  ☐ Whole word ☐ Image metadata                                │  │
│  │                                                               │  │
│  │  Extensions: [.py,.txt,.js____] ☐ File metadata              │  │
│  │  ☐ Archive search ☐ Binary/hex                                │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─ THREE-PANEL SPLITTER ───────────────────────────────────────┐  │
│  │                                                               │  │
│  │  ┌──────────┐ ┌──────────────────┐ ┌──────────────────────┐ │  │
│  │  │ FILE     │ │ SEARCH RESULTS   │ │ PREVIEW              │ │  │
│  │  │ EXPLORER │ │                  │ │                      │ │  │
│  │  │          │ │ Sort: [Path▼]    │ │ Preview:  [◄] 3/15 [►]│ │  │
│  │  │ Home     │ │                  │ │                      │ │  │
│  │  │ ├ Docs   │ │ ▶ main.py (12)  │ │ File: /src/main.py   │ │  │
│  │  │ ├ Code   │ │   Line 45: ...  │ │ Total matches: 12    │ │  │
│  │  │ │ ├ src  │ │   Line 89: ...  │ │ ═══════════════════  │ │  │
│  │  │ │ └ test │ │   Line 234: ... │ │                      │ │  │
│  │  │ ├ Pics   │ │ ▶ config.py (3) │ │    42 | import os    │ │  │
│  │  │ └ Music  │ │   Line 12: ...  │ │    43 | import re    │ │  │
│  │  │          │ │   Line 78: ...  │ │    44 |              │ │  │
│  │  │ C:       │ │                  │ │ >>> 45 | pattern =   │ │  │
│  │  │ D:       │ │                  │ │    46 | if match:    │ │  │
│  │  │          │ │                  │ │    47 |   result =   │ │  │
│  │  │          │ │                  │ │                      │ │  │
│  │  └──────────┘ └──────────────────┘ └──────────────────────┘ │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ┌─ STATUS BAR ─────────────────────────────────────────────────┐  │
│  │ Found 15 matches in 2 files                      [████████] │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Search Bar Detail

```
┌────────────────────────────────────────────────────────────────────┐
│                                                                    │
│  Search for: [pattern_with_autocomplete_history____▼] 🔍 ⏹        │
│                                                                    │
│  ┌─ Row 1 ───────────────────────────────────────────────────────┐│
│  │ Context: [2▼]  [Regex Patterns (3) ▼]  ☐ Case sensitive      ││
│  │ ☐ Whole word   ☐ Image metadata                               ││
│  └────────────────────────────────────────────────────────────────┘│
│  ┌─ Row 2 ───────────────────────────────────────────────────────┐│
│  │ Extensions: [.py,.txt__________]  ☐ File metadata             ││
│  │ ☐ Archive search   ☐ Binary/hex                               ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

### Regex Pattern Menu

```
┌─────────────────────────────┐
│ Select Regex Patterns:      │
├─────────────────────────────┤
│ ☐ Email addresses           │
│ ☑ URLs (http/https)         │
│ ☑ IPv4 addresses            │
│ ☐ Phone numbers             │
│ ☐ Dates (various formats)   │
│ ☐ Numbers                   │
│ ☐ Hex values                │
│ ☐ Words/identifiers         │
├─────────────────────────────┤
│ Custom Patterns:            │
│ ☑ TODO/FIXME comments       │
├─────────────────────────────┤
│ Manage Custom Patterns...   │
├─────────────────────────────┤
│ Clear All                   │
└─────────────────────────────┘
```

### File Explorer Panel

```
┌──────────────────────────────┐
│ File Explorer:               │
├──────────────────────────────┤
│ ▼ Home (Randy)               │
│   ├ Documents                │
│   ├ ▼ Coding                 │
│   │  ├ ▼ S.A.K.-Utility     │
│   │  │  ├ src                │
│   │  │  ├ include            │
│   │  │  ├ tests              │
│   │  │  └ docs               │
│   │  └ advanced_search       │
│   ├ Desktop                  │
│   └ Downloads                │
│ ▶ C: (Windows)               │
│ ▶ D: (Data)                  │
│ ▶ E: (USB Drive)             │
└──────────────────────────────┘
```
- Lazy-loaded on expand (placeholder "Loading..." replaced on demand)
- Directories sorted before files
- Right-click context menu: Open Directory, Open Parent, Copy Path
- Click sets search target directory

### Results Tree Panel

```
┌──────────────────────────────────────┐
│ Search Results:         Sort: [Path▼]│
├──────────────────────────────────────┤
│ ▼ C:\Code\src\main.py         (12)  │
│     Line 45: pattern = re.com...     │
│     Line 89: if regex.match(l...     │
│     Line 234: matches.append(...     │
│ ▼ C:\Code\src\search.py        (3)  │
│     Line 12: import re              │
│     Line 78: regex = re.compil...    │
│     Line 156: for match in re...     │
│ ▶ C:\Code\config.json          (1)  │
│   Line 5: "pattern": "defaul...      │
└──────────────────────────────────────┘
```
- Sort options: Path (A-Z/Z-A), Match Count (High/Low), File Size (Large/Small), Date Modified (Newest/Oldest)
- Single click → preview, Double click → open in default editor (VS Code if available)
- Right-click: Open, Open Directory, Copy Path, Copy Line Content

### Preview Pane

```
┌──────────────────────────────────────┐
│ Preview:              [◄]  3/15  [►] │
├──────────────────────────────────────┤
│ File: C:\Code\src\main.py            │
│ Total matches: 12                    │
│ ════════════════════════════════════ │
│                                      │
│    43 | import re                    │
│    44 |                              │
│ >>> 45 | pattern = re.compile(...)   │  ← highlighted yellow + orange (current)
│    46 | if match:                    │
│    47 |     result = match.group()   │
│    48 |                              │
│    ...                               │
│    88 | # Process all matches        │
│ >>> 89 | if regex.match(line):       │  ← highlighted yellow
│    90 |     count += 1               │
│                                      │
└──────────────────────────────────────┘
```
- Match lines prefixed with `>>>`, others with `   `
- Line numbers displayed (5-digit right-aligned)
- All matches highlighted yellow (`QColor(255, 255, 0)`)
- Current match highlighted orange (`QColor(255, 165, 0)`)
- Previous/Next navigation with wrapping
- Match counter: "3/15" format
- Monospace font (Consolas, 10pt)

### Image Metadata Preview (when "Image metadata" mode active)

```
┌──────────────────────────────────────┐
│ Preview:              [◄]  1/3   [►] │
├──────────────────────────────────────┤
│ Image Metadata: C:\Photos\sunset.jpg │
│ Total matches: 3                     │
│ ════════════════════════════════════ │
│                                      │
│     1 | Format: JPEG                 │
│     2 | Mode: RGB                    │
│     3 | Size: 4032x3024              │
│ >>> 4 | Make: Canon                  │  ← highlighted
│ >>> 5 | Model: Canon EOS R5          │  ← highlighted
│     6 | DateTime: 2025:12:15 14:30   │
│     7 | ExposureTime: 1/250          │
│     8 | FNumber: 2.8                 │
│     9 | ISO: 400                     │
│ >>> 10| GPS_Info: {Latitude: 34.05...│  ← highlighted
│    11 | Software: Adobe Lightroom    │
│                                      │
└──────────────────────────────────────┘
```

### Preferences Dialog

```
┌──────────────────────────────────────┐
│ Preferences                     [X]  │
├──────────────────────────────────────┤
│                                      │
│  Max Search Results:  [0 (Unlimited)]│
│                                      │
│  Max Preview File Size:  [10] MB     │
│                                      │
│  Max Search File Size:   [50] MB     │
│                                      │
│  File Cache Size:        [50]        │
│                                      │
│             [OK]  [Cancel]           │
│                                      │
└──────────────────────────────────────┘
```

---

## 📂 File Structure

### New Files to Create

#### Headers (`include/sak/`)

```
advanced_search_panel.h                  # Main UI panel (three-panel splitter)
advanced_search_controller.h             # Orchestrates search operations
advanced_search_worker.h                 # WorkerBase subclass for search execution
advanced_search_types.h                  # Shared types (SearchMatch, SearchConfig, etc.)
image_metadata_extractor.h               # EXIF/GPS/PNG metadata parser
file_metadata_extractor.h                # PDF/Office/audio/video/SQLite metadata
regex_pattern_library.h                  # Built-in + custom regex patterns
```

#### Implementation (`src/`)

```
gui/advanced_search_panel.cpp            # Three-panel UI, search bar, results, preview
gui/advanced_search_panel_preview.cpp    # Preview pane logic (highlighting, navigation)
core/advanced_search_controller.cpp      # Controller orchestration, history, preferences
core/advanced_search_worker.cpp          # Directory traversal + search execution
core/image_metadata_extractor.cpp        # EXIF/GPS/PNG metadata parsing
core/file_metadata_extractor.cpp         # PDF/Office/audio/video/SQLite extraction
core/regex_pattern_library.cpp           # Pattern management and persistence
```

#### Tests (`tests/unit/`)

```
test_advanced_search_worker.cpp          # Search logic, exclusion, file types
test_image_metadata_extractor.cpp        # EXIF parsing, GPS, PNG chunks
test_file_metadata_extractor.cpp         # PDF, DOCX, XLSX, JSON, XML, SQLite
test_regex_pattern_library.cpp           # Built-in patterns, custom CRUD, combination
test_advanced_search_integration.cpp     # End-to-end search with real files
```

#### Test Fixtures (`tests/fixtures/`)

```
advanced_search/
├─ sample_text.txt                       # Simple text file for basic search tests
├─ sample_code.py                        # Python file with known patterns
├─ sample_photo.jpg                      # JPEG with EXIF/GPS metadata
├─ sample_document.docx                  # Word doc with known author/title
├─ sample_spreadsheet.xlsx               # Excel with known properties
├─ sample_archive.zip                    # ZIP containing searchable text files
├─ sample_data.json                      # JSON with known structure
├─ sample_data.csv                       # CSV with known headers
├─ sample_data.xml                       # XML with known root/attributes
├─ sample_database.sqlite                # SQLite with known tables/schema
└─ sample_binary.bin                     # Binary file with known hex pattern
```

---

## 🔧 Third-Party Dependencies

### QuaZip (ZIP Archive Access)

| Property | Value |
|----------|-------|
| **Library** | QuaZip |
| **Version** | 1.4 (latest stable) |
| **License** | LGPL-2.1 (compatible with SAK's AGPL-3.0) |
| **Source** | https://github.com/stachenov/quazip |
| **Purpose** | Read ZIP archives (ZIP, EPUB, DOCX, XLSX, PPTX, ODS, ODT, Celtx) |
| **Why this library** | Qt-native API, header-only option, well-maintained, wraps zlib (already in vcpkg) |
| **vcpkg install** | `vcpkg install quazip` |
| **Alternative considered** | `minizip` — lower-level, more manual work |

### TagLib (Audio/Video Metadata) — Optional

| Property | Value |
|----------|-------|
| **Library** | TagLib |
| **Version** | 2.0.2 (latest stable) |
| **License** | LGPL-2.1 / MPL-1.1 (dual-licensed, compatible) |
| **Source** | https://taglib.org / https://github.com/taglib/taglib |
| **Purpose** | Read ID3v2 (MP3), MP4/M4A, FLAC, Ogg, WMA tags |
| **Why this library** | Industry standard C++ audio tag library, no external dependencies |
| **vcpkg install** | `vcpkg install taglib` |
| **Alternative considered** | Custom ID3 parser — rejected due to format complexity |

### Dependencies Already Available

| Capability | Library / API | Status |
|------------|---------------|--------|
| Regex (PCRE2) | `QRegularExpression` | Qt 6.5 (already linked) |
| File system traversal | `QDirIterator`, `QFileInfo` | Qt 6.5 Core |
| JSON parsing | `QJsonDocument`, `QJsonObject` | Qt 6.5 Core |
| XML parsing | `QXmlStreamReader` | Qt 6.5 Core |
| SQLite access | `QSqlDatabase`, `QSqlQuery` | Qt 6.5 SQL |
| Image loading | `QImage` | Qt 6.5 GUI |
| Text editing / highlighting | `QTextEdit`, `QTextCharFormat` | Qt 6.5 Widgets |
| Tree widget | `QTreeWidget`, `QTreeWidgetItem` | Qt 6.5 Widgets |
| Worker threads | `WorkerBase` (SAK) | Already in codebase |
| Settings persistence | `ConfigManager` (SAK) | Already in codebase |
| zlib | vcpkg `zlib` | Already installed for SAK |

---

## 🔧 Implementation Phases

### Phase 1: Core Search Engine (Week 1-3)

**Goals**:
- Text content search with regex support
- Directory traversal with exclusion patterns
- Context lines extraction
- Worker thread integration via `WorkerBase`

**Tasks**:
1. Define `SearchMatch`, `SearchConfig`, `SearchPreferences` structs in `advanced_search_types.h`
2. Implement `AdvancedSearchWorker` extending `WorkerBase`
3. Directory recursive search via `QDirIterator`
4. Line-by-line text content search with `QRegularExpression`
5. Context lines extraction (0-10 lines before/after)
6. File extension filtering
7. Exclusion pattern matching (`.git`, `node_modules`, etc.)
8. Max results / max file size limits
9. Cancellation via `checkStop()`
10. Batch result emission for UI responsiveness
11. UNC network path detection and timeout handling
12. Write unit tests (`test_advanced_search_worker.cpp`)

**Acceptance Criteria**:
- ✅ Plain text search finds all occurrences in test files
- ✅ Regex search with case sensitivity and whole-word options works correctly
- ✅ Context lines (0-10) extracted accurately around matches
- ✅ Exclusion patterns skip `.git`, `node_modules`, etc.
- ✅ File extension filtering works correctly
- ✅ Search cancels within 100ms of `requestStop()`
- ✅ Max results limit stops search at configured count
- ✅ Files exceeding max size are skipped

---

### Phase 2: Metadata Extractors (Week 4-7)

**Goals**:
- Image metadata extraction (EXIF, GPS, PNG)
- File metadata extraction for all 20+ supported formats
- Archive content search
- Binary/hex search

**Tasks**:
1. Implement `ImageMetadataExtractor` with custom EXIF parser
   - JPEG APP1 marker → TIFF IFD parsing
   - GPS IFD sub-directory extraction
   - PNG tEXt/iTXt/zTXt chunk parsing
   - Test with sample photos (with/without EXIF, with/without GPS)
2. Implement `FileMetadataExtractor`
   - Add QuaZip dependency via vcpkg
   - PDF metadata: lightweight binary parser for `/Info` dictionary
   - DOCX: QuaZip → `docProps/core.xml` → Dublin Core properties
   - XLSX: QuaZip → `docProps/core.xml` + sheet count
   - PPTX: QuaZip → `docProps/core.xml` + slide count
   - ODS/ODT/ODP: QuaZip → `meta.xml` → Dublin Core
   - EPUB: QuaZip → `content.opf` → Dublin Core
   - RTF: Regex-based header metadata extraction
   - CSV: Header row extraction
   - JSON: Top-level structure analysis (type, keys/items)
   - XML: Root tag, namespace, attributes, child count
   - SQLite: `QSqlDatabase` → table list, schema, row counts
   - FDX: QXmlStreamReader → Final Draft metadata
   - Fountain: Line-by-line title page parsing
   - Celtx: QuaZip → project.celtx extraction
3. Add TagLib dependency via vcpkg for audio/video tags
   - MP3 (ID3v2), FLAC, M4A, OGG, WMA
   - MP4, AVI, MKV, MOV, WMV
   - Extract: title, artist, album, year, duration, bitrate
4. Implement archive content search via QuaZip
   - ZIP: Iterate entries → read as text → search
   - EPUB: Same approach (EPUB is ZIP)
   - File size limit per archive member
5. Implement binary/hex search
   - Read file as `QByteArray`
   - Text pattern match on UTF-8 decoded content
   - Hex dump context generation (16 bytes before/after)
   - Byte offset as line number
6. Write unit tests for each extractor
7. Create test fixture files

**Acceptance Criteria**:
- ✅ EXIF data extracted from JPEG files (camera make/model, exposure, GPS)
- ✅ PNG metadata chunks extracted
- ✅ PDF author/title/page count extracted
- ✅ DOCX/XLSX/PPTX properties extracted (author, title, dates)
- ✅ Audio tags extracted (artist, album, duration)
- ✅ SQLite schema inspection works (table names, columns, row counts)
- ✅ ZIP archive content searchable without extraction
- ✅ Binary search shows byte offsets with hex context
- ✅ All extractors handle corrupt/missing files gracefully (no crashes)

---

### Phase 3: Regex Pattern Library (Week 8)

**Goals**:
- 8 built-in regex pattern presets
- Custom pattern CRUD with persistent storage
- Pattern combination (OR-joined)
- Pattern validation

**Tasks**:
1. Implement `RegexPatternLibrary` class
2. Define 8 built-in patterns (email, URL, IPv4, phone, dates, numbers, hex, identifiers)
3. Custom pattern add/edit/remove with JSON persistence
4. Pattern combination — OR-join enabled patterns into single regex
5. Validation — test combined pattern compiles successfully
6. ConfigManager integration for storage path
7. Write unit tests (`test_regex_pattern_library.cpp`)

**Acceptance Criteria**:
- ✅ All 8 built-in patterns match their intended targets
- ✅ Custom patterns persist across app restarts
- ✅ Combined pattern correctly OR-joins multiple enabled patterns
- ✅ Invalid regex patterns reported as errors (not crashes)
- ✅ Pattern enable/disable state tracked per session

---

### Phase 4: UI Implementation — Three-Panel Layout (Week 9-13)

**Goals**:
- Complete Advanced Search Panel GUI
- File Explorer with lazy loading
- Results Tree with 8 sort modes
- Preview Pane with highlighting and navigation
- Search bar with all options
- Regex pattern menu

**Tasks**:
1. Create `AdvancedSearchPanel` with horizontal `QSplitter` (3 panels)
2. **Left Panel — File Explorer**:
   - `QTreeWidget` with lazy loading on expand
   - Home directory pre-populated with subdirectories
   - Drive letters enumerated (Windows)
   - Right-click context menu (Open Directory, Open Parent, Copy Path)
   - Single-click sets search target
3. **Search Bar** (above splitter):
   - `QComboBox` (editable) with search history autocomplete
   - Search/Stop toggle button
   - Context lines dropdown (0-10)
   - Regex Patterns button → popup `QMenu` with checkboxes
   - Checkboxes: Case sensitive, Whole word, Image metadata
   - File extensions input (`QLineEdit`)
   - Checkboxes: File metadata, Archive search, Binary/hex
4. **Middle Panel — Results Tree**:
   - `QTreeWidget` with file-level and match-level items
   - Sort dropdown with 8 modes
   - File item: path + match count
   - Match item: line number + truncated content (80 chars)
   - Single-click → preview, Double-click → open file
   - Right-click context menu
5. **Right Panel — Preview Pane**:
   - `QTextEdit` (read-only, Consolas 10pt)
   - Header: file path, total matches, separator
   - Full file content with `>>>` prefix on match lines
   - 5-digit right-aligned line numbers
   - All matches highlighted yellow (`QTextCharFormat`)
   - Current match highlighted orange
   - Previous/Next navigation buttons with wrapping
   - Match counter label ("3/15")
6. **Metadata Preview Modes**:
   - Image metadata display (key: value format)
   - File metadata display (key: value format)
   - Same highlighting and navigation as text preview
7. **LRU File Cache**:
   - Cache file contents in memory (configurable size)
   - Evict oldest on overflow
   - Invalidate on file modification (check file size)
8. Preferences dialog (max results, max file sizes, cache size)
9. Custom Pattern Manager dialog
10. Connect all signals/slots to `AdvancedSearchController`
11. SAK style compliance (match existing color scheme, margins, gradients)
12. Add panel to `MainWindow::createToolPanels()` with keyboard shortcut

**Acceptance Criteria**:
- ✅ Three-panel splitter resizable with saved proportions
- ✅ File Explorer lazy-loads directories on expand
- ✅ Search starts on Enter key or Search button click
- ✅ Results tree populates in real-time during search
- ✅ All 8 sort modes work correctly
- ✅ Preview pane shows full file with correct highlighting
- ✅ Match navigation (Previous/Next) cycles through all matches
- ✅ Regex pattern menu enables/disables patterns correctly
- ✅ Preferences persist via ConfigManager
- ✅ Panel matches SAK UI theme (colors, margins, fonts)
- ✅ Tab switching is instant

---

### Phase 5: Controller & Integration (Week 14-15)

**Goals**:
- Search history persistence
- Full MainWindow integration
- Settings persistence via ConfigManager

**Tasks**:
1. Implement `AdvancedSearchController` with full state management
2. Search history: load/save to JSON, max 50 entries, dedup
3. Preferences: load/save via `ConfigManager`
4. Add panel to `MainWindow::createToolPanels()`
5. Add panel include to `main_window.cpp`
6. Add keyboard shortcut tooltip
7. Connect `statusMessage` signal to status bar
8. Connect `progressUpdate` signal to progress bar
9. Add all new source files to `CMakeLists.txt`
10. Add QuaZip and TagLib to vcpkg dependencies
11. Add Qt6::Sql to CMake link targets (for SQLite)
12. Update `.github/workflows/build-release.yml` for new vcpkg packages
13. Test full build (0 errors, 0 warnings)

**Acceptance Criteria**:
- ✅ Panel appears as a tab in MainWindow
- ✅ Search history persists across app restarts
- ✅ Preferences persist across app restarts
- ✅ Custom regex patterns persist across app restarts
- ✅ Build succeeds locally and on CI
- ✅ All existing tests still pass (65+)

---

### Phase 6: Testing & Polish (Week 16-18)

**Goals**:
- Comprehensive test suite
- Edge case handling
- Performance optimization
- Documentation updates

**Tasks**:
1. Write unit tests for all components
   - `test_advanced_search_worker.cpp` — text search, regex, exclusions, limits
   - `test_image_metadata_extractor.cpp` — EXIF, GPS, PNG, missing data
   - `test_file_metadata_extractor.cpp` — PDF, DOCX, XLSX, JSON, XML, SQLite
   - `test_regex_pattern_library.cpp` — built-in, custom, combination, validation
   - `test_advanced_search_integration.cpp` — end-to-end with test fixtures
2. Create test fixture files (sample photos, documents, archives, databases)
3. Test edge cases:
   - Empty files
   - Very large files (> 50 MB limit)
   - Binary files in text search mode
   - Corrupt EXIF data
   - Password-protected ZIP files
   - Unicode filenames and content
   - Network (UNC) paths — accessible and inaccessible
   - Permission-denied files
   - Deeply nested directories (1000+ levels)
   - Very long filenames
4. Performance testing:
   - 100,000 files directory search
   - 1 GB file skip behavior
   - LRU cache hit/miss rates
   - UI responsiveness during large searches
5. TigerStyle compliance scan
6. Update `README.md` changelog for v0.8.0
7. Update `THIRD_PARTY_LICENSES.md` (QuaZip, TagLib)
8. Update `docs/ENTERPRISE_HARDENING_TRACKER.md`
9. Update `docs/CODEBASE_AUDIT_TRACKER.md`

**Acceptance Criteria**:
- ✅ All new unit tests pass
- ✅ All existing 65 tests still pass
- ✅ 0 compiler warnings
- ✅ TigerStyle lint: 0 errors
- ✅ Search 100K files completes in < 60 seconds
- ✅ UI remains responsive during search (< 100ms frame time)
- ✅ No crashes on corrupt/missing/permission-denied files
- ✅ CI build passes
- ✅ Documentation updated

---

**Total Timeline**: 18 weeks (4.5 months)

---

## 📋 CMakeLists.txt Changes

### New Source Files
```cmake
# Add to CORE_SOURCES:
src/core/advanced_search_controller.cpp
src/core/advanced_search_worker.cpp
src/core/image_metadata_extractor.cpp
src/core/file_metadata_extractor.cpp
src/core/regex_pattern_library.cpp
include/sak/advanced_search_controller.h
include/sak/advanced_search_worker.h
include/sak/advanced_search_types.h
include/sak/image_metadata_extractor.h
include/sak/file_metadata_extractor.h
include/sak/regex_pattern_library.h

# Add to GUI_SOURCES:
src/gui/advanced_search_panel.cpp
src/gui/advanced_search_panel_preview.cpp
include/sak/advanced_search_panel.h

# Add to Qt link targets:
Qt6::Sql                        # For SQLite metadata extraction

# Add to vcpkg dependencies:
# vcpkg.json (or vcpkg install):
#   quazip                      # ZIP archive reading
#   taglib                      # Audio/video metadata (optional)
```

### vcpkg.json Updates
```json
{
    "dependencies": [
        "zlib",
        "bzip2",
        "liblzma",
        "quazip",
        "taglib"
    ]
}
```

### CI Workflow Updates
```yaml
# In .github/workflows/build-release.yml
# Update vcpkg install step:
- name: Install vcpkg packages
  run: |
    vcpkg install zlib:x64-windows bzip2:x64-windows liblzma:x64-windows
    vcpkg install quazip:x64-windows taglib:x64-windows
```

---

## 📋 Configuration & Settings

### ConfigManager Extensions

```cpp
// Advanced Search Settings

// Search defaults
int getAdvSearchMaxResults() const;
void setAdvSearchMaxResults(int count);
int getAdvSearchMaxPreviewFileSizeMb() const;
void setAdvSearchMaxPreviewFileSizeMb(int mb);
int getAdvSearchMaxSearchFileSizeMb() const;
void setAdvSearchMaxSearchFileSizeMb(int mb);
int getAdvSearchCacheSize() const;
void setAdvSearchCacheSize(int count);
int getAdvSearchContextLines() const;
void setAdvSearchContextLines(int lines);

// Splitter state
QByteArray getAdvSearchSplitterState() const;
void setAdvSearchSplitterState(const QByteArray& state);

// Last search directory
QString getAdvSearchLastDirectory() const;
void setAdvSearchLastDirectory(const QString& path);
```

**Default Values**:
```cpp
advsearch/max_results = 0                    // unlimited
advsearch/max_preview_file_size_mb = 10
advsearch/max_search_file_size_mb = 50
advsearch/cache_size = 50
advsearch/context_lines = 2
advsearch/splitter_state = ""                // default 300:400:700
advsearch/last_directory = "%USERPROFILE%"
```

---

## 🧪 Testing Strategy

### Unit Tests

**test_advanced_search_worker.cpp**:
- Plain text search finds exact matches
- Regex search with quantifiers and alternation
- Case-sensitive vs. case-insensitive search
- Whole-word matching
- Context lines extraction (0, 1, 5, 10)
- File extension filtering (include and exclude)
- Exclusion pattern matching
- Max results limit enforcement
- Max file size skip behavior
- Empty pattern handling
- Invalid regex pattern handling
- UNC path detection
- Single file search vs. directory search

**test_image_metadata_extractor.cpp**:
- JPEG with full EXIF data (make, model, exposure)
- JPEG with GPS coordinates
- PNG with tEXt chunks
- Image with no EXIF data
- Corrupt JPEG file (no crash)
- Unsupported image format (graceful empty result)

**test_file_metadata_extractor.cpp**:
- PDF metadata extraction (author, title, pages)
- DOCX properties (author, title, created, paragraphs)
- XLSX properties (creator, sheets)
- JSON structure analysis (object vs. array)
- XML root tag and attributes
- CSV header extraction
- SQLite schema inspection (tables, columns, rows)
- Corrupt PDF file (no crash)
- Password-protected DOCX (graceful failure)

**test_regex_pattern_library.cpp**:
- Built-in email pattern matches valid emails
- Built-in URL pattern matches HTTP/HTTPS URLs
- Built-in IPv4 pattern matches valid IPs
- Custom pattern add/remove/update
- Combined pattern OR-joins correctly
- Pattern enable/disable tracking
- Invalid regex detection
- Persistence (save/load JSON round-trip)

**test_advanced_search_integration.cpp**:
- Search text in known fixture files
- Search regex pattern across directory
- Image metadata search with known EXIF data
- File metadata search with known DOCX author
- Archive search inside known ZIP contents
- Binary search with known hex pattern
- Search cancellation during long operation
- Sort results by all 8 modes

### Manual Testing

1. **Small directory** (< 100 files) — instant results
2. **Large directory** (10,000+ files) — verify progress and responsiveness
3. **Network share** (UNC path) — verify timeout handling
4. **Unicode content** — Chinese, Arabic, emoji in filenames and content
5. **Various file formats** — PDF, DOCX, XLSX, photos, audio, SQLite
6. **Dense regex** — multiple enabled patterns finding thousands of matches
7. **Binary files** — hex pattern search in `.exe`, `.dll`, `.bin`
8. **Nested archives** — ZIP containing ZIP (should only search one level)
9. **No permissions** — directory with access denied
10. **Empty directory** — no files to search

---

## 🚧 Limitations & Challenges

### Technical Limitations

**EXIF Parser Complexity**:
- ⚠️ Custom EXIF parser required since Qt doesn't expose EXIF metadata
- ⚠️ EXIF format varies between camera manufacturers (some non-standard tags)
- ⚠️ Maker notes are manufacturer-specific binary data — not parsed
- **Mitigation**: Focus on standard IFD0, EXIF SubIFD, and GPS IFD tags. Non-standard tags shown as `Unknown_XXXX`.

**PDF Metadata Extraction**:
- ⚠️ PDF is a complex binary format — full parser would be thousands of lines
- ⚠️ Encrypted/password-protected PDFs won't yield metadata
- ⚠️ Linearized PDFs may have metadata at different offsets
- **Mitigation**: Lightweight regex-based extraction from `/Info` dictionary. Covers 95%+ of real-world PDFs. If a user needs full PDF support, a dedicated PDF viewer is the right tool.

**Audio/Video Tag Variety**:
- ⚠️ ID3v1, ID3v2.3, ID3v2.4, MP4 atoms, Vorbis comments, APEv2 — many tag formats
- ⚠️ TagLib handles most but adds ~1 MB to binary size
- **Mitigation**: TagLib as optional dependency. If not available, audio metadata search is disabled with a clear message.

**Archive Search Limits**:
- ⚠️ Only searches one level deep (no nested archives)
- ⚠️ Encrypted ZIP files cannot be searched
- ⚠️ Very large archives (> 1 GB) may be slow
- **Mitigation**: Per-member file size limit. Skip encrypted entries. Log skipped entries for user visibility.

**Memory Usage for Large File Cache**:
- ⚠️ LRU cache stores file contents in memory — 50 files × 10 MB = 500 MB worst case
- **Mitigation**: Cache stores line arrays, not raw bytes. Configurable cache size. Default 50 entries. Preview file size limit (10 MB default).

### Workarounds

**Qt6 SQL Module Not Linked**:
```cmake
# Add Qt6::Sql to target link libraries
target_link_libraries(sak_utility PRIVATE Qt6::Sql)
```

**QuaZip Not Found on CI**:
```yaml
# Ensure QuaZip installed via vcpkg before CMake configure
- run: vcpkg install quazip:x64-windows
```

**No TagLib Available** (fallback):
```cpp
#ifdef SAK_HAVE_TAGLIB
    #include <taglib/fileref.h>
    #include <taglib/tag.h>
#endif

QMap<QString, QString> FileMetadataExtractor::extractAudioMetadata(const QString& filePath) {
    QMap<QString, QString> metadata;
    
#ifdef SAK_HAVE_TAGLIB
    TagLib::FileRef f(filePath.toStdWString().c_str());
    if (!f.isNull() && f.tag()) {
        auto tag = f.tag();
        if (!tag->title().isEmpty()) metadata["Title"] = QString::fromStdWString(tag->title().toWString());
        if (!tag->artist().isEmpty()) metadata["Artist"] = QString::fromStdWString(tag->artist().toWString());
        if (!tag->album().isEmpty()) metadata["Album"] = QString::fromStdWString(tag->album().toWString());
        if (tag->year() > 0) metadata["Year"] = QString::number(tag->year());
        metadata["Track"] = QString::number(tag->track());
    }
    if (!f.isNull() && f.audioProperties()) {
        auto props = f.audioProperties();
        int minutes = props->lengthInSeconds() / 60;
        int seconds = props->lengthInSeconds() % 60;
        metadata["Duration"] = QString("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
        metadata["Bitrate"] = QString("%1 kbps").arg(props->bitrate());
    }
#else
    Q_UNUSED(filePath);
    // Audio metadata search requires TagLib — disabled in this build
#endif
    
    return metadata;
}
```

---

## 🎯 Success Metrics

| Metric | Target | Importance |
|--------|--------|------------|
| Text search (100 files) | < 1 second | Critical |
| Text search (10,000 files) | < 10 seconds | High |
| Text search (100,000 files) | < 60 seconds | Medium |
| Regex search overhead | < 10% vs. plain text | High |
| EXIF metadata extraction | < 50 ms per file | High |
| PDF metadata extraction | < 100 ms per file | Medium |
| DOCX metadata extraction | < 200 ms per file | Medium |
| Archive search (100 entry ZIP) | < 5 seconds | Medium |
| Binary search (50 MB file) | < 2 seconds | Medium |
| Preview pane render | < 100 ms | Critical |
| Match highlighting | < 200 ms (1000 matches) | High |
| UI frame time during search | < 100 ms (responsive) | Critical |
| File cache hit rate | > 80% (repeated access) | Medium |
| Memory usage (50-entry cache) | < 200 MB | Medium |
| Panel tab switch | < 100 ms | High |
| Search cancellation latency | < 100 ms | High |

---

## 🔒 Security Considerations

### File Access
- Search reads file contents — only files accessible to the current user
- SQLite `QSQLITE_OPEN_READONLY` flag prevents write access
- No file modification — all operations are read-only

### Pattern Injection
- `QRegularExpression` handles catastrophic backtracking via PCRE2 JIT — safer than Python's `re`
- Set `QRegularExpression::PatternOption::DontCaptureOption` to prevent capture group overhead
- Validate regex compilation before search starts — report errors to user

### Archive Safety
- QuaZip opens in read-only mode (`QuaZip::mdUnzip`)
- ZIP bomb detection: skip archive members exceeding `maxSearchFileSize`
- No file extraction to disk — content read into memory only

### Network Path Safety
- UNC paths checked with timeout (5 seconds) before traversal
- Failed network checks cached to avoid repeated timeout delays
- No credential handling — relies on Windows authentication

### Data Sensitivity
- File contents shown in preview pane — sensitive data may be visible
- Search history may reveal searched terms — clearable via Settings
- LRU cache holds file contents in memory — cleared on app close

---

## 💡 Future Enhancements (Post-v0.8.0)

### v0.9.0 - Enhanced Search
- **Real-time Search** - Search-as-you-type for small directories
- **Search Profiles** - Save/load complete search configurations (pattern + options + directory)
- **Result Export** - Export results to CSV, JSON, or HTML report
- **Syntax Highlighting** - Language-aware syntax highlighting in preview pane
- **Diff View** - Compare two files from results side-by-side

### v1.0.0 - Advanced Features
- **Replace Mode** - Find and replace across files with preview/undo
- **Content Indexing** - Background file index for instant repeat searches
- **Bookmarks** - Save interesting results for later reference
- **Search Templates** - Pre-configured searches for common tasks (security audit, code review)
- **Clipboard Search** - Search clipboard content in a directory

### v1.1.0 - Metadata Enhancements
- **Video Thumbnail Preview** - Show video thumbnails in preview pane
- **Image Preview** - Show image thumbnails alongside EXIF metadata
- **PDF Text Search** - Extract and search PDF text content (not just metadata)
- **Extended Audio Tags** - Album art, lyrics, chapter markers
- **AI Pattern Suggestions** - Suggest regex patterns based on search intent

---

## 📚 Resources

### Official Documentation
- [QRegularExpression](https://doc.qt.io/qt-6/qregularexpression.html) — PCRE2-based regex engine
- [QDirIterator](https://doc.qt.io/qt-6/qdiriterator.html) — Recursive directory traversal
- [QTreeWidget](https://doc.qt.io/qt-6/qtreewidget.html) — Tree view for results and file explorer
- [QTextEdit](https://doc.qt.io/qt-6/qtextedit.html) — Rich text preview with formatting
- [QTextCharFormat](https://doc.qt.io/qt-6/qtextcharformat.html) — Text highlighting
- [QSplitter](https://doc.qt.io/qt-6/qsplitter.html) — Three-panel layout
- [QSqlDatabase](https://doc.qt.io/qt-6/qsqldatabase.html) — SQLite metadata access
- [QXmlStreamReader](https://doc.qt.io/qt-6/qxmlstreamreader.html) — XML parsing for ODF/EPUB/FDX

### Community Resources
- [QuaZip GitHub](https://github.com/stachenov/quazip) — ZIP archive library
- [TagLib GitHub](https://github.com/taglib/taglib) — Audio tag library
- [EXIF Specification](https://www.cipa.jp/std/documents/e/DC-010-2020_E.pdf) — CIPA EXIF 3.0
- [Office Open XML](https://en.wikipedia.org/wiki/Office_Open_XML) — DOCX/XLSX/PPTX format reference
- [EPUB Specification](https://www.w3.org/TR/epub-33/) — W3C EPUB 3.3
- [Advanced Search Tool](https://github.com/RandyNorthrup/advanced_search) — Original Python implementation

### SAK Codebase Reference
- `include/sak/worker_base.h` — Base class for worker threads
- `src/gui/organizer_panel.cpp` — Similar panel pattern (directory scanning + results)
- `src/gui/main_window.cpp` — Panel registration and tab creation
- `include/sak/config_manager.h` — Settings persistence
- `include/sak/error_codes.h` — Error code definitions

---

## 📞 Support

**Questions?** Open a GitHub Discussion  
**Found a Bug?** Open a GitHub Issue  
**Want to Contribute?** See [CONTRIBUTING.md](CONTRIBUTING.md)

---

**Document Version**: 1.0  
**Last Updated**: March 2, 2026  
**Author**: Randy Northrup  
**Status**: ✅ Ready for Implementation
