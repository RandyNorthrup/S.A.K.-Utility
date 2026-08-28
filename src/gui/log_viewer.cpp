// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/log_viewer.h"

#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QSaveFile>
#include <QScrollBar>
#include <QTextDocument>
#include <QTextStream>
#include <QVBoxLayout>

namespace {
// Bounded retention so a flood of appendLog() calls or a huge loaded log file cannot
// grow m_all_logs without limit and exhaust memory. Trim in a batch once the hard cap
// is exceeded so steady-state appends stay amortized O(1). Fail closed on size.
constexpr int kMaxRetainedLogEntries = 100'000;
constexpr int kRetainedTrimTarget = 90'000;
}  // namespace

LogViewer::LogViewer(QWidget* parent)
    : QWidget(parent)
    , m_text_browser(nullptr)
    , m_clear_button(nullptr)
    , m_save_button(nullptr)
    , m_auto_scroll_checkbox(nullptr)
    , m_filter_combo(nullptr)
    , m_search_edit(nullptr)
    , m_current_filter(LogLevel::All)
    , m_auto_scroll(true) {
    setupUi();
}

// Builds the toolbar control row (severity filter, search box, auto-scroll toggle,
// clear/save buttons) and wires each control to its slot. Split out of setupUi() so
// the widget-construction body stays within the length gate.
QHBoxLayout* LogViewer::createToolbar() {
    auto* toolbar_layout = new QHBoxLayout();
    toolbar_layout->setSpacing(sak::ui::kSpacingMedium);

    // Filter combo
    m_filter_combo = new QComboBox(this);
    m_filter_combo->setAccessibleName("Log severity filter");
    m_filter_combo->addItem("All Logs");
    m_filter_combo->addItem("Info");
    m_filter_combo->addItem("Warning");
    m_filter_combo->addItem("Error");
    m_filter_combo->setToolTip("Show only logs at or above the selected severity level");
    connect(m_filter_combo,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &LogViewer::onFilterChanged);
    toolbar_layout->addWidget(m_filter_combo);

    // Search box
    m_search_edit = new QLineEdit(this);
    m_search_edit->setAccessibleName("Log search");
    m_search_edit->setPlaceholderText("Search logs...");
    m_search_edit->setClearButtonEnabled(true);
    connect(m_search_edit, &QLineEdit::textChanged, this, &LogViewer::onSearchTextChanged);
    toolbar_layout->addWidget(m_search_edit);

    // Auto-scroll checkbox
    m_auto_scroll_checkbox = new QCheckBox("Auto-scroll", this);
    m_auto_scroll_checkbox->setAccessibleName("Auto-scroll logs");
    m_auto_scroll_checkbox->setChecked(true);
    connect(m_auto_scroll_checkbox, &QCheckBox::toggled, this, &LogViewer::onAutoScrollToggled);
    toolbar_layout->addWidget(m_auto_scroll_checkbox);

    // Clear button
    m_clear_button = new QPushButton("Clear", this);
    m_clear_button->setAccessibleName("Clear log viewer");
    m_clear_button->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    connect(m_clear_button, &QPushButton::clicked, this, &LogViewer::onClearClicked);
    toolbar_layout->addWidget(m_clear_button);

    // Save button
    m_save_button = new QPushButton("Save Log", this);
    m_save_button->setAccessibleName("Save log file");
    m_save_button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    connect(m_save_button, &QPushButton::clicked, this, &LogViewer::onSaveClicked);
    toolbar_layout->addWidget(m_save_button);

    return toolbar_layout;
}

void LogViewer::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    constexpr int kLogFontPointSize = sak::ui::kFontSizeNote;
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(sak::ui::kSpacingMedium);
    main_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    main_layout->addLayout(createToolbar());

    // Text browser
    m_text_browser = new QTextBrowser(this);
    m_text_browser->setAccessibleName("Log messages");
    m_text_browser->setOpenExternalLinks(false);
    m_text_browser->setReadOnly(true);
    m_text_browser->setFont(QFont("Consolas", kLogFontPointSize));
    // Bound the browser document so it discards the oldest blocks instead of
    // growing without limit alongside m_all_logs.
    m_text_browser->document()->setMaximumBlockCount(kMaxRetainedLogEntries);
    main_layout->addWidget(m_text_browser);

    Q_ASSERT(m_search_edit);
    Q_ASSERT(m_filter_combo);
}

void LogViewer::appendLog(const QString& message, LogLevel level) {
    Q_ASSERT(m_search_edit);
    Q_ASSERT(m_text_browser);
    const QString formatted_msg = formatLogMessage(message, level);
    m_all_logs.append(formatted_msg);
    if (m_all_logs.size() > kMaxRetainedLogEntries) {
        // Drop the oldest entries in a batch so retention stays bounded.
        m_all_logs.remove(0, m_all_logs.size() - kRetainedTrimTarget);
    }

    // Apply current filter
    if (m_current_filter == LogLevel::All || m_current_filter == level) {
        const QString search_text = m_search_edit->text();
        if (search_text.isEmpty() || message.contains(search_text, Qt::CaseInsensitive)) {
            m_text_browser->append(formatted_msg);

            if (m_auto_scroll) {
                QScrollBar* scrollbar = m_text_browser->verticalScrollBar();
                scrollbar->setValue(scrollbar->maximum());
            }
        }
    }
}

bool LogViewer::loadLogFile(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sak::logError("Failed to open log file: {}", file_path.toStdString());
        sak::showWarningLogged(this,
                               "Load Error",
                               QString("Failed to open log file: %1").arg(file_path));
        return false;
    }

    clear();

    QTextStream in(&file);
    int lines_loaded = 0;
    while (!in.atEnd()) {
        if (lines_loaded >= kMaxRetainedLogEntries) {
            // Fail closed on an oversized file: stop rather than read an unbounded
            // number of lines into memory, and tell the user it was truncated.
            sak::logWarning(
                "Log file '{}' exceeds the viewer's {}-line cap; showing the "
                "first {} lines only",
                file_path.toStdString(),
                kMaxRetainedLogEntries,
                kMaxRetainedLogEntries);
            sak::showWarningLogged(
                this,
                "Log Truncated",
                QString("This log is larger than the %1-line viewer limit. Showing the "
                        "first %1 lines only.")
                    .arg(kMaxRetainedLogEntries));
            break;
        }
        const QString line = in.readLine();

        // Try to detect log level from line content
        LogLevel level = LogLevel::Info;
        if (line.contains("ERROR", Qt::CaseInsensitive) ||
            line.contains("FATAL", Qt::CaseInsensitive)) {
            level = LogLevel::Error;
        } else if (line.contains("WARN", Qt::CaseInsensitive)) {
            level = LogLevel::Warning;
        }

        appendLog(line, level);
        ++lines_loaded;
    }

    file.close();

    sak::logInfo("Loaded log file: {}", file_path.toStdString());
    return true;
}

void LogViewer::clear() {
    m_text_browser->clear();
    m_all_logs.clear();
}

void LogViewer::setAutoScroll(bool enabled) {
    m_auto_scroll = enabled;
    m_auto_scroll_checkbox->setChecked(enabled);
}

void LogViewer::onClearClicked() {
    auto reply = sak::showQuestionLogged(this,
                                         "Clear Logs",
                                         "Are you sure you want to clear all logs?",
                                         QMessageBox::Yes | QMessageBox::No,
                                         QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        clear();
    }
}

void LogViewer::onSaveClicked() {
    Q_ASSERT(m_text_browser);
    const QString file_path = QFileDialog::getSaveFileName(
        this, "Save Log File", QString(), "Log Files (*.log *.txt);;All Files (*.*)");

    if (file_path.isEmpty()) {
        return;
    }

    // QSaveFile stages to a temporary and only replaces the target on commit(). A plain
    // QFile with Truncate destroyed an existing log the moment it opened, and close()
    // without checking the stream flush reported "Save Complete" over a disk-full or I/O
    // failure -- so the user was told their log was saved while the old one was already
    // gone and the new one was short or empty.
    QSaveFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logError("Failed to save log file: {}", file_path.toStdString());
        sak::showWarningLogged(this,
                               "Save Error",
                               QString("Failed to save log file: %1").arg(file_path));
        return;
    }

    QTextStream out(&file);
    out << m_text_browser->toPlainText();
    out.flush();
    if (out.status() != QTextStream::Ok || !file.commit()) {
        file.cancelWriting();
        sak::logError("Failed to write log file: {}", file_path.toStdString());
        sak::showWarningLogged(
            this,
            "Save Error",
            QString("Failed to write log file: %1 (the existing file was left unchanged)")
                .arg(file_path));
        return;
    }

    sak::logInfo("Saved log file: {}", file_path.toStdString());
    sak::showInformationLogged(this, "Save Complete", QString("Log saved to: %1").arg(file_path));
}

void LogViewer::onFilterChanged(int index) {
    m_current_filter = static_cast<LogLevel>(index);
    applyFilters();
}

void LogViewer::onSearchTextChanged(const QString& text) {
    Q_UNUSED(text);
    applyFilters();
}

void LogViewer::onAutoScrollToggled(bool checked) {
    m_auto_scroll = checked;
}

void LogViewer::applyFilters() {
    Q_ASSERT(m_text_browser);
    Q_ASSERT(m_search_edit);
    m_text_browser->clear();

    const QString search_text = m_search_edit->text().toLower();

    for (const QString& log : m_all_logs) {
        // Apply level filter
        bool level_match = (m_current_filter == LogLevel::All);
        if (!level_match) {
            const QString level_text = getLevelText(m_current_filter);
            level_match = log.contains(level_text, Qt::CaseInsensitive);
        }

        // Apply search filter
        const bool search_match = search_text.isEmpty() || log.toLower().contains(search_text);

        if (level_match && search_match) {
            m_text_browser->append(log);
        }
    }
}

QString LogViewer::formatLogMessage(const QString& message, LogLevel level) const {
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    const QString level_text = getLevelText(level);
    const QString color = getLevelColor(level);

    // kHtmlLogMessage is an HTML template and the result is handed to QTextBrowser::append(), so
    // the log text -- file paths, program names, command output, AI text -- is escaped before it
    // is spliced in. Every other argument is a palette token, a timestamp, or an enum label.
    return QString::fromLatin1(sak::ui::kHtmlLogMessage)
        .arg(sak::ui::htmlColor(sak::ui::kColorTextMuted),
             timestamp,
             color,
             QString::number(sak::ui::kFontWeightBold),
             level_text,
             message.toHtmlEscaped());
}

QString LogViewer::getLevelColor(LogLevel level) const {
    switch (level) {
    case LogLevel::Info:
        return sak::ui::kStatusColorRunning;
    case LogLevel::Warning:
        return sak::ui::kStatusColorWarning;
    case LogLevel::Error:
        return sak::ui::kStatusColorError;
    default:
        return sak::ui::htmlColor(sak::ui::kColorTextBody);
    }
}

QString LogViewer::getLevelText(LogLevel level) {
    switch (level) {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    default:
        return "ALL";
    }
}
