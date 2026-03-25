# File Converter Tab — Comprehensive Implementation Plan

**Version**: 1.0  
**Date**: March 24, 2026  
**Status**: 📋 Planned  
**Parent Panel**: File Management (OrganizerPanel)  
**Tab Position**: Tab 3 (after File Organizer and Duplicate Finder)

---

## 🎯 Executive Summary

The File Converter tab adds universal file conversion capabilities to the File Management panel. Technicians can convert between common document, image, audio, video, spreadsheet, text, and PDF formats through a drag-and-drop interface with batch processing. The converter runs entirely offline using bundled libraries — no cloud APIs, no file uploads, no internet required.

### Key Objectives
- **Document Conversion** — DOCX → PDF/HTML/RTF/TXT, RTF → PDF/HTML/DOCX/TXT, ODT → PDF/HTML/RTF/TXT, TXT → PDF/HTML/RTF/DOCX
- **Image Conversion** — PNG, JPEG, BMP, TIFF, GIF, WebP, ICO, SVG (raster export)
- **Audio Conversion** — MP3, WAV, FLAC, OGG, AAC, WMA, M4A
- **Video Conversion** — MP4, AVI, MKV, MOV, WMV, WebM, GIF (animated)
- **Spreadsheet Conversion** — CSV ↔ XLSX, TSV ↔ CSV, CSV ↔ JSON
- **PDF Operations** — Images → PDF, PDF → Images, Merge PDFs, Split PDF
- **Text/Data Conversion** — JSON ↔ YAML, JSON ↔ XML, Markdown → HTML, TXT encoding conversion
- **Batch Processing** — Convert multiple files in a single operation with progress tracking
- **Quality/Compression Controls** — Per-format options (JPEG quality, audio bitrate, video codec, PDF DPI)

---

## 📊 Project Scope

### What is File Conversion?

**File Conversion** is the process of transforming a file from one format to another while preserving as much content and fidelity as possible. Technicians frequently need to convert files during data migration, system setup, report generation, and troubleshooting — tasks where the customer's machine may not have the original application installed.

**Conversion Workflow**:
1. Technician opens File Management → File Converter tab
2. Drags files onto the input area (or uses the file browser)
3. Selects the target output format from a filtered list of compatible formats
4. Adjusts quality/compression settings if needed
5. Clicks Convert — files are processed in a background thread
6. Output files appear in the designated output directory

**Key Constraints**:
- **Fully offline** — No network calls, no cloud services, no API keys
- **No external runtime dependencies** — Must work on machines with nothing installed. No Office, no LibreOffice, no Java
- **Bundled tooling** — All conversion engines are compiled into the EXE (vcpkg/Qt) or bundled as companion binaries in `tools/`
- **Non-destructive** — Source files are never modified; output goes to a separate directory
- **Cancellable** — All conversions can be cancelled mid-operation

---

## 🎯 Use Cases

### 1. **Customer Needs PDF from Word Documents**
**Scenario**: Customer has 50 DOCX files from an old system and needs PDFs for archival.

**Workflow**:
1. Open File Management → File Converter
2. Drag all 50 DOCX files onto the input area
3. Select output format: PDF
4. Click Convert — progress bar shows 1/50, 2/50, etc.
5. All 50 PDFs appear in the output folder with basic formatting preserved

**Benefits**:
- No need to install Microsoft Office, LibreOffice, or Adobe Acrobat
- In-tree OOXML parser + QTextDocument renders DOCX to PDF entirely in-process
- Batch processing handles all 50 files in one operation
- Basic formatting preserved: bold, italic, fonts, colors, alignment, tables

---

### 2. **Image Format Standardization**
**Scenario**: Website migration — customer has a mix of BMP, TIFF, and PNG images that need to be WebP for web optimization.

**Workflow**:
1. Open File Converter
2. Drag the folder of mixed images onto the input area
3. Select output format: WebP
4. Set quality to 85% (good balance of size/quality)
5. Enable "Preserve directory structure" for nested folders
6. Convert — all images converted to WebP with significant size reduction

**Benefits**:
- Handles mixed input formats in a single batch
- WebP output dramatically reduces file sizes
- Directory structure preserved for easy replacement

---

### 3. **Audio Format Conversion for Car Stereo**
**Scenario**: Customer's car stereo only plays MP3. They have a FLAC music collection.

**Workflow**:
1. Open File Converter
2. Drag FLAC files onto the input area
3. Select output format: MP3
4. Set bitrate to 320 kbps (highest quality MP3)
5. Convert — metadata (artist, album, track) preserved in ID3 tags

**Benefits**:
- Preserves audio metadata during conversion
- Configurable bitrate for size/quality tradeoff
- Batch converts entire music library

---

### 4. **Video Compression for Email**
**Scenario**: Customer recorded a 2 GB screen capture (AVI) and needs to email it.

**Workflow**:
1. Open File Converter
2. Drop the AVI file
3. Select output format: MP4 (H.264)
4. Set quality preset to "Email/Web" (720p, moderate bitrate)
5. Convert — output is ~50 MB, suitable for email attachment

**Benefits**:
- Massive file size reduction with modern codecs
- Quality presets eliminate guesswork
- No need to install separate video editing software

---

### 5. **Spreadsheet Data Migration**
**Scenario**: Legacy system exports data as TSV files. New system needs XLSX.

**Workflow**:
1. Open File Converter
2. Drag all TSV files
3. Select output format: XLSX
4. Configure: delimiter = Tab, first row = headers, auto-detect column types
5. Convert — each TSV becomes a properly formatted XLSX with typed columns

**Benefits**:
- Handles encoding issues (UTF-8, Latin-1, etc.)
- Auto-detects data types (numbers, dates, text)
- Preserves header rows

---

### 6. **PDF Operations — Merge and Split**
**Scenario**: Customer has 12 monthly invoices as separate PDFs, needs one combined PDF for tax filing.

**Workflow**:
1. Open File Converter
2. Drag all 12 PDF files (they appear in alphabetical order)
3. Reorder by dragging rows in the file list if needed
4. Select operation: "Merge PDFs"
5. Click Convert — single combined PDF with all 12 invoices in order

**Benefits**:
- Drag-to-reorder before merge
- Also supports splitting a single PDF into individual pages
- No Adobe Acrobat required

---

## 🏗️ Architecture Overview

### Component Hierarchy

```
OrganizerPanel (existing)
├─ Tab 0: File Organizer (existing)
├─ Tab 1: Duplicate Finder (existing)
└─ Tab 2: File Converter (NEW)
   │
   ├─ FileConverterWidget (QWidget)
   │  ├─ Input: File list with drag-and-drop (QTableWidget)
   │  ├─ Format selector: Source format auto-detected, target format combo
   │  ├─ Options panel: Per-format quality/compression settings
   │  ├─ Output directory selector
   │  ├─ Progress bar + log area
   │  └─ Convert / Cancel buttons
   │
   └─ FileConverterWorker (WorkerBase subclass) [Worker Thread]
      ├─ Receives: ConversionJob (list of files + target format + options)
      ├─ Delegates to format-specific converters:
      │  ├─ ImageConverter — Qt QImage + libwebp + SVG rasterization
      │  ├─ AudioConverter — FFmpeg (bundled ffmpeg.exe)
      │  ├─ VideoConverter — FFmpeg (bundled ffmpeg.exe)
      │  ├─ DocumentConverter — QTextDocument Hub: DocxReader + OdtReader parse
      │  │                      ZIP+XML → HTML → QTextDocument → export to any target.
      │  │                      DocxWriter generates OOXML from QTextDocument for DOCX output.
      │  │                      RTF: QTextDocument loads/exports natively. All compiled in.
      │  ├─ PdfConverter — QPdfDocument (read) + QPainter→QPdfWriter (write),
      │  │                 qpdf (bundled) for merge/split
      │  ├─ SpreadsheetConverter — Native CSV/TSV parser + xlsxwriter (vcpkg)
      │  └─ TextDataConverter — Native JSON/YAML/XML/Markdown parsers
      │
      ├─ Emits: fileProgress, conversionComplete, errorOccurred
      └─ Supports: cancel() via atomic flag
```

### Integration with OrganizerPanel

The File Converter tab plugs into the existing `OrganizerPanel` tab widget:

```cpp
// In OrganizerPanel constructor — after existing tabs
m_file_converter_widget = new FileConverterWidget(this);
m_tab_widget->addTab(m_file_converter_widget, "File Converter");
```

The `FileConverterWidget` is self-contained: it owns its worker thread and all UI elements. The only integration point with `OrganizerPanel` is the shared `statusMessage` signal.

---

## 🛠️ Technical Specifications

### Conversion Engine Dependencies

| Category | Engine | Source | Bundled Via | License |
|----------|--------|--------|------------|---------|
| Images | Qt QImage / QImageReader / QImageWriter | Qt 6.5.3 | Compiled in | LGPL-3.0 |
| Images (WebP) | libwebp | vcpkg | Compiled in | BSD-3-Clause |
| Images (SVG read) | Qt SVG module | Qt 6.5.3 | Compiled in | LGPL-3.0 |
| Audio/Video | FFmpeg | Build script download | `tools/ffmpeg.exe` | LGPL-2.1+ |
| Documents (RTF) | Qt QTextDocument | Qt 6.5.3 | Compiled in | LGPL-3.0 |
| Documents (high-fidelity) | Pandoc (universal document converter) | Build script download | `tools/pandoc.exe` | GPL-2.0+ |
| Documents (DOCX read) | In-tree DocxReader (OOXML→HTML→QTextDocument) | In-tree | Compiled in | N/A |
| Documents (ODT read) | In-tree OdtReader (ODF→HTML→QTextDocument) | In-tree | Compiled in | N/A |
| Documents (DOCX write) | In-tree DocxWriter (QTextDocument→OOXML ZIP) | In-tree | Compiled in | N/A |
| ZIP I/O (for DOCX/ODT) | QuaZip (Qt wrapper around zlib) | vcpkg (`quazip`) | Compiled in | LGPL-2.1 |
| PDF read | QPdfDocument | Qt 6.5.3 (Qt PDF module) | Compiled in | LGPL-3.0 |
| PDF write | QPdfWriter / QPainter | Qt 6.5.3 | Compiled in | LGPL-3.0 |
| PDF merge/split | qpdf | Build script download | `tools/qpdf.exe` | Apache-2.0 |
| Spreadsheets (XLSX) | QXlsx | vcpkg (`qxlsx`) | Compiled in | MIT |
| Spreadsheets (CSV/TSV) | Native C++ parser | In-tree | Compiled in | N/A |
| YAML | yaml-cpp | vcpkg | Compiled in | MIT |
| Markdown → HTML | cmark | vcpkg | Compiled in | BSD-2-Clause |
| XML | Qt XML (QDomDocument) | Qt 6.5.3 | Compiled in | LGPL-3.0 |

> **Portable deployment rule**: Every conversion engine is either statically linked into
> the SAK executable (vcpkg/Qt libraries) or ships as a companion binary in `tools/`
> alongside it. Nothing requires installation on the target machine.

### Bundled Binary: FFmpeg

FFmpeg is the industry-standard tool for audio/video conversion. It will be bundled as `tools/ffmpeg.exe` alongside the SAK executable (similar to how iPerf3 is bundled for the Network Diagnostics panel).

**Bundle Script**: `scripts/bundle_ffmpeg.ps1`
- Downloads a specific tagged release of FFmpeg (GPL or LGPL build)
- Verifies SHA-256 checksum against a pinned hash
- Extracts `ffmpeg.exe` and `ffprobe.exe` into `tools/`
- Updates `THIRD_PARTY_LICENSES.md`

**FFmpeg Invocation Pattern**:
```cpp
// All FFmpeg calls go through a thin wrapper that:
// 1. Locates ffmpeg.exe relative to the SAK executable
// 2. Builds the argument list from structured ConversionOptions
// 3. Runs via QProcess with timeout and cancellation
// 4. Parses stderr for progress (frame=, time=, speed=)
// 5. Returns std::expected<ConversionResult, ErrorCode>
```

### Bundled Binary: qpdf (PDF Merge/Split)

qpdf is a lightweight, command-line PDF transformation tool. It will be bundled as `tools/qpdf.exe` for PDF merge, split, and page extraction operations.

**Bundle Script**: `scripts/bundle_qpdf.ps1`

### Bundled Binary: Pandoc (High-Fidelity Document Conversion)

[Pandoc](https://pandoc.org/) is a universal document converter written in Haskell. It ships as a
single statically-linked binary with **zero runtime dependencies** — just unzip and use.
Pandoc reads and writes DOCX, ODT, RTF, HTML, and dozens of other formats, handling
features that the in-tree QTextDocument Hub cannot: **embedded images, tracked changes,
footnotes, and table styles**.

**Size**: ~39 MB (Windows x86_64 ZIP, self-contained)  
**License**: GPL-2.0-or-later  
**Version**: 3.9.0.2 (March 2026)

**Bundle Script**: `scripts/bundle_pandoc.ps1`
- Downloads the specific `pandoc-3.9.0.2-windows-x86_64.zip` release
- Verifies SHA-256 checksum against a pinned hash
- Extracts `pandoc.exe` into `tools/`
- Updates `THIRD_PARTY_LICENSES.md`

**Why Pandoc + QTextDocument Hub (hybrid)?**

| Scenario | Engine | Reason |
|----------|--------|--------|
| DOCX with images → HTML/ODT/RTF | **Pandoc** | Preserves embedded images, tracked changes, footnotes |
| DOCX/ODT → PDF | **Pandoc → HTML → QTextDocument → QPdfWriter** | Pandoc has no built-in PDF engine; we pipe its HTML through Qt |
| Simple RTF → PDF/HTML/TXT | **QTextDocument Hub** | Qt handles RTF natively — no need to shell out |
| TXT → PDF/HTML/RTF/DOCX | **QTextDocument Hub** | Plain text is trivial; no Pandoc overhead needed |
| Pandoc missing/corrupt | **QTextDocument Hub (fallback)** | Basic formatting still works; user warned about reduced fidelity |

**Pandoc Invocation Pattern**:
```cpp
// All Pandoc calls go through a thin wrapper (PandocRunner) that:
// 1. Locates pandoc.exe relative to the SAK executable
// 2. Builds the argument list: -f <source_format> -t <target_format>
//    --extract-media=<temp_dir> (for embedded images)
//    --track-changes=accept (for tracked changes in DOCX)
//    --standalone (for complete HTML output)
//    --sandbox (security: no filesystem access beyond input)
// 3. Runs via QProcess with timeout and cancellation
// 4. Returns std::expected<ConversionResult, ErrorCode>
```

**Pandoc Key Options**:
- `--track-changes=accept|reject|all` — handles DOCX tracked changes (accept = apply changes, reject = ignore them, all = preserve markup)
- `--extract-media=DIR` — extracts embedded images from DOCX/ODT to a directory, rewrites references in the output
- `--sandbox` — prevents the converter from accessing arbitrary files (security)
- `--reference-doc=FILE` — uses a reference DOCX/ODT as a style template (preserves headers/footers in DOCX→DOCX)

---

### Document Engine Architecture — Hybrid: Pandoc + QTextDocument Hub

All document conversions (DOCX, ODT, RTF, TXT) work without any external office suite.
The engine uses a **dual-path hybrid architecture**:

1. **Pandoc path** (high fidelity) — Bundled `tools/pandoc.exe` handles complex documents
   with embedded images, tracked changes, footnotes, and advanced table formatting. Used
   for DOCX/ODT conversions when Pandoc is available.
2. **QTextDocument Hub** (lightweight fallback) — In-tree readers parse source formats into
   HTML, which `QTextDocument` consumes and exports. Always available (compiled in). Used
   for simple RTF/TXT conversions, PDF generation, and as a fallback when Pandoc is missing.

```
              INPUT                                   OUTPUT
              ═════                                   ══════

  ┌────────────────── PANDOC PATH (high fidelity) ─────────────────────────┐
  │                                                                         │
  │  DOCX ──┐                  ┌──→ HTML      (--standalone)               │
  │  ODT  ──┼──→ pandoc.exe ──┼──→ DOCX      (direct)                     │
  │  RTF  ──┘    (tools/)     ├──→ ODT       (direct)                     │
  │                            ├──→ RTF       (direct)                     │
  │                            └──→ TXT       (--to=plain)                 │
  │                                                                         │
  │  For PDF: pandoc → HTML → QTextDocument → QPdfWriter                    │
  └─────────────────────────────────────────────────────────────────────────┘

  ┌──────── QTEXTDOCUMENT HUB (lightweight / fallback / PDF) ──────────────┐
  │                              ┌──────────────┐                           │
  │  DOCX ──→ DocxReader → HTML →│              │──→ PDF   (QPdfWriter)    │
  │  ODT  ──→ OdtReader  → HTML →│ QTextDocument│──→ HTML  (toHtml)       │
  │  RTF  ──→ (Qt native) ──────→│  (Universal  │──→ RTF   (Qt native)    │
  │  TXT  ──→ setPlainText ─────→│   Document   │──→ TXT   (toPlainText)  │
  │                              │     Hub)     │──→ DOCX  (DocxWriter)    │
  │                              └──────────────┘                           │
  └─────────────────────────────────────────────────────────────────────────┘
```

**Engine selection logic** (in `FileConverterWorker::convertDocument()`):
```cpp
// 1. If Pandoc is available AND input is DOCX/ODT → Pandoc path (high fidelity)
// 2. If conversion is RTF/TXT → any target, or Pandoc unavailable → QTextDocument Hub
// 3. For PDF output: always finish via QTextDocument → QPdfWriter
//    (Pandoc generates intermediate HTML, which QTextDocument renders to PDF)
```

**DocxReader** (in-tree, `src/core/docx_reader.cpp`):
- Opens `.docx` ZIP via QuaZip → extracts `word/document.xml` + `word/styles.xml`
- Walks OOXML elements with `QXmlStreamReader`
- Maps to HTML: `<w:p>` → `<p>`, `<w:r>/<w:t>` → `<span>`, `<w:tbl>` → `<table>`,
  `<w:b>` → `<b>`, `<w:i>` → `<i>`, `<w:rFonts>` → `font-family`, `<w:sz>` → `font-size`,
  `<w:color>` → `color`, `<w:jc>` → `text-align`, `<w:pStyle>` headings → `<h1>`-`<h6>`,
  `<w:numPr>` → `<ol>`/`<ul>`, page breaks → `<div style="page-break-before:always">`
- Returns an HTML string that QTextDocument loads faithfully

**OdtReader** (in-tree, `src/core/odt_reader.cpp`):
- Opens `.odt` ZIP via QuaZip → extracts `content.xml` + `styles.xml`
- Maps ODF elements: `<text:p>` → `<p>`, `<text:span>` → `<span>`,
  `<text:h>` → `<h1>`-`<h6>`, `<text:list>` → `<ol>`/`<ul>`,
  `<table:table>` → `<table>`, inline styles from `styles.xml` → CSS
- Same output: clean HTML → QTextDocument

**DocxWriter** (in-tree, `src/core/docx_writer.cpp`):
- Iterates `QTextDocument`'s block structure (`QTextBlock`, `QTextFragment`, `QTextTable`)
- Maps `QTextCharFormat` → OOXML run properties (`<w:rPr>`)
- Maps `QTextBlockFormat` → OOXML paragraph properties (`<w:pPr>`)
- Maps `QTextTable` → `<w:tbl>` with rows, cells, borders
- Generates required DOCX package files: `[Content_Types].xml`, `_rels/.rels`,
  `word/_rels/document.xml.rels`, `word/document.xml`, `word/styles.xml`
- Packages via QuaZip into a valid `.docx` file

**Formatting Coverage**:

| Feature | Read (DOCX/ODT) | Write (DOCX) | Status |
|---------|-----------------|---------------|--------|
| Bold / Italic / Underline | ✅ | ✅ | Core (both engines) |
| Strikethrough | ✅ | ✅ | Core (both engines) |
| Font name and size | ✅ | ✅ | Core (both engines) |
| Text color | ✅ | ✅ | Core (both engines) |
| Background/highlight color | ✅ | ✅ | Core (both engines) |
| Paragraph alignment | ✅ | ✅ | Core (both engines) |
| Headings (H1-H6) | ✅ | ✅ | Core (both engines) |
| Bullet / numbered lists | ✅ | ✅ | Core (both engines) |
| Basic tables | ✅ | ✅ | Core (both engines) |
| Page breaks | ✅ | ✅ | Core (both engines) |
| Line spacing | ✅ | ✅ | Core (both engines) |
| Embedded images | ✅ | ✅ | Pandoc (`--extract-media`) |
| Tracked changes | ✅ | — | Pandoc (`--track-changes=accept\|reject\|all`) |
| Footnotes / endnotes | ✅ | ✅ | Pandoc (AST native) |
| Headers / footers | ⚠️ | ⚠️ | Pandoc: preserved in DOCX→DOCX via `--reference-doc`; lost in cross-format (not in Pandoc AST) |
| Multi-column layout | ❌ | ❌ | Not in Pandoc AST; requires full layout engine |
| Fields / formulas | ❌ | ❌ | Not in Pandoc AST; requires runtime evaluation |
| Text boxes / shapes / charts | ❌ | ❌ | Not in Pandoc AST; requires rendering engine |

> The hybrid engine covers the formatting technicians encounter in **reports, logs,
> correspondence, data exports, and configuration documents** — the vast majority of
> real-world conversion requests. Pandoc adds high-fidelity support for embedded images,
> tracked changes, and footnotes. The remaining out-of-scope features (multi-column
> layout, text boxes, shapes, charts, fields) require a full word-processing layout
> engine — no lightweight bundleable tool supports them.

---

### Supported Conversion Matrix

#### Image Conversions

| Source → | PNG | JPEG | BMP | TIFF | GIF | WebP | ICO |
|----------|-----|------|-----|------|-----|------|-----|
| **PNG**  | —   | ✅   | ✅  | ✅   | ✅  | ✅   | ✅  |
| **JPEG** | ✅  | —    | ✅  | ✅   | ✅  | ✅   | ✅  |
| **BMP**  | ✅  | ✅   | —   | ✅   | ✅  | ✅   | ✅  |
| **TIFF** | ✅  | ✅   | ✅  | —    | ✅  | ✅   | ✅  |
| **GIF**  | ✅  | ✅   | ✅  | ✅   | —   | ✅   | ✅  |
| **WebP** | ✅  | ✅   | ✅  | ✅   | ✅  | —    | ✅  |
| **SVG**  | ✅  | ✅   | ✅  | ✅   | ✅  | ✅   | ✅  |
| **ICO**  | ✅  | ✅   | ✅  | ✅   | ✅  | ✅   | —   |

**Engine**: Qt `QImage` for most raster formats. `libwebp` for WebP encode/decode. Qt SVG for SVG rasterization. ICO via custom writer (multi-size icon packing).

#### Audio Conversions

| Source → | MP3 | WAV | FLAC | OGG | AAC | WMA | M4A |
|----------|-----|-----|------|-----|-----|-----|-----|
| **MP3**  | —   | ✅  | ✅   | ✅  | ✅  | ✅  | ✅  |
| **WAV**  | ✅  | —   | ✅   | ✅  | ✅  | ✅  | ✅  |
| **FLAC** | ✅  | ✅  | —    | ✅  | ✅  | ✅  | ✅  |
| **OGG**  | ✅  | ✅  | ✅   | —   | ✅  | ✅  | ✅  |
| **AAC**  | ✅  | ✅  | ✅   | ✅  | —   | ✅  | ✅  |
| **WMA**  | ✅  | ✅  | ✅   | ✅  | ✅  | —   | ✅  |
| **M4A**  | ✅  | ✅  | ✅   | ✅  | ✅  | ✅  | —   |

**Engine**: FFmpeg. Metadata (ID3/Vorbis tags) preserved when target format supports it.

#### Video Conversions

| Source → | MP4 | AVI | MKV | MOV | WMV | WebM | GIF |
|----------|-----|-----|-----|-----|-----|------|-----|
| **MP4**  | —   | ✅  | ✅  | ✅  | ✅  | ✅   | ✅  |
| **AVI**  | ✅  | —   | ✅  | ✅  | ✅  | ✅   | ✅  |
| **MKV**  | ✅  | ✅  | —   | ✅  | ✅  | ✅   | ✅  |
| **MOV**  | ✅  | ✅  | ✅  | —   | ✅  | ✅   | ✅  |
| **WMV**  | ✅  | ✅  | ✅  | ✅  | —   | ✅   | ✅  |
| **WebM** | ✅  | ✅  | ✅  | ✅  | ✅  | —    | ✅  |

**Engine**: FFmpeg. Video → GIF uses palette generation for quality. Default codecs: H.264 for MP4/MOV, VP9 for WebM, H.265 optional.

#### Document Conversions

All document conversions are compiled into the executable or use the bundled Pandoc binary —
**no external office suite required**. The engine uses a **hybrid dual-path architecture**:
Pandoc handles complex DOCX/ODT conversions with full fidelity (images, tracked changes,
footnotes), while the QTextDocument Hub handles simple RTF/TXT conversions and PDF generation.

| Source → | PDF | TXT | HTML | RTF | DOCX |
|----------|-----|-----|------|-----|------|
| **DOCX** | ✅¹ | ✅¹ | ✅¹  | ✅¹ | —    |
| **RTF**  | ✅² | ✅² | ✅²  | —   | ✅³  |
| **ODT**  | ✅¹ | ✅¹ | ✅¹  | ✅¹ | ✅³  |
| **TXT**  | ✅⁴ | —   | ✅⁴  | ✅⁴ | ✅³  |

¹ **Pandoc** (primary, high fidelity): Calls `pandoc.exe` to convert DOCX/ODT directly to HTML/RTF/TXT. Preserves embedded images, tracked changes, footnotes, and advanced table formatting. For PDF output: Pandoc generates HTML → loaded into `QTextDocument` → rendered to PDF via `QPdfWriter`. **QTextDocument Hub** (fallback): In-tree DocxReader/OdtReader parse ZIP + XML into HTML → `QTextDocument` → export. Used when Pandoc is unavailable or for simpler documents.  
² Qt `QTextDocument` natively loads RTF and renders to PDF, plain text, or HTML. Preserves the same formatting subset as above.  
³ **Pandoc** (primary): Direct DOCX output with full fidelity. **DocxWriter** (fallback): In-tree writer generates OOXML XML from `QTextDocument`'s block/fragment structure, packages into a ZIP.  
⁴ Native: wraps plain text in QTextDocument, which exports to PDF pages, HTML, RTF, or DOCX.

> **Formatting fidelity note**: Document conversions via Pandoc preserve *high-fidelity*
> formatting including embedded images, tracked changes, footnotes, and advanced tables.
> The QTextDocument Hub fallback preserves *basic* formatting (bold, italic, fonts, colors,
> headings, basic tables). Features that require a full word-processing layout engine —
> multi-column layout, text boxes, shapes, charts, and fields/formulas — are **not**
> supported by either engine. The converter covers the vast majority of technician use
> cases (reports, logs, correspondence, data exports).

#### Spreadsheet Conversions

| Source → | XLSX | CSV | TSV | JSON |
|----------|------|-----|-----|------|
| **CSV**  | ✅   | —   | ✅  | ✅   |
| **TSV**  | ✅   | ✅  | —   | ✅   |
| **XLSX** | —    | ✅  | ✅  | ✅   |
| **JSON** | ✅   | ✅  | ✅  | —    |

**Engine**: Native CSV/TSV parser. QXlsx for XLSX read/write. JSON via Qt's `QJsonDocument`.

#### PDF Operations

| Operation | Description |
|-----------|-------------|
| Images → PDF | Combine multiple images into a single multi-page PDF |
| PDF → Images | Extract each page as a PNG/JPEG at configurable DPI |
| Merge PDFs | Combine multiple PDFs into one (preserves bookmarks) |
| Split PDF | Split a PDF into individual pages or page ranges |
| PDF → Text | Extract text content from PDF pages |

**Engine**: QPdfDocument for reading, QPdfWriter for writing images→PDF, qpdf for merge/split.

#### Text/Data Conversions

| Source → | JSON | YAML | XML | HTML | TXT |
|----------|------|------|-----|------|-----|
| **JSON** | —    | ✅   | ✅  | ✅⁴  | ✅  |
| **YAML** | ✅   | —    | ✅  | ✅⁴  | ✅  |
| **XML**  | ✅   | ✅   | —   | ✅⁴  | ✅  |
| **Markdown** | — | —  | —   | ✅   | ✅  |

⁴ Pretty-printed with syntax highlighting in HTML output.

**Engine**: Qt `QJsonDocument` for JSON, `yaml-cpp` for YAML, Qt `QDomDocument`/`QXmlStreamReader` for XML, `cmark` for Markdown→HTML.

#### Text Encoding Conversion

| Operation | Description |
|-----------|-------------|
| UTF-8 ↔ UTF-16 | Unicode encoding conversion |
| UTF-8 ↔ Latin-1 | Western European encoding |
| UTF-8 ↔ Windows-1252 | Windows Western encoding |
| Auto-detect → UTF-8 | Detect source encoding, normalize to UTF-8 |
| Line ending conversion | CRLF ↔ LF ↔ CR |

**Engine**: Qt `QTextCodec` / `QStringConverter`.

---

### Data Structures

```cpp
/// @brief Identifies a file format by category and extension
enum class FileFormatCategory {
    Image,
    Audio,
    Video,
    Document,
    Spreadsheet,
    Pdf,
    TextData,
    Unknown
};

/// @brief Metadata about a supported file format
struct FileFormatInfo {
    QString extension;                // "png", "mp3", "docx"
    QString display_name;             // "PNG Image", "MP3 Audio"
    FileFormatCategory category;
    QStringList mime_types;           // "image/png"
    bool can_read{false};             // Can be used as input
    bool can_write{false};            // Can be used as output
    QString engine;                   // "qt_image", "ffmpeg", "qtdoc_hub", "native"
    bool requires_bundled_tool{false}; // Requires bundled binary (FFmpeg, qpdf)
};

/// @brief Per-format conversion options
struct ConversionOptions {
    // Image options
    int image_quality{90};            // 1-100 for lossy formats (JPEG, WebP)
    int image_dpi{300};               // DPI for rasterization (SVG→PNG, PDF→Image)
    QSize image_max_size;             // Optional resize (0,0 = no resize)
    bool image_preserve_aspect{true};

    // Audio options
    int audio_bitrate_kbps{192};      // 64, 128, 192, 256, 320
    int audio_sample_rate{44100};     // 22050, 44100, 48000, 96000
    int audio_channels{0};            // 0 = preserve, 1 = mono, 2 = stereo
    bool audio_preserve_metadata{true};

    // Video options
    QString video_codec;              // "h264", "h265", "vp9", "copy"
    QString video_preset;             // "ultrafast"..."veryslow" (encoding speed/quality)
    int video_crf{23};                // Constant Rate Factor: 0 (lossless) - 51 (worst)
    QSize video_resolution;           // 0,0 = preserve original
    double video_fps{0};              // 0 = preserve original
    QString audio_codec;              // "aac", "mp3", "opus", "copy"
    bool video_strip_audio{false};

    // PDF options
    int pdf_dpi{300};                 // For PDF→Image extraction
    QString pdf_image_format{"png"};  // Output format for PDF→Image
    QVector<int> pdf_page_range;      // Empty = all pages

    // Spreadsheet options
    QChar csv_delimiter{','};
    QChar csv_quote{'"'};
    bool csv_has_headers{true};
    QString csv_encoding{"UTF-8"};

    // Text encoding options
    QString source_encoding;          // Empty = auto-detect
    QString target_encoding{"UTF-8"};
    QString line_ending;              // "crlf", "lf", "cr", empty = preserve
};

/// @brief A single file conversion job
struct ConversionJob {
    QVector<QString> input_files;     // Source file paths
    QString output_directory;         // Where converted files go
    QString target_format;            // Target extension ("pdf", "png", "mp3")
    ConversionOptions options;
    bool preserve_directory_structure{false};  // For batch with subdirs
    QString filename_pattern;         // Optional: "{name}_converted.{ext}"
};

/// @brief Result of converting a single file
struct ConversionResult {
    QString input_file;
    QString output_file;
    bool success{false};
    QString error_message;

    qint64 input_size_bytes{0};
    qint64 output_size_bytes{0};
    double conversion_time_seconds{0};
};

/// @brief Aggregate result for a batch conversion
struct BatchConversionResult {
    QVector<ConversionResult> results;
    int total_files{0};
    int succeeded{0};
    int failed{0};
    int skipped{0};
    double total_time_seconds{0};
    qint64 total_input_bytes{0};
    qint64 total_output_bytes{0};
};
```

---

### FileConverterWidget (UI)

```cpp
class FileConverterWidget : public QWidget {
    Q_OBJECT

public:
    explicit FileConverterWidget(QWidget* parent = nullptr);
    ~FileConverterWidget() override;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void conversionStarted();
    void conversionFinished(BatchConversionResult result);

private Q_SLOTS:
    void onAddFilesClicked();
    void onAddFolderClicked();
    void onRemoveSelectedClicked();
    void onClearAllClicked();
    void onConvertClicked();
    void onCancelClicked();
    void onOutputBrowseClicked();
    void onTargetFormatChanged(const QString& format);
    void onFileProgress(int current, int total, const QString& filename);
    void onFileConverted(ConversionResult result);
    void onConversionComplete(BatchConversionResult result);
    void onConversionError(const QString& error);

private:
    // ── Input area ──────────────────────────────────────────
    QTableWidget* m_file_table{nullptr};       // Columns: File, Size, Format, Status
    QPushButton* m_add_files_button{nullptr};
    QPushButton* m_add_folder_button{nullptr};
    QPushButton* m_remove_selected_button{nullptr};
    QPushButton* m_clear_all_button{nullptr};

    // ── Conversion settings ─────────────────────────────────
    QComboBox* m_target_format_combo{nullptr};  // Filtered by input formats
    QComboBox* m_quality_preset_combo{nullptr}; // "High Quality", "Balanced", "Small Size"
    QStackedWidget* m_options_stack{nullptr};    // Per-category options panels

    // ── Image options panel ─────────────────────────────────
    QSpinBox* m_image_quality_spin{nullptr};
    QSpinBox* m_image_dpi_spin{nullptr};
    QSpinBox* m_image_max_width_spin{nullptr};
    QSpinBox* m_image_max_height_spin{nullptr};
    QCheckBox* m_image_preserve_aspect{nullptr};

    // ── Audio options panel ─────────────────────────────────
    QComboBox* m_audio_bitrate_combo{nullptr};
    QComboBox* m_audio_sample_rate_combo{nullptr};
    QComboBox* m_audio_channels_combo{nullptr};
    QCheckBox* m_audio_preserve_metadata{nullptr};

    // ── Video options panel ─────────────────────────────────
    QComboBox* m_video_codec_combo{nullptr};
    QComboBox* m_video_preset_combo{nullptr};
    QSpinBox* m_video_crf_spin{nullptr};
    QComboBox* m_video_resolution_combo{nullptr};
    QCheckBox* m_video_strip_audio{nullptr};

    // ── PDF options panel ───────────────────────────────────
    QComboBox* m_pdf_operation_combo{nullptr};  // "Convert", "Merge", "Split"
    QSpinBox* m_pdf_dpi_spin{nullptr};
    QComboBox* m_pdf_image_format_combo{nullptr};
    QLineEdit* m_pdf_page_range_edit{nullptr};

    // ── Spreadsheet options panel ───────────────────────────
    QComboBox* m_csv_delimiter_combo{nullptr};
    QCheckBox* m_csv_has_headers{nullptr};
    QComboBox* m_csv_encoding_combo{nullptr};

    // ── Output settings ─────────────────────────────────────
    QLineEdit* m_output_directory_edit{nullptr};
    QPushButton* m_output_browse_button{nullptr};
    QCheckBox* m_preserve_structure_checkbox{nullptr};
    QComboBox* m_collision_strategy_combo{nullptr};  // "Rename", "Overwrite", "Skip"

    // ── Progress area ───────────────────────────────────────
    QProgressBar* m_progress_bar{nullptr};
    QLabel* m_progress_label{nullptr};          // "Converting file 3/50: report.docx"
    QTextEdit* m_log_output{nullptr};
    QPushButton* m_convert_button{nullptr};
    QPushButton* m_cancel_button{nullptr};

    // ── Worker ──────────────────────────────────────────────
    QThread* m_worker_thread{nullptr};
    FileConverterWorker* m_worker{nullptr};
    bool m_conversion_active{false};

    // ── Helpers ─────────────────────────────────────────────
    void setupUi();
    void setupConnections();
    void populateTargetFormats();
    void updateOptionsPanel(FileFormatCategory category);
    ConversionJob buildConversionJob() const;

    FileFormatCategory detectCategory(const QString& extension) const;
    QStringList compatibleOutputFormats(FileFormatCategory category) const;
    void updateFormatComboForInputFiles();

    // Drag and drop
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void addFiles(const QStringList& file_paths);
};
```

### FileConverterWorker (Background Thread)

```cpp
class FileConverterWorker : public QObject {
    Q_OBJECT

public:
    explicit FileConverterWorker(QObject* parent = nullptr);

    void setJob(const ConversionJob& job);

public Q_SLOTS:
    void process();
    void cancel();

Q_SIGNALS:
    void fileStarted(int index, int total, const QString& filename);
    void fileProgress(int current, int total, const QString& filename);
    void fileConverted(ConversionResult result);
    void conversionComplete(BatchConversionResult result);
    void errorOccurred(const QString& error);
    void logMessage(const QString& message);

private:
    ConversionJob m_job;
    std::atomic<bool> m_cancelled{false};

    // ── Format-specific converters ──────────────────────────
    ConversionResult convertImage(const QString& input, const QString& output,
                                  const ConversionOptions& options);
    ConversionResult convertAudio(const QString& input, const QString& output,
                                  const ConversionOptions& options);
    ConversionResult convertVideo(const QString& input, const QString& output,
                                  const ConversionOptions& options);
    ConversionResult convertDocument(const QString& input, const QString& output,
                                     const ConversionOptions& options);
    ConversionResult convertSpreadsheet(const QString& input, const QString& output,
                                        const ConversionOptions& options);
    ConversionResult convertPdf(const QString& input, const QString& output,
                                const ConversionOptions& options);
    ConversionResult convertTextData(const QString& input, const QString& output,
                                     const ConversionOptions& options);

    // ── PDF special operations ──────────────────────────────
    ConversionResult mergePdfs(const QVector<QString>& inputs, const QString& output);
    ConversionResult splitPdf(const QString& input, const QString& output_dir,
                              const QVector<int>& page_range);
    ConversionResult imagesToPdf(const QVector<QString>& inputs, const QString& output,
                                 int dpi);
    ConversionResult pdfToImages(const QString& input, const QString& output_dir,
                                 const ConversionOptions& options);

    // ── FFmpeg wrapper ──────────────────────────────────────
    struct FfmpegResult {
        bool success{false};
        QString error_message;
        double duration_seconds{0};
    };

    FfmpegResult runFfmpeg(const QStringList& arguments, int timeout_ms);
    QString locateFfmpeg() const;
    QStringList buildAudioArgs(const QString& input, const QString& output,
                               const ConversionOptions& options) const;
    QStringList buildVideoArgs(const QString& input, const QString& output,
                               const ConversionOptions& options) const;

    // ── Document conversion (QTextDocument hub) ────────────
    // Reads DOCX/ODT/RTF/TXT into QTextDocument, exports to target format.
    ConversionResult convertDocument(const QString& input, const QString& output,
                                     const ConversionOptions& options);

    // Reader components (parse source → QTextDocument)
    std::expected<QTextDocument*, QString> loadDocx(const QString& path) const;
    std::expected<QTextDocument*, QString> loadOdt(const QString& path) const;
    std::expected<QTextDocument*, QString> loadRtf(const QString& path) const;
    std::expected<QTextDocument*, QString> loadTxt(const QString& path) const;

    // Writer components (QTextDocument → target format)
    ConversionResult exportToPdf(QTextDocument* doc, const QString& output) const;
    ConversionResult exportToHtml(QTextDocument* doc, const QString& output) const;
    ConversionResult exportToRtf(QTextDocument* doc, const QString& output) const;
    ConversionResult exportToTxt(QTextDocument* doc, const QString& output) const;
    ConversionResult exportToDocx(QTextDocument* doc, const QString& output) const;

    // ── qpdf wrapper ────────────────────────────────────────
    FfmpegResult runQpdf(const QStringList& arguments, int timeout_ms);
    QString locateQpdf() const;

    // ── Output path helpers ─────────────────────────────────
    QString buildOutputPath(const QString& input_file, const QString& target_extension,
                            const QString& output_dir, bool preserve_structure,
                            const QString& base_input_dir) const;
    QString resolveCollision(const QString& path, const QString& strategy) const;
};
```

---

### Format Registry

A centralized registry of all supported formats and their capabilities:

```cpp
class FileFormatRegistry {
public:
    static const FileFormatRegistry& instance();

    /// @brief Get all supported input formats
    QVector<FileFormatInfo> inputFormats() const;

    /// @brief Get all supported output formats
    QVector<FileFormatInfo> outputFormats() const;

    /// @brief Get formats in a specific category
    QVector<FileFormatInfo> formatsInCategory(FileFormatCategory category) const;

    /// @brief Get compatible output formats for a given input format
    QVector<FileFormatInfo> compatibleOutputFormats(const QString& input_extension) const;

    /// @brief Get compatible output formats for a set of input formats
    QVector<FileFormatInfo> commonOutputFormats(const QStringList& input_extensions) const;

    /// @brief Detect category from file extension
    FileFormatCategory detectCategory(const QString& extension) const;

    /// @brief Detect format from file content (magic bytes)
    FileFormatInfo detectFromContent(const QString& file_path) const;

    /// @brief Check if bundled FFmpeg is available in tools/
    bool isFfmpegAvailable() const;

    /// @brief Check if bundled qpdf is available in tools/
    bool isQpdfAvailable() const;

    /// @brief Check if bundled Pandoc is available in tools/
    bool isPandocAvailable() const;

    /// @brief Get unavailable formats (missing bundled tool) with reason
    QVector<QPair<FileFormatInfo, QString>> unavailableFormats() const;

private:
    FileFormatRegistry();
    void registerBuiltinFormats();
    void probeBundledTools();

    QVector<FileFormatInfo> m_formats;
    bool m_ffmpeg_available{false};
    bool m_qpdf_available{false};
    bool m_pandoc_available{false};
    QString m_ffmpeg_path;
    QString m_qpdf_path;
    QString m_pandoc_path;
};
```

---

### FFmpeg Process Wrapper

```cpp
/// @brief Thin wrapper around FFmpeg process execution
///
/// Locates ffmpeg.exe relative to the SAK executable (tools/ffmpeg.exe),
/// builds argument lists, runs conversions, and parses progress output.
class FfmpegRunner {
public:
    explicit FfmpegRunner(const QString& ffmpeg_path);

    struct ProgressInfo {
        double current_time_seconds{0};
        double total_duration_seconds{0};
        double speed{0};       // e.g., 2.5x realtime
        int frame{0};
        double fps{0};
        qint64 size_bytes{0};
    };

    using ProgressCallback = std::function<void(const ProgressInfo&)>;

    /// @brief Run FFmpeg with the given arguments
    /// @return success status and error message
    std::expected<QString, QString> run(
        const QStringList& arguments,
        int timeout_ms,
        std::atomic<bool>& cancelled,
        const ProgressCallback& on_progress = nullptr);

    /// @brief Probe a media file for duration, format, codecs
    struct MediaInfo {
        double duration_seconds{0};
        QString format;
        // Audio
        QString audio_codec;
        int audio_sample_rate{0};
        int audio_channels{0};
        int audio_bitrate_kbps{0};
        // Video
        QString video_codec;
        int video_width{0};
        int video_height{0};
        double video_fps{0};
        int video_bitrate_kbps{0};
    };

    std::expected<MediaInfo, QString> probe(const QString& file_path);

private:
    QString m_ffmpeg_path;

    ProgressInfo parseProgressLine(const QString& line) const;
};
```

---

### Pandoc Process Wrapper

```cpp
/// @brief Thin wrapper around Pandoc process execution
///
/// Locates pandoc.exe relative to the SAK executable (tools/pandoc.exe),
/// builds argument lists for document conversions, and handles timeout/cancellation.
/// All document format conversions route through this class when Pandoc is available.
class PandocRunner {
public:
    explicit PandocRunner(const QString& pandoc_path);

    /// @brief How to handle tracked changes in DOCX input
    enum class TrackChanges {
        Accept,    // Apply all changes (default)
        Reject,    // Discard all changes
        All        // Preserve change markup in output
    };

    /// @brief Options for a single document conversion
    struct ConversionOptions {
        QString input_path;
        QString output_path;
        QString input_format;           // e.g., "docx", "odt", "rtf", "html"
        QString output_format;          // e.g., "html", "docx", "odt", "rtf", "plain"
        TrackChanges track_changes{TrackChanges::Accept};
        bool extract_media{false};      // Extract embedded images to temp dir
        QString extract_media_dir;      // Directory for extracted images
        QString reference_doc;          // Optional: reference DOCX/ODT for style template
        bool standalone{true};          // Produce complete document (not fragment)
        bool sandbox{true};             // Restrict filesystem access (security)
    };

    /// @brief Run a document conversion
    /// @return Output file path on success, error message on failure
    std::expected<QString, QString> convert(
        const ConversionOptions& options,
        int timeout_ms,
        std::atomic<bool>& cancelled);

    /// @brief Check if the Pandoc binary exists and is executable
    bool isAvailable() const;

    /// @brief Get the Pandoc version string (e.g., "3.9.0.2")
    std::expected<QString, QString> version() const;

private:
    QString m_pandoc_path;

    /// @brief Build QProcess argument list from conversion options
    QStringList buildArguments(const ConversionOptions& options) const;
};
```

---

### Constants

```cpp
// file_converter_constants.h

namespace sak::file_converter {

// ── Timeout limits ──────────────────────────────────────────
constexpr int kFfmpegTimeoutMs = 3'600'000;         // 1 hour max per file
constexpr int kQpdfTimeoutMs = 60'000;               // 1 minute per PDF operation
constexpr int kPandocTimeoutMs = 300'000;            // 5 minutes per document conversion
constexpr int kProbeTimeoutMs = 10'000;              // 10 seconds for media probe

// ── Image defaults ──────────────────────────────────────────
constexpr int kDefaultImageQuality = 90;             // 1-100
constexpr int kDefaultImageDpi = 300;
constexpr int kMinImageQuality = 1;
constexpr int kMaxImageQuality = 100;
constexpr int kMinImageDpi = 72;
constexpr int kMaxImageDpi = 1200;

// ── Audio defaults ──────────────────────────────────────────
constexpr int kDefaultAudioBitrateKbps = 192;
constexpr int kDefaultAudioSampleRate = 44100;

// ── Video defaults ──────────────────────────────────────────
constexpr int kDefaultVideoCrf = 23;                 // H.264 default CRF
constexpr int kMinVideoCrf = 0;                      // Lossless
constexpr int kMaxVideoCrf = 51;                     // Worst quality

// ── PDF defaults ────────────────────────────────────────────
constexpr int kDefaultPdfDpi = 300;
constexpr int kMinPdfDpi = 72;
constexpr int kMaxPdfDpi = 600;

// ── Spreadsheet limits ──────────────────────────────────────
constexpr qint64 kMaxCsvFileSizeBytes = 500'000'000; // 500 MB CSV limit
constexpr int kMaxCsvColumns = 16'384;               // Excel column limit

// ── Document engine (ZIP) limits ────────────────────────────
constexpr qint64 kMaxZipEntryBytes = 100'000'000;   // 100 MB per entry (ZIP bomb guard)
constexpr int kMaxZipEntryCount = 1'000;             // Max files in DOCX/ODT archive

// ── Batch limits ────────────────────────────────────────────
constexpr int kMaxBatchFiles = 10'000;
constexpr qint64 kMaxSingleFileSizeBytes = 4'294'967'296; // 4 GB

// ── UI ──────────────────────────────────────────────────────
constexpr int kFileTableColumnCount = 4;             // File, Size, Format, Status

enum FileTableColumn {
    ColumnFile = 0,
    ColumnSize = 1,
    ColumnFormat = 2,
    ColumnStatus = 3
};

}  // namespace sak::file_converter
```

---

## 🖥️ UI Layout

### File Converter Tab Layout

```
┌─────────────────────────────────────────────────────────────┐
│  File Converter                                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌─────────────────────────────────────────────┐            │
│  │  📁 Drag files here or click Add Files      │  [Add Files]│
│  │                                              │  [Add Folder]│
│  │  File              │ Size    │ Format │ Status│  [Remove]  │
│  │  photo.bmp         │ 4.2 MB  │ BMP    │ Ready │  [Clear]   │
│  │  screenshot.tiff   │ 12.1 MB │ TIFF   │ Ready │            │
│  │  logo.svg          │ 28 KB   │ SVG    │ Ready │            │
│  │  banner.png        │ 1.8 MB  │ PNG    │ Done ✓│            │
│  └─────────────────────────────────────────────┘            │
│                                                             │
│  ┌─── Conversion Settings ──────────────────────────────┐   │
│  │  Target Format: [WebP          ▼]                     │   │
│  │  Quality Preset: [Balanced     ▼]                     │   │
│  │                                                       │   │
│  │  ┌─── Image Options ───────────────────────────────┐  │   │
│  │  │  Quality: [85   ] %    DPI: [300  ]             │  │   │
│  │  │  Max Width: [0   ] px  Max Height: [0   ] px    │  │   │
│  │  │  ☑ Preserve aspect ratio                        │  │   │
│  │  └─────────────────────────────────────────────────┘  │   │
│  └───────────────────────────────────────────────────────┘   │
│                                                             │
│  Output: [C:/Users/Tech/Desktop/Converted ][Browse]         │
│  ☐ Preserve directory structure   Conflicts: [Rename ▼]    │
│                                                             │
│  [▶ Convert]  [✕ Cancel]                                    │
│  ┌──────────────────────────────────────────────────────┐   │
│  │ Converting: 2 / 4 — screenshot.tiff → WebP           │   │
│  │ ████████████████░░░░░░░░░░░░░░░  50%                  │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
│  ┌─── Log ──────────────────────────────────────────────┐   │
│  │ ✓ photo.bmp → photo.webp (4.2 MB → 312 KB, 92.6%)   │   │
│  │ ⋯ Converting screenshot.tiff...                       │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### Adaptive Options Panel

The options panel (`m_options_stack`) switches based on the detected input category:

- **Images selected** → Shows quality, DPI, resize options
- **Audio selected** → Shows bitrate, sample rate, channels
- **Video selected** → Shows codec, preset, CRF, resolution
- **PDF selected** → Shows operation type (convert/merge/split), DPI, page range
- **Spreadsheets selected** → Shows delimiter, headers, encoding
- **Text/Data selected** → Shows encoding, line endings
- **Mixed categories** → Shows only the quality preset combo (per-category defaults applied)

### Drag-and-Drop Behavior

- Accepts file drops from Windows Explorer
- Accepts folder drops (recursively adds supported files)
- Rejects unsupported file types with a status bar message
- Duplicate files are rejected (same path already in the list)
- File table supports multi-select for removal
- PDF merge: rows can be reordered by drag within the table

---

## ⚠️ Bundled Tool Verification

Most conversion engines are compiled directly into the SAK executable and are always available. Three companion binaries (`ffmpeg.exe`, `qpdf.exe`, and `pandoc.exe`) ship in the `tools/` directory alongside the EXE. The UI verifies these at tab activation:

```cpp
// On tab activation / startup:
void FileConverterWidget::verifyBundledTools() {
    auto& registry = FileFormatRegistry::instance();

    // FFmpeg ships in tools/ — if missing, audio/video disabled
    if (!registry.isFfmpegAvailable()) {
        // Show warning: "ffmpeg.exe not found in tools/ — audio and video
        //                conversion unavailable. Re-extract the SAK distribution."
        // Disable audio/video format options in the target combo
    }

    // qpdf ships in tools/ — if missing, PDF merge/split disabled
    if (!registry.isQpdfAvailable()) {
        // Show warning: "qpdf.exe not found in tools/ — PDF merge/split
        //                unavailable. Re-extract the SAK distribution."
        // Disable PDF merge/split options (single PDF operations still work via Qt)
    }

    // Pandoc ships in tools/ — if missing, document conversions use fallback
    if (!registry.isPandocAvailable()) {
        // Show warning: "pandoc.exe not found in tools/ — document conversions
        //                will use basic engine (no embedded images, tracked changes).
        //                Re-extract the SAK distribution for full fidelity."
        // Document conversions still work via QTextDocument Hub, just with
        // reduced formatting fidelity (no images, tracked changes, footnotes)
    }

    // All other engines (images, spreadsheets, text/data, single PDF
    // operations) are compiled in and always available — no checks needed.
}
```

The UI should show a small status strip at the top of the converter tab:

```
🟢 Images  🟢 PDF  🟢 Documents (full)  🟢 Spreadsheets  🟢 Text/Data  🟢 Audio/Video
```

If a bundled binary is missing (corrupted distribution), the affected category degrades:

```
🟢 Images  🟢 PDF  🟡 Documents (basic — pandoc.exe missing)  🟢 Spreadsheets  🟢 Text/Data  🔴 Audio/Video (ffmpeg.exe missing)
```

> Under normal deployment all categories are green. A missing tool indicates a
> corrupted or incomplete extraction of the SAK distribution ZIP.

---

## 🔧 Implementation Phases

### Phase 1: Core Infrastructure + Image Conversion (Week 1-3)

**Goals**:
- FileConverterWidget UI scaffold with drag-and-drop
- FileConverterWorker thread framework
- FileFormatRegistry with all format metadata
- Image conversion via Qt QImage (PNG, JPEG, BMP, TIFF, GIF, WebP, ICO)
- Integration with OrganizerPanel as Tab 2

**Tasks**:
1. Create `file_converter_constants.h` with all named constants
2. Create `FileFormatRegistry` singleton with format definitions
3. Create `FileConverterWidget` with file table, format selector, options panels
4. Implement drag-and-drop (files + folders)
5. Create `FileConverterWorker` with worker thread pattern
6. Implement `convertImage()` using Qt QImage
7. SVG rasterization via QSvgRenderer → QImage
8. WebP support via libwebp (add to vcpkg dependencies)
9. ICO write support (multi-size icon packing)
10. Wire up progress signals between worker and UI
11. Integrate tab into OrganizerPanel
12. Write unit tests for format registry, image conversion

**Acceptance Criteria**:
- ✅ Drag-and-drop adds files to the table with auto-detected format
- ✅ Target format combo filters to compatible output formats
- ✅ Image quality/DPI sliders affect output
- ✅ Batch conversion of 100 images completes without errors
- ✅ Progress bar and log update in real time
- ✅ Cancel stops conversion within 1 second

---

### Phase 2: PDF Operations (Week 4-5)

**Goals**:
- PDF → Images (page extraction at configurable DPI)
- Images → PDF (multi-page PDF creation)
- PDF merge and split via qpdf
- PDF → Text extraction

**Tasks**:
1. Implement `pdfToImages()` using QPdfDocument + QImage rendering
2. Implement `imagesToPdf()` using QPdfWriter + QPainter
3. Bundle qpdf: create `scripts/bundle_qpdf.ps1`
4. Implement `mergePdfs()` via qpdf CLI wrapper
5. Implement `splitPdf()` via qpdf CLI wrapper
6. Implement `pdfToText()` using QPdfDocument text extraction
7. Add PDF options panel to UI (operation type, DPI, page range)
8. Add reorder-by-drag for PDF merge file list
9. Write unit tests

**Acceptance Criteria**:
- ✅ PDF → PNG at 300 DPI produces high-quality page images
- ✅ 50 images → single PDF in correct order
- ✅ PDF merge preserves page content and bookmarks
- ✅ Split PDF by page range produces correct individual files
- ✅ Text extraction handles multi-column and multi-page PDFs

---

### Phase 3: Spreadsheet + Text/Data Conversion (Week 6-7)

**Goals**:
- CSV/TSV ↔ XLSX conversion
- CSV/TSV ↔ JSON conversion
- JSON ↔ YAML ↔ XML conversion
- Markdown → HTML
- Text encoding conversion

**Tasks**:
1. Implement native CSV/TSV parser with configurable delimiters
2. Add QXlsx dependency via vcpkg, implement XLSX read/write
3. Implement CSV → JSON via QJsonDocument
4. Add yaml-cpp dependency via vcpkg, implement JSON ↔ YAML
5. Implement JSON ↔ XML via QDomDocument/QXmlStreamWriter
6. Add cmark dependency via vcpkg, implement Markdown → HTML
7. Implement text encoding conversion via QStringConverter
8. Add spreadsheet/text options panels to UI
9. Write unit tests for each parser and converter

**Acceptance Criteria**:
- ✅ CSV with commas, quotes, and newlines in fields parsed correctly
- ✅ XLSX output has typed columns (numbers, dates, strings)
- ✅ JSON ↔ YAML round-trip preserves all data
- ✅ XML output is well-formed and indented
- ✅ Markdown → HTML handles headers, lists, code blocks, links
- ✅ Encoding conversion handles BOM detection and preservation

---

### Phase 4: Audio/Video Conversion via FFmpeg (Week 8-10)

**Goals**:
- Bundle FFmpeg with verification
- Audio conversion: MP3, WAV, FLAC, OGG, AAC, WMA, M4A
- Video conversion: MP4, AVI, MKV, MOV, WMV, WebM
- Video → GIF (animated)
- Real-time progress parsing from FFmpeg stderr
- Media probe (ffprobe) for input file metadata

**Tasks**:
1. Create `scripts/bundle_ffmpeg.ps1` (download, verify SHA-256, extract)
2. Implement `FfmpegRunner` wrapper class
3. Implement `probe()` using ffprobe JSON output
4. Implement `buildAudioArgs()` for all audio format combinations
5. Implement `buildVideoArgs()` for all video format combinations
6. Parse FFmpeg progress output (`time=`, `speed=`, `frame=`) for progress bar
7. Video → GIF with palette generation for quality
8. Implement cancellation (kill FFmpeg process)
9. Metadata preservation (ID3 tags, Vorbis comments)
10. Add audio/video options panels to UI
11. Update `THIRD_PARTY_LICENSES.md`
12. Write unit tests (mock FFmpeg output parsing, argument building)

**Acceptance Criteria**:
- ✅ FFmpeg located and verified at startup
- ✅ Audio conversion preserves metadata tags
- ✅ Video conversion produces playable output in all target formats
- ✅ Progress bar updates smoothly during long video conversions
- ✅ Video → GIF produces reasonable quality at configurable size
- ✅ Cancel kills FFmpeg process and cleans up partial output files
- ✅ Handles missing FFmpeg gracefully (disables audio/video options)

---

### Phase 5: Document Engine — Pandoc Integration + In-Tree Readers (Week 11-13)

**Goals**:
- Bundle Pandoc as `tools/pandoc.exe` for high-fidelity DOCX/ODT conversions
- Implement `PandocRunner` wrapper (argument building, format detection, timeout, cancellation)
- In-tree DOCX reader: parse OOXML → HTML → QTextDocument (fallback engine)
- In-tree ODT reader: parse ODF → HTML → QTextDocument (fallback engine)
- RTF/TXT loading via QTextDocument (Qt native)
- Export from QTextDocument to PDF/HTML/RTF/TXT
- Engine selection logic: Pandoc (primary) vs QTextDocument Hub (fallback)
- Full document conversion matrix operational

**Tasks**:
1. Create `scripts/bundle_pandoc.ps1` (download, verify SHA-256, extract to `tools/`)
2. Implement `PandocRunner` wrapper class: locate binary, build arguments, run via `QProcess`, handle errors
3. PandocRunner: support `--track-changes`, `--extract-media`, `--standalone`, `--sandbox` options
4. PandocRunner: support all format pairs (DOCX↔HTML/ODT/RTF/TXT, ODT↔HTML/DOCX/RTF/TXT)
5. PandocRunner: for PDF output, generate intermediate HTML then pipe through QTextDocument→QPdfWriter
6. Add QuaZip vcpkg dependency for ZIP reading/writing
7. Implement `DocxReader` (fallback): open DOCX ZIP via QuaZip, extract `word/document.xml` + `word/styles.xml`
8. DocxReader XML→HTML: map OOXML elements (`<w:p>`, `<w:r>`, `<w:rPr>`, `<w:tbl>`) to HTML equivalents
9. Handle character formatting: bold (`<w:b>`), italic (`<w:i>`), underline (`<w:u>`), strikethrough, font (`<w:rFonts>`), size (`<w:sz>`), color (`<w:color>`), highlight
10. Handle paragraph formatting: alignment (`<w:jc>`), headings (`<w:pStyle>` → `<h1>`-`<h6>`), lists (`<w:numPr>`), page breaks
11. Handle basic tables: `<w:tbl>` → `<table>`, rows, cells, cell borders/shading
12. Implement `OdtReader` (fallback): open ODT ZIP, extract `content.xml` + `styles.xml`
13. OdtReader XML→HTML: map ODF elements (`<text:p>`, `<text:span>`, `<text:list>`, `<table:table>`) to HTML
14. Implement engine selection logic in `FileConverterWorker::convertDocument()`
15. Implement RTF loading via `QTextDocument` (Qt handles natively)
16. Implement TXT loading via `QTextDocument::setPlainText()`
17. Implement PDF export: `QTextDocument` → `QPdfWriter` + `QTextDocument::print()`
18. Implement HTML export: `QTextDocument::toHtml()`
19. Implement RTF export: `QTextDocument` writes RTF natively
20. Implement TXT export: `QTextDocument::toPlainText()`
21. Write unit tests: PandocRunner argument building, DocxReader round-trip, OdtReader round-trip, engine selection
22. Test with real-world DOCX/ODT samples (documents with images, tracked changes, footnotes)

**Acceptance Criteria**:
- ✅ Pandoc located and verified at startup; graceful degradation if missing
- ✅ DOCX → HTML via Pandoc preserves embedded images, tracked changes, footnotes
- ✅ DOCX → PDF via Pandoc→HTML→QTextDocument produces formatted PDF with images
- ✅ DOCX → PDF via fallback (DocxReader) preserves bold, italic, fonts, colors, headings, tables
- ✅ ODT → HTML/DOCX/RTF/TXT via Pandoc matches DOCX quality
- ✅ RTF → PDF/HTML/DOCX/TXT works via QTextDocument's native RTF support
- ✅ Engine fallback works: if Pandoc missing, QTextDocument Hub produces basic output
- ✅ All document conversions work fully offline — zero external dependencies beyond bundled tools

---

### Phase 6: DocxWriter + Polish (Week 14-15)

**Goals**:
- In-tree DOCX writer: QTextDocument → OOXML ZIP (fallback path for DOCX output)
- Output DOCX from any input document format (RTF→DOCX, ODT→DOCX, TXT→DOCX)
- Quality presets for all categories
- Track-changes UI option for DOCX input (accept/reject/preserve)
- Error handling and edge cases
- Final polish

**Tasks**:
1. Implement `DocxWriter`: iterate `QTextDocument` blocks/fragments, generate `word/document.xml`
2. Map QTextDocument formatting → OOXML: `QTextCharFormat` → `<w:rPr>`, `QTextBlockFormat` → `<w:pPr>`
3. Handle tables: `QTextTable` → `<w:tbl>` with rows, cells, basic borders
4. Generate required DOCX packaging: `[Content_Types].xml`, `_rels/.rels`, `word/_rels/document.xml.rels`, `word/styles.xml`
5. Package into ZIP via QuaZip with `.docx` extension
6. Validate output DOCX opens correctly in Word, LibreOffice, Google Docs
7. Add track-changes combo to document options panel (Accept / Reject / Preserve All)
8. Add quality presets ("High Quality", "Balanced", "Small Size") per category
9. Collision strategy implementation (Rename, Overwrite, Skip)
10. Edge cases: zero-byte files, read-only output dirs, path length limits, deeply nested lists
11. Final UI polish: status strip, tooltips, keyboard shortcuts
12. Update `THIRD_PARTY_LICENSES.md` with Pandoc entry
13. Write integration tests for full document conversion matrix
14. Write unit tests for DocxWriter output structure validation

**Acceptance Criteria**:
- ✅ Generated DOCX files open in Microsoft Word, LibreOffice, and Google Docs
- ✅ RTF → DOCX preserves paragraph and character formatting
- ✅ ODT → DOCX preserves paragraph and character formatting (via Pandoc or DocxWriter)
- ✅ TXT → DOCX produces properly structured document
- ✅ Track-changes option correctly accepts, rejects, or preserves changes in DOCX
- ✅ Quality presets apply sensible defaults per format
- ✅ All collision strategies work correctly
- ✅ No external runtime dependencies required for any conversion

---

## 📁 File Inventory

### New Files

```
include/sak/
    file_converter_widget.h
    file_converter_worker.h
    file_converter_constants.h
    file_format_registry.h
    ffmpeg_runner.h
    pandoc_runner.h
    docx_reader.h
    odt_reader.h
    docx_writer.h

src/gui/
    file_converter_widget.cpp

src/core/
    file_converter_worker.cpp
    file_format_registry.cpp
    ffmpeg_runner.cpp
    pandoc_runner.cpp
    docx_reader.cpp
    odt_reader.cpp
    docx_writer.cpp

scripts/
    bundle_ffmpeg.ps1
    bundle_qpdf.ps1
    bundle_pandoc.ps1

tests/unit/
    test_file_format_registry.cpp
    test_file_converter_worker.cpp
    test_ffmpeg_runner.cpp
    test_pandoc_runner.cpp
    test_docx_reader.cpp
    test_odt_reader.cpp
    test_docx_writer.cpp
```

### Modified Files

```
include/sak/organizer_panel.h     — Add m_file_converter_widget member
src/gui/organizer_panel.cpp        — Add Tab 2, wire statusMessage signal
CMakeLists.txt                     — Add new sources, headers, vcpkg deps
tests/CMakeLists.txt               — Add new test targets
THIRD_PARTY_LICENSES.md            — Add FFmpeg, qpdf, Pandoc, QXlsx, QuaZip, yaml-cpp, cmark, libwebp
```

### New vcpkg Dependencies

```json
{
    "qxlsx": ">=1.4.6",
    "yaml-cpp": ">=0.8.0",
    "cmark": ">=0.31.0",
    "libwebp": ">=1.3.0",
    "quazip": ">=1.4"
}
```

---

## 🧪 Testing Strategy

### Unit Tests

| Test File | Coverage |
|-----------|----------|
| `test_file_format_registry.cpp` | Format detection, category mapping, compatible format lookup, magic byte detection |
| `test_file_converter_worker.cpp` | Image conversion (all format pairs), CSV/TSV parsing, JSON/YAML/XML round-trips, encoding detection, path collision resolution |
| `test_ffmpeg_runner.cpp` | Argument building for audio/video, progress line parsing, timeout handling, probe output parsing |
| `test_pandoc_runner.cpp` | Argument building for document conversions, format detection, track-changes option handling, extract-media path building, timeout handling, fallback logic |
| `test_docx_reader.cpp` | DOCX→QTextDocument loading, bold/italic/font/color preservation, heading detection, table parsing, malformed ZIP handling |
| `test_odt_reader.cpp` | ODT→QTextDocument loading, formatting preservation, list parsing, table parsing, malformed ZIP handling |
| `test_docx_writer.cpp` | QTextDocument→DOCX output, OOXML structure validation, formatting round-trip (DOCX→QTextDocument→DOCX), opens in external readers |

### Integration Tests

| Test | Description |
|------|-------------|
| Batch image conversion | Convert 20 mixed-format images to WebP, verify output count, sizes, format |
| CSV → XLSX round-trip | CSV → XLSX → CSV, verify data integrity |
| PDF merge + split | Merge 5 PDFs → split back → verify page count matches |
| Text encoding round-trip | UTF-8 → UTF-16 → UTF-8, verify byte-for-byte match |
| DOCX → PDF → (manual) | Convert sample DOCX to PDF, verify formatting preserved visually |
| Document round-trip | DOCX → RTF → DOCX, verify formatting survives two conversions |

### Manual Test Matrix

| Input Format | Output Format | Status |
|-------------|---------------|--------|
| PNG | JPEG, WebP, BMP, TIFF, GIF, ICO | ☐ |
| JPEG | PNG, WebP, BMP, TIFF, GIF | ☐ |
| SVG | PNG, JPEG, WebP | ☐ |
| BMP | PNG, JPEG, WebP | ☐ |
| MP3 | WAV, FLAC, OGG, AAC | ☐ |
| WAV | MP3, FLAC, OGG, AAC | ☐ |
| FLAC | MP3, WAV, OGG | ☐ |
| MP4 | AVI, MKV, WebM, GIF | ☐ |
| AVI | MP4, MKV, WebM | ☐ |
| MKV | MP4, WebM | ☐ |
| RTF | PDF, TXT, HTML, DOCX | ☐ |
| DOCX | PDF, TXT, HTML, RTF | ☐ |
| ODT | PDF, TXT, HTML, RTF, DOCX | ☐ |
| TXT | PDF, HTML, RTF, DOCX | ☐ |
| CSV | XLSX, JSON, TSV | ☐ |
| XLSX | CSV, JSON | ☐ |
| JSON | YAML, XML, CSV | ☐ |
| YAML | JSON, XML | ☐ |
| Markdown | HTML | ☐ |
| Images | PDF (multi-page) | ☐ |
| PDF | Images (per-page) | ☐ |
| PDFs | Merged PDF | ☐ |

---

## 🔐 Security Considerations

- **No arbitrary command execution** — FFmpeg/qpdf/Pandoc arguments are built from structured options, never from user-provided strings. Use `QProcess` argument list (not shell expansion). Pandoc is invoked with `--sandbox` to prevent filesystem access beyond the input file.
- **Path traversal prevention** — Output paths are validated to stay within the designated output directory. Reject any path containing `..` after normalization.
- **File size limits** — Enforce `kMaxSingleFileSizeBytes` (4 GB) to prevent memory exhaustion. Large files use streaming conversion where possible.
- **Temp file cleanup** — Intermediate files created during multi-step conversions (e.g., video→GIF palette) are cleaned up in all code paths including cancellation and error.
- **No network access** — The converter never makes network requests. FFmpeg is invoked without network access flags.
- **Input validation** — Verify file magic bytes match the declared format before conversion to prevent processing malformed files.
- **ZIP bomb prevention** — DocxReader/OdtReader enforce `kMaxZipEntryBytes` and `kMaxZipEntryCount` limits when extracting DOCX/ODT archives. Reject archives with suspiciously large decompressed sizes or excessive entry counts.

---

## 📊 Windows API / Library Summary

| Operation | Engine |
|-----------|--------|
| Image read/write (PNG, JPEG, BMP, TIFF, GIF) | Qt QImage / QImageReader / QImageWriter |
| Image WebP encode/decode | libwebp |
| SVG rasterization | Qt SVG (`QSvgRenderer`) |
| Audio/Video conversion | FFmpeg (`ffmpeg.exe` via `QProcess`) |
| Media file probing | FFprobe (`ffprobe.exe` via `QProcess`) |
| PDF reading / page rendering | Qt PDF (`QPdfDocument`) |
| PDF writing (images→PDF, text→PDF) | Qt Print (`QPdfWriter` + `QPainter`) |
| PDF merge / split | qpdf (`qpdf.exe` via `QProcess`) |
| XLSX read/write | QXlsx library |
| CSV/TSV parsing | Native C++ (in-tree) |
| JSON read/write | Qt Core (`QJsonDocument`) |
| YAML read/write | yaml-cpp |
| XML read/write | Qt XML (`QDomDocument` / `QXmlStreamWriter`) |
| Markdown → HTML | cmark |
| Text encoding | Qt Core (`QStringConverter`) |
| Document read (DOCX) | Primary: Pandoc (`pandoc.exe` via `QProcess`); Fallback: In-tree DocxReader |
| Document read (ODT) | Primary: Pandoc (`pandoc.exe` via `QProcess`); Fallback: In-tree OdtReader |
| Document read (RTF) | Qt QTextDocument (native RTF loading) |
| Document write (PDF) | QTextDocument → QPdfWriter (compiled in) |
| Document write (HTML/RTF/TXT) | Primary: Pandoc (direct); Fallback: QTextDocument export |
| Document write (DOCX) | Primary: Pandoc (direct); Fallback: DocxWriter (QTextDocument → OOXML ZIP) |
| Document embedded images | Pandoc (`--extract-media`); not supported in fallback |
| Document tracked changes | Pandoc (`--track-changes=accept\|reject\|all`); not supported in fallback |
