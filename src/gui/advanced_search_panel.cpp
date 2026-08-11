// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_panel.cpp
/// @brief Implements the three-panel Advanced Search UI

#include "sak/advanced_search_panel.h"

#include "sak/advanced_search_controller.h"
#include "sak/detachable_log_window.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/regex_pattern_library.h"
#include "sak/storage_inventory_worker.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QProcess>
#include <QScrollArea>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTextBlock>
#include <QTextStream>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <array>

#include <windows.h>

#include <shellapi.h>
#undef emit
#undef signals
#undef slots

namespace sak {

namespace {

constexpr int kContextLinesComboWidth = 60;
constexpr int kExtensionFilterMaxWidth = 200;
constexpr int kPreferencesDialogMinWidth = sak::kDialogWidthSmall;
constexpr int kFileExplorerPaneDefaultWidth = 250;
constexpr int kResultsPaneDefaultWidth = 350;
constexpr int kPreviewPaneDefaultWidth = 600;
constexpr int kMatchNavButtonSize = sak::ui::kUiButtonSizeInline;
constexpr int kMatchCounterMinWidth = 60;
constexpr int kMetadataNameColumnIndex = 0;
constexpr int kMetadataDialogWidth = 560;
constexpr int kMetadataDialogHeight = 480;
constexpr int kMetadataNameColumnWidth = 200;
constexpr int kPreviewSeparatorLength = 50;
constexpr ushort kPreviewSeparatorChar = 0x2550;
constexpr int kPreviewLineNumberWidth = 5;
constexpr int kContextLinesMax = 10;
constexpr int kMaxSearchResultsLimit = 1'000'000;
constexpr int kPreviewSizeMaxMb = 500;
constexpr int kSearchSizeMaxMb = 1000;
constexpr int kSearchCacheMaxEntries = 1000;
constexpr int kAdvancedSearchTargetBrowseMaxEntries = 10'000;
constexpr int kSortMatchCountHigh = 2;
constexpr int kSortMatchCountLow = 3;
constexpr int kSortFileSizeLarge = 4;
constexpr int kSortFileSizeSmall = 5;
constexpr int kSortModifiedNewest = 6;
constexpr int kSortModifiedOldest = 7;
constexpr int kSizeDecimalPlaces = 2;
constexpr int kMetadataSeparatorLength = 2;
constexpr qint64 kBytesPerKiB = 1024;
constexpr qint64 kBytesPerMiB = kBytesPerKiB * kBytesPerKiB;

bool matchesAreMetadataOnly(const QVector<SearchMatch>& matches) {
    return !matches.isEmpty() && std::ranges::all_of(matches, [](const SearchMatch& match) {
        return match.line_content.startsWith(QLatin1String("[Metadata]"));
    });
}

QString previewSizeLimitMessage(qint64 file_size, int limit_megabytes) {
    return QObject::tr("File too large for preview (%1 MB, limit: %2 MB)")
        .arg(static_cast<double>(file_size) / static_cast<double>(kBytesPerMiB), 0, 'f', 1)
        .arg(limit_megabytes);
}

QString buildTextPreview(QTextStream& stream,
                         const QString& file_path,
                         const QVector<SearchMatch>& matches) {
    QSet<int> match_lines;
    for (const auto& match : matches) {
        match_lines.insert(match.line_number);
    }

    QString preview_text;
    preview_text += QObject::tr("File: %1\n").arg(file_path);
    preview_text += QObject::tr("Total matches: %1\n").arg(matches.size());
    preview_text += QString(kPreviewSeparatorLength, QChar(kPreviewSeparatorChar)) +
                    QStringLiteral("\n\n");

    int line_num = 0;
    while (!stream.atEnd()) {
        ++line_num;
        const QString line = stream.readLine();
        const QString prefix = match_lines.contains(line_num) ? QStringLiteral(">>> ")
                                                              : QStringLiteral("    ");
        preview_text += QStringLiteral("%1%2 | %3\n")
                            .arg(prefix)
                            .arg(line_num, kPreviewLineNumberWidth)
                            .arg(line);
    }
    return preview_text;
}

struct SearchBarRow1Ui {
    QComboBox* search_combo = nullptr;
    QPushButton* search_button = nullptr;
    QPushButton* stop_button = nullptr;
};

SearchBarRow1Ui buildSearchBarRow1(AdvancedSearchPanel* panel, QHBoxLayout* row) {
    Q_ASSERT(panel);
    Q_ASSERT(row);
    SearchBarRow1Ui ui;

    auto* search_label = new QLabel(QObject::tr("Search for:"), panel);
    row->addWidget(search_label);

    ui.search_combo = new QComboBox(panel);
    ui.search_combo->setEditable(true);
    ui.search_combo->setInsertPolicy(QComboBox::NoInsert);
    ui.search_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    ui.search_combo->setToolTip(QObject::tr("Enter search pattern (text or regex)"));
    setAccessible(ui.search_combo,
                  QObject::tr("Search pattern"),
                  QObject::tr("Enter text or regex pattern to search for"));
    row->addWidget(ui.search_combo);

    ui.search_button = new QPushButton(QObject::tr("Search"), panel);
    ui.search_button->setStyleSheet(ui::kPrimaryButtonStyle);
    ui.search_button->setToolTip(QObject::tr("Start search (Enter)"));
    setAccessible(ui.search_button, QObject::tr("Start search"));
    row->addWidget(ui.search_button);

    ui.stop_button = new QPushButton(QObject::tr("Stop"), panel);
    ui.stop_button->setStyleSheet(ui::kDangerButtonStyle);
    ui.stop_button->setToolTip(QObject::tr("Cancel current search"));
    ui.stop_button->setEnabled(false);
    setAccessible(ui.stop_button, QObject::tr("Stop search"));
    row->addWidget(ui.stop_button);

    return ui;
}

struct SearchBarRow2Ui {
    QComboBox* context_lines_combo = nullptr;
    QPushButton* regex_patterns_button = nullptr;
    QCheckBox* case_sensitive_check = nullptr;
    QCheckBox* whole_word_check = nullptr;
    QCheckBox* use_regex_check = nullptr;
    QCheckBox* image_metadata_check = nullptr;
};

SearchBarRow2Ui buildSearchBarRow2(AdvancedSearchPanel* panel,
                                   QHBoxLayout* row,
                                   int default_context_lines) {
    SearchBarRow2Ui ui;

    auto* context_label = new QLabel(QObject::tr("Context:"), panel);
    row->addWidget(context_label);

    ui.context_lines_combo = new QComboBox(panel);
    for (int i = 0; i <= kContextLinesMax; ++i) {
        ui.context_lines_combo->addItem(QString::number(i), i);
    }
    ui.context_lines_combo->setCurrentIndex(default_context_lines);
    ui.context_lines_combo->setToolTip(QObject::tr("Lines of context before/after matches"));
    ui.context_lines_combo->setFixedWidth(kContextLinesComboWidth);
    setAccessible(ui.context_lines_combo, QObject::tr("Context lines"));
    row->addWidget(ui.context_lines_combo);

    ui.regex_patterns_button = new QPushButton(QObject::tr("Regex Patterns \u25BE"), panel);
    ui.regex_patterns_button->setStyleSheet(ui::kPrimaryButtonStyle);
    ui.regex_patterns_button->setToolTip(QObject::tr("Select built-in or custom regex patterns"));
    setAccessible(ui.regex_patterns_button, QObject::tr("Regex pattern library"));
    row->addWidget(ui.regex_patterns_button);

    ui.case_sensitive_check = new QCheckBox(QObject::tr("Case sensitive"), panel);
    setAccessible(ui.case_sensitive_check, QObject::tr("Case sensitive search"));
    row->addWidget(ui.case_sensitive_check);

    ui.whole_word_check = new QCheckBox(QObject::tr("Whole word"), panel);
    setAccessible(ui.whole_word_check, QObject::tr("Match whole words only"));
    row->addWidget(ui.whole_word_check);

    ui.use_regex_check = new QCheckBox(QObject::tr("Regex"), panel);
    ui.use_regex_check->setToolTip(QObject::tr("Interpret search pattern as a regular expression"));
    setAccessible(ui.use_regex_check, QObject::tr("Use regex search"));
    row->addWidget(ui.use_regex_check);

    ui.image_metadata_check = new QCheckBox(QObject::tr("Image metadata"), panel);
    ui.image_metadata_check->setToolTip(QObject::tr("Search EXIF/GPS metadata in image files"));
    setAccessible(ui.image_metadata_check, QObject::tr("Search image metadata"));
    row->addWidget(ui.image_metadata_check);

    row->addStretch();

    return ui;
}

struct SearchBarRow3Ui {
    QLineEdit* extensions_edit = nullptr;
    QCheckBox* file_metadata_check = nullptr;
    QCheckBox* archive_search_check = nullptr;
    QCheckBox* binary_hex_check = nullptr;
    QPushButton* preferences_button = nullptr;
};

SearchBarRow3Ui buildSearchBarRow3(AdvancedSearchPanel* panel, QHBoxLayout* row) {
    Q_ASSERT(panel);
    Q_ASSERT(row);
    SearchBarRow3Ui ui;

    auto* ext_label = new QLabel(QObject::tr("Extensions:"), panel);
    row->addWidget(ext_label);

    ui.extensions_edit = new QLineEdit(panel);
    ui.extensions_edit->setPlaceholderText(QObject::tr(".py,.txt,.js,.cpp"));
    ui.extensions_edit->setToolTip(
        QObject::tr("Comma-separated file extensions to include (empty = all files)"));
    ui.extensions_edit->setMaximumWidth(kExtensionFilterMaxWidth);
    setAccessible(ui.extensions_edit, QObject::tr("File extension filter"));
    row->addWidget(ui.extensions_edit);

    ui.file_metadata_check = new QCheckBox(QObject::tr("File metadata"), panel);
    ui.file_metadata_check->setToolTip(
        QObject::tr("Search metadata in PDF, Office, audio/video files"));
    setAccessible(ui.file_metadata_check, QObject::tr("Search file metadata"));
    row->addWidget(ui.file_metadata_check);

    ui.archive_search_check = new QCheckBox(QObject::tr("Archive search"), panel);
    ui.archive_search_check->setToolTip(QObject::tr("Search inside ZIP/EPUB archives"));
    setAccessible(ui.archive_search_check, QObject::tr("Search archives"));
    row->addWidget(ui.archive_search_check);

    ui.binary_hex_check = new QCheckBox(QObject::tr("Binary/hex"), panel);
    ui.binary_hex_check->setToolTip(QObject::tr("Search binary files for hex patterns"));
    setAccessible(ui.binary_hex_check, QObject::tr("Binary hex search"));
    row->addWidget(ui.binary_hex_check);

    row->addStretch();

    return ui;
}

struct PreferencesDialogUi {
    QDialog* dialog = nullptr;
    QSpinBox* max_results_spin = nullptr;
    QSpinBox* preview_size_spin = nullptr;
    QSpinBox* search_size_spin = nullptr;
    QSpinBox* context_lines_spin = nullptr;
    QSpinBox* cache_size_spin = nullptr;
};

PreferencesDialogUi buildPreferencesDialog(QWidget* parent, const SearchPreferences& prefs) {
    PreferencesDialogUi ui;

    ui.dialog = new QDialog(parent);
    ui.dialog->setWindowTitle(QObject::tr("Search Preferences"));
    ui.dialog->setMinimumWidth(kPreferencesDialogMinWidth);

    auto* layout = new QVBoxLayout(ui.dialog);

    auto* form = new QFormLayout();
    form->setSpacing(ui::kSpacingSmall);

    ui.max_results_spin = new QSpinBox(ui.dialog);
    ui.max_results_spin->setRange(0, kMaxSearchResultsLimit);
    ui.max_results_spin->setSpecialValueText(QObject::tr("Unlimited"));
    ui.max_results_spin->setValue(prefs.max_results);
    ui.max_results_spin->setToolTip(QObject::tr("Maximum total matches (0 = unlimited)"));
    form->addRow(QObject::tr("Max results:"), ui.max_results_spin);

    ui.preview_size_spin = new QSpinBox(ui.dialog);
    ui.preview_size_spin->setRange(1, kPreviewSizeMaxMb);
    ui.preview_size_spin->setSuffix(QObject::tr(" MB"));
    ui.preview_size_spin->setValue(prefs.max_preview_file_size_mb);
    ui.preview_size_spin->setToolTip(QObject::tr("Maximum file size for preview pane"));
    form->addRow(QObject::tr("Max preview file size:"), ui.preview_size_spin);

    ui.search_size_spin = new QSpinBox(ui.dialog);
    ui.search_size_spin->setRange(1, kSearchSizeMaxMb);
    ui.search_size_spin->setSuffix(QObject::tr(" MB"));
    ui.search_size_spin->setValue(prefs.max_search_file_size_mb);
    ui.search_size_spin->setToolTip(QObject::tr("Maximum file size to search"));
    form->addRow(QObject::tr("Max search file size:"), ui.search_size_spin);

    ui.context_lines_spin = new QSpinBox(ui.dialog);
    ui.context_lines_spin->setRange(0, kContextLinesMax);
    ui.context_lines_spin->setValue(prefs.context_lines);
    ui.context_lines_spin->setToolTip(QObject::tr("Default context lines before/after matches"));
    form->addRow(QObject::tr("Default context lines:"), ui.context_lines_spin);

    ui.cache_size_spin = new QSpinBox(ui.dialog);
    ui.cache_size_spin->setRange(1, kSearchCacheMaxEntries);
    ui.cache_size_spin->setValue(prefs.max_cache_size);
    ui.cache_size_spin->setToolTip(QObject::tr("Maximum LRU file cache entries"));
    form->addRow(QObject::tr("Cache size:"), ui.cache_size_spin);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         ui.dialog);
    layout->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, ui.dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, ui.dialog, &QDialog::reject);

    return ui;
}

SearchPreferences preferencesFromDialogUi(const PreferencesDialogUi& ui) {
    SearchPreferences new_prefs;
    new_prefs.max_results = ui.max_results_spin->value();
    new_prefs.max_preview_file_size_mb = ui.preview_size_spin->value();
    new_prefs.max_search_file_size_mb = ui.search_size_spin->value();
    new_prefs.context_lines = ui.context_lines_spin->value();
    new_prefs.max_cache_size = ui.cache_size_spin->value();
    return new_prefs;
}

struct FileSortEntry {
    QString path;
    QVector<SearchMatch> matches;
    qint64 file_size = 0;
    QDateTime last_modified;
};

bool compareFileEntries(const FileSortEntry& a, const FileSortEntry& b, int sort_mode) {
    switch (sort_mode) {
    case 0:  // Path A-Z
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    case 1:  // Path Z-A
        return a.path.compare(b.path, Qt::CaseInsensitive) > 0;
    case kSortMatchCountHigh:
        return a.matches.size() > b.matches.size();
    case kSortMatchCountLow:
        return a.matches.size() < b.matches.size();
    case kSortFileSizeLarge:
        return a.file_size > b.file_size;
    case kSortFileSizeSmall:
        return a.file_size < b.file_size;
    case kSortModifiedNewest:
        return a.last_modified > b.last_modified;
    case kSortModifiedOldest:
        return a.last_modified < b.last_modified;
    default:
        return a.path < b.path;
    }
}

QVector<FileSortEntry> buildSortedFileEntries(
    const QMap<QString, QVector<SearchMatch>>& all_results, int sort_mode) {
    QVector<FileSortEntry> sorted_files;
    sorted_files.reserve(all_results.size());

    const bool needs_file_info = (sort_mode >= kSortFileSizeLarge);

    for (auto it = all_results.constBegin(); it != all_results.constEnd(); ++it) {
        FileSortEntry entry;
        entry.path = it.key();
        entry.matches = it.value();

        if (needs_file_info) {
            const QFileInfo info(entry.path);
            entry.file_size = info.size();
            entry.last_modified = info.lastModified();
        }

        sorted_files.append(std::move(entry));
    }

    std::ranges::sort(sorted_files,

                      [sort_mode](const FileSortEntry& a, const FileSortEntry& b) {
                          return compareFileEntries(a, b, sort_mode);
                      });

    return sorted_files;
}

void populateSortedResultsTree(QTreeWidget* tree,
                               QWidget* icon_source,
                               const QVector<FileSortEntry>& sorted_files) {
    tree->setUpdatesEnabled(false);
    for (const auto& entry : sorted_files) {
        auto* file_item = new QTreeWidgetItem(tree);
        file_item->setText(0, QString("%1  (%2)").arg(entry.path).arg(entry.matches.size()));
        file_item->setData(0, Qt::UserRole, entry.path);
        file_item->setData(0, Qt::UserRole + 1, -1);  // Not a specific match
        file_item->setIcon(0, icon_source->style()->standardIcon(QStyle::SP_FileIcon));

        for (int i = 0; i < entry.matches.size(); ++i) {
            const auto& match = entry.matches[i];
            auto* match_item = new QTreeWidgetItem(file_item);

            const QString truncated = match.line_content.left(80).trimmed();
            match_item->setText(0, QString("Line %1: %2").arg(match.line_number).arg(truncated));
            match_item->setData(0, Qt::UserRole, entry.path);
            match_item->setData(0, Qt::UserRole + 1, i);  // Match index
        }
    }
    tree->setUpdatesEnabled(true);
}

// -- Metadata dialog helpers -------------------------------------------------

QString formatMetadataSize(qint64 bytes) {
    constexpr qint64 kKB = 1024;
    constexpr qint64 kMB = 1024 * 1024;
    constexpr qint64 kGB = 1024 * 1024 * 1024;
    if (bytes >= kGB) {
        return QString("%1 GB").arg(static_cast<double>(bytes) / kGB, 0, 'f', kSizeDecimalPlaces);
    }
    if (bytes >= kMB) {
        return QString("%1 MB").arg(static_cast<double>(bytes) / kMB, 0, 'f', kSizeDecimalPlaces);
    }
    if (bytes >= kKB) {
        return QString("%1 KB").arg(static_cast<double>(bytes) / kKB, 0, 'f', 1);
    }
    return QString("%1 bytes").arg(bytes);
}

QMap<QString, QString> parseMetadataFromMatches(const QVector<SearchMatch>& matches) {
    QMap<QString, QString> metadata;
    const QLatin1String prefix("[Metadata] ");
    for (const auto& match : matches) {
        QString line = match.line_content;
        if (line.startsWith(prefix)) {
            line = line.mid(prefix.size());
        }
        const qsizetype colon = line.indexOf(QLatin1String(": "));
        if (colon > 0) {
            metadata.insert(line.left(colon).trimmed(),
                            line.mid(colon + kMetadataSeparatorLength).trimmed());
        }
    }
    return metadata;
}

struct MetadataField {
    const char* key;
    const char* display;
};

struct MetadataCategory {
    const char* name;
    const MetadataField* fields;
    int count;
};

constexpr auto kFileFields =
    std::to_array<MetadataField>({{.key = "FileName", .display = "Name"},
                                  {.key = "FileSize", .display = "Size"},
                                  {.key = "FileType", .display = "Type"},
                                  {.key = "Created", .display = "Created"},
                                  {.key = "LastModified", .display = "Modified"}});

constexpr auto kImageFields =
    std::to_array<MetadataField>({{.key = "Width", .display = "Width"},
                                  {.key = "Height", .display = "Height"},
                                  {.key = "Dimensions", .display = "Dimensions"},
                                  {.key = "Format", .display = "Format"},
                                  {.key = "ColorDepth", .display = "Color Depth"}});

constexpr auto kCameraFields =
    std::to_array<MetadataField>({{.key = "CameraMake", .display = "Make"},
                                  {.key = "CameraModel", .display = "Model"},
                                  {.key = "ExposureTime", .display = "Exposure"},
                                  {.key = "FNumber", .display = "Aperture"},
                                  {.key = "ISOSpeed", .display = "ISO"},
                                  {.key = "FocalLength", .display = "Focal Length"},
                                  {.key = "FocalLengthIn35mm", .display = "Focal Length (35mm)"},
                                  {.key = "Flash", .display = "Flash"},
                                  {.key = "MeteringMode", .display = "Metering"},
                                  {.key = "WhiteBalance", .display = "White Balance"},
                                  {.key = "DateTimeOriginal", .display = "Date Taken"},
                                  {.key = "DateTimeDigitized", .display = "Date Digitized"},
                                  {.key = "Software", .display = "Software"},
                                  {.key = "Artist", .display = "Artist"},
                                  {.key = "Copyright", .display = "Copyright"},
                                  {.key = "Orientation", .display = "Orientation"},
                                  {.key = "ImageDescription", .display = "Description"},
                                  {.key = "LensMake", .display = "Lens Make"},
                                  {.key = "LensModel", .display = "Lens Model"}});

constexpr auto kGpsFields =
    std::to_array<MetadataField>({{.key = "GPSLatitude", .display = "Latitude"},
                                  {.key = "GPSLatitudeRef", .display = "Lat Ref"},
                                  {.key = "GPSLongitude", .display = "Longitude"},
                                  {.key = "GPSLongitudeRef", .display = "Lon Ref"},
                                  {.key = "GPSAltitude", .display = "Altitude"},
                                  {.key = "GPSAltitudeRef", .display = "Alt Ref"},
                                  {.key = "GPSDateStamp", .display = "Date Stamp"},
                                  {.key = "GPSTimeStamp", .display = "Time Stamp"}});

constexpr auto kDocFields =
    std::to_array<MetadataField>({{.key = "Title", .display = "Title"},
                                  {.key = "Author", .display = "Author"},
                                  {.key = "Subject", .display = "Subject"},
                                  {.key = "Keywords", .display = "Keywords"},
                                  {.key = "Creator", .display = "Creator"},
                                  {.key = "Producer", .display = "Producer"},
                                  {.key = "Company", .display = "Company"},
                                  {.key = "Version", .display = "Version"},
                                  {.key = "CreationDate", .display = "Created"},
                                  {.key = "ModDate", .display = "Modified"}});

constexpr auto kAudioFields =
    std::to_array<MetadataField>({{.key = "Title", .display = "Title"},
                                  {.key = "Artist", .display = "Artist"},
                                  {.key = "Album", .display = "Album"},
                                  {.key = "Year", .display = "Year"},
                                  {.key = "Genre", .display = "Genre"},
                                  {.key = "Track", .display = "Track"},
                                  {.key = "Comment", .display = "Comment"},
                                  {.key = "AlbumArtist", .display = "Album Artist"},
                                  {.key = "Composer", .display = "Composer"},
                                  {.key = "Publisher", .display = "Publisher"},
                                  {.key = "Copyright", .display = "Copyright"}});

constexpr auto kMetadataCategories = std::to_array<MetadataCategory>(
    {{.name = "File Information", .fields = kFileFields.data(), .count = std::size(kFileFields)},
     {.name = "Image Properties", .fields = kImageFields.data(), .count = std::size(kImageFields)},
     {.name = "Camera / EXIF", .fields = kCameraFields.data(), .count = std::size(kCameraFields)},
     {.name = "GPS Location", .fields = kGpsFields.data(), .count = std::size(kGpsFields)},
     {.name = "Document Properties", .fields = kDocFields.data(), .count = std::size(kDocFields)},
     {.name = "Audio Tags", .fields = kAudioFields.data(), .count = std::size(kAudioFields)}});

void populateMetadataTree(QTreeWidget* tree, const QMap<QString, QString>& metadata) {
    const QString dash = QStringLiteral("\u2014");
    for (const auto& category : kMetadataCategories) {
        auto* cat_item = new QTreeWidgetItem(tree);
        cat_item->setText(0, QString::fromUtf8(category.name));
        cat_item->setFlags(cat_item->flags() & ~Qt::ItemIsSelectable);
        QFont bold = cat_item->font(0);
        bold.setBold(true);
        cat_item->setFont(0, bold);

        int populated = 0;
        for (int i = 0; i < category.count; ++i) {
            const auto& field = category.fields[i];
            auto* field_item = new QTreeWidgetItem(cat_item);
            field_item->setText(0, QString::fromUtf8(field.display));
            const QString value = metadata.value(QString::fromUtf8(field.key), dash);
            field_item->setText(1, value);
            if (value != dash) {
                ++populated;
            }
        }
        cat_item->setText(1, QString("(%1/%2)").arg(populated).arg(category.count));
        cat_item->setExpanded(populated > 0);
    }
}

}  // namespace

// -- Construction / Destruction ----------------------------------------------

AdvancedSearchPanel::AdvancedSearchPanel(QWidget* parent)
    : QWidget(parent), m_controller(std::make_unique<AdvancedSearchController>(this)) {
    setupUi();

    // Connect controller signals
    connect(m_controller.get(),
            &AdvancedSearchController::searchStarted,
            this,
            &AdvancedSearchPanel::onSearchStarted);
    connect(m_controller.get(),
            &AdvancedSearchController::resultsReceived,
            this,
            &AdvancedSearchPanel::onResultsReceived);
    // fileSearched is handled internally by the controller for counting;
    // results are delivered via resultsReceived -- no panel slot needed.
    connect(m_controller.get(),
            &AdvancedSearchController::searchFinished,
            this,
            &AdvancedSearchPanel::onSearchFinished);
    connect(m_controller.get(),
            &AdvancedSearchController::searchFailed,
            this,
            &AdvancedSearchPanel::onSearchFailed);
    connect(m_controller.get(),
            &AdvancedSearchController::searchCancelled,
            this,
            &AdvancedSearchPanel::onSearchCancelled);
    connect(m_controller.get(),
            &AdvancedSearchController::statusMessage,
            this,
            &AdvancedSearchPanel::statusMessage);
    connect(m_controller.get(),
            &AdvancedSearchController::progressUpdate,
            this,
            &AdvancedSearchPanel::progressUpdate);

    // Populate search history (dropdown only -- don't pre-fill the edit field)
    m_search_combo->addItems(m_controller->searchHistory());
    m_search_combo->setCurrentIndex(-1);
    m_search_combo->lineEdit()->setPlaceholderText(tr("Enter search pattern..."));

    logInfo("AdvancedSearchPanel initialized");
}

AdvancedSearchPanel::~AdvancedSearchPanel() {
    logInfo("AdvancedSearchPanel destroyed");
}

// -- UI Setup ----------------------------------------------------------------

void AdvancedSearchPanel::setupUi() {
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    root_layout->setSpacing(sak::ui::kSpacingNone);

    auto* content_widget = new QWidget(this);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    main_layout->setSpacing(ui::kSpacingDefault);

    root_layout->addWidget(content_widget);

    createSearchBar(main_layout);
    createThreePanelSplitter(main_layout);
    createStatusBar(main_layout);
    createRegexPatternMenu();
}

void AdvancedSearchPanel::createSearchBar(QVBoxLayout* layout) {
    Q_ASSERT(m_controller);
    auto* search_group = new QGroupBox(tr("Search"), this);
    auto* search_layout = new QVBoxLayout(search_group);
    search_layout->setSpacing(ui::kSpacingSmall);

    createSearchTargetRow(search_layout);

    // -- Row 1: Search input + buttons --
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(ui::kSpacingSmall);

    {
        const SearchBarRow1Ui ui = buildSearchBarRow1(this, row1);
        m_search_combo = ui.search_combo;
        m_search_button = ui.search_button;
        m_stop_button = ui.stop_button;
    }

    search_layout->addLayout(row1);

    // -- Row 2: Context + Regex Patterns + Checkboxes --
    auto* row2 = new QHBoxLayout();
    row2->setSpacing(ui::kSpacingMedium);

    {
        const SearchBarRow2Ui ui =
            buildSearchBarRow2(this, row2, m_controller->preferences().context_lines);
        m_context_lines_combo = ui.context_lines_combo;
        m_regex_patterns_button = ui.regex_patterns_button;
        m_case_sensitive_check = ui.case_sensitive_check;
        m_whole_word_check = ui.whole_word_check;
        m_use_regex_check = ui.use_regex_check;
        m_image_metadata_check = ui.image_metadata_check;
    }
    search_layout->addLayout(row2);

    // -- Row 3: Extensions + More checkboxes --
    auto* row3 = new QHBoxLayout();
    row3->setSpacing(ui::kSpacingMedium);

    {
        const SearchBarRow3Ui ui = buildSearchBarRow3(this, row3);
        m_extensions_edit = ui.extensions_edit;
        m_file_metadata_check = ui.file_metadata_check;
        m_archive_search_check = ui.archive_search_check;
        m_binary_hex_check = ui.binary_hex_check;
    }

    search_layout->addLayout(row3);

    layout->addWidget(search_group);

    populateSearchTargets(FileManagementFileSystemBridge::mountedTargets());

    connectSearchBarSignals();
}

void AdvancedSearchPanel::createSearchTargetRow(QVBoxLayout* search_layout) {
    auto* target_row = new QHBoxLayout();
    target_row->setSpacing(ui::kSpacingSmall);
    m_target_combo = new QComboBox(this);
    m_target_combo->setAccessibleName(tr("Advanced search filesystem target"));
    m_target_combo->setToolTip(tr("Choose a mounted volume or supported raw/image file system"));
    target_row->addWidget(m_target_combo, 1);

    m_target_refresh_button = new QPushButton(tr("Refresh"), this);
    m_target_refresh_button->setAccessibleName(tr("Refresh mounted search targets"));
    m_target_refresh_button->setStyleSheet(ui::kSecondaryButtonStyle);
    target_row->addWidget(m_target_refresh_button);

    m_target_scan_button = new QPushButton(tr("Scan Disks"), this);
    m_target_scan_button->setAccessibleName(tr("Scan disk and partition search targets"));
    m_target_scan_button->setStyleSheet(ui::kPrimaryButtonStyle);
    target_row->addWidget(m_target_scan_button);

    m_target_manual_button = new QPushButton(tr("Add Raw/Image"), this);
    m_target_manual_button->setAccessibleName(tr("Add raw or image search target"));
    m_target_manual_button->setStyleSheet(ui::kPrimaryButtonStyle);
    target_row->addWidget(m_target_manual_button);
    search_layout->addLayout(target_row);
}

void AdvancedSearchPanel::connectSearchBarSignals() {
    // Connect search actions
    connect(m_search_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onSearchClicked);
    connect(m_stop_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onStopClicked);
    connect(m_regex_patterns_button,
            &QPushButton::clicked,
            this,
            &AdvancedSearchPanel::onRegexPatternsClicked);
    connect(m_target_combo, &QComboBox::currentIndexChanged, this, [this](int) {
        populateFileExplorerRoot();
    });
    connect(m_target_refresh_button, &QPushButton::clicked, this, [this]() {
        populateSearchTargets(FileManagementFileSystemBridge::mountedTargets());
        Q_EMIT statusMessage(tr("Mounted search targets refreshed"), sak::kTimerStatusMessageMs);
    });
    connect(m_target_scan_button, &QPushButton::clicked, this, [this]() {
        Q_EMIT statusMessage(tr("Scanning disk and partition search targets..."), 0);
        setEnabled(false);
        const auto inventory = StorageInventoryWorker::scanCurrentSystem();
        setEnabled(true);
        populateSearchTargets(FileManagementFileSystemBridge::targetsFromInventory(inventory));
        if (inventory.hasPartitionEnumerationFailure() || !inventory.warnings.isEmpty()) {
            // Enumeration was not faithful (a disk's partitions failed to enumerate, or the
            // scan recorded warnings), so the target list is incomplete. Report it as such
            // instead of a clean "scanned" that hides the missing volumes.
            Q_EMIT statusMessage(
                tr("Search targets scanned with warnings; some disks may be missing"),
                sak::kTimerStatusDefaultMs);
        } else {
            Q_EMIT statusMessage(tr("Search targets scanned"), sak::kTimerStatusDefaultMs);
        }
    });
    connect(m_target_manual_button,
            &QPushButton::clicked,
            this,
            &AdvancedSearchPanel::addManualSearchTarget);

    // Enter key triggers search
    connect(m_search_combo->lineEdit(),
            &QLineEdit::returnPressed,
            this,
            &AdvancedSearchPanel::onSearchClicked);
}

void AdvancedSearchPanel::createThreePanelSplitter(QVBoxLayout* layout) {
    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setChildrenCollapsible(false);

    createFileExplorer();
    createResultsTree();
    createPreviewPane();

    m_splitter->addWidget(m_file_explorer->parentWidget());
    m_splitter->addWidget(m_results_tree->parentWidget());
    m_splitter->addWidget(m_preview_edit->parentWidget());

    // Default proportions: 250 : 350 : 600
    m_splitter->setSizes(
        {kFileExplorerPaneDefaultWidth, kResultsPaneDefaultWidth, kPreviewPaneDefaultWidth});

    layout->addWidget(m_splitter, 1);  // stretch = 1 to fill space
}

void AdvancedSearchPanel::createFileExplorer() {
    auto* container = new QWidget(this);
    auto* container_layout = new QVBoxLayout(container);
    container_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    container_layout->setSpacing(ui::kSpacingTight);

    auto* header_label = new QLabel(tr("File Explorer:"), container);
    QFont header_font = header_label->font();
    header_font.setBold(true);
    header_label->setFont(header_font);
    container_layout->addWidget(header_label);

    m_file_explorer = new QTreeWidget(container);
    m_file_explorer->setHeaderHidden(true);
    m_file_explorer->setContextMenuPolicy(Qt::CustomContextMenu);
    m_file_explorer->setToolTip(tr("Select a directory to search"));
    setAccessible(m_file_explorer,
                  tr("File explorer tree"),
                  tr("Navigate and select directories to search"));
    container_layout->addWidget(m_file_explorer);

    // Connect file explorer signals
    connect(m_file_explorer,
            &QTreeWidget::itemClicked,
            this,
            &AdvancedSearchPanel::onFileExplorerItemClicked);
    connect(m_file_explorer,
            &QTreeWidget::itemExpanded,
            this,
            &AdvancedSearchPanel::onFileExplorerItemExpanded);
    connect(m_file_explorer,
            &QTreeWidget::customContextMenuRequested,
            this,
            &AdvancedSearchPanel::onFileExplorerContextMenu);

    populateFileExplorerRoot();
}

void AdvancedSearchPanel::createResultsTree() {
    auto* container = new QWidget(this);
    auto* container_layout = new QVBoxLayout(container);
    container_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    container_layout->setSpacing(ui::kSpacingTight);

    // Header with sort combo
    auto* header_row = new QHBoxLayout();
    auto* header_label = new QLabel(tr("Search Results:"), container);
    QFont header_font = header_label->font();
    header_font.setBold(true);
    header_label->setFont(header_font);
    header_row->addWidget(header_label);

    m_results_count_label = new QLabel(container);
    m_results_count_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    header_row->addWidget(m_results_count_label);

    header_row->addStretch();

    auto* sort_label = new QLabel(tr("Sort:"), container);
    header_row->addWidget(sort_label);

    m_sort_combo = new QComboBox(container);
    m_sort_combo->addItems({tr("Path (A-Z)"),
                            tr("Path (Z-A)"),
                            tr("Match Count (High)"),
                            tr("Match Count (Low)"),
                            tr("File Size (Large)"),
                            tr("File Size (Small)"),
                            tr("Date Modified (Newest)"),
                            tr("Date Modified (Oldest)")});
    m_sort_combo->setToolTip(tr("Sort search results"));
    setAccessible(m_sort_combo, tr("Result sort order"));
    header_row->addWidget(m_sort_combo);

    container_layout->addLayout(header_row);

    m_results_tree = new QTreeWidget(container);
    m_results_tree->setHeaderHidden(true);
    m_results_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_results_tree->setRootIsDecorated(true);
    m_results_tree->setToolTip(tr("Click a result to preview, double-click to open"));
    setAccessible(m_results_tree,
                  tr("Search results tree"),
                  tr("Click results to preview, double-click to open in editor"));
    container_layout->addWidget(m_results_tree);

    // Connect results signals
    connect(
        m_results_tree, &QTreeWidget::itemClicked, this, &AdvancedSearchPanel::onResultItemClicked);
    connect(m_results_tree,
            &QTreeWidget::itemDoubleClicked,
            this,
            &AdvancedSearchPanel::onResultItemDoubleClicked);
    connect(m_results_tree,
            &QTreeWidget::customContextMenuRequested,
            this,
            &AdvancedSearchPanel::onResultContextMenu);
    connect(m_sort_combo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &AdvancedSearchPanel::onSortChanged);
}

void AdvancedSearchPanel::createPreviewPane() {
    auto* container = new QWidget(this);
    auto* container_layout = new QVBoxLayout(container);
    container_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    container_layout->setSpacing(ui::kSpacingTight);

    // Header with match navigation
    auto* header_row = new QHBoxLayout();

    m_preview_header_label = new QLabel(tr("Preview:"), container);
    QFont header_font = m_preview_header_label->font();
    header_font.setBold(true);
    m_preview_header_label->setFont(header_font);
    header_row->addWidget(m_preview_header_label);

    header_row->addStretch();

    m_prev_match_button = new QPushButton(QStringLiteral("\u25C4"), container);
    m_prev_match_button->setFixedSize(kMatchNavButtonSize, kMatchNavButtonSize);
    m_prev_match_button->setToolTip(tr("Previous match"));
    m_prev_match_button->setEnabled(false);
    m_prev_match_button->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    setAccessible(m_prev_match_button, tr("Previous match"));
    header_row->addWidget(m_prev_match_button);

    m_match_counter_label = new QLabel(container);
    m_match_counter_label->setMinimumWidth(kMatchCounterMinWidth);
    m_match_counter_label->setAlignment(Qt::AlignCenter);
    header_row->addWidget(m_match_counter_label);

    m_next_match_button = new QPushButton(QStringLiteral("\u25BA"), container);  // >
    m_next_match_button->setFixedSize(kMatchNavButtonSize, kMatchNavButtonSize);
    m_next_match_button->setToolTip(tr("Next match"));
    m_next_match_button->setEnabled(false);
    m_next_match_button->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    setAccessible(m_next_match_button, tr("Next match"));
    header_row->addWidget(m_next_match_button);

    container_layout->addLayout(header_row);

    m_preview_edit = new QTextEdit(container);
    m_preview_edit->setReadOnly(true);
    m_preview_edit->setFont(QFont("Consolas", ui::kFontSizeBody));
    m_preview_edit->setToolTip(tr("File preview with highlighted matches"));
    m_preview_edit->setStyleSheet(
        ui::textEditPreviewStyle(ui::kColorBgWhite, ui::kColorTextBody, ui::kColorBorderDefault));
    setAccessible(m_preview_edit,
                  tr("File preview pane"),
                  tr("Shows file content with highlighted search matches"));
    container_layout->addWidget(m_preview_edit);

    // Connect navigation buttons
    connect(
        m_prev_match_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onPreviousMatch);
    connect(m_next_match_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onNextMatch);
}

void AdvancedSearchPanel::createStatusBar(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    auto* status_row = new QHBoxLayout();
    status_row->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kSpacingTight, sak::ui::kMarginNone, sak::ui::kMarginNone);

    m_preferences_button = new QPushButton(tr("Settings"), this);
    m_preferences_button->setToolTip(tr("Search settings (max results, file sizes, etc.)"));
    m_preferences_button->setAccessibleName(tr("Search settings"));
    m_preferences_button->setStyleSheet(ui::kPrimaryButtonStyle);
    status_row->addWidget(m_preferences_button);
    connect(m_preferences_button,
            &QPushButton::clicked,
            this,
            &AdvancedSearchPanel::onPreferencesClicked);

    status_row->addStretch();

    layout->addLayout(status_row);
}

void AdvancedSearchPanel::createRegexPatternMenu() {
    Q_ASSERT(m_controller);
    m_regex_menu = new QMenu(this);

    const auto* library = m_controller->patternLibrary();
    const auto builtins = library->builtinPatterns();

    for (const auto& pattern : builtins) {
        auto* action = m_regex_menu->addAction(pattern.label);
        action->setCheckable(true);
        action->setChecked(pattern.enabled);
        action->setData(pattern.key);

        connect(action, &QAction::toggled, this, [this, key = pattern.key](bool checked) {
            m_controller->patternLibrary()->setPatternEnabled(key, checked);
            updateRegexPatternsButton();
        });
    }

    // Separator before custom patterns
    m_regex_menu->addSeparator();

    const auto customs = library->customPatterns();
    if (!customs.isEmpty()) {
        auto* custom_header = m_regex_menu->addAction(tr("Custom Patterns:"));
        custom_header->setEnabled(false);

        for (const auto& pattern : customs) {
            auto* action = m_regex_menu->addAction(pattern.label);
            action->setCheckable(true);
            action->setChecked(pattern.enabled);
            action->setData(pattern.key);

            connect(action, &QAction::toggled, this, [this, key = pattern.key](bool checked) {
                m_controller->patternLibrary()->setPatternEnabled(key, checked);
                updateRegexPatternsButton();
            });
        }
    }

    m_regex_menu->addSeparator();
    auto* clear_action = m_regex_menu->addAction(tr("Clear All"));
    connect(clear_action, &QAction::triggered, this, [this]() {
        m_controller->patternLibrary()->clearAll();
        // Refresh menu checkmarks
        for (auto* action : m_regex_menu->actions()) {
            if (action->isCheckable()) {
                action->setChecked(false);
            }
        }
        updateRegexPatternsButton();
    });
}

// -- File Explorer Population ------------------------------------------------

void AdvancedSearchPanel::populateSearchTargets(QVector<FileManagementTarget> targets) {
    m_search_targets = std::move(targets);
    if (m_target_combo == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_target_combo);
    m_target_combo->clear();
    for (const auto& target : m_search_targets) {
        m_target_combo->addItem(target.label);
    }
    if (!m_search_targets.isEmpty()) {
        m_target_combo->setCurrentIndex(0);
    }
    if (m_file_explorer != nullptr) {
        populateFileExplorerRoot();
    }
}

void AdvancedSearchPanel::addManualSearchTarget() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add Raw or Image Search Target"));
    dialog.setMinimumWidth(sak::kDialogWidthLarge);
    auto* layout = new QFormLayout(&dialog);

    auto* path = new QLineEdit(&dialog);
    path->setAccessibleName(tr("Raw or image search target path"));
    auto* browse = new QPushButton(tr("Browse"), &dialog);
    browse->setStyleSheet(ui::kSecondaryButtonStyle);
    auto* path_row = new QHBoxLayout();
    path_row->addWidget(path, 1);
    path_row->addWidget(browse);
    layout->addRow(tr("Target path:"), path_row);

    auto* fs = new QComboBox(&dialog);
    fs->addItems({QStringLiteral("ext2"),
                  QStringLiteral("ext3"),
                  QStringLiteral("ext4"),
                  QStringLiteral("HFS+"),
                  QStringLiteral("HFSX"),
                  QStringLiteral("APFS"),
                  QStringLiteral("XFS"),
                  QStringLiteral("Btrfs")});
    fs->setAccessibleName(tr("Manual search target file system"));
    layout->addRow(tr("File system:"), fs);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Add Target"));
    layout->addRow(buttons);
    connect(browse, &QPushButton::clicked, &dialog, [this, path]() {
        const QString file = QFileDialog::getOpenFileName(this, tr("Select Raw or Image Target"));
        if (!file.isEmpty()) {
            path->setText(file);
        }
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted || path->text().trimmed().isEmpty()) {
        return;
    }
    m_search_targets.append(
        FileManagementFileSystemBridge::manualTarget(path->text(), fs->currentText()));
    m_target_combo->addItem(m_search_targets.constLast().label);
    m_target_combo->setCurrentIndex(static_cast<int>(m_search_targets.size() - 1));
}

FileManagementTarget AdvancedSearchPanel::currentSearchTarget() const {
    const int index = (m_target_combo != nullptr) ? m_target_combo->currentIndex() : -1;
    if (index < 0 || index >= m_search_targets.size()) {
        return {};
    }
    return m_search_targets.at(index);
}

void AdvancedSearchPanel::populateFileExplorerRoot() {
    Q_ASSERT(m_file_explorer);
    // Expanding the root lists the target's root directory synchronously, so
    // defer it while the panel is hidden (it starts life as a non-current
    // sub-tab); showEvent finishes any pending population.
    if (!isVisible()) {
        m_file_explorer_pending = true;
        return;
    }
    m_file_explorer_pending = false;
    m_file_explorer->clear();

    const auto target = currentSearchTarget();
    if (target.root_path.isEmpty()) {
        return;
    }

    auto* root_item = new QTreeWidgetItem(m_file_explorer);
    root_item->setText(0, target.label);
    root_item->setData(0,
                       Qt::UserRole,
                       target.local_file_system ? target.root_path : QStringLiteral("/"));
    root_item->setData(0, Qt::UserRole + 1, true);
    root_item->setIcon(0,
                       style()->standardIcon(target.local_file_system ? QStyle::SP_DriveHDIcon
                                                                      : QStyle::SP_DirIcon));
    addPlaceholderChild(root_item);
    m_file_explorer->expandItem(root_item);
}

void AdvancedSearchPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (m_file_explorer_pending) {
        populateFileExplorerRoot();
    }
}

void AdvancedSearchPanel::populateDirectoryChildren(QTreeWidgetItem* parent_item,
                                                    const QString& dir_path) {
    const auto target = currentSearchTarget();
    if (target.root_path.isEmpty()) {
        return;
    }

    const auto listing = FileManagementFileSystemBridge::listDirectory(
        target, dir_path, kAdvancedSearchTargetBrowseMaxEntries);
    if (!listing.ok) {
        auto* item = new QTreeWidgetItem(parent_item);
        item->setText(0, listing.blockers.join(QStringLiteral("; ")));
        item->setFlags(Qt::NoItemFlags);
        return;
    }

    for (const auto& entry : listing.entries) {
        auto* item = new QTreeWidgetItem(parent_item);
        item->setText(0, entry.name);
        item->setData(0, Qt::UserRole, entry.path);
        item->setData(0, Qt::UserRole + 1, entry.directory);

        if (entry.directory) {
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            addPlaceholderChild(item);  // Enable expansion
        } else {
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        }
    }
}

void AdvancedSearchPanel::addPlaceholderChild(QTreeWidgetItem* parent_item) {
    auto* placeholder = new QTreeWidgetItem(parent_item);
    placeholder->setText(0, kPlaceholderText);
    placeholder->setFlags(Qt::NoItemFlags);
}

void AdvancedSearchPanel::removePlaceholderChildren(QTreeWidgetItem* parent_item) {
    for (int i = parent_item->childCount() - 1; i >= 0; --i) {
        auto* child = parent_item->child(i);
        if (child->text(0) == kPlaceholderText) {
            delete parent_item->takeChild(i);
        }
    }
}

// -- Search Controls ---------------------------------------------------------

void AdvancedSearchPanel::onSearchClicked() {
    Q_ASSERT(m_controller);
    const SearchConfig config = buildSearchConfig();

    if (config.pattern.isEmpty() && config.root_path.isEmpty()) {
        sak::logWarning("Search: no directory selected and no pattern entered");
        Q_EMIT statusMessage(tr("Please select a directory and enter a search pattern"),
                             sak::kTimerStatusMessageMs);
        return;
    }

    if (config.pattern.isEmpty()) {
        sak::logWarning("Search: empty search pattern");
        Q_EMIT statusMessage(tr("Please enter a search pattern"), sak::kTimerStatusMessageMs);
        return;
    }

    if (config.root_path.isEmpty()) {
        sak::logWarning("Search: no directory selected");
        Q_EMIT statusMessage(tr("Please select a directory to search"), sak::kTimerStatusMessageMs);
        return;
    }

    clearResults();
    m_controller->startSearch(config);
}

void AdvancedSearchPanel::onStopClicked() {
    m_controller->cancelSearch();
}

void AdvancedSearchPanel::onRegexPatternsClicked() {
    if (m_regex_menu != nullptr) {
        m_regex_menu->popup(
            m_regex_patterns_button->mapToGlobal(QPoint(0, m_regex_patterns_button->height())));
    }
}

void AdvancedSearchPanel::onPreferencesClicked() {
    Q_ASSERT(m_controller);
    Q_ASSERT(m_context_lines_combo);
    const auto prefs = m_controller->preferences();
    const PreferencesDialogUi ui = buildPreferencesDialog(this, prefs);

    if (ui.dialog->exec() == QDialog::Accepted) {
        const SearchPreferences new_prefs = preferencesFromDialogUi(ui);
        m_controller->setPreferences(new_prefs);
        m_context_lines_combo->setCurrentIndex(new_prefs.context_lines);
        logMessage(tr("Preferences updated"));
    }

    ui.dialog->deleteLater();
}

SearchConfig AdvancedSearchPanel::buildSearchConfig() const {
    Q_ASSERT(m_file_explorer);
    Q_ASSERT(m_controller);
    SearchConfig config;

    // Get selected directory from file explorer
    auto* selected_item = m_file_explorer->currentItem();
    if (selected_item != nullptr) {
        config.root_path = selected_item->data(0, Qt::UserRole).toString();
    }
    config.file_system_target = currentSearchTarget();
    config.use_file_system_target = !config.file_system_target.root_path.isEmpty() &&
                                    !config.file_system_target.local_file_system;

    // Get search pattern -- use regex patterns if active, otherwise text input
    const auto* library = m_controller->patternLibrary();
    if (library->activeCount() > 0) {
        config.pattern = library->combinedPattern();
        config.use_regex = true;  // Pattern library always produces regex
    } else {
        config.pattern = m_search_combo->currentText().trimmed();
        config.use_regex = m_use_regex_check->isChecked();
    }

    config.case_sensitive = m_case_sensitive_check->isChecked();
    config.whole_word = m_whole_word_check->isChecked();
    config.search_image_metadata = m_image_metadata_check->isChecked();
    config.search_file_metadata = m_file_metadata_check->isChecked();
    config.search_in_archives = m_archive_search_check->isChecked();
    config.hex_search = m_binary_hex_check->isChecked();

    config.context_lines = m_context_lines_combo->currentData().toInt();

    // Parse file extensions
    const QString ext_text = m_extensions_edit->text().trimmed();
    if (!ext_text.isEmpty()) {
        const auto parts = ext_text.split(',', Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            config.file_extensions.append(part.trimmed());
        }
    }

    // Note: regex mode is only enabled by the pattern library or explicitly
    // by the user. We do NOT auto-detect regex from the pattern text because
    // common characters like . and \ appear in filenames and Windows paths.

    return config;
}

// -- Controller Signal Handlers ----------------------------------------------

void AdvancedSearchPanel::onSearchStarted(const QString& pattern) {
    setSearchRunning(true);
    Q_EMIT statusMessage(tr("Searching for: %1").arg(pattern), 0);
    Q_EMIT progressUpdate(0, 0);
    logMessage(tr("Search started: %1").arg(pattern));

    // Update search combo history
    if (m_search_combo->findText(pattern) == -1) {
        m_search_combo->insertItem(0, pattern);
    }
}

QTreeWidgetItem* AdvancedSearchPanel::findOrCreateFileItem(
    const QString& file_path, const QVector<SearchMatch>& file_matches) {
    for (int i = 0; i < m_results_tree->topLevelItemCount(); ++i) {
        auto* item = m_results_tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == file_path) {
            // Keep the children already materialized for this file: onResultsReceived
            // appends only the newly-arrived matches. Deleting and rebuilding every child
            // on each batch is quadratic in a file's total match count, which lets a file
            // with many matches freeze the UI thread.
            item->setText(0, QString("%1  (%2)").arg(file_path).arg(file_matches.size()));
            return item;
        }
    }
    auto* file_item = new QTreeWidgetItem(m_results_tree);
    file_item->setText(0, QString("%1  (%2)").arg(file_path).arg(file_matches.size()));
    file_item->setData(0, Qt::UserRole, file_path);
    file_item->setData(0, Qt::UserRole + 1, -1);
    file_item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
    return file_item;
}

void AdvancedSearchPanel::onResultsReceived(QVector<sak::SearchMatch> matches) {
    if (matches.isEmpty() || (m_results_tree == nullptr)) {
        return;
    }

    // Record, per affected file, how many matches were already present before this batch
    // so only the newly-arrived matches are materialized as child items below. The child's
    // stored index (UserRole+1) stays equal to the match's position in m_all_results, which
    // navigateToMatch/onResultItemClicked rely on.
    QMap<QString, int> base_count;
    QVector<QString> order;
    for (const auto& match : matches) {
        if (!base_count.contains(match.file_path)) {
            const auto existing = m_all_results.constFind(match.file_path);
            base_count.insert(match.file_path,
                              existing == m_all_results.constEnd()
                                  ? 0
                                  : static_cast<int>(existing.value().size()));
            order.append(match.file_path);
        }
        m_all_results[match.file_path].append(match);
    }

    m_results_tree->setUpdatesEnabled(false);
    for (const auto& file_path : order) {
        const auto& file_matches = m_all_results[file_path];
        auto* file_item = findOrCreateFileItem(file_path, file_matches);

        for (int i = base_count.value(file_path); i < file_matches.size(); ++i) {
            const auto& m = file_matches[i];
            auto* match_item = new QTreeWidgetItem(file_item);
            const QString truncated = m.line_content.left(80).trimmed();
            match_item->setText(0, QString("Line %1: %2").arg(m.line_number).arg(truncated));
            match_item->setData(0, Qt::UserRole, file_path);
            match_item->setData(0, Qt::UserRole + 1, i);
        }
    }
    m_results_tree->setUpdatesEnabled(true);
}

void AdvancedSearchPanel::onSearchFinished(int total_matches, int total_files, bool complete) {
    Q_ASSERT(m_results_count_label);
    setSearchRunning(false);
    Q_EMIT statusMessage(tr("Found %1 matches in %2 files").arg(total_matches).arg(total_files),
                         sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(total_matches, total_matches);
    m_results_count_label->setText(
        tr("(%1 matches, %2 files)").arg(total_matches).arg(total_files));
    if (!complete) {
        // The log pane is the durable record of the run. Writing "Search complete"
        // here for a run that skipped files would contradict the status bar and
        // let the user read a partial result as an authoritative "not found".
        logMessage(tr("Search INCOMPLETE: %1 matches in %2 files; some files could not be "
                      "searched, results may be missing matches")
                       .arg(total_matches)
                       .arg(total_files));
        return;
    }
    logMessage(tr("Search complete: %1 matches in %2 files").arg(total_matches).arg(total_files));
}

void AdvancedSearchPanel::onSearchFailed(const QString& error) {
    setSearchRunning(false);
    Q_EMIT statusMessage(tr("Search failed: %1").arg(error), sak::kTimerStatusDefaultMs);
    logMessage(tr("Search failed: %1").arg(error));
}

void AdvancedSearchPanel::onSearchCancelled() {
    setSearchRunning(false);
    Q_EMIT statusMessage(tr("Search cancelled"), sak::kTimerStatusDefaultMs);
    logMessage(tr("Search cancelled"));
}

// -- File Explorer Handlers --------------------------------------------------

void AdvancedSearchPanel::onFileExplorerItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty()) {
        Q_EMIT statusMessage(tr("Selected: %1").arg(path), 0);
    }
}

void AdvancedSearchPanel::onFileExplorerItemExpanded(QTreeWidgetItem* item) {
    if (item == nullptr) {
        return;
    }

    // Check if placeholder child exists (lazy loading)
    if (item->childCount() == 1 && item->child(0)->text(0) == kPlaceholderText) {
        removePlaceholderChildren(item);

        const QString dir_path = item->data(0, Qt::UserRole).toString();
        populateDirectoryChildren(item, dir_path);
    }
}

void AdvancedSearchPanel::onFileExplorerContextMenu(const QPoint& pos) {
    Q_ASSERT(m_file_explorer);
    auto* item = m_file_explorer->itemAt(pos);
    if (item == nullptr) {
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();
    const auto target = currentSearchTarget();

    QMenu menu(this);

    auto* open_dir_action = menu.addAction(tr("Open in Explorer"));
    open_dir_action->setEnabled(target.local_file_system);
    connect(open_dir_action, &QAction::triggered, this, [path]() {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
    });

    auto* copy_path_action = menu.addAction(tr("Copy Path"));
    connect(copy_path_action, &QAction::triggered, this, [path]() {
        QApplication::clipboard()->setText(path);
    });

    menu.exec(m_file_explorer->viewport()->mapToGlobal(pos));
}

// -- Results Tree Handlers ---------------------------------------------------

void AdvancedSearchPanel::onResultItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }

    const QString file_path = item->data(0, Qt::UserRole).toString();
    if (file_path.isEmpty()) {
        return;
    }

    const auto it = m_all_results.constFind(file_path);
    if (it != m_all_results.constEnd()) {
        showFilePreview(file_path, it.value());

        // If clicking a specific match line, navigate to it
        const int match_idx = item->data(0, Qt::UserRole + 1).toInt();
        if (match_idx >= 0 && match_idx < it.value().size()) {
            navigateToMatch(match_idx);
        }
    }
}

void AdvancedSearchPanel::onResultItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (item == nullptr) {
        return;
    }

    const QString file_path = item->data(0, Qt::UserRole).toString();
    if (!file_path.isEmpty() && currentSearchTarget().local_file_system) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(file_path));
    }
}

void AdvancedSearchPanel::onResultContextMenu(const QPoint& pos) {
    Q_ASSERT(m_results_tree);
    auto* item = m_results_tree->itemAt(pos);
    if (item == nullptr) {
        return;
    }

    const QString file_path = item->data(0, Qt::UserRole).toString();

    QMenu menu(this);

    auto* open_action = menu.addAction(tr("Open File"));
    open_action->setEnabled(currentSearchTarget().local_file_system);
    connect(open_action, &QAction::triggered, this, [file_path]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(file_path));
    });

    auto* open_dir_action = menu.addAction(tr("Open Directory"));
    open_dir_action->setEnabled(currentSearchTarget().local_file_system);
    connect(open_dir_action, &QAction::triggered, this, [file_path]() {
        const QString native = QDir::toNativeSeparators(file_path);
        // explorer.exe by absolute Windows-directory path (it is NOT in System32): a bare
        // name is resolved through the CreateProcess search order like any other launch.
        (void)sak::startDetachedWindowsTool(QStringLiteral("explorer.exe"),
                                            {QStringLiteral("/select,") + native});
    });

    menu.addSeparator();

    auto* view_meta_action = menu.addAction(tr("View Metadata"));
    connect(view_meta_action, &QAction::triggered, this, [this, file_path]() {
        showMetadataDialog(file_path);
    });

    auto* view_props_action = menu.addAction(tr("View Properties"));
    view_props_action->setEnabled(currentSearchTarget().local_file_system);
    connect(view_props_action, &QAction::triggered, this, [file_path]() {
        const QString native = QDir::toNativeSeparators(file_path);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(SHELLEXECUTEINFOW);
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        sei.lpFile = reinterpret_cast<LPCWSTR>(native.utf16());
        sei.nShow = SW_SHOW;
        ShellExecuteExW(&sei);
    });

    menu.addSeparator();

    auto* copy_path_action = menu.addAction(tr("Copy Path"));
    connect(copy_path_action, &QAction::triggered, this, [file_path]() {
        QApplication::clipboard()->setText(file_path);
    });

    menu.exec(m_results_tree->viewport()->mapToGlobal(pos));
}

void AdvancedSearchPanel::onSortChanged(int /*index*/) {
    sortResults();
}

// -- Results Management ------------------------------------------------------

void AdvancedSearchPanel::clearResults() {
    Q_ASSERT(m_results_tree);
    Q_ASSERT(m_preview_edit);
    m_results_tree->clear();
    m_all_results.clear();
    m_preview_edit->clear();
    m_current_matches.clear();
    m_current_preview_file.clear();
    m_current_match_index = -1;
    m_results_count_label->clear();
    m_match_counter_label->clear();
    m_preview_header_label->setText(tr("Preview:"));
    m_prev_match_button->setEnabled(false);
    m_next_match_button->setEnabled(false);
}

void AdvancedSearchPanel::sortResults() {
    m_results_tree->clear();

    const int sort_mode = m_sort_combo->currentIndex();
    const QVector<FileSortEntry> sorted_files = buildSortedFileEntries(m_all_results, sort_mode);
    populateSortedResultsTree(m_results_tree, this, sorted_files);
}

// -- Preview -----------------------------------------------------------------

void AdvancedSearchPanel::showFilePreview(const QString& file_path,
                                          const QVector<SearchMatch>& matches) {
    m_current_preview_file = file_path;
    m_current_matches = matches;
    m_current_match_index = matches.isEmpty() ? -1 : 0;

    m_preview_header_label->setText(tr("Preview:"));

    if (matchesAreMetadataOnly(matches)) {
        showMetadataPreview(file_path, matches);
        return;
    }

    const auto target = currentSearchTarget();
    if (!target.local_file_system) {
        const auto prefs = m_controller->preferences();
        const uint64_t max_size = static_cast<uint64_t>(prefs.max_preview_file_size_mb) *
                                  kBytesPerMiB;
        const auto read = FileManagementFileSystemBridge::readFile(target, file_path, max_size);
        if (!read.ok) {
            m_preview_edit->setPlainText(read.blockers.join(QStringLiteral("\n")));
            return;
        }
        QString content = QString::fromUtf8(read.data);
        QTextStream stream(&content, QIODevice::ReadOnly);
        m_preview_edit->setPlainText(buildTextPreview(stream, file_path, matches));
        highlightMatches();
        updateMatchCounter();
        const bool has_matches = !matches.isEmpty();
        m_prev_match_button->setEnabled(has_matches);
        m_next_match_button->setEnabled(has_matches);
        if (has_matches) {
            navigateToMatch(0);
        }
        return;
    }

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_preview_edit->setPlainText(tr("Unable to open file: %1").arg(file_path));
        return;
    }

    const auto prefs = m_controller->preferences();
    const qint64 max_size = static_cast<qint64>(prefs.max_preview_file_size_mb) * kBytesPerMiB;
    if (file.size() > max_size) {
        m_preview_edit->setPlainText(
            previewSizeLimitMessage(file.size(), prefs.max_preview_file_size_mb));
        return;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    m_preview_edit->setPlainText(buildTextPreview(stream, file_path, matches));

    highlightMatches();
    updateMatchCounter();

    const bool has_matches = !matches.isEmpty();
    m_prev_match_button->setEnabled(has_matches);
    m_next_match_button->setEnabled(has_matches);

    // Navigate to first match
    if (has_matches) {
        navigateToMatch(0);
    }
}

QMap<QString, QString> AdvancedSearchPanel::collectRemoteMetadata(const QString& file_path) const {
    QMap<QString, QString> metadata;
    const auto target = currentSearchTarget();
    metadata[QStringLiteral("FilePath")] = file_path;
    metadata[QStringLiteral("Target")] = target.label;
    metadata[QStringLiteral("FileSystem")] = target.file_system;
    if (m_all_results.contains(file_path)) {
        const auto parsed = parseMetadataFromMatches(m_all_results.value(file_path));
        for (auto it = parsed.cbegin(); it != parsed.cend(); ++it) {
            metadata.insert(it.key(), it.value());
        }
    }
    return metadata;
}

QMap<QString, QString> AdvancedSearchPanel::collectLocalMetadata(const QString& file_path) const {
    const QFileInfo fi(file_path);
    QMap<QString, QString> metadata;
    metadata[QStringLiteral("FileName")] = fi.fileName();
    metadata[QStringLiteral("FileSize")] = formatMetadataSize(fi.size());
    metadata[QStringLiteral("FileType")] = fi.suffix().toUpper();
    metadata[QStringLiteral("Created")] = fi.birthTime().toString(Qt::ISODate);
    metadata[QStringLiteral("LastModified")] = fi.lastModified().toString(Qt::ISODate);

    const QImageReader reader(file_path);
    if (reader.canRead()) {
        const QSize size = reader.size();
        if (size.isValid()) {
            metadata[QStringLiteral("Width")] = QString::number(size.width()) +
                                                QStringLiteral(" px");
            metadata[QStringLiteral("Height")] = QString::number(size.height()) +
                                                 QStringLiteral(" px");
            metadata[QStringLiteral("Dimensions")] =
                QString("%1 x %2").arg(size.width()).arg(size.height());
        }
        const auto format = QString::fromUtf8(reader.format()).toUpper();
        if (!format.isEmpty()) {
            metadata[QStringLiteral("Format")] = format;
        }
    }

    if (m_all_results.contains(file_path)) {
        const auto parsed = parseMetadataFromMatches(m_all_results.value(file_path));
        for (auto it = parsed.cbegin(); it != parsed.cend(); ++it) {
            if (!metadata.contains(it.key())) {
                metadata.insert(it.key(), it.value());
            }
        }
    }
    return metadata;
}

void AdvancedSearchPanel::showMetadataTreeDialog(const QString& title,
                                                 const QMap<QString, QString>& metadata) {
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(kMetadataDialogWidth, kMetadataDialogHeight);
    auto* layout = new QVBoxLayout(&dialog);
    auto* tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels({tr("Property"), tr("Value")});
    tree->setColumnWidth(kMetadataNameColumnIndex, kMetadataNameColumnWidth);
    tree->setAlternatingRowColors(true);
    tree->setRootIsDecorated(true);
    populateMetadataTree(tree, metadata);
    layout->addWidget(tree);
    auto* close_btn = new QPushButton(tr("Close"), &dialog);
    close_btn->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(close_btn, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto* btn_layout = new QHBoxLayout();
    btn_layout->addStretch();
    btn_layout->addWidget(close_btn);
    layout->addLayout(btn_layout);
    dialog.exec();
}

void AdvancedSearchPanel::showMetadataDialog(const QString& file_path) {
    if (!currentSearchTarget().local_file_system) {
        showMetadataTreeDialog(tr("Metadata - %1").arg(file_path),
                               collectRemoteMetadata(file_path));
        return;
    }
    if (!QFileInfo::exists(file_path)) {
        return;
    }
    showMetadataTreeDialog(tr("Metadata \u2014 %1").arg(QFileInfo(file_path).fileName()),
                           collectLocalMetadata(file_path));
}

void AdvancedSearchPanel::showMetadataPreview(const QString& file_path,
                                              const QVector<SearchMatch>& matches) {
    m_current_preview_file = file_path;
    m_current_matches = matches;
    m_current_match_index = matches.isEmpty() ? -1 : 0;

    m_preview_header_label->setText(tr("Preview:"));

    // Build a clean metadata listing instead of reading the binary file
    QString preview_text;
    preview_text += tr("File: %1\n").arg(file_path);
    preview_text += tr("Metadata matches: %1\n").arg(matches.size());
    preview_text += QString(kPreviewSeparatorLength, QChar(kPreviewSeparatorChar)) + "\n\n";

    for (const auto& match : matches) {
        preview_text += match.line_content + "\n";
    }

    m_preview_edit->setPlainText(preview_text);

    // Highlight matches within the metadata lines
    highlightMetadataMatches();
    updateMatchCounter();

    const bool has_matches = !matches.isEmpty();
    m_prev_match_button->setEnabled(has_matches);
    m_next_match_button->setEnabled(has_matches);

    if (has_matches) {
        navigateToMatch(0);
    }
}

void AdvancedSearchPanel::highlightMatches() {
    Q_ASSERT(m_preview_edit);
    if (m_current_matches.isEmpty()) {
        return;
    }

    // Find lines with matches and highlight them in the preview
    QTextDocument* doc = m_preview_edit->document();
    QTextCursor cursor(doc);

    // Header lines offset: "File:", "Total matches:", separator, blank => +4 lines
    constexpr int kHeaderLines = 4;

    for (int i = 0; i < m_current_matches.size(); ++i) {
        const auto& match = m_current_matches[i];
        const int preview_line = match.line_number + kHeaderLines - 1;

        // Move to that line in the document
        const QTextBlock block = doc->findBlockByLineNumber(preview_line);
        if (!block.isValid()) {
            continue;
        }

        // Find the match text within the line
        // The line format is: ">>> NNNNN | content" or "    NNNNN | content"
        // The actual content starts after "| "
        const QString block_text = block.text();
        const int pipe_idx = static_cast<int>(block_text.indexOf('|'));
        if (pipe_idx < 0) {
            continue;
        }

        const int content_offset = pipe_idx + 2;  // After "| "
        const int highlight_start = content_offset + match.match_start;
        const int highlight_len = match.match_end - match.match_start;

        cursor.setPosition(block.position() + highlight_start);
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, highlight_len);

        QTextCharFormat fmt;
        if (i == m_current_match_index) {
            // Current match: orange highlight
            fmt.setBackground(QColor(QString::fromLatin1(ui::kColorSearchCurrentMatchHighlight)));
        } else {
            // Other matches: yellow highlight
            fmt.setBackground(QColor(QString::fromLatin1(ui::kColorSearchOtherMatchHighlight)));
        }
        cursor.setCharFormat(fmt);
    }
}

void AdvancedSearchPanel::highlightMetadataMatches() {
    Q_ASSERT(m_preview_edit);
    if (m_current_matches.isEmpty()) {
        return;
    }

    QTextDocument* doc = m_preview_edit->document();
    QTextCursor cursor(doc);

    // Header: "File:", "Metadata matches:", separator, blank => 4 lines
    constexpr int kHeaderLines = 4;

    for (int i = 0; i < m_current_matches.size(); ++i) {
        const auto& match = m_current_matches[i];
        // Each metadata match is on its own line after the header
        const int preview_line = kHeaderLines + i;

        const QTextBlock block = doc->findBlockByLineNumber(preview_line);
        if (!block.isValid()) {
            continue;
        }

        const int highlight_start = match.match_start;
        const int highlight_len = match.match_end - match.match_start;
        if (highlight_len <= 0) {
            continue;
        }

        const int abs_pos = block.position() + highlight_start;
        if (abs_pos + highlight_len > doc->characterCount()) {
            continue;
        }

        cursor.setPosition(abs_pos);
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, highlight_len);

        QTextCharFormat fmt;
        if (i == m_current_match_index) {
            fmt.setBackground(QColor(QString::fromLatin1(ui::kColorSearchCurrentMatchHighlight)));
        } else {
            fmt.setBackground(QColor(QString::fromLatin1(ui::kColorSearchOtherMatchHighlight)));
        }
        cursor.setCharFormat(fmt);
    }
}

void AdvancedSearchPanel::navigateToMatch(int match_index) {
    Q_ASSERT(m_preview_edit);
    if (match_index < 0 || match_index >= m_current_matches.size()) {
        return;
    }

    m_current_match_index = match_index;

    // Reset all formatting
    QTextCursor reset_cursor(m_preview_edit->document());
    reset_cursor.select(QTextCursor::Document);
    const QTextCharFormat default_fmt;
    reset_cursor.setCharFormat(default_fmt);

    // Detect metadata mode and apply appropriate highlighting
    const bool metadata_mode =
        !m_current_matches.isEmpty() &&
        m_current_matches[0].line_content.startsWith(QLatin1String("[Metadata]"));

    if (metadata_mode) {
        highlightMetadataMatches();
    } else {
        highlightMatches();
    }

    // Scroll to the current match line
    constexpr int kHeaderLines = 4;
    const auto& match = m_current_matches[match_index];
    int preview_line;
    if (metadata_mode) {
        preview_line = kHeaderLines + match_index;
    } else {
        preview_line = match.line_number + kHeaderLines - 1;
    }

    const QTextBlock block = m_preview_edit->document()->findBlockByLineNumber(preview_line);
    if (block.isValid()) {
        const QTextCursor scroll_cursor(block);
        m_preview_edit->setTextCursor(scroll_cursor);
        m_preview_edit->ensureCursorVisible();
    }

    updateMatchCounter();
}

void AdvancedSearchPanel::onPreviousMatch() {
    if (m_current_matches.isEmpty()) {
        return;
    }

    int new_index = m_current_match_index - 1;
    if (new_index < 0) {
        new_index = static_cast<int>(m_current_matches.size() - 1);  // Wrap around
    }
    navigateToMatch(new_index);
}

void AdvancedSearchPanel::onNextMatch() {
    if (m_current_matches.isEmpty()) {
        return;
    }

    int new_index = m_current_match_index + 1;
    if (new_index >= m_current_matches.size()) {
        new_index = 0;  // Wrap around
    }
    navigateToMatch(new_index);
}

void AdvancedSearchPanel::updateMatchCounter() {
    Q_ASSERT(m_match_counter_label);
    if (m_current_matches.isEmpty()) {
        m_match_counter_label->clear();
        return;
    }

    m_match_counter_label->setText(
        QString("%1/%2").arg(m_current_match_index + 1).arg(m_current_matches.size()));
}

// -- Utility -----------------------------------------------------------------

void AdvancedSearchPanel::setSearchRunning(bool running) {
    Q_ASSERT(m_search_button);
    Q_ASSERT(m_stop_button);
    m_search_button->setEnabled(!running);
    m_stop_button->setEnabled(running);
    m_search_combo->setEnabled(!running);
    m_case_sensitive_check->setEnabled(!running);
    m_whole_word_check->setEnabled(!running);
    m_use_regex_check->setEnabled(!running);
    m_image_metadata_check->setEnabled(!running);
    m_file_metadata_check->setEnabled(!running);
    m_archive_search_check->setEnabled(!running);
    m_binary_hex_check->setEnabled(!running);
    m_extensions_edit->setEnabled(!running);
    m_regex_patterns_button->setEnabled(!running);
    m_context_lines_combo->setEnabled(!running);
}

void AdvancedSearchPanel::logMessage(const QString& message) {
    Q_EMIT logOutput(message);
}

void AdvancedSearchPanel::updateRegexPatternsButton() {
    const int count = m_controller->patternLibrary()->activeCount();
    if (count > 0) {
        m_regex_patterns_button->setText(tr("Regex Patterns (%1) \u25BE").arg(count));
    } else {
        m_regex_patterns_button->setText(tr("Regex Patterns \u25BE"));
    }
}

}  // namespace sak
