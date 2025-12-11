// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/log_viewer.h"
#include "sak/logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QScrollBar>

LogViewer::LogViewer(QWidget* parent)
    : QWidget(parent)
    , m_text_browser(nullptr)
    , m_clear_button(nullptr)
    , m_save_button(nullptr)
    , m_auto_scroll_checkbox(nullptr)
    , m_filter_combo(nullptr)
    , m_search_edit(nullptr)
    , m_current_filter(LogLevel::All)
    , m_auto_scroll(true)
{
    setup_ui();
}

void LogViewer::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(8);
    main_layout->setContentsMargins(0, 0, 0, 0);

    // Toolbar
    auto* toolbar_layout = new QHBoxLayout();
    toolbar_layout->setSpacing(8);

    // Filter combo
    m_filter_combo = new QComboBox(this);
    m_filter_combo->addItem("All Logs");
    m_filter_combo->addItem("Info");
    m_filter_combo->addItem("Warning");
    m_filter_combo->addItem("Error");
    m_filter_combo->setToolTip("Filter logs by level");
    connect(m_filter_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LogViewer::on_filter_changed);
    toolbar_layout->addWidget(m_filter_combo);

    // Search box
    m_search_edit = new QLineEdit(this);
    m_search_edit->setPlaceholderText("Search logs...");
    m_search_edit->setClearButtonEnabled(true);
    connect(m_search_edit, &QLineEdit::textChanged,
            this, &LogViewer::on_search_text_changed);
    toolbar_layout->addWidget(m_search_edit);

    // Auto-scroll checkbox
    m_auto_scroll_checkbox = new QCheckBox("Auto-scroll", this);
    m_auto_scroll_checkbox->setChecked(true);
    connect(m_auto_scroll_checkbox, &QCheckBox::toggled,
            this, &LogViewer::on_auto_scroll_toggled);
    toolbar_layout->addWidget(m_auto_scroll_checkbox);

    // Clear button
    m_clear_button = new QPushButton("Clear", this);
    connect(m_clear_button, &QPushButton::clicked,
            this, &LogViewer::on_clear_clicked);
    toolbar_layout->addWidget(m_clear_button);

    // Save button
    m_save_button = new QPushButton("Save Log", this);
    connect(m_save_button, &QPushButton::clicked,
            this, &LogViewer::on_save_clicked);
    toolbar_layout->addWidget(m_save_button);

    main_layout->addLayout(toolbar_layout);

    // Text browser
    m_text_browser = new QTextBrowser(this);
    m_text_browser->setOpenExternalLinks(false);
    m_text_browser->setReadOnly(true);
    m_text_browser->setFont(QFont("Consolas", 9));
    main_layout->addWidget(m_text_browser);
}

void LogViewer::appendLog(const QString& message, LogLevel level)
{
    QString formatted_msg = format_log_message(message, level);
    m_all_logs.append(formatted_msg);
    
    // Apply current filter
    if (m_current_filter == LogLevel::All || m_current_filter == level) {
        QString search_text = m_search_edit->text();
        if (search_text.isEmpty() || message.contains(search_text, Qt::CaseInsensitive)) {
            m_text_browser->append(formatted_msg);
            
            if (m_auto_scroll) {
                QScrollBar* scrollbar = m_text_browser->verticalScrollBar();
                scrollbar->setValue(scrollbar->maximum());
            }
        }
    }
}

bool LogViewer::loadLogFile(const QString& file_path)
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Load Error",
                           QString("Failed to open log file: %1").arg(file_path));
        return false;
    }

    clear();
    
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        
        // Try to detect log level from line content
        LogLevel level = LogLevel::Info;
        if (line.contains("ERROR", Qt::CaseInsensitive) || 
            line.contains("FATAL", Qt::CaseInsensitive)) {
            level = LogLevel::Error;
        } else if (line.contains("WARN", Qt::CaseInsensitive)) {
            level = LogLevel::Warning;
        }
        
        appendLog(line, level);
    }
    
    file.close();
    
    sak::log_info("Loaded log file: {}", file_path.toStdString());
    return true;
}

void LogViewer::clear()
{
    m_text_browser->clear();
    m_all_logs.clear();
}

void LogViewer::setAutoScroll(bool enabled)
{
    m_auto_scroll = enabled;
    m_auto_scroll_checkbox->setChecked(enabled);
}

void LogViewer::on_clear_clicked()
{
    auto reply = QMessageBox::question(
        this,
        "Clear Logs",
        "Are you sure you want to clear all logs?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        clear();
    }
}

void LogViewer::on_save_clicked()
{
    QString file_path = QFileDialog::getSaveFileName(
        this,
        "Save Log File",
        QString(),
        "Log Files (*.log *.txt);;All Files (*.*)"
    );
    
    if (file_path.isEmpty()) {
        return;
    }
    
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Save Error",
                           QString("Failed to save log file: %1").arg(file_path));
        return;
    }
    
    QTextStream out(&file);
    out << m_text_browser->toPlainText();
    file.close();
    
    sak::log_info("Saved log file: {}", file_path.toStdString());
    QMessageBox::information(this, "Save Complete",
                           QString("Log saved to: %1").arg(file_path));
}

void LogViewer::on_filter_changed(int index)
{
    m_current_filter = static_cast<LogLevel>(index);
    apply_filters();
}

void LogViewer::on_search_text_changed(const QString& text)
{
    Q_UNUSED(text);
    apply_filters();
}

void LogViewer::on_auto_scroll_toggled(bool checked)
{
    m_auto_scroll = checked;
}

void LogViewer::apply_filters()
{
    m_text_browser->clear();
    
    QString search_text = m_search_edit->text().toLower();
    
    for (const QString& log : m_all_logs) {
        // Apply level filter
        bool level_match = (m_current_filter == LogLevel::All);
        if (!level_match) {
            QString level_text = get_level_text(m_current_filter);
            level_match = log.contains(level_text, Qt::CaseInsensitive);
        }
        
        // Apply search filter
        bool search_match = search_text.isEmpty() || 
                           log.toLower().contains(search_text);
        
        if (level_match && search_match) {
            m_text_browser->append(log);
        }
    }
}

QString LogViewer::format_log_message(const QString& message, LogLevel level) const
{
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    QString level_text = get_level_text(level);
    QString color = get_level_color(level);
    
    return QString("<span style='color: gray;'>%1</span> "
                   "<span style='color: %2; font-weight: bold;'>[%3]</span> %4")
           .arg(timestamp, color, level_text, message);
}

QString LogViewer::get_level_color(LogLevel level) const
{
    switch (level) {
        case LogLevel::Info:    return "#4A90E2";
        case LogLevel::Warning: return "#F39C12";
        case LogLevel::Error:   return "#E74C3C";
        default:                return "#333333";
    }
}

QString LogViewer::get_level_text(LogLevel level) const
{
    switch (level) {
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error:   return "ERROR";
        default:                return "ALL";
    }
}
