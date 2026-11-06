// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_theme_widget_states.cpp
/// @brief What the theme draws for the states a screenshot review found wrong: a checked box with
/// no tick, white text on a pale selection, disabled inputs that looked enabled, a black focus
/// frame in item views, a selected row in three shades and in pieces, spin boxes taller than
/// combos, faint placeholder text, page-grey patches in white dialogs, a link button painted as a
/// primary button, a submenu arrow touching its label, File Explorer buttons stuck "checked", and
/// colour tokens under WCAG AA. Each is drawn offscreen with the real theme and read back by pixel,
/// or computed from the tokens the theme uses.

#include "sak/advanced_search_panel.h"
#include "sak/color_constants.h"
#include "sak/file_explorer_style_constants.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/windows11_theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSlider>
#include <QSpinBox>
#include <QStatusBar>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextDocument>
#include <QTreeWidget>
#include <QtTest/QtTest>
#include <QVBoxLayout>

#include <cmath>

using sak::ui::AppThemeMode;

namespace {

constexpr double kWcagAaNormalText = 4.5;
constexpr int kWhiteLightnessFloor = 230;
constexpr int kMinTickPixels = 6;
constexpr int kMinFilledBoxPixels = 40;
constexpr int kDisabledLightnessGap = 40;
constexpr int kHeightTolerancePx = 2;
constexpr int kSubmenuArrowGapPx = 10;
constexpr int kTextDarknessThreshold = 160;

double channel(int value) {
    const double c = value / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor& color) {
    return (0.2126 * channel(color.red())) + (0.7152 * channel(color.green())) +
           (0.0722 * channel(color.blue()));
}

double contrast(const QColor& a, const QColor& b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/// "rgba(r, g, b, a)" or "#rrggbb" drawn over @p backdrop.
QColor over(const QString& css, const QColor& backdrop) {
    static const QRegularExpression kRgba(
        QStringLiteral(R"(rgba\((\d+),\s*(\d+),\s*(\d+),\s*([0-9.]+)\))"));
    const auto match = kRgba.match(css);
    if (!match.hasMatch()) {
        return QColor(css);
    }
    const double alpha = match.captured(4).toDouble();
    const auto mix = [alpha](int top, int base) {
        return qRound((alpha * top) + ((1.0 - alpha) * base));
    };
    return QColor(mix(match.captured(1).toInt(), backdrop.red()),
                  mix(match.captured(2).toInt(), backdrop.green()),
                  mix(match.captured(3).toInt(), backdrop.blue()));
}

QColor token(const char* value) {
    return QColor(QString::fromLatin1(value));
}

/// Shows @p window offscreen and returns a grab of @p widget inside it.
QImage shot(QWidget& window, QWidget* widget) {
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
        return {};
    }
    QCoreApplication::processEvents();
    return widget->grab().toImage();
}

int countPixels(const QImage& image, const QRect& area, const std::function<bool(QColor)>& test) {
    int count = 0;
    const QRect bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            if (test(QColor(image.pixel(x, y)))) {
                ++count;
            }
        }
    }
    return count;
}

QColor darkestPixel(const QImage& image, const QRect& area) {
    QColor darkest(Qt::white);
    const QRect bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor pixel(image.pixel(x, y));
            if (pixel.lightness() < darkest.lightness()) {
                darkest = pixel;
            }
        }
    }
    return darkest;
}

QColor lightestPixel(const QImage& image, const QRect& area) {
    QColor lightest(Qt::black);
    const QRect bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const QColor pixel(image.pixel(x, y));
            if (pixel.lightness() > lightest.lightness()) {
                lightest = pixel;
            }
        }
    }
    return lightest;
}

bool isPrimaryFill(const QColor& color) {
    // The checked box's fill (kColorPrimary): clearly blue, neither white nor grey.
    return color.blue() > 200 && color.red() < 120 && color.green() < 170;
}

/// The block of the app style sheet that starts with @p selectors.
QString sheetBlock(const QString& selectors) {
    const QString sheet = qApp->styleSheet();
    const qsizetype start = sheet.indexOf(selectors);
    if (start < 0) {
        return {};
    }
    const qsizetype end = sheet.indexOf(QLatin1Char('}'), start);
    return sheet.mid(start, end - start);
}

QString propertyIn(const QString& block, const QString& name) {
    const QRegularExpression pattern(QStringLiteral(R"((?<![-\w])%1\s*:\s*([^;]+);)").arg(name));
    return pattern.match(block).captured(1).trimmed();
}

}  // namespace

class ThemeWidgetStatesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanupTestCase();
    void checkedCheckBoxDrawsATick_data();
    void checkedCheckBoxDrawsATick();
    void checkedRadioButtonDrawsADot();
    void itemViewCheckBoxIsTheSameFilledBox();
    void selectedInputTextIsReadable_data();
    void selectedInputTextIsReadable();
    void disabledInputsLookDisabled_data();
    void disabledInputsLookDisabled();
    void itemViewsDrawNoBlackFocusFrame();
    void selectedRowIsOneShadeInEveryView();
    void selectedTableRowHasNoGapsAtCellCorners();
    void spinBoxesAndDateEditsAreAsTallAsCombos();
    void placeholderTextReachesAa();
    void plainContainersTakeTheirParentsSurface();
    void groupBoxesCardsAndTabPanesShareOneBorder();
    void statusBarLinkButtonsHaveNoButtonFill();
    void submenuArrowClearsTheLongestLabel();
    void explorerChromeButtonsShowTheirOwnState();
    void textTokensReachAaOnTheLightPage();
    void darkStatusTextReachesAaOnDarkSurfaces();
    void buttonLabelsReachAaOnTheirGradients();
    void selectedTabLabelReachesAa();
    void tableHeadersKeepOneWeightWhileARowIsSelected();
    void pageTitlesAreSectionSized();
    void richTextRulesUseTheBorderColour();
    void buildDateReadsAsADate();
    void countsAgreeWithTheirNoun();
    void registryDatesReadAsDates();
    // The dark review (D1-D8).
    void plainButtonsKeepTheirFillInDark();
    void progressBarsUseTheThemesBorder_data();
    void progressBarsUseTheThemesBorder();
    void disabledButtonsDoNotPassForSecondaryOnes_data();
    void disabledButtonsDoNotPassForSecondaryOnes();
    void secondaryTextReachesAaInBothThemes_data();
    void secondaryTextReachesAaInBothThemes();
    void uncheckedBoxesHaveAVisibleOutline_data();
    void uncheckedBoxesHaveAVisibleOutline();
    void panelsAndGlyphsFollowTheDarkTheme();
    void anInvalidItemColourLeavesTheViewsOwn();
    void statusBarItemsHaveNoSeparators_data();
    void statusBarItemsHaveNoSeparators();
    // The dark re-check (RD1, DN1).
    void aResolvedColourIsNotAliasedAgain();
    void searchHighlightsCarryDarkText();
    // Partition bars (F32, the Create Partition handle).
    void aSegmentTooNarrowDrawsNoLabel();
    void aBarLabelStepsAroundItsHandles();
    void theWorkflowHeadingFitsItsPane();
};

// The app runs on Qt's Windows 11 style with the theme's sheet on top; the offscreen platform would
// otherwise pick Fusion, which draws focus frames and placeholders differently.
void ThemeWidgetStatesTests::initTestCase() {
    QStyle* style = QStyleFactory::create(QStringLiteral("windows11"));
    QVERIFY2(style != nullptr, "the windows11 style plugin is not deployed next to the tests");
    QApplication::setStyle(style);
}

void ThemeWidgetStatesTests::init() {
    sak::ui::applyWindows11Theme(*qApp, AppThemeMode::Light);
}

void ThemeWidgetStatesTests::cleanupTestCase() {
    sak::ui::applyWindows11Theme(*qApp, AppThemeMode::Light);
}

void ThemeWidgetStatesTests::checkedCheckBoxDrawsATick_data() {
    QTest::addColumn<AppThemeMode>("mode");
    QTest::newRow("light") << AppThemeMode::Light;
    QTest::newRow("dark") << AppThemeMode::Dark;
}

// A style sheet cannot load a data: URL, so the tick was dropped and a checked box was a filled
// square: the Windows picture of "partly checked". The tick is white on the filled box.
void ThemeWidgetStatesTests::checkedCheckBoxDrawsATick() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* box = new QCheckBox(QStringLiteral("Verify"), &window);
    box->setChecked(true);
    layout->addWidget(box);
    const QImage image = shot(window, box);
    QStyleOptionButton option;
    option.initFrom(box);
    const QRect indicator =
        box->style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, box);
    QVERIFY(!indicator.isEmpty());
    const QRect inside = indicator.adjusted(3, 3, -3, -3);
    const int fill = countPixels(image, indicator, isPrimaryFill);
    const int tick = countPixels(image, inside, [](const QColor& c) {
        return c.lightness() >= kWhiteLightnessFloor;
    });
    QVERIFY2(fill >= kMinFilledBoxPixels, qPrintable(QStringLiteral("fill %1").arg(fill)));
    QVERIFY2(tick >= kMinTickPixels,
             qPrintable(QStringLiteral("only %1 white pixels in the checked box").arg(tick)));
}

void ThemeWidgetStatesTests::checkedRadioButtonDrawsADot() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* radio = new QRadioButton(QStringLiteral("Quick"), &window);
    radio->setChecked(true);
    layout->addWidget(radio);
    const QImage image = shot(window, radio);
    QStyleOptionButton option;
    option.initFrom(radio);
    const QRect indicator =
        radio->style()->subElementRect(QStyle::SE_RadioButtonIndicator, &option, radio);
    QVERIFY(!indicator.isEmpty());
    const QColor centre(image.pixel(indicator.center()));
    QVERIFY2(centre.lightness() >= kWhiteLightnessFloor,
             qPrintable(QStringLiteral("centre of a chosen option is %1").arg(centre.name())));
    QVERIFY(countPixels(image, indicator, isPrimaryFill) >= kMinFilledBoxPixels / 2);
}

// A check box in a list or table drew a bare tick with no box when checked; it is now the same
// filled box with a tick as a QCheckBox.
void ThemeWidgetStatesTests::itemViewCheckBoxIsTheSameFilledBox() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* list = new QListWidget(&window);
    auto* item = new QListWidgetItem(QStringLiteral("Documents"), list);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::Checked);
    list->clearSelection();
    layout->addWidget(list);
    const QImage image = shot(window, list->viewport());
    const QRect row = list->visualItemRect(item);
    const QRect indicator_area(row.left(), row.top(), row.height() + 8, row.height());
    QVERIFY2(countPixels(image, indicator_area, isPrimaryFill) >= kMinFilledBoxPixels,
             "a checked item has no filled box");
}

void ThemeWidgetStatesTests::selectedInputTextIsReadable_data() {
    checkedCheckBoxDrawsATick_data();
}

// Selected text was white on a pale blue (1.4:1). The selection colour and its text now reach AA
// against each other, over the input's own fill.
void ThemeWidgetStatesTests::selectedInputTextIsReadable() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    const QString selector = QStringLiteral("QLineEdit, QPlainTextEdit, QTextEdit, QComboBox");
    const QString sheet = qApp->styleSheet();
    // The dark block comes after the light one and wins; read the last block for the mode.
    const qsizetype start = sheet.lastIndexOf(selector);
    QVERIFY(start >= 0);
    const QString block = sheet.mid(start, sheet.indexOf(QLatin1Char('}'), start) - start);
    const QString background = propertyIn(block, QStringLiteral("selection-background-color"));
    const QString text = propertyIn(block, QStringLiteral("selection-color"));
    QVERIFY2(!text.isEmpty(), "inputs name no selection-color");
    const QColor input_fill = mode == AppThemeMode::Dark ? token(sak::ui::kColorDarkBgInput)
                                                         : token(sak::ui::kColorBgWhite);
    const double ratio = contrast(over(text, input_fill), over(background, input_fill));
    QVERIFY2(ratio >= kWcagAaNormalText,
             qPrintable(QStringLiteral("selected text %1 on %2 is %3:1")
                            .arg(text, background)
                            .arg(ratio, 0, 'f', 2)));
}

void ThemeWidgetStatesTests::disabledInputsLookDisabled_data() {
    checkedCheckBoxDrawsATick_data();
}

// Disabled combos, date edits and line edits kept the enabled text colour. Their text is now the
// theme's disabled colour: well away from the enabled text, towards the field's fill.
void ThemeWidgetStatesTests::disabledInputsLookDisabled() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    QList<QPair<QWidget*, QWidget*>> pairs;
    for (int index = 0; index < 3; ++index) {
        QWidget* enabled = nullptr;
        QWidget* disabled = nullptr;
        if (index == 0) {
            enabled = new QLineEdit(QStringLiteral("MMMM 2026"), &window);
            disabled = new QLineEdit(QStringLiteral("MMMM 2026"), &window);
        } else if (index == 1) {
            auto* on = new QComboBox(&window);
            auto* off = new QComboBox(&window);
            on->addItem(QStringLiteral("Balanced MMMM"));
            off->addItem(QStringLiteral("Balanced MMMM"));
            enabled = on;
            disabled = off;
        } else {
            enabled = new QDateEdit(QDate(2026, 11, 4), &window);
            disabled = new QDateEdit(QDate(2026, 11, 4), &window);
        }
        disabled->setEnabled(false);
        layout->addWidget(enabled);
        layout->addWidget(disabled);
        pairs.append(qMakePair(enabled, disabled));
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const auto& [enabled, disabled] : pairs) {
        const QImage on = enabled->grab().toImage();
        const QImage off = disabled->grab().toImage();
        const QRect text_area = on.rect().adjusted(8, 6, -40, -6);
        const QColor on_text = darkestPixel(on, text_area);
        const QColor off_text = darkestPixel(off, text_area);
        if (mode == AppThemeMode::Light) {
            QVERIFY2(off_text.lightness() - on_text.lightness() >= kDisabledLightnessGap,
                     qPrintable(QStringLiteral("%1: enabled text %2, disabled text %3")
                                    .arg(QString::fromLatin1(enabled->metaObject()->className()),
                                         on_text.name(),
                                         off_text.name())));
        } else {
            // Dark: the enabled text is the lightest ink; the disabled ink is far darker than it.
            const QColor on_light = lightestPixel(on, text_area);
            const QColor off_light = lightestPixel(off, text_area);
            QVERIFY2(on_light.lightness() - off_light.lightness() >= kDisabledLightnessGap,
                     qPrintable(QStringLiteral("%1: enabled text %2, disabled text %3")
                                    .arg(QString::fromLatin1(enabled->metaObject()->className()),
                                         on_light.name(),
                                         off_light.name())));
        }
    }
}

// Every item view drew a 1px black frame round the current item. The frame is gone; the
// selection shows the current row.
void ThemeWidgetStatesTests::itemViewsDrawNoBlackFocusFrame() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* list = new QListWidget(&window);
    // The current item has no label: any dark pixel in it is the frame.
    list->addItems({QStringLiteral("7zip"), QString(), QStringLiteral("VLC")});
    layout->addWidget(list);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.activateWindow();
    list->setFocus(Qt::OtherFocusReason);
    list->setCurrentRow(1, QItemSelectionModel::NoUpdate);
    list->clearSelection();
    QTRY_VERIFY(list->hasFocus());
    const QImage image = list->viewport()->grab().toImage();
    const QRect row = list->visualItemRect(list->item(1));
    const int black = countPixels(image, row, [](const QColor& c) { return c.lightness() < 40; });
    QVERIFY2(black == 0, qPrintable(QStringLiteral("%1 black frame pixels").arg(black)));
}

// Tables painted the selection colour and then the selected item's colour on top, so a selected
// row was darker in a table than in a tree or list on the same screen. One opaque shade now.
void ThemeWidgetStatesTests::selectedRowIsOneShadeInEveryView() {
    QWidget window;
    auto* layout = new QHBoxLayout(&window);
    auto* table = new QTableWidget(2, 2, &window);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->selectRow(0);
    auto* tree = new QTreeWidget(&window);
    tree->setColumnCount(1);
    auto* tree_item = new QTreeWidgetItem(tree, {QString()});
    tree_item->setSelected(true);
    auto* list = new QListWidget(&window);
    auto* list_item = new QListWidgetItem(QString(), list);
    list_item->setSelected(true);
    layout->addWidget(table);
    layout->addWidget(tree);
    layout->addWidget(list);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QImage table_image = table->viewport()->grab().toImage();
    const QImage tree_image = tree->viewport()->grab().toImage();
    const QImage list_image = list->viewport()->grab().toImage();
    const QRect cell = table->visualRect(table->model()->index(0, 0));
    const QColor table_shade(table_image.pixel(cell.center()));
    const QColor tree_shade(tree_image.pixel(tree->visualItemRect(tree_item).center()));
    const QColor list_shade(list_image.pixel(list->visualItemRect(list_item).center()));
    QCOMPARE(table_shade.name(), tree_shade.name());
    QCOMPARE(list_shade.name(), tree_shade.name());
}

// Each column of a selected row was its own rounded pill with gaps between (a tree draws each
// column's item separately); the row is one band.
void ThemeWidgetStatesTests::selectedTableRowHasNoGapsAtCellCorners() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* tree = new QTreeWidget(&window);
    tree->setColumnCount(3);
    tree->setRootIsDecorated(false);
    auto* item = new QTreeWidgetItem(tree, {QString(), QString(), QString()});
    new QTreeWidgetItem(tree, {QString(), QString(), QString()});
    item->setSelected(true);
    layout->addWidget(tree);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QImage image = tree->viewport()->grab().toImage();
    const QRect cell = tree->visualRect(tree->model()->index(0, 1));
    const QColor middle(image.pixel(cell.center()));
    for (const QPoint corner : {QPoint(cell.left(), cell.top()),
                                QPoint(cell.right(), cell.top()),
                                QPoint(cell.left(), cell.bottom()),
                                QPoint(cell.right(), cell.bottom())}) {
        QCOMPARE(QColor(image.pixel(corner)).name(), middle.name());
    }
}

// A QSS min-height is the content box; padding and border came on top and made spin boxes and
// date edits 46-50 px beside 32 px combos.
void ThemeWidgetStatesTests::spinBoxesAndDateEditsAreAsTallAsCombos() {
    QWidget window;
    auto* layout = new QHBoxLayout(&window);
    auto* combo = new QComboBox(&window);
    combo->addItem(QStringLiteral("Balanced"));
    auto* line = new QLineEdit(&window);
    auto* spin = new QSpinBox(&window);
    auto* date = new QDateEdit(&window);
    for (QWidget* widget : {static_cast<QWidget*>(combo),
                            static_cast<QWidget*>(line),
                            static_cast<QWidget*>(spin),
                            static_cast<QWidget*>(date)}) {
        layout->addWidget(widget, 0, Qt::AlignTop);
    }
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const int reference = combo->sizeHint().height();
    QVERIFY2(qAbs(line->sizeHint().height() - reference) <= kHeightTolerancePx,
             qPrintable(QStringLiteral("line edit %1 vs combo %2")
                            .arg(line->sizeHint().height())
                            .arg(reference)));
    QVERIFY2(qAbs(spin->sizeHint().height() - reference) <= kHeightTolerancePx,
             qPrintable(QStringLiteral("spin box %1 vs combo %2")
                            .arg(spin->sizeHint().height())
                            .arg(reference)));
    QVERIFY2(qAbs(date->sizeHint().height() - reference) <= kHeightTolerancePx,
             qPrintable(QStringLiteral("date edit %1 vs combo %2")
                            .arg(date->sizeHint().height())
                            .arg(reference)));
}

// Placeholder text came from the text colour at half strength (2.6-3.4:1). It is the muted token,
// which reaches AA on the input's fill.
void ThemeWidgetStatesTests::placeholderTextReachesAa() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* edit = new QLineEdit(&window);
    // Its own text colour, as the email search and the AI composer set: Qt then made the
    // placeholder that colour at half strength.
    edit->setStyleSheet(sak::ui::emailInspectorSearchStyle());
    edit->setPlaceholderText(QStringLiteral("MMMM WWWW HHHH"));
    edit->setFocusPolicy(Qt::NoFocus);  // a focused edit draws its cursor in the text colour
    QFont bold = edit->font();
    bold.setBold(true);
    edit->setFont(bold);
    layout->addWidget(edit);
    const QImage image = shot(window, edit);
    const QColor fill(image.pixel(image.width() - 6, image.height() / 2));
    const QColor ink = darkestPixel(image, image.rect().adjusted(8, 6, -8, -6));
    const double ratio = contrast(ink, fill);
    QVERIFY2(ratio >= kWcagAaNormalText,
             qPrintable(QStringLiteral("placeholder %1 on %2 is %3:1")
                            .arg(ink.name(), fill.name())
                            .arg(ratio, 0, 'f', 2)));
}

// A plain container widget took the page grey: a label and its (?) button in a white dialog, a
// row in a group box, and a slider were grey patches. They take the surface they sit on.
void ThemeWidgetStatesTests::plainContainersTakeTheirParentsSurface() {
    QDialog dialog;
    auto* layout = new QVBoxLayout(&dialog);
    auto* row = new QWidget(&dialog);
    auto* row_layout = new QHBoxLayout(row);
    row_layout->addWidget(new QLabel(QStringLiteral("Hash"), row));
    row_layout->addStretch();
    auto* slider = new QSlider(Qt::Horizontal, &dialog);
    auto* group = new QGroupBox(QStringLiteral("Options"), &dialog);
    auto* group_layout = new QVBoxLayout(group);
    auto* group_row = new QWidget(group);
    group_row->setMinimumHeight(30);
    group_layout->addWidget(group_row);
    layout->addWidget(row);
    layout->addWidget(slider);
    layout->addWidget(group);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    const QImage image = dialog.grab().toImage();
    const QColor dialog_fill(image.pixel(2, 2));
    const QPoint row_point = row->mapTo(&dialog, QPoint(row->width() - 4, row->height() / 2));
    QCOMPARE(QColor(image.pixel(row_point)).name(), dialog_fill.name());
    const QPoint slider_point = slider->mapTo(&dialog, QPoint(slider->width() - 2, 1));
    QCOMPARE(QColor(image.pixel(slider_point)).name(), dialog_fill.name());
    const QImage group_image = group->grab().toImage();
    const QPoint inside_group = group_row->mapTo(group, QPoint(group_row->width() / 2, 4));
    const QPoint group_fill_point = group_row->mapTo(group, QPoint(-3, 4));
    QCOMPARE(QColor(group_image.pixel(inside_group)).name(),
             QColor(group_image.pixel(group_fill_point)).name());
}

// Cards, group boxes and tab panes drew three border shades and two fills for one role.
void ThemeWidgetStatesTests::groupBoxesCardsAndTabPanesShareOneBorder() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* group = new QGroupBox(QStringLiteral("Options"), &window);
    group->setMinimumSize(200, 80);
    auto* card = new QFrame(&window);
    card->setProperty("sakCard", true);
    card->setStyleSheet(sak::ui::cardFrameStyle());
    card->setMinimumSize(200, 80);
    auto* tabs = new QTabWidget(&window);
    tabs->addTab(new QWidget(tabs), QStringLiteral("One"));
    tabs->setMinimumSize(200, 120);
    layout->addWidget(group);
    layout->addWidget(card);
    layout->addWidget(tabs);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QString border = QString::fromLatin1(sak::ui::kColorBorderDefault);
    const QImage group_image = group->grab().toImage();
    const QImage card_image = card->grab().toImage();
    QCOMPARE(QColor(group_image.pixel(0, group_image.height() / 2)).name(), border);
    QCOMPARE(QColor(card_image.pixel(0, card_image.height() / 2)).name(), border);
    QCOMPARE(QColor(group_image.pixel(4, group_image.height() / 2)).name(),
             QColor(card_image.pixel(4, card_image.height() / 2)).name());
    const QImage tab_image = tabs->grab().toImage();
    QWidget* page = tabs->widget(0);
    const QPoint pane_edge = page->mapTo(tabs, QPoint(0, page->height() / 2));
    bool found = false;
    for (int x = pane_edge.x(); x >= 0 && !found; --x) {
        found = QColor(tab_image.pixel(x, pane_edge.y())).name() == border;
    }
    QVERIFY2(found, "the tab pane's border is not the default border colour");
}

// The status bar's Theme and Run as Admin buttons drew the primary gradient under dark text.
void ThemeWidgetStatesTests::statusBarLinkButtonsHaveNoButtonFill() {
    QWidget window;
    auto* layout = new QHBoxLayout(&window);
    auto* button = new QPushButton(QStringLiteral("Run as Admin"), &window);
    button->setFlat(true);
    button->setStyleSheet(sak::ui::compactLinkButtonStyle());
    layout->addWidget(button);
    const QImage window_image = shot(window, &window);
    const QPoint inside = button->mapTo(&window, QPoint(2, button->height() / 2));
    const QColor page(window_image.pixel(1, 1));
    QCOMPARE(QColor(window_image.pixel(inside)).name(), page.name());
}

// "Export ALL Mail Folders as" ran into its submenu arrow.
void ThemeWidgetStatesTests::submenuArrowClearsTheLongestLabel() {
    QMenu menu;
    auto* sub = new QMenu(QStringLiteral("Export ALL Mail Folders as"), &menu);
    sub->addAction(QStringLiteral("MBOX"));
    menu.addAction(QStringLiteral("Search in This Folder..."));
    menu.addMenu(sub);
    menu.ensurePolished();
    menu.adjustSize();
    const QImage image = menu.grab().toImage();
    const QRect item = menu.actionGeometry(sub->menuAction());
    QVERIFY(!item.isEmpty());
    // Columns that hold ink in the item's middle band, right to left: the arrow, a gap, the label.
    const int top = item.top() + (item.height() / 4);
    const int bottom = item.bottom() - (item.height() / 4);
    const auto inked = [&](int x) {
        for (int y = top; y <= bottom; ++y) {
            if (QColor(image.pixel(x, y)).lightness() < kTextDarknessThreshold) {
                return true;
            }
        }
        return false;
    };
    int x = item.right() - 1;
    while (x > item.left() && !inked(x)) {
        --x;
    }
    while (x > item.left() && inked(x)) {
        --x;  // the arrow
    }
    const int arrow_left = x + 1;
    while (x > item.left() && !inked(x)) {
        --x;
    }
    const int gap = arrow_left - x - 1;
    QVERIFY2(gap >= kSubmenuArrowGapPx,
             qPrintable(QStringLiteral("%1 px between the label and the arrow").arg(gap)));
}

// The explorer's chrome rules wrote each state once after a list of five selectors; the state
// bound to the last only, so the other four took every state's fill and colour at once.
void ThemeWidgetStatesTests::explorerChromeButtonsShowTheirOwnState() {
    QWidget window;
    window.setObjectName(QStringLiteral("fileExplorerRoot"));
    window.setStyleSheet(sak::ui::fileExplorerShellStyleSheetText());
    auto* layout = new QVBoxLayout(&window);
    auto* bar = new QWidget(&window);
    bar->setObjectName(QStringLiteral("fileExplorerCommandBar"));
    auto* bar_layout = new QHBoxLayout(bar);
    auto* enabled = new QPushButton(QStringLiteral("New"), bar);
    auto* disabled = new QPushButton(QStringLiteral("Cut"), bar);
    disabled->setEnabled(false);
    bar_layout->addWidget(enabled);
    bar_layout->addWidget(disabled);
    layout->addWidget(bar);
    const QImage image = shot(window, bar);
    const QColor bar_fill(image.pixel(1, 1));
    const QPoint enabled_inside = enabled->mapTo(bar, QPoint(3, enabled->height() / 2));
    QCOMPARE(QColor(image.pixel(enabled_inside)).name(), bar_fill.name());
    // Read the labels off the bar's own picture: a transparent button grabbed alone is black.
    const QColor on_ink = darkestPixel(image, enabled->geometry());
    const QColor off_ink = darkestPixel(image, disabled->geometry());
    QVERIFY2(off_ink.lightness() - on_ink.lightness() >= kDisabledLightnessGap,
             qPrintable(QStringLiteral("enabled label %1, disabled label %2")
                            .arg(on_ink.name(), off_ink.name())));
}

// Normal text tokens on the light page (#f3f5f9) and window background (#eef2f7).
void ThemeWidgetStatesTests::textTokensReachAaOnTheLightPage() {
    const QColor page = token(sak::ui::kColorBgPage);
    const QColor window = token(sak::ui::kColorBgPageHover);
    for (const char* text : {sak::ui::kColorTextMuted,
                             sak::ui::kStatusColorIdle,
                             sak::ui::kColorSuccess,
                             sak::ui::kStatusColorSuccess,
                             sak::ui::kStatusColorWarning,
                             sak::ui::kColorWarning,
                             sak::ui::kColorError,
                             sak::ui::kStatusColorError}) {
        const double on_page = contrast(token(text), page);
        const double on_window = contrast(token(text), window);
        QVERIFY2(on_page >= kWcagAaNormalText && on_window >= kWcagAaNormalText,
                 qPrintable(QStringLiteral("%1 is %2:1 on the page, %3:1 on the window")
                                .arg(QString::fromLatin1(text))
                                .arg(on_page, 0, 'f', 2)
                                .arg(on_window, 0, 'f', 2)));
    }
}

// The light status colours were darkened for the light page; on the dark theme the same tokens
// paint their dark counterpart, which reaches AA on every dark surface.
void ThemeWidgetStatesTests::darkStatusTextReachesAaOnDarkSurfaces() {
    sak::ui::applyWindows11Theme(*qApp, AppThemeMode::Dark);
    for (const char* text : {sak::ui::kColorSuccess,
                             sak::ui::kStatusColorSuccess,
                             sak::ui::kStatusColorWarning,
                             sak::ui::kColorWarning,
                             sak::ui::kColorError}) {
        const QColor painted = sak::ui::themedColor(text);
        QCOMPARE(sak::ui::cssColor(text), painted.name());
        for (const char* surface : {sak::ui::kColorDarkBgPage,
                                    sak::ui::kColorDarkBgInput,
                                    sak::ui::kColorDarkBgSurface,
                                    sak::ui::kColorDarkBgPanel}) {
            const double ratio = contrast(painted, token(surface));
            QVERIFY2(ratio >= kWcagAaNormalText,
                     qPrintable(QStringLiteral("%1 (painted %2) on %3 is %4:1")
                                    .arg(QString::fromLatin1(text),
                                         painted.name(),
                                         QString::fromLatin1(surface))
                                    .arg(ratio, 0, 'f', 2)));
        }
    }
}

// White label text on the middle stop of each button gradient, over a white dialog.
void ThemeWidgetStatesTests::buttonLabelsReachAaOnTheirGradients() {
    const QColor white = token(sak::ui::kColorButtonTextOnTone);
    const QColor dialog = token(sak::ui::kColorBgWhite);
    struct Stop {
        const char* name;
        const char* stop;
    };
    const Stop stops[] = {
        {"primary", sak::ui::kColorPrimaryButtonNormalMiddle},
        {"primary hover", sak::ui::kColorPrimaryButtonHoverMiddle},
        {"secondary", sak::ui::kColorSecondaryButtonNormalMiddle},
        {"secondary hover", sak::ui::kColorSecondaryButtonHoverMiddle},
        {"success", sak::ui::kColorSuccessButtonNormalMiddle},
        {"success hover", sak::ui::kColorSuccessButtonHoverMiddle},
        {"danger", sak::ui::kColorDangerButtonNormalMiddle},
        {"danger hover", sak::ui::kColorDangerButtonHoverMiddle},
        {"discord", sak::ui::kColorDiscordButtonNormalMiddle},
        {"discord hover", sak::ui::kColorDiscordButtonHoverMiddle},
    };
    for (const Stop& stop : stops) {
        const QColor fill = over(QString::fromLatin1(stop.stop), dialog);
        const double ratio = contrast(white, fill);
        QVERIFY2(ratio >= kWcagAaNormalText,
                 qPrintable(QStringLiteral("%1 button: white on %2 is %3:1")
                                .arg(QString::fromLatin1(stop.name), fill.name())
                                .arg(ratio, 0, 'f', 2)));
    }
}

// The selected tab's white label on the middle of its diagonal gradient, read from the sheet.
void ThemeWidgetStatesTests::selectedTabLabelReachesAa() {
    const QString block = sheetBlock(QStringLiteral("QTabBar::tab:selected"));
    QVERIFY(!block.isEmpty());
    static const QRegularExpression kStops(QStringLiteral(
        R"(stop:0\s+(rgba\([^)]*\)|#[0-9a-fA-F]{6}),\s*stop:1\s+(rgba\([^)]*\)|#[0-9a-fA-F]{6}))"));
    const auto match = kStops.match(block);
    QVERIFY2(match.hasMatch(), qPrintable(block));
    const QColor backdrop = token(sak::ui::kColorBgWhite);
    const QColor top = over(match.captured(1), backdrop);
    const QColor bottom = over(match.captured(2), backdrop);
    const QColor middle((top.red() + bottom.red()) / 2,
                        (top.green() + bottom.green()) / 2,
                        (top.blue() + bottom.blue()) / 2);
    const QColor text(propertyIn(block, QStringLiteral("color")));
    const double ratio = contrast(text, middle);
    QVERIFY2(ratio >= kWcagAaNormalText,
             qPrintable(QStringLiteral("selected tab label on %1 is %2:1")
                            .arg(middle.name())
                            .arg(ratio, 0, 'f', 2)));
}

// S20: a table bolded every header section while a row was selected; trees and lists did not.
void ThemeWidgetStatesTests::tableHeadersKeepOneWeightWhileARowIsSelected() {
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* table = new QTableWidget(2, 2, &window);
    table->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Status")});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(table);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const QImage before = table->horizontalHeader()->grab().toImage();
    table->selectRow(0);
    QCoreApplication::processEvents();
    QVERIFY(!table->horizontalHeader()->highlightSections());
    QCOMPARE(table->horizontalHeader()->grab().toImage(), before);
}

// S26: the theme's "* { font-size: 10pt }" overrode the header title's QFont, so page titles drew
// at 10pt under 13pt card titles.
void ThemeWidgetStatesTests::pageTitlesAreSectionSized() {
    QWidget page;
    auto* layout = new QVBoxLayout(&page);
    const auto header = sak::createDynamicPanelHeader(
        &page, QString(), QStringLiteral("Diagnostics"), QStringLiteral("Hardware"), layout);
    page.show();
    QVERIFY(QTest::qWaitForWindowExposed(&page));
    header.titleLabel->ensurePolished();
    QCOMPARE(header.titleLabel->font().pointSizeF(), double(sak::ui::kFontSizeSection));
    QVERIFY(header.titleLabel->font().bold());
}

// F26: QTextDocument ignores a CSS border on <hr> and drew the rule in the text colour.
void ThemeWidgetStatesTests::richTextRulesUseTheBorderColour() {
    QTextDocument document;
    document.setHtml(QStringLiteral("<p>Above</p>") +
                     QString::fromLatin1(sak::ui::kHtmlHorizontalRule)
                         .arg(sak::ui::kCssBorderWidthDefaultPx)
                         .arg(QString::fromLatin1(sak::ui::kColorBorderDefault)) +
                     QStringLiteral("<p>Below</p>"));
    document.setTextWidth(200);
    QImage image(200, int(document.size().height()) + 4, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    document.drawContents(&painter);
    painter.end();
    const QColor border(QString::fromLatin1(sak::ui::kColorBorderDefault));
    const QColor text(QString::fromLatin1(sak::ui::kColorTextPrimary));
    int rule_pixels = 0;
    int dark_rule_pixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        int row_ink = 0;
        int row_dark = 0;
        for (int x = 20; x < 180; ++x) {
            const QColor pixel(image.pixel(x, y));
            row_ink += pixel != QColor(Qt::white) ? 1 : 0;
            row_dark += pixel.lightness() < 120 ? 1 : 0;
        }
        if (row_ink >= 150) {  // a full-width row of ink is the rule, not text
            rule_pixels += row_ink;
            dark_rule_pixels += row_dark;
        }
    }
    QVERIFY2(rule_pixels > 0, "no rule was drawn");
    QVERIFY2(dark_rule_pixels == 0,
             qPrintable(QStringLiteral("the rule is drawn in the text colour %1, not %2")
                            .arg(text.name(), border.name())));
}

// F37: the About line showed the compiler's "Nov  4 2026", double space and all.
void ThemeWidgetStatesTests::buildDateReadsAsADate() {
    QCOMPARE(sak::readableBuildDate("Nov  4 2026"), QStringLiteral("November 4, 2026"));
    QCOMPARE(sak::readableBuildDate("Dec 25 2026"), QStringLiteral("December 25, 2026"));
}

// S24: "1 hops", "1 Operations Pending", "Found 1 email files".
void ThemeWidgetStatesTests::countsAgreeWithTheirNoun() {
    QCOMPARE(sak::countOf(1, QStringLiteral("hop"), QStringLiteral("hops")),
             QStringLiteral("1 hop"));
    QCOMPARE(sak::countOf(0, QStringLiteral("hop"), QStringLiteral("hops")),
             QStringLiteral("0 hops"));
    QCOMPARE(sak::countOf(3, QStringLiteral("hop"), QStringLiteral("hops")),
             QStringLiteral("3 hops"));
}

// Dark review DF10: install dates showed the registry's raw "20261104".
void ThemeWidgetStatesTests::registryDatesReadAsDates() {
    QCOMPARE(sak::readableRegistryDate(QStringLiteral("20261104")), QStringLiteral("2026-11-04"));
    QCOMPARE(sak::readableRegistryDate(QStringLiteral("11/4/2026")), QStringLiteral("11/4/2026"));
    QCOMPARE(sak::readableRegistryDate(QString()), QString());
}

// D1: the dark sheet's "QWidget { background-color }" came after the button rule at the same
// specificity, so a button with no sheet of its own lost its primary fill in dark.
void ThemeWidgetStatesTests::plainButtonsKeepTheirFillInDark() {
    sak::ui::applyWindows11Theme(*qApp, AppThemeMode::Dark);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* button = new QPushButton(QStringLiteral("Refresh Disks"), &window);
    layout->addWidget(button);
    const QImage image = shot(window, &window);
    const QPoint inside = button->mapTo(&window, QPoint(6, button->height() / 2));
    const QColor fill(image.pixel(inside));
    QVERIFY2(fill.blue() > 180 && fill.blue() > fill.red() + 80,
             qPrintable(QStringLiteral("a plain dark button is filled %1").arg(fill.name())));
}

void ThemeWidgetStatesTests::progressBarsUseTheThemesBorder_data() {
    checkedCheckBoxDrawsATick_data();
}

// D3: progress bars kept the light #cbd5e1 frame on the dark page.
void ThemeWidgetStatesTests::progressBarsUseTheThemesBorder() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* bar = new QProgressBar(&window);
    bar->setRange(0, 100);
    bar->setValue(10);
    bar->setTextVisible(false);
    bar->setMinimumWidth(200);
    layout->addWidget(bar);
    const QImage image = shot(window, &window);
    const QRect rect = bar->geometry();
    const QColor edge(image.pixel(rect.right() - 20, rect.top()));
    const QColor page(image.pixel(1, 1));
    const bool dark_edge = edge.lightness() < 128;
    QVERIFY2(dark_edge == (mode == AppThemeMode::Dark),
             qPrintable(
                 QStringLiteral("progress border %1 on page %2").arg(edge.name(), page.name())));
}

void ThemeWidgetStatesTests::disabledButtonsDoNotPassForSecondaryOnes_data() {
    checkedCheckBoxDrawsATick_data();
}

// D2: on the dark page a disabled button and a secondary one had nearly the same fill.
void ThemeWidgetStatesTests::disabledButtonsDoNotPassForSecondaryOnes() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* secondary = new QPushButton(QStringLiteral("Skip Step"), &window);
    secondary->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    auto* disabled = new QPushButton(QStringLiteral("Run Full Suite"), &window);
    disabled->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    disabled->setEnabled(false);
    layout->addWidget(secondary);
    layout->addWidget(disabled);
    const QImage image = shot(window, &window);
    const QColor on(image.pixel(secondary->mapTo(&window, QPoint(6, secondary->height() / 2))));
    const QColor off(image.pixel(disabled->mapTo(&window, QPoint(6, disabled->height() / 2))));
    const QColor page(image.pixel(1, 1));
    // A disabled fill sits close to the page; a secondary fill stands well away from it.
    QVERIFY2(qAbs(off.lightness() - page.lightness()) * 2 < qAbs(on.lightness() - page.lightness()),
             qPrintable(QStringLiteral("page %1, secondary %2, disabled %3")
                            .arg(page.name(), on.name(), off.name())));
    // Review 2 (D2): in dark the disabled fill was still 1.28:1 from a secondary one and 1.37:1
    // from its panel, so only the label told them apart. A disabled button is an outline: no fill.
    QVERIFY2(contrast(off, page) < 1.05,
             qPrintable(QStringLiteral("disabled fill %1 on %2 is %3:1")
                            .arg(off.name(), page.name())
                            .arg(contrast(off, page), 0, 'f', 2)));
}

void ThemeWidgetStatesTests::secondaryTextReachesAaInBothThemes_data() {
    checkedCheckBoxDrawsATick_data();
}

// D5: the palette's Dark role, File Explorer's secondary text, was 2.6:1 in light and 3.1-3.7:1
// in dark; running blue, violet and error red had no dark values.
void ThemeWidgetStatesTests::secondaryTextReachesAaInBothThemes() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    const QPalette palette = QApplication::palette();
    const double on_base = contrast(palette.color(QPalette::Dark), palette.color(QPalette::Base));
    QVERIFY2(on_base >= kWcagAaNormalText,
             qPrintable(QStringLiteral("palette(dark) on base is %1:1").arg(on_base, 0, 'f', 2)));
    if (mode == AppThemeMode::Dark) {
        for (const char* text : {sak::ui::kStatusColorRunning,
                                 sak::ui::kColorPrimaryDark,
                                 sak::ui::kColorAccentPurple}) {
            for (const char* surface : {sak::ui::kColorDarkBgPage,
                                        sak::ui::kColorDarkBgInput,
                                        sak::ui::kColorDarkBgSurface,
                                        sak::ui::kColorDarkBgPanel}) {
                const double ratio = contrast(QColor(sak::ui::cssColor(text)), token(surface));
                QVERIFY2(ratio >= kWcagAaNormalText,
                         qPrintable(
                             QStringLiteral("%1 on %2 is %3:1")
                                 .arg(QString::fromLatin1(text), QString::fromLatin1(surface))
                                 .arg(ratio, 0, 'f', 2)));
            }
        }
        const double error_panel = contrast(QColor(sak::ui::cssColor(sak::ui::kColorErrorText)),
                                            QColor(sak::ui::cssColor(sak::ui::kColorBgErrorPanel)));
        QVERIFY2(error_panel >= kWcagAaNormalText,
                 qPrintable(QStringLiteral("error text on the error panel is %1:1")
                                .arg(error_panel, 0, 'f', 2)));
    }
}

void ThemeWidgetStatesTests::uncheckedBoxesHaveAVisibleOutline_data() {
    checkedCheckBoxDrawsATick_data();
}

// D8: an unchecked box's outline was 2.6:1 in light and 1.9:1 in dark, under the 3:1 a control
// needs to be seen.
void ThemeWidgetStatesTests::uncheckedBoxesHaveAVisibleOutline() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    const QString sheet = qApp->styleSheet();
    const qsizetype start =
        sheet.lastIndexOf(QStringLiteral("QCheckBox::indicator, QAbstractItemView::indicator {"));
    QVERIFY(start >= 0);
    const QString block = sheet.mid(start, sheet.indexOf(QLatin1Char('}'), start) - start);
    static const QRegularExpression kBorder(
        QStringLiteral(R"(border:\s*1px solid (#[0-9a-fA-F]{6}))"));
    const auto match = kBorder.match(block);
    QVERIFY2(match.hasMatch(), qPrintable(block));
    const QColor outline(match.captured(1));
    const QStringList surfaces =
        mode == AppThemeMode::Dark ? QStringList{QString::fromLatin1(sak::ui::kColorDarkBgPage),
                                                 QString::fromLatin1(sak::ui::kColorDarkBgPanel),
                                                 QString::fromLatin1(sak::ui::kColorDarkBgSurface)}
                                   : QStringList{QString::fromLatin1(sak::ui::kColorBgWhite),
                                                 QString::fromLatin1(sak::ui::kColorBgSurface),
                                                 QString::fromLatin1(sak::ui::kColorBgPage)};
    for (const QString& surface : surfaces) {
        const double ratio = contrast(outline, QColor(surface));
        QVERIFY2(ratio >= 3.0,
                 qPrintable(QStringLiteral("outline %1 on %2 is %3:1")
                                .arg(outline.name(), surface)
                                .arg(ratio, 0, 'f', 2)));
    }
}

// D4, D6: the per-user warning box hard-coded the light error panel, and the email folder glyphs
// were black on the dark surfaces.
void ThemeWidgetStatesTests::panelsAndGlyphsFollowTheDarkTheme() {
    sak::ui::applyWindows11Theme(*qApp, AppThemeMode::Dark);
    QVERIFY2(!sak::ui::warningPanelStyle().contains(
                 QString::fromLatin1(sak::ui::kColorBgErrorPanel)),
             qPrintable(sak::ui::warningPanelStyle()));
    const QImage glyph = sak::themedGlyphIcon(QStringLiteral(":/icons/icons/icons8-folder.svg"))
                             .pixmap(16, 16)
                             .toImage();
    int ink = 0;
    int black = 0;
    for (int y = 0; y < glyph.height(); ++y) {
        for (int x = 0; x < glyph.width(); ++x) {
            const QColor pixel = glyph.pixelColor(x, y);
            if (pixel.alpha() > 200) {
                ++ink;
                black += pixel.lightness() < 60 ? 1 : 0;
            }
        }
    }
    QVERIFY(ink > 0);
    QCOMPARE(black, 0);
}

// DF3, DF4: an invalid QColor given to setForeground() painted the text black.
void ThemeWidgetStatesTests::anInvalidItemColourLeavesTheViewsOwn() {
    QTableWidgetItem item(QStringLiteral("LISTEN"));
    item.setForeground(QColor(Qt::red));
    sak::setItemForeground(&item, QColor());
    QVERIFY(!item.data(Qt::ForegroundRole).isValid());
    sak::setItemForeground(&item, QColor(Qt::green));
    QCOMPARE(item.foreground().color(), QColor(Qt::green));
}

void ThemeWidgetStatesTests::statusBarItemsHaveNoSeparators_data() {
    checkedCheckBoxDrawsATick_data();
}

// D7: the status bar framed each permanent item with a native separator in a colour from no token
// (#d7d7d7), bright on the dark bar.
void ThemeWidgetStatesTests::statusBarItemsHaveNoSeparators() {
    QFETCH(AppThemeMode, mode);
    sak::ui::applyWindows11Theme(*qApp, mode);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    auto* bar = new QStatusBar(&window);
    bar->setSizeGripEnabled(false);
    bar->setMinimumWidth(400);
    auto* theme = new QLabel(QStringLiteral("Theme: System"), bar);
    auto* ready = new QLabel(QStringLiteral("Ready"), bar);
    bar->addPermanentWidget(theme);
    bar->addPermanentWidget(ready);
    layout->addWidget(bar);
    const QImage image = shot(window, bar);
    QVERIFY(!image.isNull());
    const QColor ground(image.pixel(4, image.height() / 2));
    QStringList marks;
    for (const QLabel* label : {theme, ready}) {
        // Where QStatusBar draws an item's frame: its geometry widened by 2 px and 1 px.
        const QRect frame = label->geometry().adjusted(-2, -1, 2, 1);
        for (const int x : {frame.left(), frame.right()}) {
            const QPoint at(x, frame.center().y());
            if (!image.rect().contains(at)) {
                continue;
            }
            const QColor pixel(image.pixel(at));
            const int distance = std::abs(pixel.red() - ground.red()) +
                                 std::abs(pixel.green() - ground.green()) +
                                 std::abs(pixel.blue() - ground.blue());
            if (distance > 24) {
                marks << QStringLiteral("%1 at x %2 on %3")
                             .arg(pixel.name())
                             .arg(x)
                             .arg(ground.name());
            }
        }
    }
    QVERIFY2(marks.isEmpty(), qPrintable(marks.join(QStringLiteral(", "))));
}

// Dark re-check RD1: the dark text colour #f8fafc is also the light surface token, and a label
// given it went through the token aliases a second time and became the dark surface. A colour
// that is already resolved stays itself; a token still follows the theme.
void ThemeWidgetStatesTests::aResolvedColourIsNotAliasedAgain() {
    sak::ui::applyWindows11Theme(*qApp, AppThemeMode::Dark);
    const QString dark_text = sak::ui::htmlColor(sak::ui::kColorTextHeading);
    QCOMPARE(QString::fromLatin1(sak::ui::kColorBgSurface), QStringLiteral("#f8fafc"));
    for (const QString& resolved : {dark_text,
                                    QStringLiteral("#f8fafc"),
                                    QString::fromLatin1(sak::ui::kColorBorderDefault)}) {
        QCOMPARE(sak::ui::textColorAndFontSizeStyle(resolved, 9),
                 QStringLiteral("color: %1; font-size: 9pt;").arg(resolved));
        QCOMPARE(sak::ui::textColorStyle(resolved), QStringLiteral("color: %1;").arg(resolved));
        QCOMPARE(sak::ui::fontWeightAndColorStyle(700, resolved),
                 QStringLiteral("font-weight: 700; color: %1;").arg(resolved));
        QCOMPARE(sak::ui::fontSizeWeightColorStyle(10, 700, resolved),
                 QStringLiteral("font-size: 10pt; font-weight: 700; color: %1;").arg(resolved));
        QVERIFY(sak::ui::transcriptToggleStyle(resolved).contains(
            QStringLiteral("color: %1;").arg(resolved)));
    }
    // The token itself still means the surface.
    QCOMPARE(sak::ui::textColorStyle(sak::ui::kColorBgSurface),
             QStringLiteral("color: palette(alternate-base);"));
}

// Dark re-check DN1: Advanced Search marked a match with a yellow or orange fill only, and in the
// dark theme the preview's light text sat on it at 1.3:1.
void ThemeWidgetStatesTests::searchHighlightsCarryDarkText() {
    for (const AppThemeMode mode : {AppThemeMode::Light, AppThemeMode::Dark}) {
        sak::ui::applyWindows11Theme(*qApp, mode);
        for (const bool current : {true, false}) {
            const QTextCharFormat format = sak::AdvancedSearchPanel::matchHighlightFormat(current);
            QVERIFY(format.hasProperty(QTextFormat::ForegroundBrush));
            const double ratio = contrast(format.foreground().color(), format.background().color());
            QVERIFY2(ratio >= kWcagAaNormalText,
                     qPrintable(QStringLiteral("%1 on %2 is %3:1")
                                    .arg(format.foreground().color().name(),
                                         format.background().color().name())
                                    .arg(ratio, 0, 'f', 2)));
        }
    }
}

// Review 2 (F32): Copy Disk's target bar drew ": ." in segments a few pixels wide.
void ThemeWidgetStatesTests::aSegmentTooNarrowDrawsNoLabel() {
    const QFontMetrics metrics(QApplication::font());
    const QString label = QStringLiteral("*: 16.0 MB");
    const int whole = metrics.horizontalAdvance(label);
    QCOMPARE(sak::labelThatFits(metrics, label, whole), label);
    QVERIFY(sak::labelThatFits(metrics, label, whole - 1).endsWith(QChar(0x2026)));
    QCOMPARE(sak::labelThatFits(metrics, label, metrics.horizontalAdvance(QStringLiteral("*:"))),
             QString());
    QCOMPARE(sak::labelThatFits(metrics, label, 3), QString());
}

// Review 2: Create Partition's size handle sat in the middle of "New 1.00 GB | Remaining 1.98 GB".
void ThemeWidgetStatesTests::aBarLabelStepsAroundItsHandles() {
    const QRect track(100, 10, 300, 20);  // x 100 to 399, centre 249
    constexpr int kText = 100;            // centred: 199 to 299
    constexpr int kClear = 7;
    // Nothing in the way: the whole track, the text centred on it.
    QCOMPARE(sak::labelRectClearOfMarks(track, kText, {120, 380}, kClear), track);
    // A handle at 230 crosses it: the text goes right of it, the wider side.
    QCOMPARE(sak::labelRectClearOfMarks(track, kText, {230}, kClear), QRect(238, 10, 162, 20));
    // At 290 the left side is the wider one.
    QCOMPARE(sak::labelRectClearOfMarks(track, kText, {290}, kClear), QRect(100, 10, 183, 20));
    // Two handles: the widest of the three stretches, here the middle.
    QCOMPARE(sak::labelRectClearOfMarks(track, kText, {280, 140}, kClear), QRect(148, 10, 125, 20));
}
// Review 2 (N17): the workflow's name was a 13 pt heading in a 290 px pane, wrapping to two lines
// and larger than the page's own title.
void ThemeWidgetStatesTests::theWorkflowHeadingFitsItsPane() {
    const QRegularExpression h1(QStringLiteral("h1\\{font-size:(\\d+)pt"));
    const QRegularExpressionMatch match = h1.match(sak::ui::aiWorkflowMarkdownStyle());
    QVERIFY(match.hasMatch());
    QVERIFY2(match.captured(1).toInt() <= sak::ui::kFontSizeStatus, qPrintable(match.captured(1)));
}
QTEST_MAIN(ThemeWidgetStatesTests)
#include "test_theme_widget_states.moc"
