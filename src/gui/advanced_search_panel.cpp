// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_panel.cpp
/// @brief Implements the three-panel Advanced Search UI

#include "sak/advanced_search_panel.h"

#include "sak/advanced_search_controller.h"
#include "sak/detachable_log_window.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/regex_pattern_library.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
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
#include <QSpinBox>
#include <QStorageInfo>
#include <QTextBlock>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include <windows.h>

#include <shellapi.h>
#undef emit
#undef signals
#undef slots

namespace sak {

namespace {

struct SearchBarRow1Ui {
    QComboBox* search_combo = nullptr;
    QPushButton* search_button = nullptr;
    QPushButton* stop_button = nullptr;
};

SearchBarRow1Ui buildSearchBarRow1(AdvancedSearchPanel* panel, QHBoxLayout* row) {
    Q_ASSERT(panel);
    Q_ASSERT(row);
    SearchBarRow1Ui ui;

    auto* searchLabel = new QLabel(QObject::tr("Search for:"), panel);
    row->addWidget(searchLabel);

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
                                   int defaultContextLines) {
    SearchBarRow2Ui ui;

    auto* contextLabel = new QLabel(QObject::tr("Context:"), panel);
    row->addWidget(contextLabel);

    ui.context_lines_combo = new QComboBox(panel);
    for (int i = 0; i <= 10; ++i) {
        ui.context_lines_combo->addItem(QString::number(i), i);
    }
    ui.context_lines_combo->setCurrentIndex(defaultContextLines);
    ui.context_lines_combo->setToolTip(QObject::tr("Lines of context before/after matches"));
    ui.context_lines_combo->setFixedWidth(60);
    setAccessible(ui.context_lines_combo, QObject::tr("Context lines"));
    row->addWidget(ui.context_lines_combo);

    ui.regex_patterns_button = new QPushButton(QObject::tr("Regex Patterns \u25BE"), panel);
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

    auto* extLabel = new QLabel(QObject::tr("Extensions:"), panel);
    row->addWidget(extLabel);

    ui.extensions_edit = new QLineEdit(panel);
    ui.extensions_edit->setPlaceholderText(QObject::tr(".py,.txt,.js,.cpp"));
    ui.extensions_edit->setToolTip(
        QObject::tr("Comma-separated file extensions to include (empty = all files)"));
    ui.extensions_edit->setMaximumWidth(200);
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
    QSpinBox* maxResultsSpin = nullptr;
    QSpinBox* previewSizeSpin = nullptr;
    QSpinBox* searchSizeSpin = nullptr;
    QSpinBox* contextLinesSpin = nullptr;
    QSpinBox* cacheSizeSpin = nullptr;
};

PreferencesDialogUi buildPreferencesDialog(QWidget* parent, const SearchPreferences& prefs) {
    PreferencesDialogUi ui;

    ui.dialog = new QDialog(parent);
    ui.dialog->setWindowTitle(QObject::tr("Search Preferences"));
    ui.dialog->setMinimumWidth(380);

    auto* layout = new QVBoxLayout(ui.dialog);

    auto* form = new QFormLayout();
    form->setSpacing(ui::kSpacingSmall);

    ui.maxResultsSpin = new QSpinBox(ui.dialog);
    ui.maxResultsSpin->setRange(0, 1'000'000);
    ui.maxResultsSpin->setSpecialValueText(QObject::tr("Unlimited"));
    ui.maxResultsSpin->setValue(prefs.max_results);
    ui.maxResultsSpin->setToolTip(QObject::tr("Maximum total matches (0 = unlimited)"));
    form->addRow(QObject::tr("Max results:"), ui.maxResultsSpin);

    ui.previewSizeSpin = new QSpinBox(ui.dialog);
    ui.previewSizeSpin->setRange(1, 500);
    ui.previewSizeSpin->setSuffix(QObject::tr(" MB"));
    ui.previewSizeSpin->setValue(prefs.max_preview_file_size_mb);
    ui.previewSizeSpin->setToolTip(QObject::tr("Maximum file size for preview pane"));
    form->addRow(QObject::tr("Max preview file size:"), ui.previewSizeSpin);

    ui.searchSizeSpin = new QSpinBox(ui.dialog);
    ui.searchSizeSpin->setRange(1, 1000);
    ui.searchSizeSpin->setSuffix(QObject::tr(" MB"));
    ui.searchSizeSpin->setValue(prefs.max_search_file_size_mb);
    ui.searchSizeSpin->setToolTip(QObject::tr("Maximum file size to search"));
    form->addRow(QObject::tr("Max search file size:"), ui.searchSizeSpin);

    ui.contextLinesSpin = new QSpinBox(ui.dialog);
    ui.contextLinesSpin->setRange(0, 10);
    ui.contextLinesSpin->setValue(prefs.context_lines);
    ui.contextLinesSpin->setToolTip(QObject::tr("Default context lines before/after matches"));
    form->addRow(QObject::tr("Default context lines:"), ui.contextLinesSpin);

    ui.cacheSizeSpin = new QSpinBox(ui.dialog);
    ui.cacheSizeSpin->setRange(1, 1000);
    ui.cacheSizeSpin->setValue(prefs.max_cache_size);
    ui.cacheSizeSpin->setToolTip(QObject::tr("Maximum LRU file cache entries"));
    form->addRow(QObject::tr("Cache size:"), ui.cacheSizeSpin);

    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         ui.dialog);
    layout->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, ui.dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, ui.dialog, &QDialog::reject);

    return ui;
}

SearchPreferences preferencesFromDialogUi(const PreferencesDialogUi& ui) {
    SearchPreferences newPrefs;
    newPrefs.max_results = ui.maxResultsSpin->value();
    newPrefs.max_preview_file_size_mb = ui.previewSizeSpin->value();
    newPrefs.max_search_file_size_mb = ui.searchSizeSpin->value();
    newPrefs.context_lines = ui.contextLinesSpin->value();
    newPrefs.max_cache_size = ui.cacheSizeSpin->value();
    return newPrefs;
}

struct FileSortEntry {
    QString path;
    QVector<SearchMatch> matches;
    qint64 fileSize = 0;
    QDateTime lastModified;
};

bool compareFileEntries(const FileSortEntry& a, const FileSortEntry& b, int sortMode) {
    switch (sortMode) {
    case 0:  // Path A-Z
        return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
    case 1:  // Path Z-A
        return a.path.compare(b.path, Qt::CaseInsensitive) > 0;
    case 2:  // Match Count High
        return a.matches.size() > b.matches.size();
    case 3:  // Match Count Low
        return a.matches.size() < b.matches.size();
    case 4:  // File Size Large
        return a.fileSize > b.fileSize;
    case 5:  // File Size Small
        return a.fileSize < b.fileSize;
    case 6:  // Date Modified Newest
        return a.lastModified > b.lastModified;
    case 7:  // Date Modified Oldest
        return a.lastModified < b.lastModified;
    default:
        return a.path < b.path;
    }
}

QVector<FileSortEntry> buildSortedFileEntries(const QMap<QString, QVector<SearchMatch>>& allResults,
                                              int sortMode) {
    QVector<FileSortEntry> sortedFiles;
    sortedFiles.reserve(allResults.size());

    const bool needsFileInfo = (sortMode >= 4);  // Size or Date sorts

    for (auto it = allResults.constBegin(); it != allResults.constEnd(); ++it) {
        FileSortEntry entry;
        entry.path = it.key();
        entry.matches = it.value();

        if (needsFileInfo) {
            const QFileInfo info(entry.path);
            entry.fileSize = info.size();
            entry.lastModified = info.lastModified();
        }

        sortedFiles.append(std::move(entry));
    }

    std::sort(sortedFiles.begin(),
              sortedFiles.end(),
              [sortMode](const FileSortEntry& a, const FileSortEntry& b) {
                  return compareFileEntries(a, b, sortMode);
              });

    return sortedFiles;
}

void populateSortedResultsTree(QTreeWidget* tree,
                               QWidget* iconSource,
                               const QVector<FileSortEntry>& sortedFiles) {
    tree->setUpdatesEnabled(false);
    for (const auto& entry : sortedFiles) {
        auto* fileItem = new QTreeWidgetItem(tree);
        fileItem->setText(0, QString("%1  (%2)").arg(entry.path).arg(entry.matches.size()));
        fileItem->setData(0, Qt::UserRole, entry.path);
        fileItem->setData(0, Qt::UserRole + 1, -1);  // Not a specific match
        fileItem->setIcon(0, iconSource->style()->standardIcon(QStyle::SP_FileIcon));

        for (int i = 0; i < entry.matches.size(); ++i) {
            const auto& match = entry.matches[i];
            auto* matchItem = new QTreeWidgetItem(fileItem);

            const QString truncated = match.line_content.left(80).trimmed();
            matchItem->setText(0, QString("Line %1: %2").arg(match.line_number).arg(truncated));
            matchItem->setData(0, Qt::UserRole, entry.path);
            matchItem->setData(0, Qt::UserRole + 1, i);  // Match index
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
        return QString("%1 GB").arg(static_cast<double>(bytes) / kGB, 0, 'f', 2);
    }
    if (bytes >= kMB) {
        return QString("%1 MB").arg(static_cast<double>(bytes) / kMB, 0, 'f', 2);
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
        const int colon = line.indexOf(QLatin1String(": "));
        if (colon > 0) {
            metadata.insert(line.left(colon).trimmed(), line.mid(colon + 2).trimmed());
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

constexpr MetadataField kFileFields[] = {{"FileName", "Name"},
                                         {"FileSize", "Size"},
                                         {"FileType", "Type"},
                                         {"Created", "Created"},
                                         {"LastModified", "Modified"}};

constexpr MetadataField kImageFields[] = {{"Width", "Width"},
                                          {"Height", "Height"},
                                          {"Dimensions", "Dimensions"},
                                          {"Format", "Format"},
                                          {"ColorDepth", "Color Depth"}};

constexpr MetadataField kCameraFields[] = {{"CameraMake", "Make"},
                                           {"CameraModel", "Model"},
                                           {"ExposureTime", "Exposure"},
                                           {"FNumber", "Aperture"},
                                           {"ISOSpeed", "ISO"},
                                           {"FocalLength", "Focal Length"},
                                           {"FocalLengthIn35mm", "Focal Length (35mm)"},
                                           {"Flash", "Flash"},
                                           {"MeteringMode", "Metering"},
                                           {"WhiteBalance", "White Balance"},
                                           {"DateTimeOriginal", "Date Taken"},
                                           {"DateTimeDigitized", "Date Digitized"},
                                           {"Software", "Software"},
                                           {"Artist", "Artist"},
                                           {"Copyright", "Copyright"},
                                           {"Orientation", "Orientation"},
                                           {"ImageDescription", "Description"},
                                           {"LensMake", "Lens Make"},
                                           {"LensModel", "Lens Model"}};

constexpr MetadataField kGpsFields[] = {{"GPSLatitude", "Latitude"},
                                        {"GPSLatitudeRef", "Lat Ref"},
                                        {"GPSLongitude", "Longitude"},
                                        {"GPSLongitudeRef", "Lon Ref"},
                                        {"GPSAltitude", "Altitude"},
                                        {"GPSAltitudeRef", "Alt Ref"},
                                        {"GPSDateStamp", "Date Stamp"},
                                        {"GPSTimeStamp", "Time Stamp"}};

constexpr MetadataField kDocFields[] = {{"Title", "Title"},
                                        {"Author", "Author"},
                                        {"Subject", "Subject"},
                                        {"Keywords", "Keywords"},
                                        {"Creator", "Creator"},
                                        {"Producer", "Producer"},
                                        {"Company", "Company"},
                                        {"Version", "Version"},
                                        {"CreationDate", "Created"},
                                        {"ModDate", "Modified"}};

constexpr MetadataField kAudioFields[] = {{"Title", "Title"},
                                          {"Artist", "Artist"},
                                          {"Album", "Album"},
                                          {"Year", "Year"},
                                          {"Genre", "Genre"},
                                          {"Track", "Track"},
                                          {"Comment", "Comment"},
                                          {"AlbumArtist", "Album Artist"},
                                          {"Composer", "Composer"},
                                          {"Publisher", "Publisher"},
                                          {"Copyright", "Copyright"}};

constexpr MetadataCategory kMetadataCategories[] = {
    {"File Information", kFileFields, std::size(kFileFields)},
    {"Image Properties", kImageFields, std::size(kImageFields)},
    {"Camera / EXIF", kCameraFields, std::size(kCameraFields)},
    {"GPS Location", kGpsFields, std::size(kGpsFields)},
    {"Document Properties", kDocFields, std::size(kDocFields)},
    {"Audio Tags", kAudioFields, std::size(kAudioFields)}};

void populateMetadataTree(QTreeWidget* tree, const QMap<QString, QString>& metadata) {
    const QString dash = QStringLiteral("\u2014");
    for (const auto& category : kMetadataCategories) {
        auto* catItem = new QTreeWidgetItem(tree);
        catItem->setText(0, QString::fromUtf8(category.name));
        catItem->setFlags(catItem->flags() & ~Qt::ItemIsSelectable);
        QFont bold = catItem->font(0);
        bold.setBold(true);
        catItem->setFont(0, bold);

        int populated = 0;
        for (int i = 0; i < category.count; ++i) {
            const auto& field = category.fields[i];
            auto* fieldItem = new QTreeWidgetItem(catItem);
            fieldItem->setText(0, QString::fromUtf8(field.display));
            const QString value = metadata.value(QString::fromUtf8(field.key), dash);
            fieldItem->setText(1, value);
            if (value != dash) {
                ++populated;
            }
        }
        catItem->setText(1, QString("(%1/%2)").arg(populated).arg(category.count));
        catItem->setExpanded(populated > 0);
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
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    auto* contentWidget = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);
    mainLayout->setSpacing(ui::kSpacingDefault);

    rootLayout->addWidget(contentWidget);

    createSearchBar(mainLayout);
    createThreePanelSplitter(mainLayout);
    createStatusBar(mainLayout);
    createRegexPatternMenu();
}

void AdvancedSearchPanel::createSearchBar(QVBoxLayout* layout) {
    Q_ASSERT(m_controller);
    auto* searchGroup = new QGroupBox(tr("Search"), this);
    auto* searchLayout = new QVBoxLayout(searchGroup);
    searchLayout->setSpacing(ui::kSpacingSmall);

    // -- Row 1: Search input + buttons --
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(ui::kSpacingSmall);

    {
        const SearchBarRow1Ui ui = buildSearchBarRow1(this, row1);
        m_search_combo = ui.search_combo;
        m_search_button = ui.search_button;
        m_stop_button = ui.stop_button;
    }

    searchLayout->addLayout(row1);

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
    searchLayout->addLayout(row2);

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

    searchLayout->addLayout(row3);

    layout->addWidget(searchGroup);

    // Connect search actions
    connect(m_search_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onSearchClicked);
    connect(m_stop_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onStopClicked);
    connect(m_regex_patterns_button,
            &QPushButton::clicked,
            this,
            &AdvancedSearchPanel::onRegexPatternsClicked);

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
    m_splitter->setSizes({250, 350, 600});

    layout->addWidget(m_splitter, 1);  // stretch = 1 to fill space
}

void AdvancedSearchPanel::createFileExplorer() {
    auto* container = new QWidget(this);
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(ui::kSpacingTight);

    auto* headerLabel = new QLabel(tr("File Explorer:"), container);
    QFont headerFont = headerLabel->font();
    headerFont.setBold(true);
    headerLabel->setFont(headerFont);
    containerLayout->addWidget(headerLabel);

    m_file_explorer = new QTreeWidget(container);
    m_file_explorer->setHeaderHidden(true);
    m_file_explorer->setContextMenuPolicy(Qt::CustomContextMenu);
    m_file_explorer->setToolTip(tr("Select a directory to search"));
    setAccessible(m_file_explorer,
                  tr("File explorer tree"),
                  tr("Navigate and select directories to search"));
    containerLayout->addWidget(m_file_explorer);

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
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(ui::kSpacingTight);

    // Header with sort combo
    auto* headerRow = new QHBoxLayout();
    auto* headerLabel = new QLabel(tr("Search Results:"), container);
    QFont headerFont = headerLabel->font();
    headerFont.setBold(true);
    headerLabel->setFont(headerFont);
    headerRow->addWidget(headerLabel);

    m_results_count_label = new QLabel(container);
    m_results_count_label->setStyleSheet(QString("color: %1;").arg(ui::kColorTextMuted));
    headerRow->addWidget(m_results_count_label);

    headerRow->addStretch();

    auto* sortLabel = new QLabel(tr("Sort:"), container);
    headerRow->addWidget(sortLabel);

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
    headerRow->addWidget(m_sort_combo);

    containerLayout->addLayout(headerRow);

    m_results_tree = new QTreeWidget(container);
    m_results_tree->setHeaderHidden(true);
    m_results_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_results_tree->setRootIsDecorated(true);
    m_results_tree->setToolTip(tr("Click a result to preview, double-click to open"));
    setAccessible(m_results_tree,
                  tr("Search results tree"),
                  tr("Click results to preview, double-click to open in editor"));
    containerLayout->addWidget(m_results_tree);

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
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);
    containerLayout->setSpacing(ui::kSpacingTight);

    // Header with match navigation
    auto* headerRow = new QHBoxLayout();

    m_preview_header_label = new QLabel(tr("Preview:"), container);
    QFont headerFont = m_preview_header_label->font();
    headerFont.setBold(true);
    m_preview_header_label->setFont(headerFont);
    headerRow->addWidget(m_preview_header_label);

    headerRow->addStretch();

    m_prev_match_button = new QPushButton(QStringLiteral("\u25C4"), container);
    m_prev_match_button->setFixedSize(28, 28);
    m_prev_match_button->setToolTip(tr("Previous match"));
    m_prev_match_button->setEnabled(false);
    setAccessible(m_prev_match_button, tr("Previous match"));
    headerRow->addWidget(m_prev_match_button);

    m_match_counter_label = new QLabel(container);
    m_match_counter_label->setMinimumWidth(60);
    m_match_counter_label->setAlignment(Qt::AlignCenter);
    headerRow->addWidget(m_match_counter_label);

    m_next_match_button = new QPushButton(QStringLiteral("\u25BA"), container);  // >
    m_next_match_button->setFixedSize(28, 28);
    m_next_match_button->setToolTip(tr("Next match"));
    m_next_match_button->setEnabled(false);
    setAccessible(m_next_match_button, tr("Next match"));
    headerRow->addWidget(m_next_match_button);

    containerLayout->addLayout(headerRow);

    m_preview_edit = new QTextEdit(container);
    m_preview_edit->setReadOnly(true);
    m_preview_edit->setFont(QFont("Consolas", ui::kFontSizeBody));
    m_preview_edit->setToolTip(tr("File preview with highlighted matches"));
    m_preview_edit->setStyleSheet(
        QString("QTextEdit { background: %1; color: %2; border: 1px solid %3; }")
            .arg(ui::kColorBgWhite)
            .arg(ui::kColorTextBody)
            .arg(ui::kColorBorderDefault));
    setAccessible(m_preview_edit,
                  tr("File preview pane"),
                  tr("Shows file content with highlighted search matches"));
    containerLayout->addWidget(m_preview_edit);

    // Connect navigation buttons
    connect(
        m_prev_match_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onPreviousMatch);
    connect(m_next_match_button, &QPushButton::clicked, this, &AdvancedSearchPanel::onNextMatch);
}

void AdvancedSearchPanel::createStatusBar(QVBoxLayout* layout) {
    Q_ASSERT(layout);
    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(0, 4, 0, 0);

    // Log toggle on the left -- matches all other panels
    m_log_toggle = new LogToggleSwitch(tr("Log"), this);
    statusRow->addWidget(m_log_toggle);

    // Settings button next to log toggle
    m_preferences_button = new QPushButton(tr("Settings"), this);
    m_preferences_button->setToolTip(tr("Search settings (max results, file sizes, etc.)"));
    m_preferences_button->setAccessibleName(tr("Search settings"));
    statusRow->addWidget(m_preferences_button);
    connect(m_preferences_button,
            &QPushButton::clicked,
            this,
            &AdvancedSearchPanel::onPreferencesClicked);

    statusRow->addStretch();

    layout->addLayout(statusRow);
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
        auto* customHeader = m_regex_menu->addAction(tr("Custom Patterns:"));
        customHeader->setEnabled(false);

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
    auto* clearAction = m_regex_menu->addAction(tr("Clear All"));
    connect(clearAction, &QAction::triggered, this, [this]() {
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

void AdvancedSearchPanel::populateFileExplorerRoot() {
    Q_ASSERT(m_file_explorer);
    m_file_explorer->clear();

    // Add home directory
    const QString homePath = QDir::homePath();
    auto* homeItem = new QTreeWidgetItem(m_file_explorer);
    homeItem->setText(0, tr("Home (%1)").arg(QFileInfo(homePath).fileName()));
    homeItem->setData(0, Qt::UserRole, homePath);
    homeItem->setIcon(0, style()->standardIcon(QStyle::SP_DirHomeIcon));
    addPlaceholderChild(homeItem);

    // Add drive letters (Windows)
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const auto& vol : volumes) {
        if (!vol.isValid() || !vol.isReady()) {
            continue;
        }

        const QString rootPath = vol.rootPath();
        QString label = vol.displayName();
        if (label.isEmpty()) {
            label = rootPath;
        }

        auto* driveItem = new QTreeWidgetItem(m_file_explorer);
        driveItem->setText(0, QString("%1 (%2)").arg(label, rootPath.left(2)));
        driveItem->setData(0, Qt::UserRole, rootPath);
        driveItem->setIcon(0, style()->standardIcon(QStyle::SP_DriveHDIcon));
        addPlaceholderChild(driveItem);
    }

    // Expand home directory by default
    m_file_explorer->expandItem(homeItem);
}

void AdvancedSearchPanel::populateDirectoryChildren(QTreeWidgetItem* parentItem,
                                                    const QString& dirPath) {
    const QDir dir(dirPath);
    if (!dir.exists()) {
        return;
    }

    // Get directories first, then files
    const auto entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Files,
                                           QDir::DirsFirst | QDir::IgnoreCase | QDir::Name);

    for (const auto& entry : entries) {
        auto* item = new QTreeWidgetItem(parentItem);
        item->setText(0, entry.fileName());
        item->setData(0, Qt::UserRole, entry.absoluteFilePath());

        if (entry.isDir()) {
            item->setIcon(0, style()->standardIcon(QStyle::SP_DirIcon));
            addPlaceholderChild(item);  // Enable expansion
        } else {
            item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
        }
    }
}

void AdvancedSearchPanel::addPlaceholderChild(QTreeWidgetItem* parentItem) {
    auto* placeholder = new QTreeWidgetItem(parentItem);
    placeholder->setText(0, kPlaceholderText);
    placeholder->setFlags(Qt::NoItemFlags);
}

void AdvancedSearchPanel::removePlaceholderChildren(QTreeWidgetItem* parentItem) {
    for (int i = parentItem->childCount() - 1; i >= 0; --i) {
        auto* child = parentItem->child(i);
        if (child->text(0) == kPlaceholderText) {
            delete parentItem->takeChild(i);
        }
    }
}

// -- Search Controls ---------------------------------------------------------

void AdvancedSearchPanel::onSearchClicked() {
    Q_ASSERT(m_controller);
    const SearchConfig config = buildSearchConfig();

    if (config.pattern.isEmpty() && config.root_path.isEmpty()) {
        sak::logWarning("Search: no directory selected and no pattern entered");
        Q_EMIT statusMessage(tr("Please select a directory and enter a search pattern"), 3000);
        return;
    }

    if (config.pattern.isEmpty()) {
        sak::logWarning("Search: empty search pattern");
        Q_EMIT statusMessage(tr("Please enter a search pattern"), 3000);
        return;
    }

    if (config.root_path.isEmpty()) {
        sak::logWarning("Search: no directory selected");
        Q_EMIT statusMessage(tr("Please select a directory to search"), 3000);
        return;
    }

    clearResults();
    m_controller->startSearch(config);
}

void AdvancedSearchPanel::onStopClicked() {
    m_controller->cancelSearch();
}

void AdvancedSearchPanel::onRegexPatternsClicked() {
    if (m_regex_menu) {
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
        const SearchPreferences newPrefs = preferencesFromDialogUi(ui);
        m_controller->setPreferences(newPrefs);
        m_context_lines_combo->setCurrentIndex(newPrefs.context_lines);
        logMessage(tr("Preferences updated"));
    }

    ui.dialog->deleteLater();
}

SearchConfig AdvancedSearchPanel::buildSearchConfig() const {
    Q_ASSERT(m_file_explorer);
    Q_ASSERT(m_controller);
    SearchConfig config;

    // Get selected directory from file explorer
    auto* selectedItem = m_file_explorer->currentItem();
    if (selectedItem) {
        config.root_path = selectedItem->data(0, Qt::UserRole).toString();
    }

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
    const QString extText = m_extensions_edit->text().trimmed();
    if (!extText.isEmpty()) {
        const auto parts = extText.split(',', Qt::SkipEmptyParts);
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
    const QString& filePath, const QVector<SearchMatch>& fileMatches) {
    for (int i = 0; i < m_results_tree->topLevelItemCount(); ++i) {
        auto* item = m_results_tree->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == filePath) {
            while (item->childCount() > 0) {
                delete item->takeChild(0);
            }
            item->setText(0, QString("%1  (%2)").arg(filePath).arg(fileMatches.size()));
            return item;
        }
    }
    auto* file_item = new QTreeWidgetItem(m_results_tree);
    file_item->setText(0, QString("%1  (%2)").arg(filePath).arg(fileMatches.size()));
    file_item->setData(0, Qt::UserRole, filePath);
    file_item->setData(0, Qt::UserRole + 1, -1);
    file_item->setIcon(0, style()->standardIcon(QStyle::SP_FileIcon));
    return file_item;
}

void AdvancedSearchPanel::onResultsReceived(QVector<sak::SearchMatch> matches) {
    if (matches.isEmpty() || !m_results_tree) {
        return;
    }

    for (const auto& match : matches) {
        m_all_results[match.file_path].append(match);
    }

    QSet<QString> affectedFiles;
    for (const auto& match : matches) {
        affectedFiles.insert(match.file_path);
    }

    m_results_tree->setUpdatesEnabled(false);
    for (const auto& filePath : affectedFiles) {
        const auto& fileMatches = m_all_results[filePath];
        auto* fileItem = findOrCreateFileItem(filePath, fileMatches);

        for (int i = 0; i < fileMatches.size(); ++i) {
            const auto& m = fileMatches[i];
            auto* matchItem = new QTreeWidgetItem(fileItem);
            const QString truncated = m.line_content.left(80).trimmed();
            matchItem->setText(0, QString("Line %1: %2").arg(m.line_number).arg(truncated));
            matchItem->setData(0, Qt::UserRole, filePath);
            matchItem->setData(0, Qt::UserRole + 1, i);
        }
    }
    m_results_tree->setUpdatesEnabled(true);
}

void AdvancedSearchPanel::onSearchFinished(int totalMatches, int totalFiles) {
    Q_ASSERT(m_results_count_label);
    setSearchRunning(false);
    Q_EMIT statusMessage(tr("Found %1 matches in %2 files").arg(totalMatches).arg(totalFiles),
                         sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(totalMatches, totalMatches);
    m_results_count_label->setText(tr("(%1 matches, %2 files)").arg(totalMatches).arg(totalFiles));
    logMessage(tr("Search complete: %1 matches in %2 files").arg(totalMatches).arg(totalFiles));
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
    if (!item) {
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();
    if (!path.isEmpty()) {
        Q_EMIT statusMessage(tr("Selected: %1").arg(path), 0);
    }
}

void AdvancedSearchPanel::onFileExplorerItemExpanded(QTreeWidgetItem* item) {
    if (!item) {
        return;
    }

    // Check if placeholder child exists (lazy loading)
    if (item->childCount() == 1 && item->child(0)->text(0) == kPlaceholderText) {
        removePlaceholderChildren(item);

        const QString dirPath = item->data(0, Qt::UserRole).toString();
        populateDirectoryChildren(item, dirPath);
    }
}

void AdvancedSearchPanel::onFileExplorerContextMenu(const QPoint& pos) {
    Q_ASSERT(m_file_explorer);
    auto* item = m_file_explorer->itemAt(pos);
    if (!item) {
        return;
    }

    const QString path = item->data(0, Qt::UserRole).toString();

    QMenu menu(this);

    auto* openDirAction = menu.addAction(tr("Open in Explorer"));
    connect(openDirAction, &QAction::triggered, this, [path]() {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()));
    });

    auto* copyPathAction = menu.addAction(tr("Copy Path"));
    connect(copyPathAction, &QAction::triggered, this, [path]() {
        QApplication::clipboard()->setText(path);
    });

    menu.exec(m_file_explorer->viewport()->mapToGlobal(pos));
}

// -- Results Tree Handlers ---------------------------------------------------

void AdvancedSearchPanel::onResultItemClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) {
        return;
    }

    const QString filePath = item->data(0, Qt::UserRole).toString();
    if (filePath.isEmpty()) {
        return;
    }

    const auto it = m_all_results.constFind(filePath);
    if (it != m_all_results.constEnd()) {
        showFilePreview(filePath, it.value());

        // If clicking a specific match line, navigate to it
        const int matchIdx = item->data(0, Qt::UserRole + 1).toInt();
        if (matchIdx >= 0 && matchIdx < it.value().size()) {
            navigateToMatch(matchIdx);
        }
    }
}

void AdvancedSearchPanel::onResultItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) {
        return;
    }

    const QString filePath = item->data(0, Qt::UserRole).toString();
    if (!filePath.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    }
}

void AdvancedSearchPanel::onResultContextMenu(const QPoint& pos) {
    Q_ASSERT(m_results_tree);
    auto* item = m_results_tree->itemAt(pos);
    if (!item) {
        return;
    }

    const QString filePath = item->data(0, Qt::UserRole).toString();

    QMenu menu(this);

    auto* openAction = menu.addAction(tr("Open File"));
    connect(openAction, &QAction::triggered, this, [filePath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    });

    auto* openDirAction = menu.addAction(tr("Open Directory"));
    connect(openDirAction, &QAction::triggered, this, [filePath]() {
        const QString native = QDir::toNativeSeparators(filePath);
        QProcess::startDetached(QStringLiteral("explorer.exe"),
                                {QStringLiteral("/select,") + native});
    });

    menu.addSeparator();

    auto* viewMetaAction = menu.addAction(tr("View Metadata"));
    connect(viewMetaAction, &QAction::triggered, this, [this, filePath]() {
        showMetadataDialog(filePath);
    });

    auto* viewPropsAction = menu.addAction(tr("View Properties"));
    connect(viewPropsAction, &QAction::triggered, this, [filePath]() {
        const QString native = QDir::toNativeSeparators(filePath);
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(SHELLEXECUTEINFOW);
        sei.fMask = SEE_MASK_INVOKEIDLIST;
        sei.lpVerb = L"properties";
        sei.lpFile = reinterpret_cast<LPCWSTR>(native.utf16());
        sei.nShow = SW_SHOW;
        ShellExecuteExW(&sei);
    });

    menu.addSeparator();

    auto* copyPathAction = menu.addAction(tr("Copy Path"));
    connect(copyPathAction, &QAction::triggered, this, [filePath]() {
        QApplication::clipboard()->setText(filePath);
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

    const int sortMode = m_sort_combo->currentIndex();
    const QVector<FileSortEntry> sortedFiles = buildSortedFileEntries(m_all_results, sortMode);
    populateSortedResultsTree(m_results_tree, this, sortedFiles);
}

// -- Preview -----------------------------------------------------------------

void AdvancedSearchPanel::showFilePreview(const QString& filePath,
                                          const QVector<SearchMatch>& matches) {
    m_current_preview_file = filePath;
    m_current_matches = matches;
    m_current_match_index = matches.isEmpty() ? -1 : 0;

    m_preview_header_label->setText(tr("Preview:"));

    // Detect metadata-only results (e.g., image EXIF data)
    const bool all_metadata = !matches.isEmpty() &&
                              std::all_of(matches.begin(), matches.end(), [](const SearchMatch& m) {
                                  return m.line_content.startsWith(QLatin1String("[Metadata]"));
                              });

    if (all_metadata) {
        showMetadataPreview(filePath, matches);
        return;
    }

    // Load the file content for text preview
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_preview_edit->setPlainText(tr("Unable to open file: %1").arg(filePath));
        return;
    }

    // Check file size limit
    const auto prefs = m_controller->preferences();
    const qint64 maxSize = static_cast<qint64>(prefs.max_preview_file_size_mb) * 1024 * 1024;
    if (file.size() > maxSize) {
        m_preview_edit->setPlainText(tr("File too large for preview (%1 MB, limit: %2 MB)")
                                         .arg(file.size() / (1024.0 * 1024.0), 0, 'f', 1)
                                         .arg(prefs.max_preview_file_size_mb));
        return;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    // Build line numbers with matching line indicators
    QSet<int> matchLines;
    for (const auto& match : matches) {
        matchLines.insert(match.line_number);
    }

    QString previewText;
    previewText += tr("File: %1\n").arg(filePath);
    previewText += tr("Total matches: %1\n").arg(matches.size());
    previewText += QString(50, QChar(0x2550)) + "\n\n";  // = separator

    int lineNum = 0;
    while (!stream.atEnd()) {
        ++lineNum;
        const QString line = stream.readLine();

        const QString prefix = matchLines.contains(lineNum) ? ">>> " : "    ";
        previewText += QString("%1%2 | %3\n").arg(prefix).arg(lineNum, 5).arg(line);
    }

    file.close();

    m_preview_edit->setPlainText(previewText);

    // Apply highlighting
    highlightMatches();
    updateMatchCounter();

    // Enable navigation buttons
    const bool hasMatches = !matches.isEmpty();
    m_prev_match_button->setEnabled(hasMatches);
    m_next_match_button->setEnabled(hasMatches);

    // Navigate to first match
    if (hasMatches) {
        navigateToMatch(0);
    }
}

void AdvancedSearchPanel::showMetadataDialog(const QString& filePath) {
    const QFileInfo fi(filePath);
    if (!fi.exists()) {
        return;
    }

    // Collect metadata from QFileInfo
    QMap<QString, QString> metadata;
    metadata[QStringLiteral("FileName")] = fi.fileName();
    metadata[QStringLiteral("FileSize")] = formatMetadataSize(fi.size());
    metadata[QStringLiteral("FileType")] = fi.suffix().toUpper();
    metadata[QStringLiteral("Created")] = fi.birthTime().toString(Qt::ISODate);
    metadata[QStringLiteral("LastModified")] = fi.lastModified().toString(Qt::ISODate);

    // Image properties from QImageReader
    QImageReader reader(filePath);
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

    // Merge metadata from search results (if available)
    if (m_all_results.contains(filePath)) {
        const auto parsed = parseMetadataFromMatches(m_all_results.value(filePath));
        for (auto it = parsed.cbegin(); it != parsed.cend(); ++it) {
            if (!metadata.contains(it.key())) {
                metadata.insert(it.key(), it.value());
            }
        }
    }

    // Build dialog with categorized tree
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Metadata \u2014 %1").arg(fi.fileName()));
    dialog.resize(560, 480);

    auto* layout = new QVBoxLayout(&dialog);

    auto* tree = new QTreeWidget(&dialog);
    tree->setHeaderLabels({tr("Property"), tr("Value")});
    tree->setColumnWidth(0, 200);
    tree->setAlternatingRowColors(true);
    tree->setRootIsDecorated(true);
    populateMetadataTree(tree, metadata);
    layout->addWidget(tree);

    auto* close_btn = new QPushButton(tr("Close"), &dialog);
    connect(close_btn, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto* btn_layout = new QHBoxLayout();
    btn_layout->addStretch();
    btn_layout->addWidget(close_btn);
    layout->addLayout(btn_layout);

    dialog.exec();
}

void AdvancedSearchPanel::showMetadataPreview(const QString& filePath,
                                              const QVector<SearchMatch>& matches) {
    m_current_preview_file = filePath;
    m_current_matches = matches;
    m_current_match_index = matches.isEmpty() ? -1 : 0;

    m_preview_header_label->setText(tr("Preview:"));

    // Build a clean metadata listing instead of reading the binary file
    QString previewText;
    previewText += tr("File: %1\n").arg(filePath);
    previewText += tr("Metadata matches: %1\n").arg(matches.size());
    previewText += QString(50, QChar(0x2550)) + "\n\n";

    for (const auto& match : matches) {
        previewText += match.line_content + "\n";
    }

    m_preview_edit->setPlainText(previewText);

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
        const int previewLine = match.line_number + kHeaderLines - 1;

        // Move to that line in the document
        QTextBlock block = doc->findBlockByLineNumber(previewLine);
        if (!block.isValid()) {
            continue;
        }

        // Find the match text within the line
        // The line format is: ">>> NNNNN | content" or "    NNNNN | content"
        // The actual content starts after "| "
        const QString blockText = block.text();
        const int pipeIdx = blockText.indexOf('|');
        if (pipeIdx < 0) {
            continue;
        }

        const int contentOffset = pipeIdx + 2;  // After "| "
        const int highlightStart = contentOffset + match.match_start;
        const int highlightLen = match.match_end - match.match_start;

        cursor.setPosition(block.position() + highlightStart);
        cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, highlightLen);

        QTextCharFormat fmt;
        if (i == m_current_match_index) {
            // Current match: orange highlight
            fmt.setBackground(QColor(255, 165, 0));  // Orange
        } else {
            // Other matches: yellow highlight
            fmt.setBackground(QColor(255, 255, 0));  // Yellow
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

        QTextBlock block = doc->findBlockByLineNumber(preview_line);
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
            fmt.setBackground(QColor(255, 165, 0));
        } else {
            fmt.setBackground(QColor(255, 255, 0));
        }
        cursor.setCharFormat(fmt);
    }
}

void AdvancedSearchPanel::navigateToMatch(int matchIndex) {
    Q_ASSERT(m_preview_edit);
    if (matchIndex < 0 || matchIndex >= m_current_matches.size()) {
        return;
    }

    m_current_match_index = matchIndex;

    // Reset all formatting
    QTextCursor resetCursor(m_preview_edit->document());
    resetCursor.select(QTextCursor::Document);
    QTextCharFormat defaultFmt;
    resetCursor.setCharFormat(defaultFmt);

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
    const auto& match = m_current_matches[matchIndex];
    int preview_line;
    if (metadata_mode) {
        preview_line = kHeaderLines + matchIndex;
    } else {
        preview_line = match.line_number + kHeaderLines - 1;
    }

    QTextBlock block = m_preview_edit->document()->findBlockByLineNumber(preview_line);
    if (block.isValid()) {
        QTextCursor scrollCursor(block);
        m_preview_edit->setTextCursor(scrollCursor);
        m_preview_edit->ensureCursorVisible();
    }

    updateMatchCounter();
}

void AdvancedSearchPanel::onPreviousMatch() {
    if (m_current_matches.isEmpty()) {
        return;
    }

    int newIndex = m_current_match_index - 1;
    if (newIndex < 0) {
        newIndex = m_current_matches.size() - 1;  // Wrap around
    }
    navigateToMatch(newIndex);
}

void AdvancedSearchPanel::onNextMatch() {
    if (m_current_matches.isEmpty()) {
        return;
    }

    int newIndex = m_current_match_index + 1;
    if (newIndex >= m_current_matches.size()) {
        newIndex = 0;  // Wrap around
    }
    navigateToMatch(newIndex);
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
